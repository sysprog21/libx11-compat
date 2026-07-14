#include "X11/Xlibint.h"
#include "X11/xcms/Xcmsint.h"
#include "X11/Xresource.h"
#include "X11/Xutil.h"
#include "X11/Xatom.h"
#include "X11/XKBlib.h"
#include "X11/extensions/XKB.h"
#include "X11/Xlocale.h"
#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include "util.h"
#include "gc.h"
#include "display.h"
#include "errors.h"
#include "events.h"
#include "font.h"
#include "input.h"
#include "input-method.h"
#include "window.h"
#include "atoms.h"

int _Xdebug;

/* Saturating 64-bit multiply-add for geometry math. Real Xlib only ever deals
 * with display-scale pixels, but XParseGeometry happily accepts UINT_MAX units
 * which, multiplied by a UINT_MAX font cell, overshoots int64_t. Saturate
 * instead of wrapping so the caller's window placement never lands at a
 * nonsensical negative coordinate.
 */
static int64_t satMulAdd(int64_t a, int64_t b, int64_t c)
{
    int64_t prod;
    if (__builtin_mul_overflow(a, b, &prod))
        prod = ((a > 0) == (b > 0)) ? INT64_MAX : INT64_MIN;
    int64_t sum;
    if (__builtin_add_overflow(prod, c, &sum))
        return prod > 0 ? INT64_MAX : INT64_MIN;
    return sum;
}

static int decodeUtf8Codepoint(const unsigned char *src,
                               size_t len,
                               wchar_t *cp);

static int clampInt64ToInt(int64_t v)
{
    if (v > INT_MAX)
        return INT_MAX;
    if (v < INT_MIN)
        return INT_MIN;
    return (int) v;
}

static void fillTextExtents(XFontStruct *fs,
                            int width,
                            int *dir,
                            int *font_ascent,
                            int *font_descent,
                            XCharStruct *overall);
static void freeQueriedFontStruct(XFontStruct *fs);
static XFontStruct *queryTextExtentsFont(Display *dpy,
                                         XID fontOrGc,
                                         Font *temporaryFontReturn);

static void freeTextList(char **list, int count)
{
    if (!list)
        return;
    for (int i = 0; i < count; i++)
        free(list[i]);
    free(list);
}

static size_t latin1Utf8Length(const unsigned char *src, size_t len)
{
    size_t outLen = 0;
    for (size_t i = 0; i < len; i++)
        outLen += src[i] < 0x80 ? 1 : 2;
    return outLen;
}

static char *copyTextPropertyString(const unsigned char *src,
                                    size_t len,
                                    Bool latin1)
{
    size_t outLen = latin1 ? latin1Utf8Length(src, len) : len;
    char *out = malloc(outLen + 1);
    if (!out)
        return NULL;
    if (!latin1) {
        memcpy(out, src, len);
        out[len] = '\0';
        return out;
    }

    char *dst = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = src[i];
        if (c < 0x80) {
            *dst++ = (char) c;
        } else {
            *dst++ = (char) (0xc0 | (c >> 6));
            *dst++ = (char) (0x80 | (c & 0x3f));
        }
    }
    *dst = '\0';
    return out;
}

static int encodeLocaleCodepoint(wchar_t cp,
                                 char *out,
                                 size_t *outLen,
                                 size_t outCap,
                                 mbstate_t *state)
{
    char bytes[MB_LEN_MAX];
    size_t n = wcrtomb(bytes, cp, state);
    if (n == (size_t) -1 || n > outCap || *outLen > outCap - n)
        return XConverterNotFound;
    memcpy(out + *outLen, bytes, n);
    *outLen += n;
    return Success;
}

static char *copyTextPropertyStringLocale(const unsigned char *src,
                                          size_t len,
                                          Bool latin1,
                                          int *status)
{
    *status = XNoMemory;
    size_t maxChar = MB_CUR_MAX ? MB_CUR_MAX : 1;
    if (len > (SIZE_MAX / maxChar) - 1)
        return NULL;
    size_t outCap = (len + 1) * maxChar;
    char *out = malloc(outCap);
    if (!out)
        return NULL;

    mbstate_t state;
    memset(&state, 0, sizeof(state));
    size_t outLen = 0;
    size_t i = 0;
    while (i < len) {
        wchar_t cp;
        int used;
        if (latin1) {
            cp = src[i];
            used = 1;
        } else {
            used = decodeUtf8Codepoint(src + i, len - i, &cp);
        }
        if (used <= 0 || encodeLocaleCodepoint(cp, out, &outLen, outCap,
                                               &state) != Success) {
            free(out);
            *status = XConverterNotFound;
            return NULL;
        }
        i += (size_t) used;
    }
    if (encodeLocaleCodepoint(L'\0', out, &outLen, outCap, &state) != Success) {
        free(out);
        *status = XConverterNotFound;
        return NULL;
    }
    *status = Success;
    return out;
}

/* Compatibility stub triage:
 * - Build/smoke coverage: return stable Xlib-compatible defaults where callers
 *   commonly probe capability before choosing another path.
 * - Window-manager and resource helpers: preserve properties when enough local
 *   state exists; otherwise keep harmless no-ops.
 * - Xcms, fontset, and full input-method APIs: unsupported for now and reported
 *   through WARN_UNIMPLEMENTED without spamming raw printf/LOG probes.
 * - Drawing primitives here are placeholders until they move to real SDL2
 * paths.
 */

#ifdef XDestroyImage
#undef XDestroyImage
#endif

/* XGetSelectionOwner lives in src/selection.c. */

static Bool useFontSetFontForGC(GC gc, XFontSet font_set, Font *oldFontReturn)
{
    if (oldFontReturn)
        *oldFontReturn = None;
    CompatFontSet *set = GET_FONT_SET(font_set);
    if (!gc || !set || !set->font || set->font->fid == None)
        return False;
    GraphicContext *gContext = GET_GC(gc);
    Font newFont = set->font->fid;
    Font oldFont = gContext->font;
    if (oldFont == newFont)
        return False;
    if (!compatFontRetainForGC(newFont))
        return False;
    compatFontReleaseForGC(oldFont);
    gContext->font = newFont;
    GC_BUMP_GENERATION(gContext);
    if (oldFontReturn)
        *oldFontReturn = oldFont;
    return True;
}

static void restoreGCFontAfterFontSet(GC gc, Font oldFont)
{
    if (!gc)
        return;
    GraphicContext *gContext = GET_GC(gc);
    Font current = gContext->font;
    if (current == oldFont)
        return;
    if (oldFont != None && !compatFontRetainForGC(oldFont))
        return;
    compatFontReleaseForGC(current);
    gContext->font = oldFont;
    GC_BUMP_GENERATION(gContext);
}

void XSetTextProperty(Display *dpy, Window w, XTextProperty *tp, Atom property)
{
    XChangeProperty(dpy, w, property, tp->encoding, tp->format, PropModeReplace,
                    tp->value, tp->nitems);
}

void XSetWMName(Display *dpy, Window w, XTextProperty *tp)
{
    XSetTextProperty(dpy, w, tp, XA_WM_NAME);
}

void XSetWMIconName(Display *dpy, Window w, XTextProperty *tp)
{
    XSetTextProperty(dpy, w, tp, XA_WM_ICON_NAME);
}

int XClearWindow(Display *dpy, Window w)
{
    return XClearArea(dpy, w, 0, 0, 0, 0, False);
}

long XMaxRequestSize(Display *dpy)
{
    (void) dpy;
    return 262140;
}

long XExtendedMaxRequestSize(Display *dpy)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XAllocColorCells(register Display *dpy,
                        Colormap cmap,
                        Bool contig,
                        unsigned long *masks /* LISTofCARD32 */ /* RETURN */,
                        unsigned int nplanes /* CARD16 */,
                        unsigned long *pixels /* LISTofCARD32 */ /* RETURN */,
                        unsigned int ncolors /* CARD16 */)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XQueryColor(register Display *dpy, Colormap cmap, XColor *def) /* RETURN */
{
    /* Same pixel-back-to-RGB decoding as XQueryColors. With TrueColor the pixel
     * encodes the RGB directly, so no per-colormap cache is needed.
     */
    if (!def)
        return 0;
    return XQueryColors(dpy, cmap, def, 1);
}

void XLockDisplay(register Display *dpy)
{
    LockDisplay(dpy);
}

void XUnlockDisplay(register Display *dpy)
{
    UnlockDisplay(dpy);
}

int XSetArcMode(register Display *dpy, register GC gc, int arc_mode)
{
    XGCValues values;
    values.arc_mode = arc_mode;
    return XChangeGC(dpy, gc, GCArcMode, &values);
}

int XSetFillStyle(register Display *dpy, register GC gc, int fill_style)
{
    XGCValues values;
    values.fill_style = fill_style;
    return XChangeGC(dpy, gc, GCFillStyle, &values);
}

int XSetScreenSaver(register Display *dpy,
                    int timeout,
                    int interval,
                    int prefer_blank,
                    int allow_exp)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

void XUnsetICFocus(XIC ic)
{
    /* A NULL IC is a no-op rather than clearing whichever IC happens to hold
     * focus.
     */
    if (!ic)
        return;
    inputMethodUnsetFocus(ic);
}

/* Translate a single keysym to UTF-8. Covers ASCII, Latin-1, and the X11
 * 0x01000000 unicode keysym prefix.
 *
 * Returns the byte count written (0 for keysyms with no associated character).
 */
static int keysym_to_utf8(KeySym keysym, char *buffer, int nbytes)
{
    unsigned long codepoint = 0;
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
        break;
    }
    if (codepoint == 0 && ((keysym >= 0x0020 && keysym <= 0x007e) ||
                           (keysym >= 0x00a0 && keysym <= 0x00ff))) {
        codepoint = keysym;
    } else if (codepoint == 0 && (keysym & 0xff000000UL) == 0x01000000UL) {
        codepoint = keysym & 0x00ffffffUL;
    } else if (codepoint == 0) {
        return 0;
    }
    if (codepoint < 0x80) {
        if (nbytes < 1)
            return 0;
        buffer[0] = (char) codepoint;
        return 1;
    }
    if (codepoint < 0x800) {
        if (nbytes < 2)
            return 0;
        buffer[0] = (char) (0xc0 | (codepoint >> 6));
        buffer[1] = (char) (0x80 | (codepoint & 0x3f));
        return 2;
    }
    if (codepoint < 0x10000) {
        if (nbytes < 3)
            return 0;
        buffer[0] = (char) (0xe0 | (codepoint >> 12));
        buffer[1] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        buffer[2] = (char) (0x80 | (codepoint & 0x3f));
        return 3;
    }
    if (codepoint <= 0x10ffff) {
        if (nbytes < 4)
            return 0;
        buffer[0] = (char) (0xf0 | (codepoint >> 18));
        buffer[1] = (char) (0x80 | ((codepoint >> 12) & 0x3f));
        buffer[2] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        buffer[3] = (char) (0x80 | (codepoint & 0x3f));
        return 4;
    }
    return 0;
}

static int do_locale_lookup(XKeyEvent *ev,
                            char *buffer,
                            int nbytes,
                            KeySym *keysym_return,
                            Status *status_return)
{
    KeySym keysym = NoSymbol;
    int produced = 0;
    if (ev && ev->type == KeyPress) {
        unsigned int consumedModifiers = 0;
        XkbLookupKeySym(ev->display, ev->keycode, ev->state, &consumedModifiers,
                        &keysym);
        if (buffer && nbytes > 0)
            produced = keysym_to_utf8(keysym, buffer, nbytes);
    }
    if (keysym_return)
        *keysym_return = keysym;
    if (status_return) {
        if (keysym == NoSymbol && produced == 0)
            *status_return = XLookupNone;
        else if (keysym != NoSymbol && produced == 0)
            *status_return = XLookupKeySym;
        else if (produced > 0 && keysym != NoSymbol)
            *status_return = XLookupBoth;
        else
            *status_return = XLookupChars;
    }
    return produced;
}

int XmbLookupString(XIC ic,
                    XKeyEvent *ev,
                    char *buffer,
                    int nbytes,
                    KeySym *keysym,
                    Status *status)
{
    /* IM commits are forwarded as UTF-8, which matches the locale encoding for
     * the UTF-8 locales this layer targets; a non-UTF-8 locale would want the
     * commit transcoded, but no current workload exercises that path.
     */
    if (ev && ev->keycode == 0)
        return Xutf8LookupString(ic, ev, buffer, nbytes, keysym, status);
    return do_locale_lookup(ev, buffer, nbytes, keysym, status);
}
/* Xutf8LookupString lives in src/input-method.c with the IM-driven path. */

Bool XSupportsLocale(void)
{
    /* Quiet probe: GTK and Qt call this on startup. Returning True keeps them
     * happy; the XmbDrawString family routes through XDrawString.
     */
    return True;
}

/* Adapt copyTextPropertyString (no status out, NULL means out of memory) to the
 * segment-converter signature the shared parser calls. UTF-8 output never hits
 * a locale encoding error, so the only failure is allocation.
 */
static char *convertSegmentUtf8(const unsigned char *src,
                                size_t len,
                                Bool latin1,
                                int *status)
{
    char *s = copyTextPropertyString(src, len, latin1);
    *status = s ? Success : XNoMemory;
    return s;
}

/* Shared body of Xmb/Xutf8 TextPropertyToTextList: identical property
 * validation, null-separated splitting, list allocation, and error unwinding;
 * only the per-segment byte conversion differs, passed in as convert. Keeping
 * one parser means the two entry points cannot drift apart.
 */
static int textPropertyToTextList(
    Display *dpy,
    const XTextProperty *text_prop,
    char ***list_ret,
    int *count_ret,
    char *(*convert)(const unsigned char *, size_t, Bool, int *) )
{
    if (!text_prop || !list_ret || !count_ret) {
        if (count_ret)
            *count_ret = 0;
        if (list_ret)
            *list_ret = NULL;
        return XNoMemory;
    }
    *list_ret = NULL;
    *count_ret = 0;
    if (text_prop->format != 8)
        return XConverterNotFound;
    /* Reject obviously bogus nitems before any arithmetic. The +1 NUL
     * terminator on the duplicated payload must not wrap, and count_ret is a
     * signed int. Cap at INT_MAX so the eventual memcpy length is always
     * representable.
     */
    if (text_prop->nitems > (unsigned long) INT_MAX)
        return XNoMemory;
    /* Only accept encodings whose 8-bit payload is already UTF-8 (the
     * UTF8_STRING atom) or Latin-1 (XA_STRING, the ICCCM "8-bit string"
     * representation). COMPOUND_TEXT requires real iconv work and is refused
     * here rather than mis-decoded.
     */
    Atom utf8 = dpy ? XInternAtom(dpy, "UTF8_STRING", False) : None;
    if (text_prop->encoding != utf8 && text_prop->encoding != XA_STRING)
        return XConverterNotFound;

    const unsigned char *value = text_prop->value;
    unsigned long nitems = value ? text_prop->nitems : 0;
    int count = 1;
    for (unsigned long i = 0; i < nitems; i++) {
        if (value[i] == '\0') {
            if (count == INT_MAX)
                return XNoMemory;
            count++;
        }
    }
    char **list = calloc((size_t) count + 1, sizeof(*list));
    if (!list)
        return XNoMemory;

    Bool latin1 = text_prop->encoding == XA_STRING;
    unsigned long start = 0;
    int item = 0;
    for (unsigned long i = 0; i <= nitems; i++) {
        if (i < nitems && value[i] != '\0')
            continue;
        size_t len = (size_t) (i - start);
        const unsigned char *src = value ? value + start : (unsigned char *) "";
        int status = Success;
        list[item] = convert(src, len, latin1, &status);
        if (!list[item]) {
            freeTextList(list, item);
            return status;
        }
        item++;
        start = i + 1;
    }
    *list_ret = list;
    *count_ret = count;
    return Success;
}

int XmbTextPropertyToTextList(Display *dpy,
                              const XTextProperty *text_prop,
                              char ***list_ret,
                              int *count_ret)
{
    /* Xmb owes its callers the process locale's multibyte encoding, so segments
     * are re-encoded via wcrtomb (copyTextPropertyStringLocale).
     */
    return textPropertyToTextList(dpy, text_prop, list_ret, count_ret,
                                  copyTextPropertyStringLocale);
}

int Xutf8TextPropertyToTextList(Display *dpy,
                                const XTextProperty *text_prop,
                                char ***list_ret,
                                int *count_ret)
{
    /* Xutf8 always yields UTF-8, independent of the process locale. */
    return textPropertyToTextList(dpy, text_prop, list_ret, count_ret,
                                  convertSegmentUtf8);
}

int XmbTextListToTextProperty(Display *dpy,
                              char **list,
                              int count,
                              XICCEncodingStyle style,
                              XTextProperty *text_prop)
{
    if (!text_prop || count < 0)
        return XNoMemory;

    if (!XStringListToTextProperty(list, count, text_prop))
        return XNoMemory;

    switch (style) {
    case XStringStyle:
    case XStdICCTextStyle:
        text_prop->encoding = XA_STRING;
        break;
    case XCompoundTextStyle:
        text_prop->encoding = XInternAtom(dpy, "COMPOUND_TEXT", False);
        break;
    case XTextStyle:
        text_prop->encoding = XInternAtom(dpy, "TEXT", False);
        break;
    case XUTF8StringStyle:
        text_prop->encoding = XInternAtom(dpy, "UTF8_STRING", False);
        break;
    default:
        text_prop->encoding = XA_STRING;
        break;
    }
    text_prop->format = 8;
    return Success;
}

int Xutf8TextListToTextProperty(Display *dpy,
                                char **list,
                                int count,
                                XICCEncodingStyle style,
                                XTextProperty *text_prop)
{
    return XmbTextListToTextProperty(dpy, list, count, style, text_prop);
}

int XSetClipRectangles(register Display *dpy,
                       GC gc,
                       int clip_x_origin,
                       int clip_y_origin,
                       XRectangle *rectangles,
                       int n,
                       int ordering)
{
    (void) dpy;
    (void) ordering;
    if (!gc)
        return 0;
    if (n < 0 || (n > 0 && !rectangles))
        return 0;
    GraphicContext *gContext = GET_GC(gc);
    if (!gContext)
        return 0;
    if (gContext->clipRects) {
        free(gContext->clipRects);
        gContext->clipRects = NULL;
        gContext->clipRectCount = 0;
    }
    gContext->clipRectanglesSet = False;
    gContext->clipOriginX = clip_x_origin;
    gContext->clipOriginY = clip_y_origin;
    if (n > 0) {
        gContext->clipRects = malloc(sizeof(XRectangle) * (size_t) n);
        if (!gContext->clipRects)
            return 0;
        memcpy(gContext->clipRects, rectangles,
               sizeof(XRectangle) * (size_t) n);
        gContext->clipRectCount = n;
    }
    gContext->clipRectanglesSet = True;
    GC_BUMP_GENERATION(gContext);
    return 1;
}

int XGetScreenSaver(register Display *dpy,
                    /* the following are return only vars */ int *timeout,
                    int *interval,
                    int *prefer_blanking,
                    int *allow_exp) /*boolean */
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Bool XkbSetDetectableAutoRepeat(Display *dpy, Bool detectable, Bool *supported)
{
    (void) dpy;
    (void) detectable;
    if (supported)
        *supported = True;
    return True;
}

/* Synthetic XKB extension codes. libx11-compat does not implement a real XKB
 * protocol, but Motif / GTK / xfreerdp probe XkbUseExtension and
 * XkbQueryExtension before any keyboard work. Reporting "available" with
 * consistent base codes from both probes keeps the downstream dispatch logic
 * happy; the actual XKB requests are still WARN_UNIMPLEMENTED stubs.
 *
 * The opcode/event/error bases are chosen above the core-protocol ranges so a
 * caller doing event.type - eventBase math does not collide with X core events
 * (0..34).
 */
#define XKB_SYNTHETIC_OPCODE 135
#define XKB_SYNTHETIC_EVENT_BASE 85
#define XKB_SYNTHETIC_ERROR_BASE 137

Bool XkbUseExtension(Display *dpy, int *major_rtrn, int *minor_rtrn)
{
    (void) dpy;
    if (major_rtrn)
        *major_rtrn = XkbMajorVersion;
    if (minor_rtrn)
        *minor_rtrn = XkbMinorVersion;
    return True;
}

Bool XkbQueryExtension(Display *dpy,
                       int *opcodeReturn,
                       int *eventBaseReturn,
                       int *errorBaseReturn,
                       int *majorRtrn,
                       int *minorRtrn)
{
    (void) dpy;
    if (opcodeReturn)
        *opcodeReturn = XKB_SYNTHETIC_OPCODE;
    if (eventBaseReturn)
        *eventBaseReturn = XKB_SYNTHETIC_EVENT_BASE;
    if (errorBaseReturn)
        *errorBaseReturn = XKB_SYNTHETIC_ERROR_BASE;
    if (majorRtrn)
        *majorRtrn = XkbMajorVersion;
    if (minorRtrn)
        *minorRtrn = XkbMinorVersion;
    return True;
}

/* TkX11 (and other XKB-aware toolkits) open their connection through
 * XkbOpenDisplay rather than XOpenDisplay so the XKB event/error bases are
 * reported in one call. Mirror the stock Xlib control flow: validate the
 * caller's requested library version first, open the display, probe the
 * synthetic XKB extension, and hand back the same bases XkbQueryExtension
 * reports. The version handshake must run before opening the display so an
 * incompatible caller is rejected with XkbOD_BadLibraryVersion rather than
 * having its requested version silently overwritten by XkbQueryExtension.
 */
Display *XkbOpenDisplay(_Xconst char *name,
                        int *ev_rtrn,
                        int *err_rtrn,
                        int *major_rtrn,
                        int *minor_rtrn,
                        int *reason)
{
    /* Real Xlib runs XkbLibraryVersion on the caller-supplied major/minor
     * before anything else. Only the major version must match; mismatch means
     * the caller was built against an incompatible XKB library. The pointers
     * are optional and only carry a version handshake when both are supplied,
     * so a caller passing just one (as an output slot) is not a version request
     * and must not be rejected.
     */
    if (major_rtrn && minor_rtrn &&
        (*major_rtrn != XkbMajorVersion || *minor_rtrn > XkbMinorVersion)) {
        if (major_rtrn)
            *major_rtrn = XkbMajorVersion;
        if (minor_rtrn)
            *minor_rtrn = XkbMinorVersion;
        if (reason)
            *reason = XkbOD_BadLibraryVersion;
        return NULL;
    }
    Display *dpy = XOpenDisplay(name);
    if (!dpy) {
        if (reason)
            *reason = XkbOD_ConnectionRefused;
        return NULL;
    }
    if (!XkbQueryExtension(dpy, NULL, ev_rtrn, err_rtrn, major_rtrn,
                           minor_rtrn)) {
        if (reason)
            *reason = XkbOD_NonXkbServer;
        XCloseDisplay(dpy);
        return NULL;
    }
    if (reason)
        *reason = XkbOD_Success;
    return dpy;
}

int XkbTranslateKeySym(Display *dpy,
                       KeySym *sym_rtrn,
                       unsigned int mods,
                       char *buffer,
                       int nbytes,
                       int *extra_rtrn)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XStoreColor(register Display *dpy, Colormap cmap, XColor *def)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

/* Make sure this produces the same string as DefineLocal/DefineSelf in xdm.
 * Otherwise, Xau will not be able to find your cookies in the Xauthority file.
 *
 * Note: POSIX says that the ``nodename'' member of utsname does _not_ have
 *       to have sufficient information for interfacing to the network,
 *       and so, you may be better off using gethostname (if it exists).
 */

#if (defined(_POSIX_SOURCE) && !defined(AIXV3) && !defined(__QNX__)) || \
    defined(hpux) || defined(SVR4)
#define NEED_UTSNAME
#include <sys/utsname.h>
#else
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#endif

/* _XGetHostname - similar to gethostname but allows special processing. */
int _XGetHostname(char *buf, int maxlen)
{
    int len;

#ifdef NEED_UTSNAME
    struct utsname name;

    if (maxlen <= 0 || !buf)
        return 0;

    uname(&name);
    len = (int) strlen(name.nodename);
    if (len >= maxlen)
        len = maxlen - 1;
    strncpy(buf, name.nodename, (size_t) len);
    buf[len] = '\0';
#else
    if (maxlen <= 0 || !buf)
        return 0;

    buf[0] = '\0';
    (void) gethostname(buf, maxlen);
    buf[maxlen - 1] = '\0';
    len = (int) strlen(buf);
#endif /* NEED_UTSNAME */
    return len;
}

/* XSetWMProperties sets the following properties:
 * 	WM_NAME		  type: TEXT		format: varies?
 * 	WM_ICON_NAME	  type: TEXT		format: varies?
 * 	WM_HINTS	  type: WM_HINTS	format: 32
 * 	WM_COMMAND	  type: TEXT		format: varies?
 * 	WM_CLIENT_MACHINE type: TEXT		format: varies?
 * 	WM_NORMAL_HINTS	  type: WM_SIZE_HINTS 	format: 32
 * 	WM_CLASS	  type: STRING/STRING	format: 8
 * 	WM_LOCALE_NAME	  type: STRING		format: 8
 */

void XSetWMProperties(
    Display *dpy,
    Window w /* window to decorate */,
    XTextProperty *windowName /* name of application */,
    XTextProperty *iconName /* name string for icon */,
    char **argv /* command line */,
    int argc /* size of command line */,
    XSizeHints *sizeHints /* size hints for window in its normal state */,
    XWMHints *wmHints /* miscellaneous window manager hints */,
    XClassHint *classHints /* resource name and class */)
{
    XTextProperty textprop;
    char hostName[256];
    int len = _XGetHostname(hostName, sizeof hostName);
    char *locale;

    /* set names of window and icon */
    if (windowName)
        XSetWMName(dpy, w, windowName);
    if (iconName)
        XSetWMIconName(dpy, w, iconName);

    /* set the command if given */
    if (argv) {
        /* for UNIX and other operating systems which use nul-terminated arrays
         * of STRINGs.
         */
        XSetCommand(dpy, w, argv, argc);
    }

    /* set the name of the machine on which this application is running */
    textprop.value = (unsigned char *) hostName;
    textprop.encoding = XA_STRING;
    textprop.format = 8;
    textprop.nitems = (unsigned long) len;
    XSetWMClientMachine(dpy, w, &textprop);

    /* set hints about how geometry and window manager interaction */
    if (sizeHints)
        XSetWMNormalHints(dpy, w, sizeHints);
    if (wmHints)
        XSetWMHints(dpy, w, wmHints);
    if (classHints) {
        XClassHint tmp;

        if (!classHints->res_name) {
            tmp.res_name = getenv("RESOURCE_NAME");
            if (!tmp.res_name && argv && argv[0]) {
                /* UNIX uses /dir/subdir/.../basename; other operating systems
                 * will have to change this.
                 */
                char *cp = strrchr(argv[0], '/');
#ifdef __UNIXOS2__
                char *os2_cp = strrchr(argv[0], '\\');
                char *dot_cp = strrchr(argv[0], '.');
                if (os2_cp && (os2_cp > cp)) {
                    if (dot_cp && (dot_cp > os2_cp))
                        *dot_cp = '\0';
                    cp = os2_cp;
                }
#endif
                tmp.res_name = (cp ? cp + 1 : argv[0]);
            }
            tmp.res_class = classHints->res_class;
            classHints = &tmp;
        }
        XSetClassHint(dpy, w, classHints);
    }

    locale = setlocale(LC_CTYPE, (char *) NULL);
    if (locale)
        XChangeProperty(dpy, w, XInternAtom(dpy, "WM_LOCALE_NAME", False),
                        XA_STRING, 8, PropModeReplace, (unsigned char *) locale,
                        (int) strlen(locale));
}

XWMHints *XAllocWMHints(void)
{
    return Xcalloc(1, sizeof(XWMHints));
}

char *XDisplayName(_Xconst char *display)
{
    char *d;
    if (display != (char *) NULL && *display != '\0')
        return ((char *) display);
    if ((d = getenv("DISPLAY")) != (char *) NULL)
        return (d);
    return ((char *) "");
}

Display *XDisplayOfIM(XIM im)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

char *XLocaleOfIM(XIM im)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

char *XmbResetIC(XIC ic)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

int XPending(Display *dpy)
{
    return XEventsQueued(dpy, QueuedAfterFlush);
}

int XUnloadFont(register Display *dpy, Font font)
{
    return compatFontClose(dpy, font);
}


int XGetErrorText(register Display *dpy,
                  register int code,
                  char *buffer,
                  int nbytes)
{
    (void) dpy;
    if (buffer && nbytes > 0)
        snprintf(buffer, (size_t) nbytes, "X error %d", code);
    return 0;
}


int XAutoRepeatOn(register Display *dpy)
{
    (void) dpy;
    return 1;
}

int XAutoRepeatOff(register Display *dpy)
{
    (void) dpy;
    return 1;
}

Status XQueryBestCursor(register Display *dpy,
                        Drawable drawable,
                        unsigned int width,
                        unsigned int height,
                        unsigned int *ret_width,
                        unsigned int *ret_height)
{
    return XQueryBestSize(dpy, CursorShape, drawable, width, height, ret_width,
                          ret_height);
}

int XRotateWindowProperties(register Display *dpy,
                            Window w,
                            Atom *properties,
                            register int nprops,
                            int npositions)
{
    SET_X_SERVER_REQUEST(dpy, X_RotateProperties);
    TYPE_CHECK(w, WINDOW, dpy, 0);
    if (nprops < 0 || (nprops > 0 && !properties)) {
        handleError(0, dpy, None, 0, BadValue, 0);
        return 0;
    }
    if (nprops == 0)
        return 1;
    for (int i = 0; i < nprops; i++) {
        if (!isValidAtom(properties[i])) {
            handleError(0, dpy, properties[i], 0, BadAtom, 0);
            return 0;
        }
        for (int j = i + 1; j < nprops; j++) {
            if (properties[i] == properties[j]) {
                handleError(0, dpy, properties[i], 0, BadMatch, 0);
                return 0;
            }
        }
    }

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(w);
    WindowProperty **props = malloc(sizeof(WindowProperty *) * (size_t) nprops);
    if (!props) {
        handleOutOfMemory(0, dpy, 0, 0);
        return 0;
    }
    for (int i = 0; i < nprops; i++) {
        props[i] = findProperty(&windowStruct->properties, properties[i], NULL);
        if (!props[i]) {
            free(props);
            handleError(0, dpy, properties[i], 0, BadMatch, 0);
            return 0;
        }
    }

    int shift = npositions % nprops;
    if (shift < 0)
        shift += nprops;
    if (shift != 0) {
        WindowProperty *snapshot =
            malloc(sizeof(WindowProperty) * (size_t) nprops);
        if (!snapshot) {
            free(props);
            handleOutOfMemory(0, dpy, 0, 0);
            return 0;
        }
        for (int i = 0; i < nprops; i++) {
            snapshot[i].dataFormat = props[i]->dataFormat;
            snapshot[i].dataLength = props[i]->dataLength;
            snapshot[i].type = props[i]->type;
            snapshot[i].data = props[i]->data;
        }
        for (int i = 0; i < nprops; i++) {
            int src = (i - shift + nprops) % nprops;
            props[i]->dataFormat = snapshot[src].dataFormat;
            props[i]->dataLength = snapshot[src].dataLength;
            props[i]->type = snapshot[src].type;
            props[i]->data = snapshot[src].data;
            postEvent(dpy, w, PropertyNotify, props[i]->property,
                      PropertyNewValue);
        }
        free(snapshot);
    }
    free(props);
    return 1;
}

int XChangeKeyboardMapping(register Display *dpy,
                           int first_keycode,
                           int keysyms_per_keycode,
                           KeySym *keysyms,
                           int nkeycodes)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

static Window childAtIndex(Array *children, size_t index)
{
    return (Window) children->array[index];
}

/* RaiseLowest: raise the lowest mapped child that is occluded by another mapped
 * sibling. If no such child exists, circulation is a no-op.
 */
static void circulateChildrenUp(Display *dpy, Window w)
{
    Array *childArray = &GET_WINDOW_STRUCT(w)->children;
    Window *children = GET_CHILDREN(w);
    if (childArray->length < 2)
        return;
    for (size_t i = 0; i < childArray->length; i++) {
        if (GET_WINDOW_STRUCT(children[i])->mapState != Mapped)
            continue;
        for (size_t j = i + 1; j < childArray->length; j++) {
            if (GET_WINDOW_STRUCT(children[j])->mapState == Mapped &&
                windowsOverlap(children[i], children[j])) {
                moveChildToIndexAndExpose(dpy, childAtIndex(childArray, i),
                                          childArray->length - 1);
                return;
            }
        }
    }
}

/* LowerHighest: lower the highest mapped child that occludes another mapped
 * sibling. If none match, the stacking order must stay unchanged.
 */
static void circulateChildrenDown(Display *dpy, Window w)
{
    Array *childArray = &GET_WINDOW_STRUCT(w)->children;
    Window *children = GET_CHILDREN(w);
    if (childArray->length < 2)
        return;
    for (size_t i = childArray->length; i-- > 0;) {
        if (GET_WINDOW_STRUCT(children[i])->mapState != Mapped)
            continue;
        for (size_t j = 0; j < i; j++) {
            if (GET_WINDOW_STRUCT(children[j])->mapState == Mapped &&
                windowsOverlap(children[i], children[j])) {
                moveChildToIndexAndExpose(dpy, childAtIndex(childArray, i), 0);
                return;
            }
        }
    }
}

int XCirculateSubwindowsUp(register Display *dpy, Window w)
{
    SET_X_SERVER_REQUEST(dpy, X_CirculateWindow);
    TYPE_CHECK(w, WINDOW, dpy, 0);
    circulateChildrenUp(dpy, w);
    return 1;
}

int XCirculateSubwindows(register Display *dpy, Window w, int direction)
{
    SET_X_SERVER_REQUEST(dpy, X_CirculateWindow);
    TYPE_CHECK(w, WINDOW, dpy, 0);
    switch (direction) {
    case RaiseLowest:
        circulateChildrenUp(dpy, w);
        return 1;
    case LowerHighest:
        circulateChildrenDown(dpy, w);
        return 1;
    default:
        handleError(0, dpy, None, 0, BadValue, 0);
        return 0;
    }
}

int XInstallColormap(register Display *dpy, Colormap cmap)
{
    /* The in-process model has a single TrueColor visual; "installing" a
     * colormap is a no-op success. Real Xlib returns 0 on failure only.
     */
    SET_X_SERVER_REQUEST(dpy, X_InstallColormap);
    postEvent(dpy, RootWindow(dpy, DefaultScreen(dpy)), ColormapNotify, cmap,
              False, ColormapInstalled);
    return 1;
}

Status XAllocColorPlanes(register Display *dpy,
                         Colormap cmap,
                         Bool contig,
                         unsigned long *pixels,
                         /* LISTofCARD32 */ /* RETURN */ int ncolors,
                         int nreds,
                         int ngreens,
                         int nblues,
                         unsigned long *rmask,
                         unsigned long *gmask,
                         unsigned long *bmask) /* CARD32 */ /* RETURN */
{
    /* TrueColor has only read-only cells, so XStoreColors against any pixels
     * XAllocColorPlanes would hand back would be a no-op and produce visual
     * artifacts instead of dynamic color updates. Real Xlib also fails this on
     * read-only visuals, so this entry point matches that behavior so
     * well-behaved clients fall back to XAllocColor.
     */
    SET_X_SERVER_REQUEST(dpy, X_AllocColorPlanes);
    (void) dpy;
    (void) cmap;
    (void) contig;
    (void) pixels;
    (void) ncolors;
    (void) nreds;
    (void) ngreens;
    (void) nblues;
    (void) rmask;
    (void) gmask;
    (void) bmask;
    return 0;
}

static XIOErrorHandler ioErrorHandler = NULL;

XIOErrorHandler XSetIOErrorHandler(XIOErrorHandler handler)
{
    XIOErrorHandler previous = ioErrorHandler;
    ioErrorHandler = handler;
    return previous;
}

void triggerIOError(Display *display)
{
    /* Real Xlib only reaches _XIOError after a socket operation fails, so a
     * client's XIOErrorHandler always runs with errno set to the connection
     * loss code. We invoke it synthetically (an SDL close or quit with no
     * WM_DELETE handler), so seed errno to match a broken connection. GDK's
     * gdk_x_io_error formats g_strerror(errno) through a glib charset path that
     * segfaults for arbitrary errno values; EPIPE steers it onto its clean
     * "connection broken" branch instead, and matches real teardown semantics.
     */
    errno = EPIPE;
    if (ioErrorHandler) {
        /* Per Xlib spec, the IO error handler is not expected to return; if it
         * does, the client is killed.
         */
        (void) ioErrorHandler(display);
    }
    /* No handler, or handler returned: emulate the X server tearing down the
     * connection. Real Xlib's default behavior here is _exit(1).
     */
    fprintf(stderr, "libX11-compat: connection torn down by host\n");
    exit(0);
}


int XGetPointerControl(
    register Display *dpy,
    /* the following are return only vars */ int *accel_numer,
    int *accel_denom,
    int *threshold)
{
    (void) dpy;
    if (accel_numer)
        *accel_numer = 1;
    if (accel_denom)
        *accel_denom = 1;
    if (threshold)
        *threshold = 0;
    return 1;
}

int XGetKeyboardControl(register Display *dpy, register XKeyboardState *state)
{
    (void) dpy;
    if (!state)
        return 0;
    memset(state, 0, sizeof(*state));
    state->bell_percent = 50;
    state->bell_pitch = 400;
    state->bell_duration = 100;
    state->global_auto_repeat = AutoRepeatModeOn;
    memset(state->auto_repeats, 0xff, sizeof(state->auto_repeats));
    return 1;
}


int XCirculateSubwindowsDown(register Display *dpy, Window w)
{
    SET_X_SERVER_REQUEST(dpy, X_CirculateWindow);
    TYPE_CHECK(w, WINDOW, dpy, 0);
    circulateChildrenDown(dpy, w);
    return 1;
}

int XSetCloseDownMode(register Display *dpy, int mode)
{
    /* The in-process model holds no multi-client server state, so
     * RetainPermanent / RetainTemporary / DestroyAll collapse to a successful
     * no-op.
     */
    SET_X_SERVER_REQUEST(dpy, X_SetCloseDownMode);
    (void) mode;
    return 1;
}

XcmsCCC XcmsCreateCCC(Display *dpy,
                      int screenNumber,
                      Visual *visual,
                      XcmsColor *clientWhitePt,
                      XcmsCompressionProc gamutCompProc,
                      XPointer gamutCompClientData,
                      XcmsWhiteAdjustProc whitePtAdjProc,
                      XPointer whitePtAdjClientData)
/* Given a Display, Screen, Visual, etc., this routine creates an appropriate
 * Color Conversion Context.
 *
 * Return NULL if failed; otherwise address of the newly created XcmsCCC.
 */
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

/* Converts color specifications in an array of XcmsColor * structures from RGBi
 * format to RGB format.
 *
 * Return XcmsFailure if failed, XcmsSuccess if succeeded without gamut
 * compression. XcmsSuccessWithCompression if succeeded with gamut compression.
 */
Status XcmsRGBiToRGB(
    XcmsCCC ccc,
    XcmsColor *pXcmsColors_in_out /* pointer to XcmsColors to convert */,
    unsigned int nColors /* Number of colors */,
    Bool *pCompressed) /* pointer to a bit array */
{
    WARN_UNIMPLEMENTED;
    return 0;
}

/* Converts color specifications in an array of XcmsColor structures from CIELuv
 * format to CIEuvY format.
 *
 * Return XcmsFailure if failed, XcmsSuccess if succeeded.
 */

Status XcmsCIELuvToCIEuvY(XcmsCCC ccc,
                          XcmsColor *pLuv_WhitePt,
                          XcmsColor *pColors_in_out,
                          unsigned int nColors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XChangeActivePointerGrab(register Display *dpy,
                             unsigned int event_mask,
                             /* CARD16 */ Cursor curs,
                             Time time)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XQueryBestSize(register Display *dpy,
                      int class,
                      Drawable drawable,
                      unsigned int width,
                      unsigned int height,
                      unsigned int *ret_width,
                      unsigned int *ret_height)
{
    (void) dpy;
    (void) class;
    (void) drawable;
    if (ret_width)
        *ret_width = width;
    if (ret_height)
        *ret_height = height;
    return 1;
}

void XFlushGC(Display *dpy, GC gc)
{
    WARN_UNIMPLEMENTED;
}

Status XQueryBestTile(register Display *dpy,
                      Drawable drawable,
                      unsigned int width,
                      unsigned int height,
                      unsigned int *ret_width,
                      unsigned int *ret_height)
{
    return XQueryBestSize(dpy, TileShape, drawable, width, height, ret_width,
                          ret_height);
}

int XChangePointerControl(register Display *dpy,
                          Bool do_acc,
                          Bool do_thresh,
                          int acc_numerator,
                          int acc_denominator,
                          int threshold)
{
    (void) dpy;
    (void) do_acc;
    (void) do_thresh;
    (void) acc_numerator;
    (void) acc_denominator;
    (void) threshold;
    return 1;
}

int XQueryTextExtents(register Display *dpy,
                      Font fid,
                      register _Xconst char *string,
                      register int nchars,
                      int *dir,
                      int *font_ascent,
                      int *font_descent,
                      register XCharStruct *overall)
{
    Font temporaryFont = None;
    XFontStruct *fs = queryTextExtentsFont(dpy, fid, &temporaryFont);
    if (!fs)
        return 0;
    int width = string && nchars > 0 ? XTextWidth(fs, string, nchars) : 0;
    fillTextExtents(fs, width, dir, font_ascent, font_descent, overall);
    freeQueriedFontStruct(fs);
    if (temporaryFont != None)
        XUnloadFont(dpy, temporaryFont);
    /* Spec: Status nonzero on success. Returning 0 here made Motif and Xt
     * widget measurement paths treat the filled metrics as invalid and fall
     * back to zero-width text.
     */
    return 1;
}

int XChangeKeyboardControl(register Display *dpy,
                           unsigned long mask,
                           XKeyboardControl *value_list)
{
    (void) dpy;
    (void) mask;
    (void) value_list;
    return 1;
}

Colormap XCopyColormapAndFree(register Display *dpy, Colormap src_cmap)
{
    WARN_UNIMPLEMENTED;
    return 0;
}


int XRemoveHost(register Display *dpy, XHostAddress *host)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XRemoveHosts(register Display *dpy, XHostAddress *hosts, int n)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XChangeSaveSet(register Display *dpy, Window win, int mode)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XAddToSaveSet(register Display *dpy, Window win)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XSetFillRule(register Display *dpy, register GC gc, int fill_rule)
{
    XGCValues values;
    values.fill_rule = fill_rule;
    return XChangeGC(dpy, gc, GCFillRule, &values);
}

Status XQueryBestStipple(register Display *dpy,
                         Drawable drawable,
                         unsigned int width,
                         unsigned int height,
                         unsigned int *ret_width,
                         unsigned int *ret_height)
{
    return XQueryBestSize(dpy, StippleShape, drawable, width, height, ret_width,
                          ret_height);
}

int XKillClient(register Display *dpy, XID resource)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XActivateScreenSaver(register Display *dpy)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XDrawText16(register Display *dpy,
                Drawable d,
                GC gc,
                int x,
                int y,
                XTextItem16 *items,
                int nitems)
{
    if (!items || nitems <= 0)
        return 1;
    if (!gc) {
        handleError(0, dpy, None, 0, BadGC, 0);
        return 0;
    }
    GraphicContext *gContext = GET_GC(gc);
    Font oldFont = gContext->font;
    int cursor = x;
    for (int i = 0; i < nitems; i++) {
        cursor += items[i].delta;
        if (items[i].font != None) {
            gContext->font = items[i].font;
            GC_BUMP_GENERATION(gContext);
        }
        if (items[i].chars && items[i].nchars > 0) {
            if (!XDrawString16(dpy, d, gc, cursor, y, items[i].chars,
                               items[i].nchars)) {
                gContext->font = oldFont;
                GC_BUMP_GENERATION(gContext);
                return 0;
            }
            XFontStruct *fontStruct = XQueryFont(dpy, gContext->font);
            if (fontStruct) {
                cursor +=
                    XTextWidth16(fontStruct, items[i].chars, items[i].nchars);
                XFreeFontInfo(NULL, fontStruct, 1);
            }
        }
    }
    if (gContext->font != oldFont) {
        gContext->font = oldFont;
        GC_BUMP_GENERATION(gContext);
    }
    return 1;
}

int XEnableAccessControl(register Display *dpy)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XSetAccessControl(register Display *dpy, int mode)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

/* Convert XcmsColor structures to another format
 *
 * Return XcmsFailure if failed, XcmsSuccess if succeeded without gamut
 * compression, XcmsSuccessWithCompression if succeeded with gamut compression.
 */

Status XcmsConvertColors(XcmsCCC ccc,
                         XcmsColor *pColors_in_out,
                         unsigned int nColors,
                         XcmsColorFormat targetFormat,
                         Bool *pCompressed)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

/* Transforms an array of TekHVC color specifications, given their associated
 * white point, to CIEuvY color specifications.
 *
 * Returns XcmsFailure on failure, XcmsSuccess otherwise.
 */
Status XcmsTekHVCToCIEuvY(XcmsCCC ccc,
                          XcmsColor *pHVC_WhitePt,
                          XcmsColor *pColors_in_out,
                          unsigned int nColors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

/* Sets the specified CCC's white_adjust function and client data; returns the
 * previously installed white_adjust function.
 */
XcmsWhiteAdjustProc XcmsSetWhiteAdjustProc(
    XcmsCCC ccc,
    XcmsWhiteAdjustProc white_adjust_proc,
    XPointer client_data)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

/* Converts color specifications in an array of XcmsColor structures from RGB
 * format to RGBi format.
 *
 * Return XcmsFailure if failed, XcmsSuccess if succeeded.
 */

Status XcmsRGBToRGBi(
    XcmsCCC ccc,
    XcmsColor *pXcmsColors_in_out /* pointer to XcmsColors to convert */,
    unsigned int nColors /* Number of colors */,
    Bool *pCompressed /* pointer to a bit array */)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

/* Queries the screen number of the specified CCC.
 *
 * Return screen number.
 */

int XcmsScreenNumberOfCCC(XcmsCCC ccc)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XStoreNamedColor(register Display *dpy,
                     Colormap cmap,
                     _Xconst char *name /* STRING8 */,
                     unsigned long pixel /* CARD32 */,
                     int flags /* DoRed, DoGreen, DoBlue */)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

static void fillTextExtents(XFontStruct *fs,
                            int width,
                            int *dir,
                            int *font_ascent,
                            int *font_descent,
                            XCharStruct *overall)
{
    if (dir)
        *dir = FontLeftToRight;
    if (font_ascent)
        *font_ascent = fs ? fs->ascent : 0;
    if (font_descent)
        *font_descent = fs ? fs->descent : 0;
    if (overall) {
        /* Real Xlib XCharStruct uses short for bearing/width/ascent; clamp
         * instead of letting a wide string or huge font silently wrap negative.
         */
        int fsAscent = fs ? fs->ascent : 0;
        int fsDescent = fs ? fs->descent : 0;
        memset(overall, 0, sizeof(*overall));
        overall->lbearing = 0;
        overall->rbearing = (short) (width > SHRT_MAX   ? SHRT_MAX
                                     : width < SHRT_MIN ? SHRT_MIN
                                                        : width);
        overall->width = overall->rbearing;
        overall->ascent = (short) (fsAscent > SHRT_MAX   ? SHRT_MAX
                                   : fsAscent < SHRT_MIN ? SHRT_MIN
                                                         : fsAscent);
        overall->descent = (short) (fsDescent > SHRT_MAX   ? SHRT_MAX
                                    : fsDescent < SHRT_MIN ? SHRT_MIN
                                                           : fsDescent);
    }
}

static void freeQueriedFontStruct(XFontStruct *fs)
{
    if (!fs)
        return;
    free(fs->properties);
    free(fs->per_char);
    free(fs);
}

static XFontStruct *queryTextExtentsFont(Display *dpy,
                                         XID fontOrGc,
                                         Font *temporaryFontReturn)
{
    *temporaryFontReturn = None;
    if (IS_TYPE(fontOrGc, GRAPHICS_CONTEXT)) {
        Font font = GET_GC_FROM_XID(fontOrGc)->font;
        if (font == None) {
            font = XLoadFont(dpy, "fixed");
            *temporaryFontReturn = font;
        }
        if (font == None)
            return NULL;
        XFontStruct *fs = compatFontQueryRetained(dpy, font);
        if (!fs && *temporaryFontReturn != None) {
            XUnloadFont(dpy, *temporaryFontReturn);
            *temporaryFontReturn = None;
        }
        return fs;
    }
    return XQueryFont(dpy, fontOrGc);
}

int XTextExtents16(
    XFontStruct *fs,
    _Xconst XChar2b *string,
    int nchars,
    int *dir /* RETURN font information */,
    int *font_ascent /* RETURN font information */,
    int *font_descent /* RETURN font information */,
    register XCharStruct *overall /* RETURN character information */)
{
    int width =
        fs && string && nchars > 0 ? XTextWidth16(fs, string, nchars) : 0;
    fillTextExtents(fs, width, dir, font_ascent, font_descent, overall);
    return 0;
}

int XDrawText(register Display *dpy,
              Drawable d,
              GC gc,
              int x,
              int y,
              XTextItem *items,
              int nitems)
{
    if (!items || nitems <= 0)
        return 1;
    if (!gc) {
        handleError(0, dpy, None, 0, BadGC, 0);
        return 0;
    }
    GraphicContext *gContext = GET_GC(gc);
    Font oldFont = gContext->font;
    int cursor = x;
    for (int i = 0; i < nitems; i++) {
        cursor += items[i].delta;
        if (items[i].font != None) {
            gContext->font = items[i].font;
            GC_BUMP_GENERATION(gContext);
        }
        if (items[i].chars && items[i].nchars > 0) {
            if (!XDrawString(dpy, d, gc, cursor, y, items[i].chars,
                             items[i].nchars)) {
                gContext->font = oldFont;
                GC_BUMP_GENERATION(gContext);
                return 0;
            }
            XFontStruct *fontStruct = XQueryFont(dpy, gContext->font);
            if (fontStruct) {
                cursor +=
                    XTextWidth(fontStruct, items[i].chars, items[i].nchars);
                XFreeFontInfo(NULL, fontStruct, 1);
            }
        }
    }
    if (gContext->font != oldFont) {
        gContext->font = oldFont;
        GC_BUMP_GENERATION(gContext);
    }
    return 1;
}

int XTextExtents(
    XFontStruct *fs,
    _Xconst char *string,
    int nchars,
    int *dir /* RETURN font information */,
    int *font_ascent /* RETURN font information */,
    int *font_descent /* RETURN font information */,
    register XCharStruct *overall /* RETURN character information */)
{
    int width = string && nchars > 0 ? XTextWidth(fs, string, nchars) : 0;
    fillTextExtents(fs, width, dir, font_ascent, font_descent, overall);
    return 0;
}

int XStoreColors(register Display *dpy,
                 Colormap cmap,
                 XColor *defs,
                 int ncolors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XQueryTextExtents16(register Display *dpy,
                        Font fid,
                        _Xconst XChar2b *string,
                        register int nchars,
                        int *dir,
                        int *font_ascent,
                        int *font_descent,
                        register XCharStruct *overall)
{
    Font temporaryFont = None;
    XFontStruct *fs = queryTextExtentsFont(dpy, fid, &temporaryFont);
    if (!fs)
        return 0;
    int width = string && nchars > 0 ? XTextWidth16(fs, string, nchars) : 0;
    fillTextExtents(fs, width, dir, font_ascent, font_descent, overall);
    freeQueriedFontStruct(fs);
    if (temporaryFont != None)
        XUnloadFont(dpy, temporaryFont);
    /* See XQueryTextExtents: Status must be nonzero on success. */
    return 1;
}


int XQueryKeymap(register Display *dpy, char keys[32])
{
    WARN_UNIMPLEMENTED;
    return 0;
}

unsigned long XDisplayMotionBufferSize(Display *dpy)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XSetPointerMapping(register Display *dpy,
                       _Xconst unsigned char *map,
                       int nmaps)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XAddHost(register Display *dpy, XHostAddress *host)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XAddHosts(register Display *dpy, XHostAddress *hosts, int n)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XSetModifierMapping(register Display *dpy,
                        register XModifierKeymap *modifier_map)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XRemoveFromSaveSet(register Display *dpy, Window win)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XAllowEvents(register Display *dpy, int mode, Time time)
{
    (void) dpy;
    (void) time;
    extern Bool mouseFrozen;
    extern Bool keyboardFrozen;
    switch (mode) {
    case AsyncPointer:
        mouseFrozen = False;
        break;
    case ReplayPointer:
        mouseFrozen = False;
        releasePassivePointerGrab(dpy);
        break;
    case AsyncKeyboard:
    case ReplayKeyboard:
        keyboardFrozen = False;
        break;
    case AsyncBoth:
        mouseFrozen = False;
        keyboardFrozen = False;
        break;
    case SyncPointer:
        mouseFrozen = False;
        break;
    case SyncKeyboard:
        keyboardFrozen = False;
        break;
    case SyncBoth:
        mouseFrozen = False;
        keyboardFrozen = False;
        break;
    default:
        return 0;
    }
    return 1;
}

int XUninstallColormap(register Display *dpy, Colormap cmap)
{
    /* Mirror of XInstallColormap: a successful no-op in the in-process
     * TrueColor model.
     */
    SET_X_SERVER_REQUEST(dpy, X_UninstallColormap);
    postEvent(dpy, RootWindow(dpy, DefaultScreen(dpy)), ColormapNotify, cmap,
              False, ColormapUninstalled);
    return 1;
}

/* XGrabKey / XUngrabKey live in src/input.c next to the rest of the keyboard
 * surface so the grab table and findKeyGrabWindow helper can share state.
 */

int XDisableAccessControl(register Display *dpy)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

/* Converts color specifications in an array of XcmsColor structures from CIEXYZ
 * format to RGBi format.
 *
 * Return XcmsFailure if failed, XcmsSuccess if succeeded without gamut
 * compression. XcmsSuccessWithCompression if succeeded with gamut compression.
 */

Status XcmsCIEXYZToRGBi(
    XcmsCCC ccc,
    XcmsColor *pXcmsColors_in_out /* pointer to XcmsColors to convert */,
    unsigned int nColors /* Number of colors */,
    Bool *pCompressed /* pointer to an array of Bool */)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XGetPointerMapping(register Display *dpy,
                       unsigned char *map,
                       /* RETURN */ int nmaps)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Bool XContextDependentDrawing(XFontSet font_set)
{
    WARN_UNIMPLEMENTED;
    return False;
}

/* XUngrabKey is defined alongside XGrabKey in src/input.c. */

void XwcDrawText(Display *dpy,
                 Drawable d,
                 GC gc,
                 int x,
                 int y,
                 XwcTextItem *text_items,
                 int nitems)
{
    WARN_UNIMPLEMENTED;
}

static char *wchar_to_utf8(_Xconst wchar_t *wide, int wide_len, int *out_len)
{
    if (!wide || wide_len <= 0) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    size_t cap = (size_t) wide_len * 4 + 1;
    char *buf = malloc(cap);
    if (!buf) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    size_t pos = 0;
    for (int i = 0; i < wide_len; i++) {
        wchar_t wc = wide[i];
        unsigned long cp = (unsigned long) wc;
        if (cp < 0x80) {
            buf[pos++] = (char) cp;
        } else if (cp < 0x800) {
            buf[pos++] = (char) (0xc0 | (cp >> 6));
            buf[pos++] = (char) (0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            buf[pos++] = (char) (0xe0 | (cp >> 12));
            buf[pos++] = (char) (0x80 | ((cp >> 6) & 0x3f));
            buf[pos++] = (char) (0x80 | (cp & 0x3f));
        } else if (cp <= 0x10ffff) {
            buf[pos++] = (char) (0xf0 | (cp >> 18));
            buf[pos++] = (char) (0x80 | ((cp >> 12) & 0x3f));
            buf[pos++] = (char) (0x80 | ((cp >> 6) & 0x3f));
            buf[pos++] = (char) (0x80 | (cp & 0x3f));
        }
    }
    buf[pos] = '\0';
    if (out_len)
        *out_len = (int) pos;
    return buf;
}

void XwcDrawString(Display *dpy,
                   Drawable d,
                   XFontSet font_set,
                   GC gc,
                   int x,
                   int y,
                   _Xconst wchar_t *text,
                   int text_len)
{
    (void) font_set;
    int utf8_len = 0;
    char *utf8 = wchar_to_utf8(text, text_len, &utf8_len);
    if (!utf8)
        return;
    XDrawString(dpy, d, gc, x, y, utf8, utf8_len);
    free(utf8);
}

void XwcDrawImageString(Display *dpy,
                        Drawable d,
                        XFontSet font_set,
                        GC gc,
                        int x,
                        int y,
                        _Xconst wchar_t *text,
                        int text_len)
{
    (void) font_set;
    int utf8_len = 0;
    char *utf8 = wchar_to_utf8(text, text_len, &utf8_len);
    if (!utf8)
        return;
    XDrawImageString(dpy, d, gc, x, y, utf8, utf8_len);
    free(utf8);
}

void XmbDrawText(Display *dpy,
                 Drawable d,
                 GC gc,
                 int x,
                 int y,
                 XmbTextItem *text_items,
                 int nitems)
{
    /* TextItem chains apply font set / delta offsets per chunk. The font stack
     * only honors the GC's font, so concatenate the strings and advance x by
     * the requested delta between chunks.
     */
    if (!text_items || nitems <= 0)
        return;
    int cursor = x;
    for (int i = 0; i < nitems; i++) {
        cursor += text_items[i].delta;
        if (text_items[i].chars && text_items[i].nchars > 0) {
            Font oldFont = None;
            Bool changed =
                useFontSetFontForGC(gc, text_items[i].font_set, &oldFont);
            XDrawString(dpy, d, gc, cursor, y, text_items[i].chars,
                        text_items[i].nchars);
            if (changed)
                restoreGCFontAfterFontSet(gc, oldFont);
            cursor +=
                XmbTextEscapement(text_items[i].font_set, text_items[i].chars,
                                  text_items[i].nchars);
        }
    }
}

void XmbDrawString(Display *dpy,
                   Drawable d,
                   XFontSet font_set,
                   GC gc,
                   int x,
                   int y,
                   _Xconst char *text,
                   int text_len)
{
    Font oldFont = None;
    Bool changed = useFontSetFontForGC(gc, font_set, &oldFont);
    XDrawString(dpy, d, gc, x, y, text, text_len);
    if (changed)
        restoreGCFontAfterFontSet(gc, oldFont);
}

void XmbDrawImageString(Display *dpy,
                        Drawable d,
                        XFontSet font_set,
                        GC gc,
                        int x,
                        int y,
                        _Xconst char *text,
                        int text_len)
{
    Font oldFont = None;
    Bool changed = useFontSetFontForGC(gc, font_set, &oldFont);
    XDrawImageString(dpy, d, gc, x, y, text, text_len);
    if (changed)
        restoreGCFontAfterFontSet(gc, oldFont);
}

void Xutf8DrawString(Display *dpy,
                     Drawable d,
                     XFontSet font_set,
                     GC gc,
                     int x,
                     int y,
                     _Xconst char *text,
                     int text_len)
{
    Font oldFont = None;
    Bool changed = useFontSetFontForGC(gc, font_set, &oldFont);
    XDrawString(dpy, d, gc, x, y, text, text_len);
    if (changed)
        restoreGCFontAfterFontSet(gc, oldFont);
}

void Xutf8DrawImageString(Display *dpy,
                          Drawable d,
                          XFontSet font_set,
                          GC gc,
                          int x,
                          int y,
                          _Xconst char *text,
                          int text_len)
{
    Font oldFont = None;
    Bool changed = useFontSetFontForGC(gc, font_set, &oldFont);
    XDrawImageString(dpy, d, gc, x, y, text, text_len);
    if (changed)
        restoreGCFontAfterFontSet(gc, oldFont);
}

int XmbTextEscapement(XFontSet font_set, _Xconst char *text, int text_len)
{
    if (!text || text_len <= 0)
        return 0;
    CompatFontSet *set = GET_FONT_SET(font_set);
    int w;
    if (set && set->font) {
        w = XTextWidth(set->font, text, text_len);
    } else {
        /* 8 px-per-byte fallback runs through int64 so a long text_len cannot
         * wrap the returned int.
         */
        int64_t fallback = (int64_t) text_len * 8;
        w = clampInt64ToInt(fallback);
    }
    LOG("XmbTextEscapement: text_len=%d (preview='%.20s') -> %d\n", text_len,
        text, w);
    return w;
}

int Xutf8TextEscapement(XFontSet font_set, _Xconst char *text, int text_len)
{
    if (!text || text_len <= 0)
        return 0;
    CompatFontSet *set = GET_FONT_SET(font_set);
    if (set && set->font)
        return XTextWidth(set->font, text, text_len);
    return clampInt64ToInt((int64_t) text_len * 8);
}

XIM XIMOfIC(XIC ic)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

/* Count UTF-8 codepoints in [text, text + byteLen), clamped to INT_MAX. */
static int wcharCountUtf8(const char *text, size_t byteLen)
{
    size_t needed = 0, remaining = byteLen;
    const unsigned char *src = (const unsigned char *) text;
    while (remaining > 0) {
        wchar_t cp = 0;
        int used = decodeUtf8Codepoint(src, remaining, &cp);
        if (used <= 0 || (size_t) used > remaining)
            break;
        needed++;
        src += used;
        remaining -= (size_t) used;
    }
    return needed > (size_t) INT_MAX ? INT_MAX : (int) needed;
}

/* Decode the first `count` codepoints of [text, text + byteLen) into buffer. */
static void wcharFillUtf8(const char *text,
                          size_t byteLen,
                          wchar_t *buffer,
                          int count)
{
    size_t remaining = byteLen;
    const unsigned char *src = (const unsigned char *) text;
    for (int i = 0; i < count; i++) {
        int used = decodeUtf8Codepoint(src, remaining, &buffer[i]);
        if (used <= 0 || (size_t) used > remaining)
            break;
        src += used;
        remaining -= (size_t) used;
    }
}

int XwcLookupString(XIC ic,
                    XKeyEvent *ev,
                    wchar_t *buffer,
                    int nchars,
                    KeySym *keysym,
                    Status *status)
{
    if (ev && ev->keycode == 0) {
        /* IM commit: peek the pending UTF-8 text and only consume it once the
         * caller's wide buffer is confirmed large enough, so an overflow retry
         * with a bigger buffer still finds the committed string. Decoding the
         * pending text directly also avoids the fixed stack buffer that an
         * over-long commit could overrun.
         */
        unsigned long commitId = (unsigned long) ev->subwindow;
        char *pendingText = inputMethodPendingText(commitId);
        if (!pendingText) {
            if (status)
                *status = XLookupNone;
            return 0;
        }
        size_t byteLen = strlen(pendingText);
        int needed = wcharCountUtf8(pendingText, byteLen);
        if (!buffer || nchars < 0 || needed > nchars) {
            if (status)
                *status = XBufferOverflow;
            return needed;
        }
        wcharFillUtf8(pendingText, byteLen, buffer, needed);
        /* Mirror Xutf8LookupString: a lone ASCII byte also yields a keysym. */
        if (byteLen == 1) {
            if (status)
                *status = XLookupBoth;
            if (keysym)
                *keysym = getKeySymForChar(pendingText[0]);
        } else {
            if (status)
                *status = XLookupChars;
            if (keysym)
                *keysym = NoSymbol;
        }
        inputMethodConsumePendingText(commitId);
        return needed;
    }

    /* Real key: appendKeysymUtf8 produces at most 4 bytes (one codepoint), so
     * the 7 usable bytes here can never overflow and Xutf8LookupString never
     * reports XBufferOverflow on this path, including when status is NULL.
     * There is also no committed text to preserve. The bytes-in-range check is
     * belt and suspenders so a future change to that bound can never let the
     * decode loop read past utf8.
     */
    char utf8[8];
    int bytes =
        Xutf8LookupString(ic, ev, utf8, sizeof(utf8) - 1, keysym, status);
    if (bytes <= 0 || bytes > (int) (sizeof(utf8) - 1) ||
        (status && *status == XBufferOverflow))
        return bytes;
    int needed = wcharCountUtf8(utf8, (size_t) bytes);
    if (!buffer || nchars < 0 || needed > nchars) {
        if (status)
            *status = XBufferOverflow;
        return needed;
    }
    wcharFillUtf8(utf8, (size_t) bytes, buffer, needed);
    return needed;
}

/* Decode one UTF-8 sequence from `src[0..len)` into `cp`.
 *
 * Returns the number of bytes consumed (1..4) or 0 on a malformed sequence.
 * Treats the source as Latin-1 when the leading byte is a stray continuation
 * (0x80..0xbf) - matches what Xlib does for legacy XA_STRING properties that
 * turned out to be ISO-8859-1 in practice.
 */
static int decodeUtf8Codepoint(const unsigned char *src,
                               size_t len,
                               wchar_t *cp)
{
    if (len == 0)
        return 0;
    unsigned char c = src[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    int extra;
    wchar_t value;
    unsigned int minCp;
    if ((c & 0xe0) == 0xc0) {
        extra = 1;
        value = c & 0x1f;
        minCp = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        extra = 2;
        value = c & 0x0f;
        minCp = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
        extra = 3;
        value = c & 0x07;
        minCp = 0x10000;
    } else {
        /* Stray continuation; pass through as Latin-1. */
        *cp = c;
        return 1;
    }
    if (len < (size_t) (1 + extra)) {
        *cp = c;
        return 1;
    }
    for (int i = 0; i < extra; i++) {
        unsigned char cont = src[1 + i];
        if ((cont & 0xc0) != 0x80) {
            *cp = c;
            return 1;
        }
        value = (value << 6) | (cont & 0x3f);
    }
    if ((unsigned) value < minCp || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        /* Overlong, out-of-range, or UTF-16 surrogate codepoint - fall back to
         * Latin-1 on the lead byte so the decoder still makes forward progress.
         * Matches XftUtf8ToUcs4's policy.
         */
        *cp = c;
        return 1;
    }
    *cp = value;
    return 1 + extra;
}

/* Decode one ICCCM text-property segment (between embedded NULs) into the wide
 * buffer.
 *
 * Returns the number of wchar_t entries written, not counting the
 * caller-appended terminator. Latin-1 input widens byte-for-byte; UTF-8 input
 * runs through decodeUtf8Codepoint, which already handles malformed sequences
 * by falling back to the Latin-1 byte so the loop always makes progress.
 */
static size_t decodeTextPropertySegment(const unsigned char *segment,
                                        size_t segLen,
                                        Bool utf8Source,
                                        wchar_t *out)
{
    if (!utf8Source) {
        for (size_t j = 0; j < segLen; j++)
            out[j] = (wchar_t) segment[j];
        return segLen;
    }
    size_t written = 0;
    size_t remaining = segLen;
    while (remaining > 0) {
        wchar_t cp = 0;
        int used = decodeUtf8Codepoint(segment, remaining, &cp);
        if (used <= 0 || (size_t) used > remaining)
            break;
        out[written++] = cp;
        segment += used;
        remaining -= (size_t) used;
    }
    return written;
}

int XwcTextPropertyToTextList(Display *dpy,
                              const XTextProperty *text_prop,
                              wchar_t ***list_ret,
                              int *count_ret)
{
    if (!text_prop || !list_ret || !count_ret) {
        if (count_ret)
            *count_ret = 0;
        if (list_ret)
            *list_ret = NULL;
        return XNoMemory;
    }
    *list_ret = NULL;
    *count_ret = 0;
    if (text_prop->format != 8)
        return XConverterNotFound;
    if (text_prop->nitems > (unsigned long) INT_MAX)
        return XNoMemory;
    Atom utf8 = dpy ? XInternAtom(dpy, "UTF8_STRING", False) : None;
    Atom compoundText = dpy ? XInternAtom(dpy, "COMPOUND_TEXT", False) : None;
    /* XmbTextListToTextProperty stamps XTextStyle payloads with the "TEXT"
     * atom; per ICCCM that means "whatever locale-encoded text the server
     * wants", which on this shim is the caller's UTF-8 / Latin-1 bytes
     * round-tripping back. Accept TEXT alongside the other 8-bit encodings so
     * libXaw's text source paths (_XawTextMBToWC -> XwcTextPropertyToTextList)
     * stay functional.
     */
    Atom textAtom = dpy ? XInternAtom(dpy, "TEXT", False) : None;
    Bool utf8Source = text_prop->encoding == utf8;
    Bool latin1Source = text_prop->encoding == XA_STRING ||
                        text_prop->encoding == compoundText ||
                        text_prop->encoding == textAtom;
    if (!utf8Source && !latin1Source)
        return XConverterNotFound;

    /* ICCCM text properties may pack multiple strings separated by embedded NUL
     * bytes (WM_COMMAND argv, WM_CLIENT_MACHINE, etc.). Pre-count separators so
     * list_ret has the right number of slots, each pointing into the single
     * contiguous wchar_t buffer that XwcFreeStringList tears down via list[0] +
     * list.
     */
    const unsigned char *value = text_prop->value;
    unsigned long nitems = value ? text_prop->nitems : 0;
    int count = 1;
    for (unsigned long i = 0; i < nitems; i++) {
        if (value[i] == '\0') {
            if (count == INT_MAX)
                return XNoMemory;
            count++;
        }
    }

    wchar_t **list = calloc((size_t) count + 1, sizeof(*list));
    if (!list)
        return XNoMemory;
    /* nitems + count fits in size_t (both already INT_MAX-capped) and bounds
     * the wide buffer: each source byte decodes to at most one wchar, and each
     * segment gets one trailing NUL.
     */
    size_t cap = (size_t) nitems + (size_t) count;
    if (cap > SIZE_MAX / sizeof(wchar_t)) {
        free(list);
        return XNoMemory;
    }
    wchar_t *wstr = calloc(cap, sizeof(wchar_t));
    if (!wstr) {
        free(list);
        return XNoMemory;
    }

    size_t wlen = 0;
    unsigned long start = 0;
    int item = 0;
    for (unsigned long i = 0; i <= nitems; i++) {
        if (i < nitems && value[i] != '\0')
            continue;
        list[item++] = wstr + wlen;
        size_t segLen = (size_t) (i - start);
        if (segLen > 0 && value)
            wlen += decodeTextPropertySegment(value + start, segLen, utf8Source,
                                              wstr + wlen);
        wstr[wlen++] = 0;
        start = i + 1;
    }
    list[item] = NULL;
    *list_ret = list;
    *count_ret = count;
    return Success;
}

int XwcTextListToTextProperty(Display *dpy,
                              wchar_t **list,
                              int count,
                              XICCEncodingStyle style,
                              XTextProperty *text_prop)
{
    /* The previous WARN_UNIMPLEMENTED stub returned 0 = Success per Xlib
     * convention but left every text_prop field uninitialized. _XawTextWCToMB
     * then returned (char *) text_prop->value to libXaw as the multi-byte
     * buffer, which - having been read from the caller's stack - happened to
     * alias whatever Xt was holding in registers at the time. Athena's MultiSrc
     * then stored that bogus pointer into src->multi_src.string and freed it on
     * the next _XawMultiSave, crashing xfig's file dialog. Implement the real
     * surface here so the wide-char path round-trips through UTF-8.
     */
    if (!text_prop || count < 0)
        return XNoMemory;
    text_prop->value = NULL;
    text_prop->nitems = 0;
    text_prop->format = 8;
    Atom utf8 = dpy ? XInternAtom(dpy, "UTF8_STRING", False) : None;
    Atom textAtom = dpy ? XInternAtom(dpy, "TEXT", False) : None;
    Atom compoundText = dpy ? XInternAtom(dpy, "COMPOUND_TEXT", False) : None;
    Bool utf8Target = False;
    switch (style) {
    case XStringStyle:
    case XStdICCTextStyle:
        text_prop->encoding = XA_STRING;
        break;
    case XCompoundTextStyle:
        text_prop->encoding = compoundText;
        break;
    case XTextStyle:
        text_prop->encoding = textAtom;
        break;
    case XUTF8StringStyle:
    default:
        text_prop->encoding = utf8;
        utf8Target = True;
        break;
    }
    if (!list || count == 0) {
        text_prop->value = (unsigned char *) calloc(1, 1);
        if (!text_prop->value)
            return XNoMemory;
        return Success;
    }
    /* Worst case per wchar_t: 4 UTF-8 bytes. Add one NUL separator between
     * strings plus a trailing NUL terminator.
     */
    size_t bytes = 1;
    for (int i = 0; i < count; i++) {
        if (list[i]) {
            size_t wlen = wcslen(list[i]);
            if (wlen > SIZE_MAX / 4 - bytes - 1)
                return XNoMemory;
            bytes += wlen * 4;
        }
        bytes += 1;
    }
    unsigned char *buf = calloc(bytes, 1);
    if (!buf)
        return XNoMemory;
    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        if (list[i]) {
            for (const wchar_t *p = list[i]; *p; p++) {
                uint32_t cp = (uint32_t) *p;
                if (!utf8Target) {
                    if (cp <= 0xff)
                        buf[pos++] = (unsigned char) cp;
                    else
                        buf[pos++] = '?';
                } else if (cp < 0x80) {
                    buf[pos++] = (unsigned char) cp;
                } else if (cp < 0x800) {
                    buf[pos++] = (unsigned char) (0xc0 | (cp >> 6));
                    buf[pos++] = (unsigned char) (0x80 | (cp & 0x3f));
                } else if (cp < 0x10000) {
                    buf[pos++] = (unsigned char) (0xe0 | (cp >> 12));
                    buf[pos++] = (unsigned char) (0x80 | ((cp >> 6) & 0x3f));
                    buf[pos++] = (unsigned char) (0x80 | (cp & 0x3f));
                } else if (cp < 0x110000) {
                    buf[pos++] = (unsigned char) (0xf0 | (cp >> 18));
                    buf[pos++] = (unsigned char) (0x80 | ((cp >> 12) & 0x3f));
                    buf[pos++] = (unsigned char) (0x80 | ((cp >> 6) & 0x3f));
                    buf[pos++] = (unsigned char) (0x80 | (cp & 0x3f));
                }
            }
        }
        /* NUL-separate strings; final NUL is the C-string terminator that
         * XtFree / strlen-based callers expect.
         */
        if (i + 1 < count)
            buf[pos++] = 0;
    }
    buf[pos] = 0;
    text_prop->value = buf;
    text_prop->nitems = pos;
    return Success;
}

/* Converts color specifications in an array of XcmsColor structures from RGBi
 * format to CIEXYZ format.
 *
 * Return XcmsFailure if failed, XcmsSuccess if succeeded.
 */

Status XcmsRGBiToCIEXYZ(
    XcmsCCC ccc,
    XcmsColor *pXcmsColors_in_out /* pointer to XcmsColors to convert */,
    unsigned int nColors /* Number of colors */,
    Bool *pCompressed /* pointer to a bit array */)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XFontsOfFontSet(XFontSet font_set,
                    XFontStruct ***font_struct_list,
                    char ***font_name_list)
{
    CompatFontSet *set = GET_FONT_SET(font_set);
    if (!set)
        return 0;
    if (font_struct_list)
        *font_struct_list = set->fontStructList;
    if (font_name_list)
        *font_name_list = set->fontNameList;
    return 1;
}

/* Xrm database APIs live in src/xrm.c. */

Status XFetchName(register Display *dpy, Window w, char **name)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XGetIconName(register Display *dpy, Window w, char **icon_name)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XStoreBuffer(register Display *dpy,
                 _Xconst char *bytes,
                 int nbytes,
                 register int buffer)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XRebindKeysym(Display *dpy,
                  KeySym keysym,
                  KeySym *mlist,
                  int nm,
                  /* number of modifiers in mlist */ _Xconst unsigned char *str,
                  int nbytes)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XSetIconSizes(Display *dpy,
                  Window w,
                  /* typically, root */ XIconSize *list,
                  int count) /* number of items on the list */
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XWMGeometry(Display *dpy /* user's display connection */,
                int screen /* screen on which to do computation */,
                _Xconst char *user_geom /* user provided geometry spec */,
                _Xconst char *def_geom /* default geometry spec for window */,
                unsigned int bwidth /* border width */,
                XSizeHints *hints /* usually WM_NORMAL_HINTS */,
                int *x_return /* location of window */,
                int *y_return /* location of window */,
                int *width_return /* size of window */,
                int *height_return /* size of window */,
                int *gravity_return /* gravity of window */)
{
    int ux = 0, uy = 0;
    unsigned int uwidth = 0, uheight = 0;
    int dx = 0, dy = 0;
    unsigned int dwidth = 0, dheight = 0;
    int umask = XParseGeometry(user_geom, &ux, &uy, &uwidth, &uheight);
    int dmask = XParseGeometry(def_geom, &dx, &dy, &dwidth, &dheight);
    int rmask = umask;

    int baseWidth = 0;
    int baseHeight = 0;
    int minWidth = 0;
    int minHeight = 0;
    int widthInc = 1;
    int heightInc = 1;
    if (hints) {
        baseWidth = (hints->flags & PBaseSize)
                        ? hints->base_width
                        : ((hints->flags & PMinSize) ? hints->min_width : 0);
        baseHeight = (hints->flags & PBaseSize)
                         ? hints->base_height
                         : ((hints->flags & PMinSize) ? hints->min_height : 0);
        minWidth = (hints->flags & PMinSize) ? hints->min_width : baseWidth;
        minHeight = (hints->flags & PMinSize) ? hints->min_height : baseHeight;
        if ((hints->flags & PResizeInc) && hints->width_inc > 0)
            widthInc = hints->width_inc;
        if ((hints->flags & PResizeInc) && hints->height_inc > 0)
            heightInc = hints->height_inc;
    }

    /* All pixel math runs in int64_t and routes through satMulAdd so a UINT_MAX
     * geometry-units value times UINT_MAX width_inc cannot wrap before the
     * function clamps into the returned int.
     */
    int64_t widthUnits = (umask & WidthValue)
                             ? (int64_t) uwidth
                             : ((dmask & WidthValue) ? (int64_t) dwidth : 1);
    int64_t heightUnits = (umask & HeightValue)
                              ? (int64_t) uheight
                              : ((dmask & HeightValue) ? (int64_t) dheight : 1);
    int64_t width = satMulAdd(widthUnits, widthInc, baseWidth);
    int64_t height = satMulAdd(heightUnits, heightInc, baseHeight);
    if (width < minWidth)
        width = minWidth;
    if (height < minHeight)
        height = minHeight;
    if (hints && (hints->flags & PMaxSize)) {
        if (width > hints->max_width)
            width = hints->max_width;
        if (height > hints->max_height)
            height = hints->max_height;
    }
    int64_t border = 2 * (int64_t) bwidth;

    int64_t x = 0;
    if (umask & XValue) {
        x = (umask & XNegative)
                ? (int64_t) DisplayWidth(dpy, screen) + ux - width - border
                : ux;
    } else if (dmask & XValue) {
        if (dmask & XNegative) {
            x = (int64_t) DisplayWidth(dpy, screen) + dx - width - border;
            rmask |= XNegative;
        } else {
            x = dx;
        }
    }

    int64_t y = 0;
    if (umask & YValue) {
        y = (umask & YNegative)
                ? (int64_t) DisplayHeight(dpy, screen) + uy - height - border
                : uy;
    } else if (dmask & YValue) {
        if (dmask & YNegative) {
            y = (int64_t) DisplayHeight(dpy, screen) + dy - height - border;
            rmask |= YNegative;
        } else {
            y = dy;
        }
    }

    if (x_return)
        *x_return = clampInt64ToInt(x);
    if (y_return)
        *y_return = clampInt64ToInt(y);
    if (width_return)
        *width_return = clampInt64ToInt(width);
    if (height_return)
        *height_return = clampInt64ToInt(height);
    if (gravity_return) {
        switch (rmask & (XNegative | YNegative)) {
        case 0:
            *gravity_return = NorthWestGravity;
            break;
        case XNegative:
            *gravity_return = NorthEastGravity;
            break;
        case YNegative:
            *gravity_return = SouthWestGravity;
            break;
        default:
            *gravity_return = SouthEastGravity;
            break;
        }
    }
    return rmask;
}

int XGeometry(Display *dpy,
              int screen,
              _Xconst char *position,
              _Xconst char *default_position,
              unsigned int bwidth,
              unsigned int fwidth,
              unsigned int fheight,
              int xadder,
              int yadder,
              int *x_return,
              int *y_return,
              int *width_return,
              int *height_return)
{
    int x = 0, y = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    int mask = default_position
                   ? XParseGeometry(default_position, &x, &y, &width, &height)
                   : NoValue;
    int defaultMask = mask;
    unsigned int defaultWidth = width;
    unsigned int defaultHeight = height;

    int userX = 0, userY = 0;
    unsigned int userWidth = 0;
    unsigned int userHeight = 0;
    int userMask = position ? XParseGeometry(position, &userX, &userY,
                                             &userWidth, &userHeight)
                            : NoValue;
    if (userMask & WidthValue)
        width = userWidth;
    if (userMask & HeightValue)
        height = userHeight;
    if (userMask & XValue) {
        x = userX;
        mask = (mask & ~XNegative) | (userMask & XNegative) | XValue;
    }
    if (userMask & YValue) {
        y = userY;
        mask = (mask & ~YNegative) | (userMask & YNegative) | YValue;
    }

    /* Anchor dimensions feed the (-x, -y) corner math. Match upstream X.org
     * XGeometry: when the USER spec supplies a position, anchor with the merged
     * width (user override if any, else default). When the user spec has NO
     * position, the default-position branch anchors with the DEFAULT width even
     * if the user changed width. This matches xterm-style command-line geometry
     * handling. Routing through satMulAdd so a pathological spec cannot wrap
     * int64_t before the clamp.
     */
    unsigned int xAnchorWidth =
        (userMask & XValue) || !(defaultMask & WidthValue) ? width
                                                           : defaultWidth;
    unsigned int yAnchorHeight =
        (userMask & YValue) || !(defaultMask & HeightValue) ? height
                                                            : defaultHeight;
    int64_t xAnchorPixelWidth =
        satMulAdd((int64_t) xAnchorWidth, (int64_t) fwidth, (int64_t) xadder);
    int64_t yAnchorPixelHeight =
        satMulAdd((int64_t) yAnchorHeight, (int64_t) fheight, (int64_t) yadder);
    int64_t border = 2 * (int64_t) bwidth;
    if (x_return) {
        int64_t v = 0;
        if (mask & XValue) {
            v = (mask & XNegative) ? (int64_t) DisplayWidth(dpy, screen) -
                                         xAnchorPixelWidth - border + x
                                   : (int64_t) x;
        }
        *x_return = clampInt64ToInt(v);
    }
    if (y_return) {
        int64_t v = 0;
        if (mask & YValue) {
            v = (mask & YNegative) ? (int64_t) DisplayHeight(dpy, screen) -
                                         yAnchorPixelHeight - border + y
                                   : (int64_t) y;
        }
        *y_return = clampInt64ToInt(v);
    }
    if (width_return)
        *width_return = clampInt64ToInt((int64_t) width);
    if (height_return)
        *height_return = clampInt64ToInt((int64_t) height);
    return userMask;
}

Status XGetIconSizes(Display *dpy,
                     Window w /* typically, root */,
                     XIconSize **size_list /* RETURN */,
                     int *count /* RETURN number of items on the list */)
{
    (void) dpy;
    (void) w;
    if (size_list)
        *size_list = NULL;
    if (count)
        *count = 0;
    return 0;
}

Status XGetCommand(Display *dpy, Window w, char ***argvp, int *argcp)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *property = NULL;
    if (argvp)
        *argvp = NULL;
    if (argcp)
        *argcp = 0;
    if (!argvp || !argcp)
        return 0;
    if (XGetWindowProperty(dpy, w, XA_WM_COMMAND, 0, ((long) INT_MAX + 3) / 4,
                           False, XA_STRING, &actualType, &actualFormat,
                           &nitems, &bytesAfter, &property) != Success ||
        actualType != XA_STRING || actualFormat != 8 || bytesAfter != 0 ||
        !property) {
        if (property)
            XFree(property);
        return 0;
    }

    /* Count NUL-terminated strings inside the property. A malformed payload
     * that does not end in NUL would advance offset past nitems; cap the step
     * so the second pass cannot read past the buffer.
     */
    int argc = 0;
    unsigned long offset = 0;
    while (offset < nitems) {
        size_t len = strnlen((char *) property + offset, nitems - offset);
        argc++;
        unsigned long step =
            (unsigned long) len + (len < nitems - offset ? 1UL : 0UL);
        offset += step;
    }
    char **argv = Xcalloc((unsigned) argc + 1, sizeof(char *));
    if (!argv) {
        XFree(property);
        return 0;
    }
    offset = 0;
    for (int i = 0; i < argc; i++) {
        size_t len = strnlen((char *) property + offset, nitems - offset);
        argv[i] = Xmalloc((unsigned) len + 1);
        if (!argv[i]) {
            for (int j = 0; j < i; j++)
                XFree(argv[j]);
            XFree(argv);
            XFree(property);
            return 0;
        }
        memcpy(argv[i], property + offset, len);
        argv[i][len] = '\0';
        unsigned long step =
            (unsigned long) len + (len < nitems - offset ? 1UL : 0UL);
        offset += step;
    }
    XFree(property);
    *argvp = argv;
    *argcp = argc;
    return 1;
}

Status XGetTransientForHint(Display *dpy, Window w, Window *propWindow)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *property = NULL;
    if (propWindow)
        *propWindow = None;
    if (!propWindow)
        return 0;
    if (XGetWindowProperty(dpy, w, XA_WM_TRANSIENT_FOR, 0, 1, False, XA_WINDOW,
                           &actualType, &actualFormat, &nitems, &bytesAfter,
                           &property) != Success ||
        actualType != XA_WINDOW || actualFormat != 32 || nitems < 1 ||
        !property) {
        if (property)
            XFree(property);
        return 0;
    }
    *propWindow = ((Window *) property)[0];
    XFree(property);
    return 1;
}

Status XGetClassHint(Display *dpy, Window w, XClassHint *classhint) /* RETURN */
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *property = NULL;
    if (!classhint)
        return 0;
    classhint->res_name = NULL;
    classhint->res_class = NULL;
    if (XGetWindowProperty(dpy, w, XA_WM_CLASS, 0, ((long) INT_MAX + 3) / 4,
                           False, XA_STRING, &actualType, &actualFormat,
                           &nitems, &bytesAfter, &property) != Success ||
        actualType != XA_STRING || actualFormat != 8 || bytesAfter != 0 ||
        !property) {
        if (property)
            XFree(property);
        return 0;
    }
    size_t nameLen = strnlen((char *) property, nitems);
    if (nameLen >= nitems) {
        XFree(property);
        return 0;
    }
    size_t classOffset = nameLen + 1;
    size_t classLen =
        strnlen((char *) property + classOffset, nitems - classOffset);
    classhint->res_name = Xmalloc((unsigned) nameLen + 1);
    classhint->res_class = Xmalloc((unsigned) classLen + 1);
    if (!classhint->res_name || !classhint->res_class) {
        XFree(classhint->res_name);
        XFree(classhint->res_class);
        classhint->res_name = NULL;
        classhint->res_class = NULL;
        XFree(property);
        return 0;
    }
    memcpy(classhint->res_name, property, nameLen);
    classhint->res_name[nameLen] = '\0';
    memcpy(classhint->res_class, property + classOffset, classLen);
    classhint->res_class[classLen] = '\0';
    XFree(property);
    return 1;
}

void XwcFreeStringList(wchar_t **list)
{
    if (!list)
        return;
    /* XwcTextPropertyToTextList allocates a single contiguous wchar_t buffer
     * for list[0] (the first and only element on this shim's path) and a
     * NULL-terminated list array. Free both so the caller does not have to
     * track the allocation pair separately.
     */
    free(list[0]);
    free(list);
}

/* Per ICCCM section 6.4, the RGB_COLOR_MAP family of properties stores a list
 * of XStandardColormap entries serialized as ten CARD32 fields each in this
 * fixed order: visualid, killid, colormap, red_max, red_mult, green_max,
 * green_mult, blue_max, blue_mult, base_pixel. The C struct's field order is
 * different from the wire order, so the Get/Set helpers translate explicitly.
 */
#define XRGB_CMAP_FIELDS 10

void XSetRGBColormaps(Display *dpy,
                      Window w,
                      XStandardColormap *cmaps,
                      int count,
                      Atom property) /* XA_RGB_BEST_MAP, etc. */
{
    if (!dpy || !cmaps || count <= 0 || property == None)
        return;
    int nitems = count * XRGB_CMAP_FIELDS;
    long *data = malloc(sizeof(long) * (size_t) nitems);
    if (!data)
        return;
    for (int i = 0; i < count; i++) {
        XStandardColormap *c = &cmaps[i];
        long *p = &data[i * XRGB_CMAP_FIELDS];
        p[0] = (long) c->visualid;
        p[1] = (long) c->killid;
        p[2] = (long) c->colormap;
        p[3] = (long) c->red_max;
        p[4] = (long) c->red_mult;
        p[5] = (long) c->green_max;
        p[6] = (long) c->green_mult;
        p[7] = (long) c->blue_max;
        p[8] = (long) c->blue_mult;
        p[9] = (long) c->base_pixel;
    }
    XChangeProperty(dpy, w, property, XA_RGB_COLOR_MAP, 32, PropModeReplace,
                    (unsigned char *) data, nitems);
    free(data);
}

Status XGetWMProtocols(Display *dpy,
                       Window w,
                       Atom **protocols,
                       int *countReturn)
{
    if (!protocols || !countReturn)
        return 0;
    *protocols = NULL;
    *countReturn = 0;

    Atom property = XInternAtom(dpy, "WM_PROTOCOLS", True);
    if (property == None)
        return 0;

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, w, property, 0, LONG_MAX, False, XA_ATOM,
                           &actualType, &actualFormat, &nitems, &bytesAfter,
                           &data) != Success)
        return 0;
    if (actualType != XA_ATOM || actualFormat != 32 || bytesAfter != 0 ||
        nitems > INT_MAX) {
        XFree(data);
        return 0;
    }
    *protocols = (Atom *) data;
    *countReturn = (int) nitems;
    return 1;
}

int XWriteBitmapFile(Display *display,
                     _Xconst char *filename,
                     Pixmap bitmap,
                     unsigned int width,
                     unsigned int height,
                     int x_hot,
                     int y_hot)
{
    (void) display;
    if (!filename || width == 0 || height == 0)
        return BitmapOpenFailed;
    /* Overflow-safe scanline math: (width + 7) wraps before the divide if width
     * is close to UINT_MAX, so guard the add and the multiply separately.
     */
    if (width > UINT_MAX - 7u)
        return BitmapNoMemory;
    size_t bytesPerRow = (width + 7u) / 8u;
    if (bytesPerRow != 0 && (size_t) height > SIZE_MAX / bytesPerRow)
        return BitmapNoMemory;
    size_t total = bytesPerRow * (size_t) height;
    XImage *img = XGetImage(display, bitmap, 0, 0, width, height, 1, XYPixmap);
    if (!img)
        return BitmapOpenFailed;
    unsigned char *bits = calloc(total, 1);
    if (!bits) {
        img->f.destroy_image(img);
        return BitmapNoMemory;
    }
    for (unsigned int y = 0; y < height; y++) {
        for (unsigned int x = 0; x < width; x++) {
            if (XGetPixel(img, (int) x, (int) y))
                bits[y * bytesPerRow + (x >> 3)] |=
                    (unsigned char) (1u << (x & 7));
        }
    }
    img->f.destroy_image(img);
    FILE *f = fopen(filename, "w");
    if (!f) {
        free(bits);
        return BitmapOpenFailed;
    }
    /* X11 convention names the symbols based on the file's basename; the writer
     * pins a fixed "image" prefix to stay deterministic.
     */
    fprintf(f, "#define image_width %u\n#define image_height %u\n", width,
            height);
    if (x_hot >= 0 && y_hot >= 0) {
        fprintf(f, "#define image_x_hot %d\n#define image_y_hot %d\n", x_hot,
                y_hot);
    }
    fprintf(f, "static char image_bits[] = {\n");
    for (size_t i = 0; i < total; i++) {
        fprintf(f, "0x%02x%s", bits[i], (i + 1 == total) ? "" : ",");
        if ((i % 12) == 11)
            fputc('\n', f);
    }
    fprintf(f, "};\n");
    fclose(f);
    free(bits);
    return BitmapSuccess;
}

static int readBitmapHex(FILE *f, int *out)
{
    int c;
    /* skip whitespace and separators */
    while ((c = fgetc(f)) != EOF &&
           (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',' ||
            c == '{' || c == ';')) {
    }
    if (c == EOF)
        return 0;
    if (c == '0') {
        int x = fgetc(f);
        if (x != 'x' && x != 'X') {
            ungetc(x, f);
            *out = 0;
            return 1;
        }
        int value = 0;
        int digits = 0;
        while ((c = fgetc(f)) != EOF) {
            int v = -1;
            if (c >= '0' && c <= '9')
                v = c - '0';
            else if (c >= 'a' && c <= 'f')
                v = 10 + c - 'a';
            else if (c >= 'A' && c <= 'F')
                v = 10 + c - 'A';
            else
                break;
            value = (value << 4) | v;
            digits++;
            if (digits >= 2)
                break;
        }
        *out = value;
        return digits > 0;
    }
    return 0;
}

int XReadBitmapFile(Display *display,
                    Drawable d,
                    _Xconst char *filename,
                    unsigned int *width /* RETURNED */,
                    unsigned int *height /* RETURNED */,
                    Pixmap *pixmap /* RETURNED */,
                    int *x_hot /* RETURNED */,
                    int *y_hot /* RETURNED */)
{
    if (!filename || !width || !height || !pixmap)
        return BitmapOpenFailed;
    FILE *f = fopen(filename, "r");
    if (!f)
        return BitmapOpenFailed;
    unsigned int w = 0, h = 0;
    int xh = -1, yh = -1;
    char line[512];
    long bitsOffset = -1;
    /* Tolerate both prefixed identifiers (foo_width) and bare identifiers
     * (width); match the trailing keyword instead of assuming an underscore is
     * present.
     */
    while (fgets(line, sizeof(line), f)) {
        unsigned int v;
        char *p;
        if ((p = strstr(line, "width")) && sscanf(p, "width %u", &v) == 1) {
            w = v;
        } else if ((p = strstr(line, "height")) &&
                   sscanf(p, "height %u", &v) == 1) {
            h = v;
        } else if ((p = strstr(line, "x_hot")) &&
                   sscanf(p, "x_hot %u", &v) == 1) {
            xh = (int) v;
        } else if ((p = strstr(line, "y_hot")) &&
                   sscanf(p, "y_hot %u", &v) == 1) {
            yh = (int) v;
        } else if (strstr(line, "_bits[]") || strstr(line, "bits[]")) {
            char *brace = strchr(line, '{');
            if (brace) {
                bitsOffset =
                    ftell(f) - (long) strlen(line) + (long) (brace - line + 1);
                fseek(f, bitsOffset, SEEK_SET);
                break;
            }
            bitsOffset = ftell(f);
            break;
        }
    }
    if (bitsOffset < 0 || w == 0 || h == 0) {
        fclose(f);
        return BitmapFileInvalid;
    }
    if (w > UINT_MAX - 7u) {
        fclose(f);
        return BitmapNoMemory;
    }
    size_t bytesPerRow = (w + 7u) / 8u;
    if (bytesPerRow != 0 && (size_t) h > SIZE_MAX / bytesPerRow) {
        fclose(f);
        return BitmapNoMemory;
    }
    size_t total = bytesPerRow * (size_t) h;
    unsigned char *bits = calloc(total, 1);
    if (!bits) {
        fclose(f);
        return BitmapNoMemory;
    }
    for (size_t i = 0; i < total; i++) {
        int byte = 0;
        if (!readBitmapHex(f, &byte))
            break;
        bits[i] = (unsigned char) byte;
    }
    fclose(f);
    Pixmap pm = XCreateBitmapFromData(display, d, (char *) bits, w, h);
    free(bits);
    if (pm == None)
        return BitmapNoMemory;
    *width = w;
    *height = h;
    *pixmap = pm;
    if (x_hot)
        *x_hot = xh;
    if (y_hot)
        *y_hot = yh;
    return BitmapSuccess;
}

int XReadBitmapFileData(_Xconst char *filename,
                        unsigned int *width,
                        unsigned int *height,
                        unsigned char **data,
                        int *x_hot,
                        int *y_hot)
{
    if (!filename || !width || !height || !data)
        return BitmapOpenFailed;
    FILE *f = fopen(filename, "r");
    if (!f)
        return BitmapOpenFailed;
    unsigned int w = 0, h = 0;
    int xh = -1, yh = -1;
    char line[512];
    long bitsOffset = -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned int v;
        char *p;
        if ((p = strstr(line, "width")) && sscanf(p, "width %u", &v) == 1) {
            w = v;
        } else if ((p = strstr(line, "height")) &&
                   sscanf(p, "height %u", &v) == 1) {
            h = v;
        } else if ((p = strstr(line, "x_hot")) &&
                   sscanf(p, "x_hot %u", &v) == 1) {
            xh = (int) v;
        } else if ((p = strstr(line, "y_hot")) &&
                   sscanf(p, "y_hot %u", &v) == 1) {
            yh = (int) v;
        } else if (strstr(line, "_bits[]") || strstr(line, "bits[]")) {
            char *brace = strchr(line, '{');
            if (brace) {
                bitsOffset =
                    ftell(f) - (long) strlen(line) + (long) (brace - line + 1);
                fseek(f, bitsOffset, SEEK_SET);
                break;
            }
            bitsOffset = ftell(f);
            break;
        }
    }
    if (bitsOffset < 0 || w == 0 || h == 0) {
        fclose(f);
        return BitmapFileInvalid;
    }
    if (w > UINT_MAX - 7u) {
        fclose(f);
        return BitmapNoMemory;
    }
    size_t bytesPerRow = (w + 7u) / 8u;
    if (bytesPerRow != 0 && (size_t) h > SIZE_MAX / bytesPerRow) {
        fclose(f);
        return BitmapNoMemory;
    }
    size_t total = bytesPerRow * (size_t) h;
    unsigned char *bits = calloc(total, 1);
    if (!bits) {
        fclose(f);
        return BitmapNoMemory;
    }
    size_t read = 0;
    for (size_t i = 0; i < total; i++) {
        int byte = 0;
        if (!readBitmapHex(f, &byte))
            break;
        bits[i] = (unsigned char) byte;
        read = i + 1;
    }
    fclose(f);
    if (read < total) {
        /* Short data: the file declared an X-by-Y bitmap but ran out of hex
         * bytes before filling it. Reporting BitmapSuccess here would leave the
         * caller drawing uninitialized regions.
         */
        free(bits);
        return BitmapFileInvalid;
    }
    *width = w;
    *height = h;
    *data = bits;
    if (x_hot)
        *x_hot = xh;
    if (y_hot)
        *y_hot = yh;
    return BitmapSuccess;
}

Status XTextPropertyToStringList(XTextProperty *tp,
                                 char ***list_return,
                                 int *count_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XGetRGBColormaps(Display *dpy,
                        Window w,
                        XStandardColormap **stdcmap /* RETURN */,
                        int *count /* RETURN */,
                        Atom property /* XA_RGB_BEST_MAP, etc. */)
{
    if (!dpy || !stdcmap || !count || property == None)
        return 0;
    *stdcmap = NULL;
    *count = 0;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *raw = NULL;
    if (XGetWindowProperty(dpy, w, property, 0, LONG_MAX, False,
                           XA_RGB_COLOR_MAP, &actual_type, &actual_format,
                           &nitems, &bytes_after, &raw) != Success)
        return 0;
    if (actual_type != XA_RGB_COLOR_MAP || actual_format != 32 || nitems == 0 ||
        nitems % XRGB_CMAP_FIELDS != 0) {
        XFree(raw);
        return 0;
    }
    int n = (int) (nitems / XRGB_CMAP_FIELDS);
    XStandardColormap *arr = calloc((size_t) n, sizeof(XStandardColormap));
    if (!arr) {
        XFree(raw);
        return 0;
    }
    long *src = (long *) raw;
    for (int i = 0; i < n; i++) {
        XStandardColormap *c = &arr[i];
        long *p = &src[i * XRGB_CMAP_FIELDS];
        c->visualid = (VisualID) p[0];
        c->killid = (XID) p[1];
        c->colormap = (Colormap) p[2];
        c->red_max = (unsigned long) p[3];
        c->red_mult = (unsigned long) p[4];
        c->green_max = (unsigned long) p[5];
        c->green_mult = (unsigned long) p[6];
        c->blue_max = (unsigned long) p[7];
        c->blue_mult = (unsigned long) p[8];
        c->base_pixel = (unsigned long) p[9];
    }
    XFree(raw);
    *stdcmap = arr;
    *count = n;
    return 1;
}

int XStoreBytes(register Display *dpy, _Xconst char *bytes, int nbytes)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XGetWMName(Display *dpy, Window w, XTextProperty *tp)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XGetWMIconName(Display *dpy, Window w, XTextProperty *tp)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XGetWMClientMachine(Display *dpy, Window w, XTextProperty *tp)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Atom *XListProperties(register Display *dpy,
                      Window window,
                      int *n_props) /* RETURN */
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

char *XScreenResourceString(Screen *screen)
{
    if (!screen || !screen->display)
        return NULL;
    return screen->display->xdefaults;
}


char *XFetchBuffer(register Display *dpy, int *nbytes, register int buffer)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

char *XFetchBytes(register Display *dpy, int *nbytes)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

char *XResourceManagerString(Display *dpy)
{
    return dpy ? dpy->xdefaults : NULL;
}

Colormap *XListInstalledColormaps(register Display *dpy,
                                  Window win,
                                  int *n) /* RETURN */
{
    WARN_UNIMPLEMENTED;
    return 0;
}


XTimeCoord *XGetMotionEvents(register Display *dpy,
                             Window w,
                             Time start,
                             Time stop,
                             int *nEvents) /* RETURN */
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

XStandardColormap *XAllocStandardColormap(void)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

XIconSize *XAllocIconSize(void)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

static int _XSyncFunction(register Display *dpy)
{
    XSync(dpy, 0);
    return 0;
}

int (*XSynchronize(Display *dpy, int onoff))(Display *)
{
    int (*temp)(Display *);
    int (*func)(Display *) = NULL;

    if (onoff)
        func = _XSyncFunction;

    LockDisplay(dpy);
    if (dpy->flags & XlibDisplayPrivSync) {
        temp = dpy->savedsynchandler;
        dpy->savedsynchandler = func;
    } else {
        temp = dpy->synchandler;
        dpy->synchandler = func;
    }
    UnlockDisplay(dpy);
    return (temp);
}

int (*XSetAfterFunction(Display *dpy, int (*func)(Display *)))(Display *)
{
    int (*temp)(Display *);

    LockDisplay(dpy);
    if (dpy->flags & XlibDisplayPrivSync) {
        temp = dpy->savedsynchandler;
        dpy->savedsynchandler = func;
    } else {
        temp = dpy->synchandler;
        dpy->synchandler = func;
    }
    UnlockDisplay(dpy);
    return (temp);
}

int XGetErrorDatabaseText(Display *dpy,
                          register _Xconst char *name,
                          register _Xconst char *type,
                          _Xconst char *defaultp,
                          char *buffer,
                          int nbytes)
{
    (void) dpy;
    (void) name;
    (void) type;
    if (buffer && nbytes > 0)
        snprintf(buffer, (size_t) nbytes, "%s", defaultp ? defaultp : "");
    return 0;
}

Status XcmsQueryBlack(XcmsCCC ccc,
                      XcmsColorFormat target_format,
                      XcmsColor *pColor_ret)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

XcmsCCC XcmsDefaultCCC(Display *dpy, int screenNumber)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

void XcmsFreeCCC(XcmsCCC ccc)
{
    WARN_UNIMPLEMENTED;
}

Status XcmsTekHVCQueryMinV(XcmsCCC ccc,
                           XcmsFloat hue,
                           XcmsFloat chroma,
                           XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsAllocColor(Display *dpy,
                      Colormap colormap,
                      XcmsColor *pXcmsColor_in_out,
                      XcmsColorFormat result_format)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

XcmsColorFormat XcmsFormatOfPrefix(char *prefix)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsQueryBlue(XcmsCCC ccc,
                     XcmsColorFormat target_format,
                     XcmsColor *pColor_ret)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELabQueryMaxLC(XcmsCCC ccc,
                            XcmsFloat hue_angle,
                            XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsTekHVCQueryMaxV(XcmsCCC ccc,
                           XcmsFloat hue,
                           XcmsFloat chroma,
                           XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsQueryWhite(XcmsCCC ccc,
                      XcmsColorFormat target_format,
                      XcmsColor *pColor_ret)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELabToCIEXYZ(XcmsCCC ccc,
                          XcmsColor *pLab_WhitePt,
                          XcmsColor *pColors_in_out,
                          unsigned int nColors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsSetWhitePoint(XcmsCCC ccc, XcmsColor *pColor)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELuvQueryMaxC(XcmsCCC ccc,
                           XcmsFloat hue_angle,
                           XcmsFloat L_star,
                           XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELuvQueryMaxL(XcmsCCC ccc,
                           XcmsFloat hue_angle,
                           XcmsFloat chroma,
                           XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsTekHVCQueryMaxVSamples(XcmsCCC ccc,
                                  XcmsFloat hue,
                                  XcmsColor *pColor_in_out,
                                  unsigned int nSamples)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsAddColorSpace(XcmsColorSpace *pCS)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsQueryColors(Display *dpy,
                       Colormap colormap,
                       XcmsColor *pXcmsColors_in_out,
                       unsigned int nColors,
                       XcmsColorFormat result_format)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsTekHVCQueryMaxC(XcmsCCC ccc,
                           XcmsFloat hue,
                           XcmsFloat value,
                           XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELuvQueryMinL(XcmsCCC ccc,
                           XcmsFloat hue_angle,
                           XcmsFloat chroma,
                           XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

XcmsCCC XcmsCCCOfColormap(Display *dpy, Colormap cmap)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

Status XcmsQueryColor(Display *dpy,
                      Colormap colormap,
                      XcmsColor *pXcmsColor_in_out,
                      XcmsColorFormat result_format)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELuvQueryMaxLC(XcmsCCC ccc,
                            XcmsFloat hue_angle,
                            XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsAllocNamedColor(Display *dpy,
                           Colormap cmap,
                           _Xconst char *colorname,
                           XcmsColor *pColor_scrn_return,
                           XcmsColor *pColor_exact_return,
                           XcmsColorFormat result_format)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

/* Convert a plain C string into an XTextProperty. The encoding picks
 * UTF8_STRING when isUtf8 is True, XA_STRING otherwise.
 *
 * Returns True on success and stores a heap-allocated value the caller must
 * free with Xfree(tp.value).
 */
static Bool textPropertyFromString(Display *dpy,
                                   _Xconst char *str,
                                   Bool isUtf8,
                                   XTextProperty *tp)
{
    if (!str)
        return False;
    size_t len = strlen(str);
    char *copy = Xmalloc(len + 1);
    if (!copy)
        return False;
    memcpy(copy, str, len + 1);
    tp->value = (unsigned char *) copy;
    tp->nitems = (unsigned long) len;
    tp->format = 8;
    tp->encoding = isUtf8 ? XInternAtom(dpy, "UTF8_STRING", False) : XA_STRING;
    return True;
}

static void setWMPropertiesCommon(Display *dpy,
                                  Window w,
                                  _Xconst char *windowName,
                                  _Xconst char *iconName,
                                  char **argv,
                                  int argc,
                                  XSizeHints *sizeHints,
                                  XWMHints *wmHints,
                                  XClassHint *classHints,
                                  Bool isUtf8)
{
    XTextProperty winProp;
    XTextProperty iconProp;
    Bool haveWin = False;
    Bool haveIcon = False;
    if (windowName)
        haveWin = textPropertyFromString(dpy, windowName, isUtf8, &winProp);
    if (iconName)
        haveIcon = textPropertyFromString(dpy, iconName, isUtf8, &iconProp);
    XSetWMProperties(dpy, w, haveWin ? &winProp : NULL,
                     haveIcon ? &iconProp : NULL, argv, argc, sizeHints,
                     wmHints, classHints);
    if (haveWin)
        Xfree(winProp.value);
    if (haveIcon)
        Xfree(iconProp.value);
}

void XmbSetWMProperties(Display *dpy,
                        Window w,
                        _Xconst char *windowName,
                        _Xconst char *iconName,
                        char **argv,
                        int argc,
                        XSizeHints *sizeHints,
                        XWMHints *wmHints,
                        XClassHint *classHints)
{
    setWMPropertiesCommon(dpy, w, windowName, iconName, argv, argc, sizeHints,
                          wmHints, classHints, False);
}

void Xutf8SetWMProperties(Display *dpy,
                          Window w,
                          _Xconst char *windowName,
                          _Xconst char *iconName,
                          char **argv,
                          int argc,
                          XSizeHints *sizeHints,
                          XWMHints *wmHints,
                          XClassHint *classHints)
{
    setWMPropertiesCommon(dpy, w, windowName, iconName, argv, argc, sizeHints,
                          wmHints, classHints, True);
}

char *XGetOMValues(XOM om, ...)
{
    (void) om;
    return NULL;
}

XOM XOMOfOC(XOC oc)
{
    (void) oc;
    return NULL;
}

char *XSetOCValues(XOC oc, ...)
{
    (void) oc;
    return NULL;
}

char *XGetOCValues(XOC oc, ...)
{
    (void) oc;
    return NULL;
}

Status XcmsQueryGreen(XcmsCCC ccc,
                      XcmsColorFormat target_format,
                      XcmsColor *pColor_ret)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsStoreColors(Display *dpy,
                       Colormap colormap,
                       XcmsColor *pColors_in,
                       unsigned int nColors,
                       Bool *pCompressed)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsLookupColor(Display *dpy,
                       Colormap cmap,
                       _Xconst char *colorname,
                       XcmsColor *pColor_exact_return,
                       XcmsColor *pColor_scrn_return,
                       XcmsColorFormat result_format)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsTekHVCQueryMaxVC(XcmsCCC ccc,
                            XcmsFloat hue,
                            XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsStoreColor(Display *dpy, Colormap colormap, XcmsColor *pColor_in)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELabQueryMaxC(XcmsCCC ccc,
                           XcmsFloat hue_angle,
                           XcmsFloat L_star,
                           XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsQueryRed(XcmsCCC ccc,
                    XcmsColorFormat target_format,
                    XcmsColor *pColor_ret)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsAddFunctionSet(XcmsFunctionSet *pNewFS)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELuvWhiteShiftColors(XcmsCCC ccc,
                                  XcmsColor *pWhitePtFrom,
                                  XcmsColor *pWhitePtTo,
                                  XcmsColorFormat destSpecFmt,
                                  XcmsColor *pColors_in_out,
                                  unsigned int nColors,
                                  Bool *pCompressed)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELabQueryMaxL(XcmsCCC ccc,
                           XcmsFloat hue_angle,
                           XcmsFloat chroma,
                           XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELabQueryMinL(XcmsCCC ccc,
                           XcmsFloat hue_angle,
                           XcmsFloat chroma,
                           XcmsColor *pColor_return)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIEXYZToCIEuvY(XcmsCCC ccc,
                          XcmsColor *puvY_WhitePt,
                          XcmsColor *pColors_in_out,
                          unsigned int nColors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XRotateBuffers(register Display *dpy, int rotate)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELabWhiteShiftColors(XcmsCCC ccc,
                                  XcmsColor *pWhitePtFrom,
                                  XcmsColor *pWhitePtTo,
                                  XcmsColorFormat destSpecFmt,
                                  XcmsColor *pColors_in_out,
                                  unsigned int nColors,
                                  Bool *pCompressed)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIEXYZToCIELab(XcmsCCC ccc,
                          XcmsColor *pLab_WhitePt,
                          XcmsColor *pColors_in_out,
                          unsigned int nColors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsTekHVCWhiteShiftColors(XcmsCCC ccc,
                                  XcmsColor *pWhitePtFrom,
                                  XcmsColor *pWhitePtTo,
                                  XcmsColorFormat destSpecFmt,
                                  XcmsColor *pColors_in_out,
                                  unsigned int nColors,
                                  Bool *pCompressed)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIEuvYToCIEXYZ(XcmsCCC ccc,
                          XcmsColor *puvY_WhitePt,
                          XcmsColor *pColors_in_out,
                          unsigned int nColors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIEXYZToCIExyY(XcmsCCC ccc,
                          XcmsColor *pxyY_WhitePt,
                          XcmsColor *pColors_in_out,
                          unsigned int nColors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

XModifierKeymap *XNewModifiermap(int keyspermodifier)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

wchar_t *XwcResetIC(XIC ic)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

const char *XDefaultString(void)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

char *XcmsPrefixOfFormat(XcmsColorFormat id)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

Display *XcmsDisplayOfCCC(XcmsCCC ccc)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

Visual *XcmsVisualOfCCC(XcmsCCC ccc)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

XcmsColor *XcmsScreenWhitePointOfCCC(XcmsCCC ccc)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

XcmsColor *XcmsClientWhitePointOfCCC(XcmsCCC ccc)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

char *XBaseFontNameListOfFontSet(XFontSet font_set)
{
    CompatFontSet *set = GET_FONT_SET(font_set);
    return set ? set->baseName : NULL;
}

char *XLocaleOfFontSet(XFontSet font_set)
{
    CompatFontSet *set = GET_FONT_SET(font_set);
    return set ? set->locale : NULL;
}

XFontSetExtents *XExtentsOfFontSet(XFontSet font_set)
{
    CompatFontSet *set = GET_FONT_SET(font_set);
    return set ? &set->extents : NULL;
}

int XwcTextEscapement(XFontSet font_set, _Xconst wchar_t *text, int text_len)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XwcTextExtents(XFontSet font_set,
                   _Xconst wchar_t *text,
                   int text_len,
                   XRectangle *overall_ink_extents,
                   XRectangle *overall_logical_extents)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XwcTextPerCharExtents(XFontSet font_set,
                             _Xconst wchar_t *text,
                             int text_len,
                             XRectangle *ink_extents_buffer,
                             XRectangle *logical_extents_buffer,
                             int buffer_size,
                             int *num_chars,
                             XRectangle *max_ink_extents,
                             XRectangle *max_logical_extents)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XmbTextExtents(XFontSet font_set,
                   _Xconst char *text,
                   int text_len,
                   XRectangle *overall_ink_extents,
                   XRectangle *overall_logical_extents)
{
    CompatFontSet *set = GET_FONT_SET(font_set);
    int width = set && set->font && text && text_len > 0
                    ? XTextWidth(set->font, text, text_len)
                    : 0;
    int height = set && set->font ? set->font->ascent + set->font->descent : 0;
    LOG("XmbTextExtents: set=%p font=%p text_len=%d (preview='%.20s') "
        "-> w=%d h=%d\n",
        (void *) set, set ? (void *) set->font : NULL, text_len,
        text && text_len > 0 ? text : "(empty)", width, height);
    int ascent = set && set->font ? set->font->ascent : 0;
    if (overall_ink_extents) {
        overall_ink_extents->x = 0;
        overall_ink_extents->y = (short) -ascent;
        overall_ink_extents->width = (unsigned short) width;
        overall_ink_extents->height = (unsigned short) height;
    }
    if (overall_logical_extents) {
        overall_logical_extents->x = 0;
        overall_logical_extents->y = (short) -ascent;
        overall_logical_extents->width = (unsigned short) width;
        overall_logical_extents->height = (unsigned short) height;
    }
    return width;
}

Status XmbTextPerCharExtents(XFontSet font_set,
                             _Xconst char *text,
                             int text_len,
                             XRectangle *ink_extents_buffer,
                             XRectangle *logical_extents_buffer,
                             int buffer_size,
                             int *num_chars,
                             XRectangle *max_ink_extents,
                             XRectangle *max_logical_extents)
{
    CompatFontSet *set = GET_FONT_SET(font_set);
    /* Clamp both sides at zero: a negative buffer_size would otherwise
     * propagate into count and report a negative num_chars.
     */
    int count = text_len > 0 ? text_len : 0;
    int cap = buffer_size > 0 ? buffer_size : 0;
    if (count > cap)
        count = cap;
    int charWidth = set && set->font ? set->font->max_bounds.width : 8;
    int height = set && set->font ? set->font->ascent + set->font->descent : 0;
    int ascent = set && set->font ? set->font->ascent : 0;
    for (int i = 0; i < count; i++) {
        XRectangle rect = {
            .x = (short) (i * charWidth),
            .y = (short) -ascent,
            .width = (unsigned short) charWidth,
            .height = (unsigned short) height,
        };
        if (ink_extents_buffer)
            ink_extents_buffer[i] = rect;
        if (logical_extents_buffer)
            logical_extents_buffer[i] = rect;
    }
    if (num_chars)
        *num_chars = count;
    if (max_ink_extents) {
        max_ink_extents->x = 0;
        max_ink_extents->y = (short) -ascent;
        max_ink_extents->width = (unsigned short) charWidth;
        max_ink_extents->height = (unsigned short) height;
    }
    if (max_logical_extents) {
        max_logical_extents->x = 0;
        max_logical_extents->y = (short) -ascent;
        max_logical_extents->width = (unsigned short) charWidth;
        max_logical_extents->height = (unsigned short) height;
    }
    return count == text_len ? Success : 0;
}

int Xutf8TextExtents(XFontSet font_set,
                     _Xconst char *text,
                     int text_len,
                     XRectangle *overall_ink_extents,
                     XRectangle *overall_logical_extents)
{
    return XmbTextExtents(font_set, text, text_len, overall_ink_extents,
                          overall_logical_extents);
}

Status Xutf8TextPerCharExtents(XFontSet font_set,
                               _Xconst char *text,
                               int text_len,
                               XRectangle *ink_extents_buffer,
                               XRectangle *logical_extents_buffer,
                               int buffer_size,
                               int *num_chars,
                               XRectangle *max_ink_extents,
                               XRectangle *max_logical_extents)
{
    return XmbTextPerCharExtents(font_set, text, text_len, ink_extents_buffer,
                                 logical_extents_buffer, buffer_size, num_chars,
                                 max_ink_extents, max_logical_extents);
}

Status XcmsCIEuvYToCIELuv(XcmsCCC ccc,
                          XcmsColor *pLuv_WhitePt,
                          XcmsColor *pColors_in_out,
                          unsigned int nColors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIExyYToCIEXYZ(XcmsCCC ccc,
                          XcmsColor *pxyY_WhitePt,
                          XcmsColor *pColors_in_out,
                          unsigned int nColors)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

XcmsColorSpace XcmsCIELabColorSpace = {};
XcmsColorSpace XcmsCIEXYZColorSpace = {};

XModifierKeymap *XInsertModifiermapEntry(XModifierKeymap *map,
                                         KeyCode keycode,
                                         int modifier)
{
    return NULL;
}

XModifierKeymap *XDeleteModifiermapEntry(XModifierKeymap *map,
                                         KeyCode keycode,
                                         int modifier)
{
    return NULL;
}

/* Internal-connection plumbing used by libXt and IM clients to fold extra
 * descriptors into the Xlib event loop. The SDL-backed display owns no
 * auxiliary sockets, so there is nothing to watch and nothing to drain: report
 * an empty fd set and acknowledge watcher registration without recording the
 * callback.
 */
Status XInternalConnectionNumbers(Display *dpy,
                                  int **fd_return,
                                  int *count_return)
{
    (void) dpy;
    if (fd_return)
        *fd_return = NULL;
    if (count_return)
        *count_return = 0;
    return 1;
}

void XProcessInternalConnection(Display *dpy, int fd)
{
    (void) dpy;
    (void) fd;
}

Status XAddConnectionWatch(Display *dpy,
                           XConnectionWatchProc proc,
                           XPointer data)
{
    (void) dpy;
    (void) proc;
    (void) data;
    return 1;
}

void XRemoveConnectionWatch(Display *dpy,
                            XConnectionWatchProc proc,
                            XPointer data)
{
    (void) dpy;
    (void) proc;
    (void) data;
}
