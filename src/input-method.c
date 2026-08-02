#include "input-method.h"
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include "X11/keysym.h"
#include "X11/XKBlib.h"
#include "display.h"
#include "errors.h"
#include "window-internal.h"

// http://www.x.org/archive/X11R7.6/doc/man/man3/XOpenIM.3.xhtml
// http://www.x.org/archive/X11R7.6/doc/man/man3/XCreateIC.3.xhtml

static char *currLocaleModifierList = defaultLocaleModifierList;
static XIC focusedInputConnection = NULL;
/* The IC XSetICFocus is about to install, live only across its preedit-clear
 * callback. XDestroyIC nulls it if that callback destroys the incoming IC, so
 * XSetICFocus can bail instead of dereferencing freed memory.
 */
static XIC pendingFocusTarget = NULL;
static Bool focusedTextInputActive = False;

/* Committed IM strings awaiting a lookup, keyed by a unique id the synthetic
 * KeyPress carries in xkey.subwindow. Global because there is a single keyboard
 * and one host IME; any IC can retrieve its commit by id. Bounded so a client
 * that drops commit events without looking them up cannot grow this without
 * limit. These IM globals (this table plus focusedInputConnection) assume the
 * single-threaded host event loop the rest of the layer relies on; they are not
 * lock-guarded.
 */
static PendingCommit *pendingCommits = NULL;
static unsigned long nextCommitId = 1;
static int pendingCommitCount = 0;
#define MAX_PENDING_COMMITS 64

/* Count UTF-8 codepoints (not bytes) in a NUL-terminated string. Preedit and
 * lookup callbacks report character positions, so byte lengths would tell the
 * host app to delete or replace the wrong run of CJK text. Counting lead bytes
 * agrees with the validating decoder in missing.c on well-formed UTF-8, which
 * is all SDL emits; the two could only diverge on malformed input that never
 * reaches here.
 */
static int countUtf8Chars(const char *s)
{
    int count = 0;
    for (; s && *s; s++) {
        if ((*s & 0xc0) != 0x80)
            count++;
    }
    return count;
}

/* Encode a Latin-1/ASCII keysym as UTF-8.
 *
 * Returns the number of bytes the encoding needs (0 if the keysym is not
 * encodable). Writes into buffer only when it fits, so a return value greater
 * than nbytes means the caller must report XBufferOverflow rather than treating
 * the keysym as undecodable.
 */
static int appendKeysymUtf8(KeySym keysym, char *buffer, int nbytes)
{
    unsigned int codepoint;
    switch (keysym) {
    case XK_BackSpace:
        codepoint = '\b';
        break;
    case XK_Tab:
        codepoint = '\t';
        break;
    case XK_Linefeed:
        codepoint = '\n';
        break;
    case XK_Return:
    case XK_KP_Enter:
        codepoint = '\r';
        break;
    case XK_Escape:
        codepoint = 0x1b;
        break;
    case XK_Delete:
        codepoint = 0x7f;
        break;
    default:
        if (keysym >= XK_space && keysym <= XK_asciitilde)
            codepoint = (unsigned int) keysym;
        else if (keysym >= XK_nobreakspace && keysym <= XK_ydiaeresis)
            codepoint = (unsigned int) (keysym & 0xff);
        else if (((unsigned long) keysym & 0xff000000UL) == 0x01000000UL)
            /* X11 Unicode keysyms: 0x01000000 | codepoint. */
            codepoint = (unsigned int) ((unsigned long) keysym & 0x00ffffffUL);
        else
            return 0;
        break;
    }

    int needed;
    if (codepoint < 0x80)
        needed = 1;
    else if (codepoint < 0x800)
        needed = 2;
    else if (codepoint < 0x10000)
        needed = 3;
    else if (codepoint <= 0x10ffff)
        needed = 4;
    else
        return 0;

    if (!buffer || nbytes < needed)
        return needed;
    switch (needed) {
    case 1:
        buffer[0] = (char) codepoint;
        break;
    case 2:
        buffer[0] = (char) (0xc0 | (codepoint >> 6));
        buffer[1] = (char) (0x80 | (codepoint & 0x3f));
        break;
    case 3:
        buffer[0] = (char) (0xe0 | (codepoint >> 12));
        buffer[1] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        buffer[2] = (char) (0x80 | (codepoint & 0x3f));
        break;
    default:
        buffer[0] = (char) (0xf0 | (codepoint >> 18));
        buffer[1] = (char) (0x80 | ((codepoint >> 12) & 0x3f));
        buffer[2] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        buffer[3] = (char) (0x80 | (codepoint & 0x3f));
        break;
    }
    return needed;
}

/* Evict the oldest commit when the table is full. Only reachable when a client
 * stops looking commits up; the evicted commit's KeyPress, if still in flight,
 * then looks up as XLookupNone. That is the deliberate bound, hence the log.
 */
static void dropOldestCommit(void)
{
    PendingCommit *head = pendingCommits;
    if (!head)
        return;
    pendingCommits = head->next;
    pendingCommitCount--;
    LOG("InputMethod commit queue full; dropping unconsumed commit '%s'\n",
        head->text ? head->text : "");
    free(head->text);
    free(head);
}

unsigned long inputMethodSetCurrentText(char *text)
{
    /* A commit only makes sense while an IC owns focus; the caller takes
     * ownership of text otherwise.
     */
    if (!focusedInputConnection) {
        free(text);
        return 0;
    }
    PendingCommit *node = malloc(sizeof(*node));
    if (!node) {
        free(text);
        return 0;
    }
    /* id 0 is reserved to mean "no commit" (a real KeyPress leaves subwindow
     * None == 0), so skip it on wraparound. A collision with a still-live id
     * would need ~2^64 commits with one never consumed, so it is not guarded.
     */
    if (nextCommitId == 0)
        nextCommitId = 1;
    node->id = nextCommitId++;
    node->text = text;
    node->next = NULL;
    PendingCommit **link = &pendingCommits;
    while (*link)
        link = &(*link)->next;
    *link = node;
    if (++pendingCommitCount > MAX_PENDING_COMMITS)
        dropOldestCommit();
    return node->id;
}

char *inputMethodPendingText(unsigned long commitId)
{
    if (commitId == 0)
        return NULL;
    for (PendingCommit *node = pendingCommits; node; node = node->next) {
        if (node->id == commitId)
            return node->text;
    }
    return NULL;
}

void inputMethodConsumePendingText(unsigned long commitId)
{
    if (commitId == 0)
        return;
    PendingCommit **link = &pendingCommits;
    while (*link) {
        PendingCommit *node = *link;
        if (node->id == commitId) {
            *link = node->next;
            pendingCommitCount--;
            free(node->text);
            free(node);
            return;
        }
        link = &node->next;
    }
}

Bool inputMethodHasFocusedIC(void)
{
    return !!focusedInputConnection;
}

Bool inputMethodHasActiveTextInput(void)
{
    return focusedInputConnection && focusedTextInputActive;
}

Bool inputMethodHasActivePreedit(void)
{
    return focusedInputConnection &&
           GET_XIC_STRUCT(focusedInputConnection)->preeditActive;
}

void inputMethodNoteTextInputStopped(void)
{
    focusedTextInputActive = False;
}

static void restartTextInputIfFocused(XIC inputConnection)
{
    if (focusedInputConnection == inputConnection) {
        SDL_StartTextInput();
        focusedTextInputActive = True;
    }
}

void inputMethodUnsetFocus(XIC inputConnection)
{
    if (inputConnection && focusedInputConnection != inputConnection)
        return;
    if (focusedInputConnection)
        inputMethodHandlePreedit("", 0);
    /* The clear above can fire a done callback that re-focuses another IC; do
     * not clobber that. Only finish the unset when this IC still holds focus.
     */
    if (inputConnection && focusedInputConnection != inputConnection)
        return;
    /* If that clear was a reentrant no-op (we were called from inside a preedit
     * callback), preeditActive may still be set; clear it so the IC is not
     * stuck mid-preedit and skipping its start callback when focused again.
     */
    if (focusedInputConnection)
        GET_XIC_STRUCT(focusedInputConnection)->preeditActive = False;
    focusedInputConnection = NULL;
    focusedTextInputActive = False;
    SDL_StopTextInput();
}

static void clearInternalPreedit(_XIC *ic)
{
    if (!ic->hasPreeditDrawRect)
        return;
    if (!IS_TYPE(ic->preeditDrawWindow, WINDOW)) {
        /* The window we drew into was destroyed; drop the stale rect instead of
         * allocating a GC against a dead resource.
         */
        ic->hasPreeditDrawRect = False;
        return;
    }
    GC gc = XCreateGC(ic->display, ic->preeditDrawWindow, 0, NULL);
    if (!gc)
        return;
    XSetForeground(
        ic->display, gc,
        ic->hasPreeditBackground ? ic->preeditBackground : 0xffffffff);
    XFillRectangle(ic->display, ic->preeditDrawWindow, gc,
                   ic->preeditDrawRect.x, ic->preeditDrawRect.y,
                   (unsigned int) ic->preeditDrawRect.w,
                   (unsigned int) ic->preeditDrawRect.h);
    XFreeGC(ic->display, gc);
    ic->hasPreeditDrawRect = False;
}

static void setInternalPreeditFont(_XIC *ic, GC gc)
{
    static const char *const preeditFonts[] = {
        "*Arial Unicode*",       "*Hiragino Sans GB*", "*STHeiti*",
        "*Noto Sans CJK*",       "*Source Han Sans*",  "*WenQuanYi*",
        "*Droid Sans Fallback*", "helvetica"};
    /* Resolve the preedit font once per IC and cache it. Probing up to eight
     * patterns on every keystroke while composing is needless work, and
     * XDestroyIC frees the cached font.
     */
    if (!ic->preeditFontResolved) {
        for (size_t i = 0; i < sizeof(preeditFonts) / sizeof(preeditFonts[0]);
             i++) {
            ic->preeditFont = XLoadQueryFont(ic->display, preeditFonts[i]);
            if (ic->preeditFont)
                break;
        }
        ic->preeditFontResolved = True;
    }
    if (ic->preeditFont)
        XSetFont(ic->display, gc, ic->preeditFont->fid);
}

static void drawInternalPreedit(_XIC *ic, const char *text, int len)
{
    if ((ic->style &
         (XIMPreeditArea | XIMPreeditPosition | XIMPreeditNothing)) == 0)
        return;
    Window target = (ic->style & XIMPreeditArea) ? ic->client : ic->focus;
    if (target == None)
        target = ic->focus != None ? ic->focus : ic->client;
    if (target == None || !IS_TYPE(target, WINDOW) || IS_INPUT_ONLY(target))
        return;
    clearInternalPreedit(ic);
    if (len <= 0)
        return;
    /* Preedit strings are short; cap before len * 8 so a pathological length
     * cannot overflow the rect math or hand XDrawString a huge count.
     */
    if (len > 4096)
        len = 4096;
    int minWidth = len * 8 + 8;
    SDL_Rect rect = {0, 0, minWidth, 18};
    if (ic->inputRect) {
        rect.x = ic->inputRect->x;
        rect.y = ic->inputRect->y;
        if (ic->inputRect->w > 0)
            rect.w = ic->inputRect->w;
        if (ic->inputRect->h > 0)
            rect.h = ic->inputRect->h;
    }
    if ((ic->style & XIMPreeditNothing) && !ic->inputRect) {
        rect.x = 2;
        rect.y = 2;
    }
    if (rect.w < minWidth)
        rect.w = minWidth;
    if (rect.h < 18)
        rect.h = 18;
    GC gc = XCreateGC(ic->display, target, 0, NULL);
    if (!gc)
        return;
    unsigned long bg =
        ic->hasPreeditBackground ? ic->preeditBackground : 0xffffffff;
    unsigned long fg =
        ic->preeditForeground ? ic->preeditForeground : 0xff000000;
    XSetForeground(ic->display, gc, bg);
    XFillRectangle(ic->display, target, gc, rect.x, rect.y,
                   (unsigned int) rect.w, (unsigned int) rect.h);
    XSetForeground(ic->display, gc, fg);
    XSetBackground(ic->display, gc, bg);
    setInternalPreeditFont(ic, gc);
    Xutf8DrawString(ic->display, target, NULL, gc, rect.x + 4,
                    rect.y + rect.h - 5, text, len);
    XFreeGC(ic->display, gc);
    ic->preeditDrawWindow = target;
    ic->preeditDrawRect = rect;
    ic->hasPreeditDrawRect = True;
}

static void applyTextInputRectForIC(_XIC *ic)
{
    if (!ic->inputRect)
        return;
    Window source = (ic->style & XIMPreeditArea) ? ic->client : ic->focus;
    if (source == None || !IS_TYPE(source, WINDOW))
        source = ic->focus != None ? ic->focus : ic->client;
    if (source == None || !IS_TYPE(source, WINDOW))
        return;
    Window top = source;
    while (GET_PARENT(top) != None && GET_PARENT(top) != SCREEN_WINDOW)
        top = GET_PARENT(top);
    SDL_Rect rect = *ic->inputRect;
    if (rect.w <= 0)
        rect.w = 1;
    if (rect.h <= 0)
        rect.h = 18;
    translateWindowPoint(source, top, rect.x, rect.y, &rect.x, &rect.y);
    SDL_SetTextInputRect(&rect);
#ifdef __EMSCRIPTEN__
    char script[160];
    snprintf(script, sizeof(script),
             "if(window.__libx11CompatSetImeRect)"
             "window.__libx11CompatSetImeRect(%d,%d,%d,%d)",
             rect.x, rect.y, rect.w, rect.h);
    emscripten_run_script(script);
#endif
}

static void handlePreeditImpl(const char *text, int caret)
{
    if (!focusedInputConnection)
        return;
    /* User preedit callbacks below can re-enter Xlib and XDestroyIC or unfocus
     * this IC. Capture it so each callback can be followed by a check that
     * bails before touching freed or no-longer-focused state.
     */
    XIC self = focusedInputConnection;
    _XIC *ic = GET_XIC_STRUCT(self);
    const char *s = text ? text : "";
    int len = (int) strlen(s);
    int charLen = countUtf8Chars(s);
    /* The host reports where the insertion point sits inside the composition;
     * clamp it into the preedit so a bogus backend value cannot drive the caret
     * past the text. A negative value means "unreported", so trail at the end
     * where a fresh composition normally sits.
     */
    if (caret < 0 || caret > charLen)
        caret = charLen;
    if ((ic->style & XIMPreeditCallbacks) == 0)
        drawInternalPreedit(ic, s, len);

    /* Start fires once when preedit text first appears; it is independent of
     * whether the IC registered a draw callback.
     */
    if (!ic->preeditActive && charLen > 0) {
        ic->preeditActive = True;
        if (ic->hasPreeditStartCallback && ic->preeditStartCallback.callback) {
            ic->preeditStartCallback.callback(
                (XIM) self, ic->preeditStartCallback.client_data, NULL);
            /* A callback that destroyed/unfocused this IC drops focus, so bail
             * before touching freed or no-longer-focused state. While merely
             * destroying (focus unchanged, IC still alive) keep going so the
             * done callback below still fires and start/done stay balanced.
             */
            if (focusedInputConnection != self)
                return;
        }
    }

    /* Only deliver draw/caret while a preedit is actually live. events.c clears
     * preedit with an empty string before every commit, so without this gate a
     * callback-style client gets a spurious empty draw on each plain keystroke.
     * A real clear still fires here because preeditActive is not reset until
     * the done block below. During teardown the clearing draw fires but the
     * caret is skipped, then control falls through to the done callback.
     */
    if ((ic->preeditActive || charLen > 0) && ic->hasPreeditDrawCallback &&
        ic->preeditDrawCallback.callback) {
        XIMText ximText = {
            .length =
                (unsigned short) (charLen > USHRT_MAX ? USHRT_MAX : charLen),
            .feedback = NULL,
            .encoding_is_wchar = False,
            .string.multi_byte = (char *) s,
        };
        XIMPreeditDrawCallbackStruct draw = {
            .caret = caret,
            .chg_first = 0,
            .chg_length = ic->preeditLength,
            .text = &ximText,
        };
        ic->preeditDrawCallback.callback(
            (XIM) self, ic->preeditDrawCallback.client_data, (XPointer) &draw);
        if (focusedInputConnection != self)
            return;
        if (!ic->destroying && ic->hasPreeditCaretCallback &&
            ic->preeditCaretCallback.callback) {
            XIMPreeditCaretCallbackStruct caretCb = {
                .position = caret,
                .direction = XIMAbsolutePosition,
                .style = XIMIsPrimary,
            };
            ic->preeditCaretCallback.callback(
                (XIM) self, ic->preeditCaretCallback.client_data,
                (XPointer) &caretCb);
            if (focusedInputConnection != self)
                return;
        }
    }
    ic->preeditLength = charLen;

    /* Done fires when preedit text clears, also independent of the draw
     * callback so start/done stay balanced for status-only ICs.
     */
    if (ic->preeditActive && charLen == 0) {
        ic->preeditActive = False;
        if (ic->hasPreeditDoneCallback && ic->preeditDoneCallback.callback)
            ic->preeditDoneCallback.callback(
                (XIM) self, ic->preeditDoneCallback.client_data, NULL);
    }
}

void inputMethodHandlePreedit(const char *text, int caret)
{
    /* A preedit callback may unfocus or XDestroyIC its IC, and both route back
     * through inputMethodUnsetFocus -> inputMethodHandlePreedit("", 0), which
     * would re-fire the callbacks and recurse. Run the body at most one level
     * deep; the nested clear is redundant because teardown already drops focus.
     */
    static Bool reentered = False;
    if (reentered)
        return;
    reentered = True;
    handlePreeditImpl(text, caret);
    reentered = False;
}

KeySym getKeySymForChar(char c)
{
    const static struct {
        char character;
        KeySym keySym;
    } charMapping[] = {
        {' ', XK_space},        {'\n', XK_Linefeed},   {'\r', XK_Return},
        {'\t', XK_Tab},         {'\b', XK_BackSpace},  {'-', XK_minus},
        {'+', XK_plus},         {'#', XK_numbersign},  {'*', XK_asterisk},
        {'~', XK_asciitilde},   {'\'', XK_quoteright}, {'"', XK_quotedbl},
        {'!', XK_exclam},       {'@', XK_at},          {'%', XK_percent},
        {'&', XK_ampersand},    {'$', XK_dollar},      {'/', XK_slash},
        {'(', XK_parenleft},    {')', XK_parenright},  {'[', XK_bracketleft},
        {']', XK_bracketright}, {'{', XK_braceleft},   {'}', XK_braceright},
        {'?', XK_question},     {'\\', XK_backslash},  {'<', XK_less},
        {'>', XK_greater},      {'|', XK_bar},         {'_', XK_underscore},
        {'^', XK_asciicircum},  {',', XK_comma},       {';', XK_semicolon},
        {'.', XK_period},       {':', XK_colon},       {'=', XK_equal},
        {'`', XK_quoteleft},
    };
    if (c >= 'a' && c <= 'z')
        return XK_a + (c - 'a');
    if (c >= 'A' && c <= 'Z')
        return XK_A + (c - 'A');
    if (c >= '0' && c <= '9')
        return XK_0 + (c - '0');
    for (size_t i = 0; i < sizeof(charMapping) / sizeof(charMapping[0]); i++) {
        if (charMapping[i].character == c)
            return charMapping[i].keySym;
    }
    LOG("Did not Found mapping for char '%c' (%d)\n", c, c);
    return NoSymbol;
}

void inputMethodReset(void)
{
    /* Drop focus and every queued commit. Called on XCloseIM and on the final
     * XCloseDisplay so a stale focused IC or leftover commit cannot outlive the
     * display it belongs to. Route through inputMethodUnsetFocus so an active
     * preedit is cleared and host text input is stopped rather than left
     * dangling.
     */
    if (focusedInputConnection)
        inputMethodUnsetFocus(focusedInputConnection);
    while (pendingCommits) {
        PendingCommit *next = pendingCommits->next;
        free(pendingCommits->text);
        free(pendingCommits);
        pendingCommits = next;
    }
    pendingCommitCount = 0;
}

Status XCloseIM(XIM inputMethod)
{
    /* Unlike full Xlib this does not auto-destroy ICs created under the IM; the
     * driven toolkits (Xt/Motif) XDestroyIC explicitly. It only drops IM-global
     * state shared across the single host keyboard.
     */
    inputMethodReset();
    return 0;
}

char *XSetLocaleModifiers(_Xconst char *modifier_list)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XSetLocaleModifiers.3.xhtml
    if (!modifier_list || modifier_list[0] == '\0')
        return currLocaleModifierList;
    Bool isImSpec = modifier_list[0] == '@' && modifier_list[1] == 'i' &&
                    modifier_list[2] == 'm' && modifier_list[3] == '=';
    Bool currentIsDefault =
        !strcmp(currLocaleModifierList, defaultLocaleModifierList);
    if (!isImSpec && !currentIsDefault)
        return NULL;
    char *curr = currLocaleModifierList;
    currLocaleModifierList = strdup(modifier_list);
    if (!currLocaleModifierList) {
        currLocaleModifierList = curr;
        return NULL;
    }
    return curr;
}

XIM XOpenIM(Display *display,
            struct _XrmHashBucketRec *db,
            char *res_name,
            char *res_class)
{
    char *envXModifiers = getenv("XMODIFIERS");
    if (envXModifiers)
        XSetLocaleModifiers(envXModifiers);
    return (XIM) display;
}

void XDestroyIC(XIC inputConnection)
{
    if (!inputConnection)
        return;
    _XIC *ic = GET_XIC_STRUCT(inputConnection);
    if (ic->destroying)
        return;
    ic->destroying = True;
    /* If XSetICFocus is mid-switch into this IC, tell it the target died so it
     * does not install and dereference this freed connection.
     */
    if (pendingFocusTarget == inputConnection)
        pendingFocusTarget = NULL;
    inputMethodUnsetFocus(inputConnection);
    free(ic->inputRect);
    if (ic->preeditFont)
        XFreeFont(ic->display, ic->preeditFont);
    free(inputConnection);
}

static Bool styleIsSupported(XIMStyle style)
{
    for (int i = 0; i < supportedStyles.count_styles; i++) {
        if (supportedStyles.supported_styles[i] == style)
            return True;
    }
    return False;
}

static void consumeICAttributeValue(void **attrs, int *index)
{
    (void) attrs[(*index)++];
}

static Bool parseCommonICAttributes(XIC inputConnection,
                                    XVaNestedList attributes,
                                    Bool preedit)
{
    if (!attributes)
        return False;
    void **attrs = (void **) attributes;
    int i = 0;
    char *key;
    while ((key = attrs[i++])) {
        if (!strcmp(key, XNVaNestedList)) {
            if (!parseCommonICAttributes(inputConnection, attrs[i++], preedit))
                return False;
        } else if (!strcmp(key, XNArea)) {
            XRectangle *rect = attrs[i++];
            if ((XIMPreeditArea & GET_XIC_STRUCT(inputConnection)->style) !=
                0) {
                SDL_Rect *inputRect;
                if ((inputRect = GET_XIC_STRUCT(inputConnection)->inputRect) ==
                    NULL) {
                    inputRect = malloc(sizeof(SDL_Rect));
                    if (!inputRect)
                        return False;
                    GET_XIC_STRUCT(inputConnection)->inputRect = inputRect;
                }
                inputRect->x = rect->x;
                inputRect->y = rect->y;
                inputRect->w = rect->width;
                inputRect->h = rect->height;
                applyTextInputRectForIC(GET_XIC_STRUCT(inputConnection));
            }
        } else if (!strcmp(key, XNAreaNeeded)) {
            consumeICAttributeValue(attrs, &i);
        } else if (!strcmp(key, XNSpotLocation)) {
            XPoint *point = attrs[i++];
            if ((XIMPreeditPosition & GET_XIC_STRUCT(inputConnection)->style) !=
                0) {
                SDL_Rect *inputRect;
                if ((inputRect = GET_XIC_STRUCT(inputConnection)->inputRect) ==
                    NULL) {
                    inputRect = malloc(sizeof(SDL_Rect));
                    if (!inputRect)
                        return False;
                    GET_XIC_STRUCT(inputConnection)->inputRect = inputRect;
                }
                inputRect->x = point->x;
                inputRect->y = point->y;
                inputRect->w = 0;
                inputRect->h = 0;
                applyTextInputRectForIC(GET_XIC_STRUCT(inputConnection));
            }
        } else if (!strcmp(key, XNColormap) || !strcmp(key, XNStdColormap) ||
                   !strcmp(key, XNBackgroundPixmap) ||
                   !strcmp(key, XNLineSpace) || !strcmp(key, XNCursor) ||
                   !strcmp(key, XNStatusStartCallback) ||
                   !strcmp(key, XNStatusDoneCallback) ||
                   !strcmp(key, XNStatusDrawCallback)) {
            consumeICAttributeValue(attrs, &i);
        } else if (!strcmp(key, XNPreeditStartCallback)) {
            XIMCallback *callback = attrs[i++];
            if (preedit && callback) {
                GET_XIC_STRUCT(inputConnection)->preeditStartCallback =
                    *callback;
                GET_XIC_STRUCT(inputConnection)->hasPreeditStartCallback = True;
            }
        } else if (!strcmp(key, XNPreeditDoneCallback)) {
            XIMCallback *callback = attrs[i++];
            if (preedit && callback) {
                GET_XIC_STRUCT(inputConnection)->preeditDoneCallback =
                    *callback;
                GET_XIC_STRUCT(inputConnection)->hasPreeditDoneCallback = True;
            }
        } else if (!strcmp(key, XNPreeditDrawCallback)) {
            XIMCallback *callback = attrs[i++];
            if (preedit && callback) {
                GET_XIC_STRUCT(inputConnection)->preeditDrawCallback =
                    *callback;
                GET_XIC_STRUCT(inputConnection)->hasPreeditDrawCallback = True;
            }
        } else if (!strcmp(key, XNPreeditCaretCallback)) {
            XIMCallback *callback = attrs[i++];
            if (preedit && callback) {
                GET_XIC_STRUCT(inputConnection)->preeditCaretCallback =
                    *callback;
                GET_XIC_STRUCT(inputConnection)->hasPreeditCaretCallback = True;
            }
        } else if (!strcmp(key, XNForeground)) {
            unsigned long value = (unsigned long) (uintptr_t) attrs[i++];
            if (preedit)
                GET_XIC_STRUCT(inputConnection)->preeditForeground = value;
            else
                GET_XIC_STRUCT(inputConnection)->statusForeground = value;
        } else if (!strcmp(key, XNBackground)) {
            unsigned long value = (unsigned long) (uintptr_t) attrs[i++];
            if (preedit) {
                GET_XIC_STRUCT(inputConnection)->preeditBackground = value;
                GET_XIC_STRUCT(inputConnection)->hasPreeditBackground = True;
            } else {
                GET_XIC_STRUCT(inputConnection)->statusBackground = value;
            }
        } else if (!strcmp(key, XNFontSet)) {
            GET_XIC_STRUCT(inputConnection)->fontSet = attrs[i++];
        } else {
            LOG("parseCommonICAttributes failed for key %s\n", key);
            return False;
        }
    }
    return True;
}

Bool parsePreEditAttributes(XIC inputConnection, XVaNestedList attributes)
{
    return parseCommonICAttributes(inputConnection, attributes, True);
}

static Bool parseStatusAttributes(XIC inputConnection, XVaNestedList attributes)
{
    return parseCommonICAttributes(inputConnection, attributes, False);
}

static Bool fillCommonICAttributes(XIC inputConnection,
                                   XVaNestedList returnArgs,
                                   Bool preedit)
{
    if (!returnArgs)
        return False;
    void **attrs = (void **) returnArgs;
    int i = 0;
    char *key;
    while ((key = attrs[i++])) {
        if (!strcmp(key, XNVaNestedList)) {
            if (!fillCommonICAttributes(inputConnection, attrs[i++], preedit))
                return False;
        } else if (!strcmp(key, XNArea)) {
            if (!GET_XIC_STRUCT(inputConnection)->inputRect) {
                XRectangle *rect = attrs[i++];
                rect->x = 0;
                rect->y = 0;
                rect->width = 0;
                rect->height = 0;
                continue;
            }
            XRectangle *rect = attrs[i++];
            rect->x = GET_XIC_STRUCT(inputConnection)->inputRect->x;
            rect->y = GET_XIC_STRUCT(inputConnection)->inputRect->y;
            rect->width = GET_XIC_STRUCT(inputConnection)->inputRect->w;
            rect->height = GET_XIC_STRUCT(inputConnection)->inputRect->h;
        } else if (!strcmp(key, XNAreaNeeded)) {
            XRectangle *rect = attrs[i++];
            rect->x = 0;
            rect->y = 0;
            rect->width = 0;
            rect->height = 0;
        } else if (!strcmp(key, XNSpotLocation)) {
            if (!GET_XIC_STRUCT(inputConnection)->inputRect) {
                XPoint *point = attrs[i++];
                point->x = 0;
                point->y = 0;
                continue;
            }
            XPoint *point = attrs[i++];
            point->x = GET_XIC_STRUCT(inputConnection)->inputRect->x;
            point->y = GET_XIC_STRUCT(inputConnection)->inputRect->y;
        } else if (!strcmp(key, XNFontSet)) {
            XFontSet *fontSet = attrs[i++];
            *fontSet = GET_XIC_STRUCT(inputConnection)->fontSet;
        } else if (!strcmp(key, XNForeground)) {
            unsigned long *value = attrs[i++];
            *value = preedit
                         ? GET_XIC_STRUCT(inputConnection)->preeditForeground
                         : GET_XIC_STRUCT(inputConnection)->statusForeground;
        } else if (!strcmp(key, XNBackground)) {
            unsigned long *value = attrs[i++];
            *value = preedit
                         ? GET_XIC_STRUCT(inputConnection)->preeditBackground
                         : GET_XIC_STRUCT(inputConnection)->statusBackground;
        } else if (!strcmp(key, XNColormap) || !strcmp(key, XNStdColormap) ||
                   !strcmp(key, XNBackgroundPixmap) ||
                   !strcmp(key, XNLineSpace) || !strcmp(key, XNCursor) ||
                   !strcmp(key, XNStatusStartCallback) ||
                   !strcmp(key, XNStatusDoneCallback) ||
                   !strcmp(key, XNStatusDrawCallback)) {
            consumeICAttributeValue(attrs, &i);
        } else if (!strcmp(key, XNPreeditStartCallback)) {
            XIMCallback *callback = attrs[i++];
            if (preedit && callback &&
                GET_XIC_STRUCT(inputConnection)->hasPreeditStartCallback)
                *callback =
                    GET_XIC_STRUCT(inputConnection)->preeditStartCallback;
        } else if (!strcmp(key, XNPreeditDoneCallback)) {
            XIMCallback *callback = attrs[i++];
            if (preedit && callback &&
                GET_XIC_STRUCT(inputConnection)->hasPreeditDoneCallback)
                *callback =
                    GET_XIC_STRUCT(inputConnection)->preeditDoneCallback;
        } else if (!strcmp(key, XNPreeditDrawCallback)) {
            XIMCallback *callback = attrs[i++];
            if (preedit && callback &&
                GET_XIC_STRUCT(inputConnection)->hasPreeditDrawCallback)
                *callback =
                    GET_XIC_STRUCT(inputConnection)->preeditDrawCallback;
        } else if (!strcmp(key, XNPreeditCaretCallback)) {
            XIMCallback *callback = attrs[i++];
            if (preedit && callback &&
                GET_XIC_STRUCT(inputConnection)->hasPreeditCaretCallback)
                *callback =
                    GET_XIC_STRUCT(inputConnection)->preeditCaretCallback;
        } else {
            LOG("fillCommonICAttributes failed for key %s\n", key);
            return False;
        }
    }
    return True;
}

Bool fillPreEditAttributes(XIC inputConnection, XVaNestedList returnArgs)
{
    return fillCommonICAttributes(inputConnection, returnArgs, True);
}

static Bool fillStatusAttributes(XIC inputConnection, XVaNestedList returnArgs)
{
    return fillCommonICAttributes(inputConnection, returnArgs, False);
}

static char *setICListValues(XIC inputConnection,
                             XVaNestedList attributes,
                             Bool allowSetReadOnly)
{
    if (!attributes)
        return NULL;
    void **attrs = (void **) attributes;
    int i = 0;
    char *key;
    while ((key = attrs[i++])) {
        if (!strcmp(key, XNVaNestedList)) {
            char *failed =
                setICListValues(inputConnection, attrs[i++], allowSetReadOnly);
            if (failed)
                return failed;
        } else if (!strcmp(key, XNInputStyle)) {
            if (!allowSetReadOnly)
                return key;
            XIMStyle style = (XIMStyle) (uintptr_t) attrs[i++];
            if (!styleIsSupported(style))
                return key;
            GET_XIC_STRUCT(inputConnection)->style = style;
        } else if (!strcmp(key, XNClientWindow)) {
            Window clientWindow = (Window) attrs[i++];
            if (!IS_TYPE(clientWindow, WINDOW) ||
                clientWindow == SCREEN_WINDOW) {
                return key;
            }
            Window topLevel = clientWindow;
            while (GET_PARENT(topLevel) != SCREEN_WINDOW)
                topLevel = GET_PARENT(topLevel);
            if (IS_MAPPED_TOP_LEVEL_WINDOW(topLevel)) {
                SDL_Window *sdlWindow = GET_WINDOW_STRUCT(topLevel)->sdlWindow;
                SDL_RaiseWindow(sdlWindow);
                /* The input rect and host text input belong to the IC that owns
                 * focus; reconfiguring an unfocused IC must not retarget or
                 * stop the focused one's IME.
                 */
                if (focusedInputConnection == inputConnection) {
                    if (GET_XIC_STRUCT(inputConnection)->inputRect)
                        applyTextInputRectForIC(
                            GET_XIC_STRUCT(inputConnection));
                    inputMethodNoteTextInputStopped();
                    SDL_StopTextInput();
                    restartTextInputIfFocused(inputConnection);
                }
            }
            GET_XIC_STRUCT(inputConnection)->client = clientWindow;
            if (GET_XIC_STRUCT(inputConnection)->focus == None)
                GET_XIC_STRUCT(inputConnection)->focus = clientWindow;
        } else if (!strcmp(key, XNFocusWindow)) {
            Window focusWindow = (Window) attrs[i++];
            if (!IS_TYPE(focusWindow, WINDOW) || focusWindow == SCREEN_WINDOW)
                return key;
            GET_XIC_STRUCT(inputConnection)->focus = focusWindow;
        } else if (!strcmp(key, XNPreeditAttributes)) {
            if (!parsePreEditAttributes(inputConnection, attrs[i++]))
                return key;
        } else if (!strcmp(key, XNStatusAttributes)) {
            if (!parseStatusAttributes(inputConnection, attrs[i++]))
                return key;
        } else if (!strcmp(key, XNFilterEvents)) {
            GET_XIC_STRUCT(inputConnection)->eventFilter =
                (unsigned long) (uintptr_t) attrs[i++];
        } else if (!strcmp(key, XNGeometryCallback) ||
                   !strcmp(key, XNDestroyCallback) ||
                   !strcmp(key, XNResourceName) ||
                   !strcmp(key, XNResourceClass)) {
            consumeICAttributeValue(attrs, &i);
        } else {
            return key;
        }
    }
    return NULL;
}

char *setICValues(XIC inputConnection, va_list arguments, Bool allowSetReadOnly)
{
    char *key = NULL;
    while ((key = va_arg(arguments, char *))) {
        if (!strcmp(key, XNVaNestedList)) {
            char *failed = setICListValues(inputConnection,
                                           va_arg(arguments, XVaNestedList),
                                           allowSetReadOnly);
            if (failed)
                return failed;
        } else if (!strcmp(key, XNInputStyle)) {
            if (!allowSetReadOnly)
                break;
            GET_XIC_STRUCT(inputConnection)->style =
                va_arg(arguments, XIMStyle);
            if (!styleIsSupported(GET_XIC_STRUCT(inputConnection)->style))
                break;
        } else if (!strcmp(key, XNClientWindow)) {
            Window clientWindow = va_arg(arguments, Window);
            if (!IS_TYPE(clientWindow, WINDOW) ||
                clientWindow == SCREEN_WINDOW) {
                break;
            }
            Window topLevel = clientWindow;
            while (GET_PARENT(topLevel) != SCREEN_WINDOW)
                topLevel = GET_PARENT(topLevel);
            if (IS_MAPPED_TOP_LEVEL_WINDOW(topLevel)) {
                SDL_Window *sdlWindow = GET_WINDOW_STRUCT(topLevel)->sdlWindow;
                SDL_RaiseWindow(sdlWindow);
                /* Retarget SDL's IM window only for the IC that owns focus, so
                 * reconfiguring an unfocused IC does not move or stop the
                 * focused one's host text input. The restart keeps printable
                 * keydowns paired with TEXTINPUT.
                 */
                if (focusedInputConnection == inputConnection) {
                    if (GET_XIC_STRUCT(inputConnection)->inputRect)
                        applyTextInputRectForIC(
                            GET_XIC_STRUCT(inputConnection));
                    inputMethodNoteTextInputStopped();
                    SDL_StopTextInput();
                    restartTextInputIfFocused(inputConnection);
                }
            }
            GET_XIC_STRUCT(inputConnection)->client = clientWindow;
            if (GET_XIC_STRUCT(inputConnection)->focus == None)
                GET_XIC_STRUCT(inputConnection)->focus = clientWindow;
        } else if (!strcmp(key, XNFocusWindow)) {
            Window focusWindow = va_arg(arguments, Window);
            if (!IS_TYPE(focusWindow, WINDOW) || focusWindow == SCREEN_WINDOW)
                break;
            GET_XIC_STRUCT(inputConnection)->focus = focusWindow;
        } else if (!strcmp(key, XNPreeditAttributes)) {
            if (!parsePreEditAttributes(inputConnection,
                                        va_arg(arguments, XVaNestedList)))
                break;
        } else if (!strcmp(key, XNStatusAttributes)) {
            if (!parseStatusAttributes(inputConnection,
                                       va_arg(arguments, XVaNestedList)))
                break;
        } else if (!strcmp(key, XNFilterEvents)) {
            GET_XIC_STRUCT(inputConnection)->eventFilter =
                va_arg(arguments, unsigned long);
        } else if (!strcmp(key, XNGeometryCallback) ||
                   !strcmp(key, XNDestroyCallback) ||
                   !strcmp(key, XNResourceName) ||
                   !strcmp(key, XNResourceClass)) {
            (void) va_arg(arguments, void *);
        } else {
            break;
        }
    }
    return key;
}

XIC XCreateIC(XIM inputMethod, ...)
{
    XIC inputConnection = malloc(sizeof(_XIC));
    if (!inputConnection) {
        handleOutOfMemory(0, (Display *) inputMethod, 0, 0);
        return NULL;
    }
    GET_XIC_STRUCT(inputConnection)->display = (Display *) inputMethod;
    GET_XIC_STRUCT(inputConnection)->style = XIMUndefined;
    GET_XIC_STRUCT(inputConnection)->client = None;
    GET_XIC_STRUCT(inputConnection)->focus = None;
    GET_XIC_STRUCT(inputConnection)->fontSet = NULL;
    GET_XIC_STRUCT(inputConnection)->inputRect = NULL;
    GET_XIC_STRUCT(inputConnection)->eventFilter =
        KeyPressMask | KeyReleaseMask;
    GET_XIC_STRUCT(inputConnection)->preeditForeground = 0;
    GET_XIC_STRUCT(inputConnection)->preeditBackground = 0;
    GET_XIC_STRUCT(inputConnection)->hasPreeditBackground = False;
    GET_XIC_STRUCT(inputConnection)->statusForeground = 0;
    GET_XIC_STRUCT(inputConnection)->statusBackground = 0;
    GET_XIC_STRUCT(inputConnection)->hasPreeditStartCallback = False;
    GET_XIC_STRUCT(inputConnection)->hasPreeditDoneCallback = False;
    GET_XIC_STRUCT(inputConnection)->hasPreeditDrawCallback = False;
    GET_XIC_STRUCT(inputConnection)->hasPreeditCaretCallback = False;
    GET_XIC_STRUCT(inputConnection)->preeditActive = False;
    GET_XIC_STRUCT(inputConnection)->preeditLength = 0;
    GET_XIC_STRUCT(inputConnection)->hasPreeditDrawRect = False;
    GET_XIC_STRUCT(inputConnection)->preeditDrawWindow = None;
    GET_XIC_STRUCT(inputConnection)->preeditDrawRect = (SDL_Rect) {0, 0, 0, 0};
    GET_XIC_STRUCT(inputConnection)->preeditStartCallback =
        (XIMCallback) {NULL, NULL};
    GET_XIC_STRUCT(inputConnection)->preeditDoneCallback =
        (XIMCallback) {NULL, NULL};
    GET_XIC_STRUCT(inputConnection)->preeditDrawCallback =
        (XIMCallback) {NULL, NULL};
    GET_XIC_STRUCT(inputConnection)->preeditCaretCallback =
        (XIMCallback) {NULL, NULL};
    GET_XIC_STRUCT(inputConnection)->preeditFont = NULL;
    GET_XIC_STRUCT(inputConnection)->preeditFontResolved = False;
    GET_XIC_STRUCT(inputConnection)->destroying = False;
    va_list argumentList;
    va_start(argumentList, inputMethod);
    char *key;
    if ((key = setICValues(inputConnection, argumentList, True))) {
        LOG("setICValues failed in %s because of key %s!\n", __func__, key);
        va_end(argumentList);
        /* setICValues may have allocated inputRect before failing; XDestroyIC
         * tolerates a partially built IC and frees it.
         */
        XDestroyIC(inputConnection);
        return NULL;
    }
    va_end(argumentList);
    if (GET_XIC_STRUCT(inputConnection)->style == XIMUndefined) {
        XDestroyIC(inputConnection);
        return NULL;
    }
    if (!focusedInputConnection)
        XSetICFocus(inputConnection);
    return inputConnection;
}

char *XSetICValues(XIC inputConnection, ...)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XGetICValues.3.xhtml
    va_list argumentList;
    va_start(argumentList, inputConnection);
    char *res = setICValues(inputConnection, argumentList, False);
    va_end(argumentList);
    return res;
}

XVaNestedList XVaCreateNestedList(int dummy, ...)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XVaCreateNestedList.3.xhtml
    (void) dummy;
    va_list argumentList, argCount;
    size_t i = 0, nArgs = 0;
    void *item;
    va_start(argumentList, dummy);
    va_copy(argCount, argumentList);
    while (va_arg(argCount, void *))
        nArgs++;
    va_end(argCount);
    void **list = malloc(sizeof(void *) * (nArgs + 1));
    if (!list) {
        va_end(argumentList);
        return NULL;
    }
    while ((item = va_arg(argumentList, void *)))
        list[i++] = item;
    va_end(argumentList);
    list[i] = NULL;
    return list;
}

char *XGetICValues(XIC inputConnection, ...)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XGetICValues.3.xhtml
    va_list argumentList;
    va_start(argumentList, inputConnection);
    char *key = NULL;
    while ((key = va_arg(argumentList, char *))) {
        if (!strcmp(key, XNInputStyle)) {
            XIMStyle *styleReturn = va_arg(argumentList, XIMStyle *);
            *styleReturn = GET_XIC_STRUCT(inputConnection)->style;
        } else if (!strcmp(key, XNClientWindow)) {
            Window *clientReturn = va_arg(argumentList, Window *);
            *clientReturn = GET_XIC_STRUCT(inputConnection)->client;
        } else if (!strcmp(key, XNFocusWindow)) {
            Window *focusReturn = va_arg(argumentList, Window *);
            *focusReturn = GET_XIC_STRUCT(inputConnection)->focus;
        } else if (!strcmp(key, XNPreeditAttributes)) {
            if (!fillPreEditAttributes(inputConnection,
                                       va_arg(argumentList, XVaNestedList)))
                break;
        } else if (!strcmp(key, XNStatusAttributes)) {
            if (!fillStatusAttributes(inputConnection,
                                      va_arg(argumentList, XVaNestedList)))
                break;
        } else if (!strcmp(key, XNFilterEvents)) {
            unsigned long *filterEvents = va_arg(argumentList, unsigned long *);
            *filterEvents = GET_XIC_STRUCT(inputConnection)->eventFilter;
        } else if (!strcmp(key, XNGeometryCallback) ||
                   !strcmp(key, XNDestroyCallback) ||
                   !strcmp(key, XNResourceName) ||
                   !strcmp(key, XNResourceClass)) {
            (void) va_arg(argumentList, void *);
        } else {
            break;
        }
    }
    va_end(argumentList);
    return key;
}

void XSetICFocus(XIC inputConnection)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XSetICFocus.3. Nothing to
    // do beyond enabling SDL's host IME while this IC owns focus.
    if (!inputConnection)
        return;
    /* Switching focus directly from another IC (no XUnsetICFocus in between)
     * must tear down the old IC's on-screen preedit; clear it while it is still
     * the focused connection. That clear can fire the old IC's preedit
     * callbacks, one of which could XDestroyIC the IC being focused; publish it
     * as the pending target so XDestroyIC nulls it and we bail instead of
     * dereferencing freed memory.
     */
    if (focusedInputConnection && focusedInputConnection != inputConnection) {
        pendingFocusTarget = inputConnection;
        inputMethodHandlePreedit("", 0);
        Bool targetAlive = pendingFocusTarget == inputConnection;
        pendingFocusTarget = NULL;
        if (!targetAlive)
            return;
    }
    focusedInputConnection = inputConnection;
    applyTextInputRectForIC(GET_XIC_STRUCT(inputConnection));
    SDL_StartTextInput();
    focusedTextInputActive = True;
}

char *XGetIMValues(XIM inputMethod, ...)
{
    va_list argumentList;
    va_start(argumentList, inputMethod);
    char *key;
    while ((key = va_arg(argumentList, char *))) {
        if (!strcmp(key, XNQueryInputStyle)) {
            XIMStyles **styles = va_arg(argumentList, XIMStyles **);
            if (!styles)
                break;
            XIMStyles *copy = malloc(sizeof(*copy) + sizeof(SUPPORTED_STYLES));
            if (!copy)
                break;
            copy->count_styles = supportedStyles.count_styles;
            copy->supported_styles = (XIMStyle *) (copy + 1);
            memcpy(copy->supported_styles, SUPPORTED_STYLES,
                   sizeof(SUPPORTED_STYLES));
            *styles = copy;
        } else {
            break;
        }
    }
    va_end(argumentList);
    return key;
}

char *XSetIMValues(XIM inputMethod, ...)
{
    // https://www.x.org/archive/X11R7.6/doc/man/man3/XSetIMValues.3.xhtml
    WARN_UNIMPLEMENTED;
    return NULL;
}

void XFreeFontSet(Display *display, XFontSet font_set)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XCreateFontSet.3.xhtml
    if (!font_set)
        return;
    CompatFontSet *set = GET_FONT_SET(font_set);
    if (set->font)
        XFreeFont(display ? display : set->display, set->font);
    free(set->baseName);
    free(set->locale);
    free(set);
}

void XFreeStringList(char **list)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XFreeStringList.3.xhtml
    if (!list)
        return;
    for (char **item = list; *item; item++)
        free(*item);
    free(list);
}

static char *firstFontSetPattern(_Xconst char *base_font_name_list)
{
    if (!base_font_name_list || !*base_font_name_list)
        return strdup("fixed");
    const char *start = base_font_name_list;
    while (*start == ' ' || *start == '\t')
        start++;
    const char *end = start;
    while (*end && *end != ',')
        end++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    if (end == start)
        return strdup("fixed");
    size_t len = (size_t) (end - start);
    char *pattern = malloc(len + 1);
    if (!pattern)
        return NULL;
    memcpy(pattern, start, len);
    pattern[len] = '\0';
    return pattern;
}

static char *nativeLikeFontSetAlias(const char *pattern)
{
    if (!pattern)
        return NULL;
    if (strstr(pattern, "*medium*") || strstr(pattern, "*medium-r*")) {
        if (strstr(pattern, "--14"))
            return strdup("7x14");
        if (strstr(pattern, "--18"))
            return strdup("9x18");
        if (strstr(pattern, "--24"))
            return strdup("12x24");
    }
    if (pattern[0] == '*' && strstr(pattern, "-times-"))
        return strdup("fixed");
    return NULL;
}

XFontSet XCreateFontSet(Display *display,
                        _Xconst char *base_font_name_list,
                        char ***missing_charset_list_return,
                        int *missing_charset_count_return,
                        char **def_string_return)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XCreateFontSet.3.xhtml
    if (missing_charset_list_return)
        *missing_charset_list_return = NULL;
    if (missing_charset_count_return)
        *missing_charset_count_return = 0;
    if (def_string_return)
        *def_string_return = "";

    LOG("XCreateFontSet request: '%s'\n",
        base_font_name_list ? base_font_name_list : "(null)");
    char *pattern = firstFontSetPattern(base_font_name_list);
    if (!pattern)
        return NULL;
    /* Try the alias first when one exists; if it fails (or there is no alias)
     * try the original caller-supplied pattern before falling back to "fixed".
     * Otherwise an alias miss silently downgrades a loadable user pattern to
     * the default font.
     */
    char *loadName = nativeLikeFontSetAlias(pattern);
    XFontStruct *font = NULL;
    if (loadName)
        font = XLoadQueryFont(display, loadName);
    if (!font)
        font = XLoadQueryFont(display, pattern);
    free(loadName);
    if (!font && strcmp(pattern, "fixed") != 0)
        font = XLoadQueryFont(display, "fixed");
    if (!font) {
        free(pattern);
        return NULL;
    }

    CompatFontSet *set = calloc(1, sizeof(*set));
    if (!set) {
        XFreeFont(display, font);
        free(pattern);
        return NULL;
    }
    set->display = display;
    set->font = font;
    set->fontStructList[0] = font;
    set->fontNameList[0] = pattern;
    set->baseName = pattern;
    const char *locale = setlocale(LC_CTYPE, NULL);
    set->locale = strdup(locale ? locale : "C");
    set->extents.max_ink_extent.x = 0;
    set->extents.max_ink_extent.y = (short) -font->ascent;
    set->extents.max_ink_extent.width = font->max_bounds.width;
    set->extents.max_ink_extent.height =
        (unsigned short) (font->ascent + font->descent);
    set->extents.max_logical_extent = set->extents.max_ink_extent;
    return (XFontSet) set;
}

int Xutf8LookupString(XIC inputConnection,
                      XKeyPressedEvent *event,
                      char *buffer_return,
                      int bytes_buffer,
                      KeySym *keysym_return,
                      Status *status_return)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/Xutf8LookupString.3.xhtml
    if (!event) {
        if (status_return)
            *status_return = XLookupNone;
        return 0;
    }
    if (event->keycode == 0) {
        /* convertEvent stamped the commit id into subwindow; look up that exact
         * commit so out-of-order delivery cannot fetch the wrong one.
         */
        unsigned long commitId = (unsigned long) event->subwindow;
        char *pendingText = inputMethodPendingText(commitId);
        if (!pendingText) {
            if (status_return)
                *status_return = XLookupNone;
            return 0;
        }
        LOG("InputMethod Event! text = '%s'.\n", pendingText);
        /* Xutf8LookupString returns a byte count and an unterminated string, so
         * the NUL is excluded from the length, the buffer check, and the copy.
         * size_t throughout: narrowing strlen to int first would let a >INT_MAX
         * commit wrap negative, slip past the bounds check, and turn the copy
         * into a huge memcpy.
         */
        size_t textLen = strlen(pendingText);
        if (!buffer_return || bytes_buffer < 0 ||
            textLen > (size_t) bytes_buffer) {
            if (status_return)
                *status_return = XBufferOverflow;
            return textLen > (size_t) INT_MAX ? INT_MAX : (int) textLen;
        }
        /* A single ASCII byte maps to a keysym (XLookupBoth); a multi-byte or
         * multi-character commit is text only, so report XLookupChars with no
         * keysym rather than deriving one from a UTF-8 continuation byte.
         */
        if (textLen == 1) {
            if (status_return)
                *status_return = XLookupBoth;
            if (keysym_return)
                *keysym_return = getKeySymForChar(pendingText[0]);
        } else {
            if (status_return)
                *status_return = XLookupChars;
            if (keysym_return)
                *keysym_return = NoSymbol;
        }
        memcpy(buffer_return, pendingText, textLen);
        inputMethodConsumePendingText(commitId);
        return (int) textLen;
    }

    LOG("Normal Event, Keycode = %d, '%c'\n", event->keycode, event->keycode);
    unsigned int consumedModifiers = 0;
    KeySym keysym = NoSymbol;
    XkbLookupKeySym(event->display, event->keycode, event->state,
                    &consumedModifiers, &keysym);
    int needed = appendKeysymUtf8(keysym, buffer_return, bytes_buffer);
    if (keysym_return)
        *keysym_return = keysym;
    if (needed > 0 && (!buffer_return || needed > bytes_buffer)) {
        /* Encodable keysym but there is nowhere to put it (no buffer or it is
         * too small): report the required size instead of claiming a write that
         * did not happen.
         */
        if (status_return)
            *status_return = XBufferOverflow;
        return needed;
    }
    if (status_return) {
        if (keysym == NoSymbol && needed == 0)
            *status_return = XLookupNone;
        else if (needed == 0)
            *status_return = XLookupKeySym;
        else
            *status_return = XLookupBoth;
    }
    return needed;
}

Bool XRegisterIMInstantiateCallback(Display *display,
                                    struct _XrmHashBucketRec *rdb,
                                    char *resName,
                                    char *resClass,
                                    XIDProc callback,
                                    XPointer callbackData)
{
    // https://linux.die.net/man/3/xregisteriminstantiatecallback
    WARN_UNIMPLEMENTED;
    return False;
}
Bool XUnregisterIMInstantiateCallback(Display *display,
                                      struct _XrmHashBucketRec *rdb,
                                      char *resName,
                                      char *resClass,
                                      XIDProc callback,
                                      XPointer callbackData)
{
    // https://linux.die.net/man/3/xunregisteriminstantiatecallback
    WARN_UNIMPLEMENTED;
    return False;
}
