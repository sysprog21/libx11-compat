#include "input.h"
#include "X11/Xlibint.h"
#include "X11/Xutil.h"
#include "X11/keysym.h"
#include "X11/HPkeysym.h"
#include "X11/XKBlib.h"
#include "keysymlist.h"
#include "errors.h"
#include "display.h"
#include "events.h"
#include "input-method.h"
#include "timeline.h"
#include "window-internal.h"
#include <stdlib.h>

Window keyboardFocus = None;
int revertTo = RevertToParent;

/* Distinguishes the two non-window focus targets (None vs PointerRoot) for
 * postFocusChange's root-detail code. keyboardFocus itself stays collapsed to
 * None for both, so event routing stays binary.
 */
static FocusKind keyboardFocusKind = FocusKindNone;
static Bool keyboardFocusFromHost = False;
static Bool keyboardFocusFromClient = False;

static const struct {
    KeySym keysym;
    const char *name;
} osfKeysyms[] = {
    {osfXK_Copy, "osfCopy"},
    {osfXK_Cut, "osfCut"},
    {osfXK_Paste, "osfPaste"},
    {osfXK_BackTab, "osfBackTab"},
    {osfXK_BackSpace, "osfBackSpace"},
    {osfXK_Clear, "osfClear"},
    {osfXK_Escape, "osfEscape"},
    {osfXK_AddMode, "osfAddMode"},
    {osfXK_PrimaryPaste, "osfPrimaryPaste"},
    {osfXK_QuickPaste, "osfQuickPaste"},
    {osfXK_PageLeft, "osfPageLeft"},
    {osfXK_PageUp, "osfPageUp"},
    {osfXK_PageDown, "osfPageDown"},
    {osfXK_PageRight, "osfPageRight"},
    {osfXK_Activate, "osfActivate"},
    {osfXK_MenuBar, "osfMenuBar"},
    {osfXK_Left, "osfLeft"},
    {osfXK_Up, "osfUp"},
    {osfXK_Right, "osfRight"},
    {osfXK_Down, "osfDown"},
    {osfXK_EndLine, "osfEndLine"},
    {osfXK_BeginLine, "osfBeginLine"},
    {osfXK_EndData, "osfEndData"},
    {osfXK_BeginData, "osfBeginData"},
    {osfXK_PrevMenu, "osfPrevMenu"},
    {osfXK_NextMenu, "osfNextMenu"},
    {osfXK_PrevField, "osfPrevField"},
    {osfXK_NextField, "osfNextField"},
    {osfXK_Select, "osfSelect"},
    {osfXK_Insert, "osfInsert"},
    {osfXK_Undo, "osfUndo"},
    {osfXK_Menu, "osfMenu"},
    {osfXK_Cancel, "osfCancel"},
    {osfXK_Help, "osfHelp"},
    {osfXK_SelectAll, "osfSelectAll"},
    {osfXK_DeselectAll, "osfDeselectAll"},
    {osfXK_Reselect, "osfReselect"},
    {osfXK_Extend, "osfExtend"},
    {osfXK_Restore, "osfRestore"},
    {osfXK_Delete, "osfDelete"},
};

/* The Pointer_ButtonN keysyms (0xfee9..0xfeed) name mouse buttons for keyboard
 * pointer control. The generated KEY_SYM_LIST omits them, but toolkits that
 * bind mouse buttons through XStringToKeysym need them: Magic's macro command
 * turns "Button1" into "Pointer_Button1" and binds the result, so without these
 * names the binding silently fails and a click resolves to an unbound 0xfee9.
 */
static const struct {
    KeySym keysym;
    const char *name;
} pointerKeysyms[] = {
    {XK_Pointer_Button1, "Pointer_Button1"},
    {XK_Pointer_Button2, "Pointer_Button2"},
    {XK_Pointer_Button3, "Pointer_Button3"},
    {XK_Pointer_Button4, "Pointer_Button4"},
    {XK_Pointer_Button5, "Pointer_Button5"},
};

static KeySym physicalKeysymForOsfKeysym(KeySym keysym)
{
    switch (keysym) {
    case osfXK_BackTab:
        return XK_Tab;
    case osfXK_BackSpace:
        return XK_BackSpace;
    case osfXK_Clear:
        return XK_Clear;
    case osfXK_Escape:
    case osfXK_Cancel:
        return XK_Escape;
    case osfXK_PageLeft:
    case osfXK_PageUp:
        return XK_Prior;
    case osfXK_PageDown:
    case osfXK_PageRight:
        return XK_Next;
    case osfXK_Activate:
        return XK_Return;
    case osfXK_MenuBar:
        return XK_F10;
    case osfXK_Left:
    case osfXK_PrevMenu:
        return XK_Left;
    case osfXK_Up:
        return XK_Up;
    case osfXK_Right:
    case osfXK_NextMenu:
        return XK_Right;
    case osfXK_Down:
        return XK_Down;
    case osfXK_EndLine:
    case osfXK_EndData:
        return XK_End;
    case osfXK_BeginLine:
    case osfXK_BeginData:
        return XK_Home;
    case osfXK_PrevField:
        /* XK_ISO_Left_Tab has no SDL key, so mapping there leaves osfPrevField
         * unbound (keycode 0). Bind it to the physical Tab key like osfBackTab;
         * the backward direction comes from the Shift modifier on the event.
         */
        return XK_Tab;
    case osfXK_NextField:
        return XK_Tab;
    case osfXK_Select:
        return XK_Select;
    case osfXK_Insert:
        return XK_Insert;
    case osfXK_Menu:
        return XK_Menu;
    case osfXK_Help:
        return XK_Help;
    case osfXK_Delete:
        return XK_Delete;
    default:
        return NoSymbol;
    }
}

Window getKeyboardFocus()
{
    LOG("GET keyboard focus is %lu\n", keyboardFocus);
    return keyboardFocus;
}

FocusKind getKeyboardFocusKind(void)
{
    return keyboardFocusKind;
}

Bool isKeyboardFocusFromHost(void)
{
    return keyboardFocusFromHost;
}

Bool isKeyboardFocusFromClient(void)
{
    return keyboardFocusFromClient;
}

void setKeyboardFocus(Window window)
{
    LOG("SET keyboard focus is %lu\n", window);
    keyboardFocus = window;
}

void syncKeyboardFocusFromHost(Window window)
{
    if (window != None && !IS_TYPE(window, WINDOW))
        window = None;
    setKeyboardFocus(window);
    keyboardFocusKind = window == None ? FocusKindNone : FocusKindWindow;
    keyboardFocusFromHost = True;
    keyboardFocusFromClient = False;

    /* Host-driven focus carries no client revert_to; reset it so a later
     * destroy of this window does not apply a stale policy left over from an
     * earlier XSetInputFocus.
     */
    revertTo = RevertToNone;
}

void revertKeyboardFocusForDestroyedWindow(Display *display, Window window)
{
    /* Xlib spec: when the focus window is destroyed, focus reverts per
     * revert_to and the new revert_to becomes RevertToNone (so a cascading
     * destroy of the parent does not keep walking up). Route through
     * XSetInputFocus so postFocusChange emits the proper FocusOut/FocusIn
     * sequence and keyboardFocusKind stays in sync.
     */
    if (window == None || window != keyboardFocus)
        return;
    switch (revertTo) {
    case RevertToPointerRoot:
        XSetInputFocus(display, (Window) PointerRoot, RevertToNone,
                       CurrentTime);
        return;
    case RevertToParent: {
        /* Xlib spec: "the focus reverts to its parent (or the closest viewable
         * ancestor)"; fall back to None if none exists.
         */
        Window target = GET_PARENT(window);
        while (target != None && IS_TYPE(target, WINDOW) &&
               !isWindowEffectivelyViewable(target))
            target = GET_PARENT(target);
        Bool viewable = target != None && IS_TYPE(target, WINDOW);
        XSetInputFocus(display, viewable ? target : None, RevertToNone,
                       CurrentTime);
        return;
    }
    case RevertToNone:
    default:
        XSetInputFocus(display, None, RevertToNone, CurrentTime);
        return;
    }
}

int XSelectInput(Display *display, Window window, long event_mask)
{
    // https://tronche.com/gui/x/xlib/event-handling/XSelectInput.html
    TYPE_CHECK(window, WINDOW, display, 0);
    GET_WINDOW_STRUCT(window)->eventMask = event_mask;
    LOG("%s: window=%lu mask=0x%lx keyPress=0x%lx keyRelease=0x%lx "
        "exposure=0x%lx structure=0x%lx property=0x%lx\n",
        __func__, window, event_mask, event_mask & KeyPressMask,
        event_mask & KeyReleaseMask, event_mask & ExposureMask,
        event_mask & StructureNotifyMask, event_mask & PropertyChangeMask);
    if ((event_mask & KeyPressMask || event_mask & KeyReleaseMask) &&
        !inputMethodHasFocusedIC()) {
        /* Suppress SDL_TEXTINPUT so each key produces a single XKey event
         * instead of doubling up with a translated text event.
         */
        inputMethodNoteTextInputStopped();
        SDL_StopTextInput();
    }

    /* Previously this also called setKeyboardFocus(window) whenever
     * KeyPress/Release was selected. That made every widget that called
     * XSelectInput steal focus from whatever Motif had explicitly focused via
     * XSetInputFocus, and the focus dance broke Motif dialogs (e.g., piano's
     * Help popup never appeared because the focus restored to the parent before
     * the dialog finished mapping). XSetInputFocus is now the single source of
     * truth.
     */
    return 1;
}

KeySym *XGetKeyboardMapping(Display *display,
                            KeyCode first_keycode,
                            int count,
                            int *keysyms_per_keycode)
{
    if (count < 0) {
        handleError(0, display, None, 0, BadValue, 0);
        return NULL;
    }
    if (first_keycode < display->min_keycode) {
        LOG("The value specified in first_keycode must be greater than or "
            "equal to min_keycode %s: %d\n",
            __func__, first_keycode);
        handleError(0, display, None, 0, BadValue, 0);
        return NULL;
    }
    if (first_keycode + count - 1 > display->max_keycode) {
        LOG("The (first_keycode + keycode_count - 1) expression must be less "
            "than or equal to max_keycode %s: %d\n",
            __func__, first_keycode);
        handleError(0, display, None, 0, BadValue, 0);
        return NULL;
    }

    KeySym *mapping = calloc((size_t) count, sizeof(KeySym));
    if (!mapping) {
        LOG("Cannot alloc mapping %s\n", __func__);
        return NULL;
    }

    int firstAfterRange = first_keycode + count;
    for (int kc = SDLK_0; kc <= (int) SDLK_9; kc++) {
        if (first_keycode <= kc && kc < firstAfterRange)
            mapping[kc - first_keycode] = XkbKeycodeToKeysym(display, kc, 0, 0);
    }
    for (int kc = SDLK_a; kc <= (int) SDLK_z; kc++) {
        if (first_keycode <= kc && kc < firstAfterRange)
            mapping[kc - first_keycode] = XkbKeycodeToKeysym(display, kc, 0, 0);
    }

    /* Iterate in reverse so earlier table entries win for keycodes that map to
     * multiple keysyms.
     */
    for (int i = SDL_KEYCODE_TO_KEYSYM_LENGTH - 1; i >= 0; i--) {
        KeyCode kc = keycodeForSdlKeycode(SDLKeycodeToKeySym[i].keycode);
        if (first_keycode <= kc && kc < firstAfterRange &&
            mapping[kc - first_keycode] == NoSymbol) {
            mapping[kc - first_keycode] = SDLKeycodeToKeySym[i].keysym;
        }
    }

    if (keysyms_per_keycode)
        *keysyms_per_keycode = 1;
    return mapping;
}

#define SDL_SCANCODE_KEYCODE_BASE 128

KeyCode keycodeForSdlKeycode(int keycode)
{
    if (keycode >= 0 && keycode < SDL_SCANCODE_KEYCODE_BASE)
        return (KeyCode) keycode;

    int scancodeIndex = 0;
    for (size_t i = 0; i < SDL_KEYCODE_TO_KEYSYM_LENGTH; i++) {
        int candidate = SDLKeycodeToKeySym[i].keycode;
        if ((unsigned int) candidate >= SDLK_SCANCODE_MASK) {
            if (candidate == keycode) {
                int mapped = SDL_SCANCODE_KEYCODE_BASE + scancodeIndex;

                /* KeyCode is an 8-bit CARD8. The scancode range starts at 128,
                 * so the table may hold at most 255 - SDL_SCANCODE_KEYCODE_BASE
                 * masked entries before a mapped keycode would wrap past 255
                 * and collide with the ASCII keycodes below 128. Today the
                 * table has far fewer, but fall back to the low byte and log
                 * rather than silently misroute keys if it ever grows past that
                 * bound.
                 */
                if (mapped > 0xFF) {
                    LOG("%s: scancode index %d exceeds KeyCode range\n",
                        __func__, scancodeIndex);
                    return 0;
                }
                return (KeyCode) mapped;
            }
            scancodeIndex++;
        }
    }

    /* Not a tabulated scancode key. Its low byte is the only candidate keycode,
     * but the 128+ range is reserved for tabulated scancode indices: a low byte
     * landing there (an untabulated scancode such as Volume Up, or a Latin-1
     * key) would be read back by XkbKeycodeToKeysym as a table index and report
     * an unrelated key (Volume Up as XK_Delete).
     *
     * Return 0 (unmapped) for those rather than collide; an ASCII-range low
     * byte is a real keycode.
     */
    int lowByte = keycode & 0xFF;
    return lowByte < SDL_SCANCODE_KEYCODE_BASE ? (KeyCode) lowByte : 0;
}

int sdlKeycodeForKeycode(KeyCode keycode)
{
    if (keycode < SDL_SCANCODE_KEYCODE_BASE)
        return keycode;

    int scancodeIndex = keycode - SDL_SCANCODE_KEYCODE_BASE;
    for (size_t i = 0; i < SDL_KEYCODE_TO_KEYSYM_LENGTH; i++) {
        if ((unsigned int) SDLKeycodeToKeySym[i].keycode >=
            SDLK_SCANCODE_MASK) {
            if (scancodeIndex == 0)
                return SDLKeycodeToKeySym[i].keycode;
            scancodeIndex--;
        }
    }
    return keycode;
}

/* Single source of truth for the US-layout Shift pairs on the number /
 * punctuation rows. The forward (shiftedKeysym) and reverse
 * (unshiftedPunctuation) lookups both scan this table, so the two directions
 * cannot drift. Letters (a-z <-> A-Z) are handled by range arithmetic at the
 * call sites, not here.
 */
static const struct {
    KeySym base;
    KeySym shifted;
} usShiftPairs[] = {
    {XK_1, XK_exclam},
    {XK_2, XK_at},
    {XK_3, XK_numbersign},
    {XK_4, XK_dollar},
    {XK_5, XK_percent},
    {XK_6, XK_asciicircum},
    {XK_7, XK_ampersand},
    {XK_8, XK_asterisk},
    {XK_9, XK_parenleft},
    {XK_0, XK_parenright},
    {XK_grave, XK_asciitilde},
    {XK_minus, XK_underscore},
    {XK_equal, XK_plus},
    {XK_bracketleft, XK_braceleft},
    {XK_bracketright, XK_braceright},
    {XK_backslash, XK_bar},
    {XK_semicolon, XK_colon},
    {XK_apostrophe, XK_quotedbl},
    {XK_comma, XK_less},
    {XK_period, XK_greater},
    {XK_slash, XK_question},
};

/* The unshifted base keysym a shifted symbol is produced from, or NoSymbol.
 * Keeps the reverse lookups (keycode + modifiers) in agreement with
 * shiftedKeysym(); without it, accelerators bound on a shifted-symbol keysym
 * would register the wrong key.
 */
static KeySym unshiftedPunctuation(KeySym keysym)
{
    for (size_t i = 0; i < sizeof(usShiftPairs) / sizeof(usShiftPairs[0]); i++)
        if (usShiftPairs[i].shifted == keysym)
            return usShiftPairs[i].base;
    return NoSymbol;
}

KeyCode XKeysymToKeycode(Display *display, KeySym keysym)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XKeysymToKeycode.html
    KeySym physical = physicalKeysymForOsfKeysym(keysym);
    if (physical != NoSymbol)
        return XKeysymToKeycode(display, physical);
    if (keysym >= XK_0 && keysym <= XK_9)
        return SDLK_0 + (keysym - XK_0);
    if (keysym >= XK_a && keysym <= XK_z)
        return SDLK_a + (keysym - XK_a);
    if (keysym >= XK_A && keysym <= XK_Z)
        return SDLK_a + (keysym - XK_A);
    KeySym base = unshiftedPunctuation(keysym);
    if (base != NoSymbol)
        return XKeysymToKeycode(display, base);

    /* Reverse scan for keysyms carried by several SDL keys (SDLK_RETURN and
     * SDLK_RETURN2 both map to XK_Return; likewise Delete/Clear/Cancel/KP
     * separators). Prefer the ASCII/physical key (SDL keycode < 0x80) in a
     * first pass, then fall back to the scancode keys, mirroring the
     * ASCII-first preference XkbKeycodeToKeysym uses in the forward direction.
     * This matters because real events for these keys arrive as the low-byte
     * keycode (Return is keycode 13 via keycodeForSdlKeycode(SDLK_RETURN)); if
     * XKeysymToKeycode returned the secondary scancode keycode instead, a Motif
     * grab or translation keyed by XKeysymToKeycode(XK_Return) would never
     * match the physical key.
     *
     * Both passes gate on the round-trip: X keycodes are the low byte of the
     * SDL keycode, so a 0x4000xxxx scancode key can alias onto an ASCII key
     * (SDLK_EXECUTE 0x40000074 truncates to 116, exactly SDLK_t). Only accept a
     * keycode that maps back to this keysym; otherwise it belongs to the ASCII
     * key and returning it would misbind the special key onto it.
     */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = SDL_KEYCODE_TO_KEYSYM_LENGTH - 1; i >= 0; i--) {
            if (SDLKeycodeToKeySym[i].keysym != keysym)
                continue;
            Bool ascii = SDLKeycodeToKeySym[i].keycode < 0x80;
            if (pass == 0 && !ascii)
                continue;
            KeyCode kc = keycodeForSdlKeycode(SDLKeycodeToKeySym[i].keycode);
            if (XkbKeycodeToKeysym(display, kc, 0, 0) == keysym)
                return kc;
        }
    }
    LOG("%s: Got unimplemented keysym %lu\n", __func__, keysym);
    return 0;
}

KeySym XLookupKeysym(XKeyEvent *key_event, int index)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XLookupKeysym.html
    return XkbKeycodeToKeysym(key_event->display, key_event->keycode, 0, index);
}

KeySym XStringToKeysym(_Xconst char *string)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XStringToKeysym.html
    if (!string)
        return NoSymbol;
    if (strlen(string) == 1) {
        char chr = string[0];
        if ((chr >= '0' && chr <= '9') || (chr >= 'a' && chr <= 'z') ||
            (chr >= 'A' && chr <= 'Z')) {
            return (KeySym) ((long) chr);
        }
    }
    for (size_t i = 0; i < sizeof(osfKeysyms) / sizeof(osfKeysyms[0]); i++) {
        if (strcmp(osfKeysyms[i].name, string) == 0)
            return osfKeysyms[i].keysym;
    }
    for (size_t i = 0; i < sizeof(pointerKeysyms) / sizeof(pointerKeysyms[0]);
         i++) {
        if (strcmp(pointerKeysyms[i].name, string) == 0)
            return pointerKeysyms[i].keysym;
    }
    if (!strcmp(string, "KDelete"))
        return XK_Delete;
    for (size_t i = 0; i < KEY_SYM_LIST_LENGTH; i++) {
        if (strcmp(KEY_SYM_LIST[i].name, string) == 0)
            return KEY_SYM_LIST[i].keySym;
    }
    return NoSymbol;
}

char *XKeysymToString(KeySym keysym)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XKeysymToString.html
    for (size_t i = 0; i < sizeof(osfKeysyms) / sizeof(osfKeysyms[0]); i++) {
        if (osfKeysyms[i].keysym == keysym)
            return (char *) osfKeysyms[i].name;
    }
    for (size_t i = 0; i < sizeof(pointerKeysyms) / sizeof(pointerKeysyms[0]);
         i++) {
        if (pointerKeysyms[i].keysym == keysym)
            return (char *) pointerKeysyms[i].name;
    }
    for (size_t i = 0; i < KEY_SYM_LIST_LENGTH; i++) {
        if (KEY_SYM_LIST[i].keySym == keysym)
            return (char *) KEY_SYM_LIST[i].name;
    }
    return NULL;
}

KeySym XkbKeycodeToKeysym(Display *display,
                          KeyCode keycode,
                          int group,
                          int level)
{
    (void) display;
    (void) group;
    (void) level;
    if (keycode >= SDLK_0 && keycode <= SDLK_9)
        return XK_0 + (keycode - SDLK_0);
    if (keycode >= SDLK_a && keycode <= SDLK_z)
        return XK_a + (keycode - SDLK_a);
    if (keycode >= SDL_SCANCODE_KEYCODE_BASE) {
        int scancodeIndex = keycode - SDL_SCANCODE_KEYCODE_BASE;
        for (size_t i = 0; i < SDL_KEYCODE_TO_KEYSYM_LENGTH; i++) {
            if ((unsigned int) SDLKeycodeToKeySym[i].keycode >=
                SDLK_SCANCODE_MASK) {
                if (scancodeIndex == 0)
                    return SDLKeycodeToKeySym[i].keysym;
                scancodeIndex--;
            }
        }
    }

    /* Several SDL keycodes can share a low byte: an ASCII key (< 0x80, whose
     * keycode is its own low byte) and a 0x4000xxxx scancode key that truncates
     * onto it (e.g. SDLK_RETURN 0x0d and SDLK_AC_HOME 0x4000010d both hit
     * keycode 13). Prefer the ASCII key so its canonical keysym wins; otherwise
     * a plain-reverse scan would return the scancode key's keysym (XK_Home for
     * Return) and swallow it. This mirrors the round-trip guard
     * XKeysymToKeycode uses in the reverse direction. Fall back to the reverse
     * scan for keycodes that only a scancode key provides.
     */
    for (int i = SDL_KEYCODE_TO_KEYSYM_LENGTH - 1; i >= 0; i--) {
        if (SDLKeycodeToKeySym[i].keycode < 0x80 &&
            (SDLKeycodeToKeySym[i].keycode & 0xFF) == keycode)
            return SDLKeycodeToKeySym[i].keysym;
    }
    for (int i = SDL_KEYCODE_TO_KEYSYM_LENGTH - 1; i >= 0; i--) {
        if ((SDLKeycodeToKeySym[i].keycode & 0xFF) == keycode)
            return SDLKeycodeToKeySym[i].keysym;
    }
    LOG("%s: Got unimplemented keycode %c\n", __func__, keycode);
    return NoSymbol;
}

KeySym XKeycodeToKeysym(Display *display, KeyCode keycode, int index)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XKeycodeToKeysym.html
    return XkbKeycodeToKeysym(display, keycode, 0, index);
}

unsigned int XkbKeysymToModifiers(Display *display, KeySym keysym)
{
    (void) display;
    if (keysym >= XK_A && keysym <= XK_Z)
        return ShiftMask;
    if (unshiftedPunctuation(keysym) != NoSymbol)
        return ShiftMask;
    switch (keysym) {
    case XK_Shift_L:
    case XK_Shift_R:
        return ShiftMask;
    case XK_Caps_Lock:
    case XK_Shift_Lock:
        return LockMask;
    case XK_Control_L:
    case XK_Control_R:
        return ControlMask;
    case XK_Alt_L:
    case XK_Alt_R:
        /* Match the standard X server convention: both Alt halves share
         * Mod1Mask. Keeping them separate broke modmap/state consistency for
         * clients that fold convertModifierState(KMOD_ALT) against
         * XGetModifierMapping.
         */
        return Mod1Mask;
    case XK_Num_Lock:
        return Mod2Mask;
    case XK_Scroll_Lock:
        return Mod3Mask;
    }
    return 0;
}

/* Map an unshifted keysym to its Shift counterpart on a US layout. SDL reports
 * the modifier and the base key separately, so the shifted symbol (1 -> !,
 * - -> _, etc.) has to be synthesized here; without it Shift only upper-cased
 * letters and digits/punctuation came through unshifted.
 */
static KeySym shiftedKeysym(KeySym keysym)
{
    if (keysym >= XK_a && keysym <= XK_z)
        return XK_A + (keysym - XK_a);
    for (size_t i = 0; i < sizeof(usShiftPairs) / sizeof(usShiftPairs[0]); i++)
        if (usShiftPairs[i].base == keysym)
            return usShiftPairs[i].shifted;
    return keysym;
}

Bool XkbLookupKeySym(Display *display,
                     KeyCode keycode,
                     unsigned int modifiers,
                     unsigned int *modifiers_return,
                     KeySym *keysym_return)
{
    KeySym keysym = XkbKeycodeToKeysym(display, keycode, 0, 0);
    unsigned int consumedModifiers = 0;
    if (modifiers & ShiftMask) {
        KeySym shifted = shiftedKeysym(keysym);
        if (shifted != keysym) {
            keysym = shifted;
            consumedModifiers |= ShiftMask;
        }
    }
    if (modifiers_return)
        *modifiers_return = consumedModifiers;
    if (keysym_return)
        *keysym_return = keysym;
    return keysym != NoSymbol;
}

static int controlByteForKeysym(KeySym keysym)
{
    switch (keysym) {
    case XK_BackSpace:
        return '\b';
    case XK_Tab:
        return '\t';
    case XK_Linefeed:
        return '\n';
    case XK_Return:
    case XK_KP_Enter:
        return '\r';
    case XK_Escape:
        return 0x1b;
    case XK_Delete:
        return 0x7f;
    default:
        return -1;
    }
}

int XLookupString(XKeyEvent *event_struct,
                  char *buffer_return,
                  int bytes_buffer,
                  KeySym *keysym_return,
                  XComposeStatus *status_in_out)
{
    // https://tronche.com/gui/x/xlib/utilities/XLookupString.html
    (void) status_in_out;
    if (!event_struct)
        return 0;

    /* An IM commit arrives as a synthetic KeyPress with keycode 0, carrying its
     * commit id in subwindow. Clients that read with the simple 8-bit
     * XLookupString (rather than XmbLookupString) would otherwise lose the
     * committed text once the raw keydown is suppressed, so resolve it here
     * too.
     */
    if (event_struct->keycode == 0) {
        unsigned long commitId = (unsigned long) event_struct->subwindow;
        char *pendingText = inputMethodPendingText(commitId);
        if (!pendingText) {
            if (keysym_return)
                *keysym_return = NoSymbol;
            return 0;
        }
        size_t textLen = strlen(pendingText);
        if (keysym_return)
            *keysym_return =
                textLen == 1 ? getKeySymForChar(pendingText[0]) : NoSymbol;
        if (!buffer_return || bytes_buffer <= 0)
            return 0;

        /* Only consume once the whole commit has been delivered. If the
         * caller's buffer cannot hold it, copy the prefix but leave the commit
         * pending so a retry with a larger buffer still recovers the rest
         * rather than dropping it permanently.
         */
        if (textLen > (size_t) bytes_buffer) {
            memcpy(buffer_return, pendingText, (size_t) bytes_buffer);
            return bytes_buffer;
        }
        memcpy(buffer_return, pendingText, textLen);
        inputMethodConsumePendingText(commitId);
        return (int) textLen;
    }
    unsigned int consumedModifiers = 0;
    KeySym keysym = NoSymbol;
    if (!XkbLookupKeySym(event_struct->display, event_struct->keycode,
                         event_struct->state, &consumedModifiers, &keysym)) {
        if (keysym_return)
            *keysym_return = NoSymbol;
        return 0;
    }
    if (keysym_return)
        *keysym_return = keysym;
    if (!buffer_return || bytes_buffer <= 0)
        return 0;
    int controlByte = controlByteForKeysym(keysym);
    if (controlByte >= 0) {
        buffer_return[0] = (char) controlByte;
        return 1;
    }
    if (keysym >= XK_space && keysym <= XK_asciitilde) {
        buffer_return[0] = (char) keysym;
        return 1;
    }
    if (keysym >= XK_nobreakspace && keysym <= XK_ydiaeresis) {
        buffer_return[0] = (char) (keysym & 0xff);
        return 1;
    }
    return 0;
}

XModifierKeymap *XGetModifierMapping(Display *display)
{
    // https://tronche.com/gui/x/xlib/input/XGetModifierMapping.html
    SET_X_SERVER_REQUEST(display, X_GetModifierMapping);
    enum { KEYS_PER_MODIFIER = 2, MODIFIER_COUNT = 8 };
    static const struct {
        int mapIndex;
        int slot;
        KeySym keysym;
    } bindings[] = {
        {ShiftMapIndex, 0, XK_Shift_L},     {ShiftMapIndex, 1, XK_Shift_R},
        {LockMapIndex, 0, XK_Caps_Lock},    {ControlMapIndex, 0, XK_Control_L},
        {ControlMapIndex, 1, XK_Control_R}, {Mod1MapIndex, 0, XK_Alt_L},
        {Mod1MapIndex, 1, XK_Alt_R},        {Mod2MapIndex, 0, XK_Num_Lock},
        {Mod3MapIndex, 0, XK_Scroll_Lock},
    };
    XModifierKeymap *modifierKeymap = malloc(sizeof(XModifierKeymap));
    if (!modifierKeymap) {
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    modifierKeymap->max_keypermod = KEYS_PER_MODIFIER;
    modifierKeymap->modifiermap =
        calloc(MODIFIER_COUNT * KEYS_PER_MODIFIER, sizeof(KeyCode));
    if (!modifierKeymap->modifiermap) {
        free(modifierKeymap);
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        modifierKeymap->modifiermap[bindings[i].mapIndex * KEYS_PER_MODIFIER +
                                    bindings[i].slot] =
            XKeysymToKeycode(display, bindings[i].keysym);
    }
    return modifierKeymap;
}

int XFreeModifiermap(XModifierKeymap *modmap)
{
    // https://tronche.com/gui/x/xlib/input/XFreeModifiermap.html
    free(modmap->modifiermap);
    free(modmap);
    return 1;
}

int XGetInputFocus(Display *display,
                   Window *focus_return,
                   int *revert_to_return)
{
    // https://tronche.com/gui/x/xlib/input/XGetInputFocus.html
    SET_X_SERVER_REQUEST(display, X_GetInputFocus);

    /* Xlib spec: report the actual current focus target. The previous
     * implementation collapsed None to PointerRoot in the return value, which
     * was non-conformant and indistinguishable from a real PointerRoot focus
     * once XSetInputFocus learned to track the two cases separately.
     */
    if (keyboardFocusKind == FocusKindPointerRoot)
        *focus_return = (Window) PointerRoot;
    else if (keyboardFocusKind == FocusKindNone)
        *focus_return = None;
    else
        *focus_return = getKeyboardFocus();
    *revert_to_return = revertTo;
    return 1;
}

int XSetInputFocus(Display *display, Window focus, int revert_to, Time time)
{
    // https://tronche.com/gui/x/xlib/input/XSetInputFocus.html
    SET_X_SERVER_REQUEST(display, X_SetInputFocus);
    (void) time;
    revertTo = revert_to;
    Window oldFocus = getKeyboardFocus();
    FocusKind oldKind = keyboardFocusKind;
    FocusKind newKind;
    if (focus == PointerRoot)
        newKind = FocusKindPointerRoot;
    else if (focus == None)
        newKind = FocusKindNone;
    else
        newKind = FocusKindWindow;

    /* Storage stays collapsed: PointerRoot and None both park keyboard focus at
     * None for event routing; the kind tracker preserves the distinction so
     * postFocusChange emits the correct root detail.
     */
    Window newFocus = newKind == FocusKindWindow ? focus : None;
    keyboardFocusFromHost = False;
    keyboardFocusFromClient = True;
    if (oldKind == newKind && oldFocus == newFocus)
        return 1;
    setKeyboardFocus(newFocus);
    keyboardFocusKind = newKind;
    timelineTapSetInputFocus(newFocus, revert_to);
    postFocusChange(display, oldKind, oldFocus, newKind, newFocus);
    return 1;
}

/* Active keyboard grab (XGrabKeyboard). Single slot since the X protocol allows
 * only one keyboard grab at a time per client. Motif uses this to redirect
 * keystrokes to modal dialogs (e.g., the Help popup); the Ungrab clears the
 * slot so the previous focus regains routing.
 */
static struct {
    Bool active;
    Window grab_window;
    Bool owner_events;
} keyboardGrab;

int XGrabKeyboard(Display *display,
                  Window grab_window,
                  Bool owner_events,
                  int pointer_mode,
                  int keyboard_mode,
                  Time time)
{
    // https://tronche.com/gui/x/xlib/input/XGrabKeyboard.html
    SET_X_SERVER_REQUEST(display, X_GrabKeyboard);
    (void) pointer_mode;
    (void) keyboard_mode;
    (void) time;
    keyboardGrab.active = True;
    keyboardGrab.grab_window = grab_window;
    keyboardGrab.owner_events = owner_events;
    timelineTapGrabKeyboard(grab_window, True);
    return GrabSuccess;
}

int XUngrabKeyboard(Display *display, Time time)
{
    // https://tronche.com/gui/x/xlib/input/XUngrabKeyboard.html
    SET_X_SERVER_REQUEST(display, X_UngrabKeyboard);
    (void) time;
    keyboardGrab.active = False;
    keyboardGrab.grab_window = None;
    keyboardGrab.owner_events = False;
    timelineTapGrabKeyboard(None, False);
    return 1;
}

Window getGrabbedKeyboardWindow(void)
{
    return keyboardGrab.active ? keyboardGrab.grab_window : None;
}

Bool getKeyboardGrabOwnerEvents(void)
{
    return keyboardGrab.active && keyboardGrab.owner_events;
}

/* Passive key grabs registered via XGrabKey. Motif menus install one grab per
 * accelerator (Alt-F for File, Alt-E for Edit, etc.) on the top-level shell at
 * widget realize time; without tracking the grabs here every key event landed
 * at the focus window, accelerators silently failed, and DEBUG builds spammed
 * one warning per grab call.
 *
 * Motif demos are single-display; if multi-Display threading becomes a goal the
 * list moves under a Display pointer field.
 */
typedef struct KeyGrab {
    int key;                /* X keycode, or AnyKey for all keys */
    unsigned int modifiers; /* modifier mask, or AnyModifier */
    Window grab_window;
    Bool owner_events;
    int pointer_mode;
    int keyboard_mode;
    struct KeyGrab *next;
} KeyGrab;

static KeyGrab *keyGrabs = NULL;

int XGrabKey(Display *display,
             int key,
             unsigned int modifiers,
             Window grab_window,
             Bool owner_events,
             int pointer_mode,
             int keyboard_mode)
{
    (void) display;
    KeyGrab *grab = calloc(1, sizeof(*grab));
    if (!grab)
        return BadAlloc;
    grab->key = key;
    grab->modifiers = modifiers;
    grab->grab_window = grab_window;
    grab->owner_events = owner_events;
    grab->pointer_mode = pointer_mode;
    grab->keyboard_mode = keyboard_mode;
    grab->next = keyGrabs;
    keyGrabs = grab;
    return Success;
}

int XUngrabKey(Display *display,
               int key,
               unsigned int modifiers,
               Window grab_window)
{
    (void) display;

    /* AnyKey / AnyModifier match every grab for that window per the Xlib spec,
     * so loop without short-circuit and remove every match.
     */
    KeyGrab **p = &keyGrabs;
    while (*p) {
        KeyGrab *g = *p;
        Bool keyMatch = (key == AnyKey || g->key == key);
        Bool modMatch = (modifiers == AnyModifier || g->modifiers == modifiers);
        Bool winMatch = (g->grab_window == grab_window);
        if (keyMatch && modMatch && winMatch) {
            *p = g->next;
            free(g);
        } else {
            p = &g->next;
        }
    }
    return Success;
}

Window findKeyGrabWindow(int keycode, unsigned int modifiers)
{
    for (KeyGrab *g = keyGrabs; g; g = g->next) {
        if ((g->key == AnyKey || g->key == keycode) &&
            (g->modifiers == AnyModifier || g->modifiers == modifiers)) {
            return g->grab_window;
        }
    }
    return None;
}

int XRefreshKeyboardMapping(XMappingEvent *event_map)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XRefreshKeyboardMapping.html
    WARN_UNIMPLEMENTED;
    return 1;
}

int XDisplayKeycodes(Display *display,
                     int *minKeyCodesReturn,
                     int *maxKeyCodesReturn)
{
    // https://linux.die.net/man/3/xdisplaykeycodes
    if (minKeyCodesReturn)
        *minKeyCodesReturn = display->min_keycode;
    if (maxKeyCodesReturn)
        *maxKeyCodesReturn = display->max_keycode;
    return 1;
}
