#include "X11/Xlibint.h"
#include "X11/xcms/Xcmsint.h"
#include "X11/Xresource.h"
#include "X11/Xutil.h"
#include "X11/Xatom.h"
#include "X11/XKBlib.h"
#include "X11/extensions/XKB.h"
#include "X11/Xlocale.h"
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "util.h"
#include "gc.h"
#include "display.h"
#include "errors.h"
#include "events.h"
#include "font.h"
#include "input-method.h"
#include "window.h"
#include "atoms.h"

static void fillTextExtents(XFontStruct *fs,
                            int width,
                            int *dir,
                            int *font_ascent,
                            int *font_descent,
                            XCharStruct *overall);
static void freeQueriedFontStruct(XFontStruct *fs);

/*
 * Compatibility stub triage:
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

Status XAllocColorCells(
    register Display *dpy,
    Colormap cmap,
    Bool contig,
    unsigned long *masks,
    /* LISTofCARD32 */ /* RETURN */ unsigned int nplanes,
    /* CARD16 */ unsigned long *pixels,
    /* LISTofCARD32 */ /* RETURN */ unsigned int ncolors) /* CARD16 */
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XQueryColor(register Display *dpy, Colormap cmap, XColor *def) /* RETURN */
{
    /* Same pixel-back-to-RGB decoding as XQueryColors. With TrueColor the
     * pixel encodes the RGB directly, so no per-colormap cache is needed. */
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
    WARN_UNIMPLEMENTED;
}

/* Translate a single keysym to UTF-8. Covers ASCII, Latin-1, and the
 * X11 0x01000000 unicode keysym prefix. Returns the byte count written
 * (0 for keysyms with no associated character). */
static int keysym_to_utf8(KeySym keysym, char *buffer, int nbytes)
{
    unsigned long codepoint = 0;
    if ((keysym >= 0x0020 && keysym <= 0x007e) ||
        (keysym >= 0x00a0 && keysym <= 0x00ff)) {
        codepoint = keysym;
    } else if ((keysym & 0xff000000UL) == 0x01000000UL) {
        codepoint = keysym & 0x00ffffffUL;
    } else {
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
        keysym = XkbKeycodeToKeysym(ev->display, ev->keycode, 0, 0);
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
    (void) ic;
    return do_locale_lookup(ev, buffer, nbytes, keysym, status);
}
/* Xutf8LookupString lives in src/input-method.c with the IM-driven path. */

Bool XSupportsLocale(void)
{
    /* Quiet probe: GTK and Qt call this on startup. Returning True keeps
     * them happy; our XmbDrawString family routes through XDrawString. */
    return True;
}

int XmbTextPropertyToTextList(Display *dpy,
                              const XTextProperty *text_prop,
                              char ***list_ret,
                              int *count_ret)
{
    WARN_UNIMPLEMENTED;
    return 0;
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
    if (supported) {
        *supported = True;
    }
    return True;
}

/* Synthetic XKB extension codes. We do not implement a real XKB protocol,
 * but Motif / GTK / xfreerdp probe XkbUseExtension and XkbQueryExtension
 * before any keyboard work. Reporting "available" with consistent base
 * codes from both probes keeps the downstream dispatch logic happy; the
 * actual XKB requests are still WARN_UNIMPLEMENTED stubs.
 *
 * The opcode/event/error bases are chosen above the core-protocol ranges
 * so a caller doing event.type - eventBase math doesn't collide with X
 * core events (0..34). */
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

/*
 * _XGetHostname - similar to gethostname but allows special processing.
 */
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

/*
 * XSetWMProperties sets the following properties:
 *	WM_NAME		  type: TEXT		format: varies?
 *	WM_ICON_NAME	  type: TEXT		format: varies?
 *	WM_HINTS	  type: WM_HINTS	format: 32
 *	WM_COMMAND	  type: TEXT		format: varies?
 *	WM_CLIENT_MACHINE type: TEXT		format: varies?
 *	WM_NORMAL_HINTS	  type: WM_SIZE_HINTS 	format: 32
 *	WM_CLASS	  type: STRING/STRING	format: 8
 *	WM_LOCALE_NAME	  type: STRING		format: 8
 */

void XSetWMProperties(
    Display *dpy,
    Window w,                  /* window to decorate */
    XTextProperty *windowName, /* name of application */
    XTextProperty *iconName,   /* name string for icon */
    char **argv,               /* command line */
    int argc,                  /* size of command line */
    XSizeHints *sizeHints,     /* size hints for window in its normal state */
    XWMHints *wmHints,         /* miscellaneous window manager hints */
    XClassHint *classHints)    /* resource name and class */
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
        /*
         * for UNIX and other operating systems which use nul-terminated
         * arrays of STRINGs.
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
                /*
                 * UNIX uses /dir/subdir/.../basename; other operating
                 * systems will have to change this.
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
    if (buffer && nbytes > 0) {
        snprintf(buffer, (size_t) nbytes, "X error %d", code);
    }
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

static void moveChildIndexToTop(Array *children, size_t index)
{
    if (index + 1 >= children->length)
        return;
    void *child = children->array[index];
    memmove(&children->array[index], &children->array[index + 1],
            sizeof(void *) * (children->length - index - 1));
    children->array[children->length - 1] = child;
}

static void moveChildIndexToBottom(Array *children, size_t index)
{
    if (index == 0)
        return;
    void *child = children->array[index];
    memmove(&children->array[1], &children->array[0], sizeof(void *) * index);
    children->array[0] = child;
}

/* RaiseLowest: raise the lowest mapped child that is occluded by another
 * mapped sibling. If no such child exists, circulation is a no-op. */
static void circulateChildrenUp(Window w)
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
                moveChildIndexToTop(childArray, i);
                return;
            }
        }
    }
}

/* LowerHighest: lower the highest mapped child that occludes another mapped
 * sibling. If none match, the stacking order must stay unchanged. */
static void circulateChildrenDown(Window w)
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
                moveChildIndexToBottom(childArray, i);
                return;
            }
        }
    }
}

int XCirculateSubwindowsUp(register Display *dpy, Window w)
{
    SET_X_SERVER_REQUEST(dpy, X_CirculateWindow);
    TYPE_CHECK(w, WINDOW, dpy, 0);
    circulateChildrenUp(w);
    return 1;
}

int XCirculateSubwindows(register Display *dpy, Window w, int direction)
{
    SET_X_SERVER_REQUEST(dpy, X_CirculateWindow);
    TYPE_CHECK(w, WINDOW, dpy, 0);
    switch (direction) {
    case RaiseLowest:
        circulateChildrenUp(w);
        return 1;
    case LowerHighest:
        circulateChildrenDown(w);
        return 1;
    default:
        handleError(0, dpy, None, 0, BadValue, 0);
        return 0;
    }
}

int XInstallColormap(register Display *dpy, Colormap cmap)
{
    /* The in-process model has a single TrueColor visual; "installing" a
     * colormap is a no-op success. Real Xlib returns 0 on failure only. */
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
    /* TrueColor has only read-only cells, so XStoreColors against any
     * pixels we hand back would be a no-op and produce visual artifacts
     * instead of dynamic color updates. Real Xlib also fails this on
     * read-only visuals — match that behavior so well-behaved clients
     * fall back to XAllocColor. */
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
    if (ioErrorHandler) {
        /* Per Xlib spec, the IO error handler is not expected to return; if
         * it does, the client is killed. */
        (void) ioErrorHandler(display);
    }
    /* No handler, or handler returned: emulate the X server tearing down
     * the connection. Real Xlib's default behavior here is _exit(1). */
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
    circulateChildrenDown(w);
    return 1;
}

int XSetCloseDownMode(register Display *dpy, int mode)
{
    /* No multi-client server state to preserve in our in-process model,
     * so RetainPermanent / RetainTemporary / DestroyAll all collapse to
     * a successful no-op. */
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
/*
 *	DESCRIPTION
 *		Given a Display, Screen, Visual, etc., this routine creates
 *		an appropriate Color Conversion Context.
 *
 *	RETURNS
 *		Returns NULL if failed; otherwise address of the newly
 *		created XcmsCCC.
 *
 */
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

Status XcmsRGBiToRGB(
    XcmsCCC ccc,
    XcmsColor *pXcmsColors_in_out,
    /* pointer to XcmsColors to convert */ unsigned int nColors,
    /* Number of colors */
    Bool
        *pCompressed) /* pointer to a bit array */ /* * DESCRIPTION * Converts
                                                      color specifications in
                                                      an array of XcmsColor *
                                                      structures from RGBi
                                                      format to RGB format. * *
                                                      RETURNS * XcmsFailure if
                                                      failed, * XcmsSuccess if
                                                      succeeded without gamut
                                                      compression. *
                                                      XcmsSuccessWithCompression
                                                      if succeeded with gamut *
                                                      compression. */
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIELuvToCIEuvY(
    XcmsCCC ccc,
    XcmsColor *pLuv_WhitePt,
    XcmsColor *pColors_in_out,
    unsigned int nColors) /* * DESCRIPTION * Converts color specifications in an
                             array of XcmsColor * structures from CIELuv format
                             to CIEuvY format. * * RETURNS * XcmsFailure if
                             failed, * XcmsSuccess if succeeded. * */
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
    if (ret_width) {
        *ret_width = width;
    }
    if (ret_height) {
        *ret_height = height;
    }
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
    XFontStruct *fs = XQueryFont(dpy, fid);
    if (!fs)
        return 0;
    int width = string && nchars > 0 ? XTextWidth(fs, string, nchars) : 0;
    fillTextExtents(fs, width, dir, font_ascent, font_descent, overall);
    freeQueriedFontStruct(fs);
    /* Spec: Status nonzero on success. Returning 0 here made Motif and Xt
     * widget measurement paths treat the filled metrics as invalid and
     * fall back to zero-width text. */
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

Status XcmsConvertColors(
    XcmsCCC ccc,
    XcmsColor *pColors_in_out,
    unsigned int nColors,
    XcmsColorFormat targetFormat,
    Bool *pCompressed) /* * DESCRIPTION * Convert XcmsColor structures to
                          another format * * RETURNS * XcmsFailure if failed, *
                          XcmsSuccess if succeeded without gamut compression, *
                          XcmsSuccessWithCompression if succeeded with gamut *
                          compression. * */
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsTekHVCToCIEuvY(
    XcmsCCC ccc,
    XcmsColor *pHVC_WhitePt,
    XcmsColor *pColors_in_out,
    unsigned int
        nColors) /* * DESCRIPTION * Transforms an array of TekHVC color
                    specifications, given * their associated white point, to
                    CIECIEuvY.color * specifications. * * RETURNS * XcmsFailure
                    if failed, XcmsSuccess otherwise. * */
{
    WARN_UNIMPLEMENTED;
    return 0;
}

XcmsWhiteAdjustProc XcmsSetWhiteAdjustProc(
    XcmsCCC ccc,
    XcmsWhiteAdjustProc white_adjust_proc,
    XPointer client_data) /* * DESCRIPTION * Set the specified CCC's
                             white_adjust function and client data. * * RETURNS
                             * Returns the old white_adjust function. * */
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

Status XcmsRGBToRGBi(
    XcmsCCC ccc,
    XcmsColor *pXcmsColors_in_out,
    /* pointer to XcmsColors to convert */ unsigned int nColors,
    /* Number of colors */
    Bool *pCompressed) /* pointer to a bit array */ /* * DESCRIPTION * Converts
                                                       color specifications in
                                                       an array of XcmsColor *
                                                       structures from RGB
                                                       format to RGBi format. *
                                                       * RETURNS * XcmsFailure
                                                       if failed, * XcmsSuccess
                                                       if succeeded. */
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XcmsScreenNumberOfCCC(
    XcmsCCC ccc) /* * DESCRIPTION * Queries the screen number of the specified
                    CCC. * * RETURNS * screen number. * */
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XStoreNamedColor(register Display *dpy,
                     Colormap cmap,
                     _Xconst char *name,
                     /* STRING8 */ unsigned long pixel,
                     /* CARD32 */ int flags) /* DoRed, DoGreen, DoBlue */
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
        memset(overall, 0, sizeof(*overall));
        overall->lbearing = 0;
        overall->rbearing = (short) width;
        overall->width = (short) width;
        overall->ascent = fs ? (short) fs->ascent : 0;
        overall->descent = fs ? (short) fs->descent : 0;
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

int XTextExtents16(
    XFontStruct *fs,
    _Xconst XChar2b *string,
    int nchars,
    int *dir,
    /* RETURN font information */ int *font_ascent,
    /* RETURN font information */ int *font_descent,
    /* RETURN font information */ register XCharStruct *overall) /* RETURN
                                                                    character
                                                                    information
                                                                      */
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
    int *dir,
    /* RETURN font information */ int *font_ascent,
    /* RETURN font information */ int *font_descent,
    /* RETURN font information */ register XCharStruct *overall) /* RETURN
                                                                    character
                                                                    information
                                                                  */
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
    XFontStruct *fs = XQueryFont(dpy, fid);
    if (!fs)
        return 0;
    int width = string && nchars > 0 ? XTextWidth16(fs, string, nchars) : 0;
    fillTextExtents(fs, width, dir, font_ascent, font_descent, overall);
    freeQueriedFontStruct(fs);
    /* See XQueryTextExtents — Status must be nonzero on success. */
    return 1;
}


int XGrabButton(register Display *dpy,
                unsigned int button,
                /* CARD8 */ unsigned int modifiers,
                /* CARD16 */ Window grab_window,
                Bool owner_events,
                unsigned int event_mask,
                /* CARD16 */ int pointer_mode,
                int keyboard_mode,
                Window confine_to,
                Cursor curs)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XUngrabButton(register Display *dpy,
                  unsigned int button,
                  /* CARD8 */ unsigned int modifiers,
                  /* CARD16 */ Window grab_window)
{
    WARN_UNIMPLEMENTED;
    return 0;
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
    case ReplayPointer:
        mouseFrozen = False;
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
    case SyncKeyboard:
    case SyncBoth:
        break;
    default:
        return 0;
    }
    return 1;
}

int XUninstallColormap(register Display *dpy, Colormap cmap)
{
    /* Mirror of XInstallColormap: a successful no-op in the in-process
     * TrueColor model. */
    SET_X_SERVER_REQUEST(dpy, X_UninstallColormap);
    postEvent(dpy, RootWindow(dpy, DefaultScreen(dpy)), ColormapNotify, cmap,
              False, ColormapUninstalled);
    return 1;
}

/* XGrabKey / XUngrabKey live in src/input.c next to the rest of the
 * keyboard surface so the grab table and findKeyGrabWindow helper can
 * share state. */

int XDisableAccessControl(register Display *dpy)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsCIEXYZToRGBi(
    XcmsCCC ccc,
    XcmsColor *pXcmsColors_in_out,
    /* pointer to XcmsColors to convert */ unsigned int nColors,
    /* Number of colors */
    Bool *
        pCompressed) /* pointer to an array of Bool */ /* * DESCRIPTION *
                                                          Converts color
                                                          specifications in an
                                                          array of XcmsColor *
                                                          structures from RGB
                                                          format to RGBi
                                                          format. * * RETURNS *
                                                          XcmsFailure if
                                                          failed, * XcmsSuccess
                                                          if succeeded without
                                                          gamut compression. *
                                                          XcmsSuccessWithCompression
                                                          if succeeded with
                                                          gamut * compression.
                                                        */
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
    /* TextItem chains apply font set / delta offsets per chunk. Our font
     * stack only honors the GC's font, so concatenate the strings and
     * advance x by the requested delta between chunks. */
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
    int w =
        set && set->font ? XTextWidth(set->font, text, text_len) : text_len * 8;
    LOG("XmbTextEscapement: text_len=%d (preview='%.20s') -> %d\n", text_len,
        text, w);
    return w;
}

int Xutf8TextEscapement(XFontSet font_set, _Xconst char *text, int text_len)
{
    if (!text || text_len <= 0)
        return 0;
    CompatFontSet *set = GET_FONT_SET(font_set);
    return set && set->font ? XTextWidth(set->font, text, text_len)
                            : text_len * 8;
}

XIM XIMOfIC(XIC ic)
{
    WARN_UNIMPLEMENTED;
    return NULL;
}

int XwcLookupString(XIC ic,
                    XKeyEvent *ev,
                    wchar_t *buffer,
                    int nchars,
                    KeySym *keysym,
                    Status *status)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XwcTextPropertyToTextList(Display *dpy,
                              const XTextProperty *text_prop,
                              wchar_t ***list_ret,
                              int *count_ret)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

int XwcTextListToTextProperty(Display *dpy,
                              wchar_t **list,
                              int count,
                              XICCEncodingStyle style,
                              XTextProperty *text_prop)
{
    WARN_UNIMPLEMENTED;
    return 0;
}

Status XcmsRGBiToCIEXYZ(
    XcmsCCC ccc,
    XcmsColor *pXcmsColors_in_out,
    /* pointer to XcmsColors to convert */ unsigned int nColors,
    /* Number of colors */
    Bool *pCompressed) /* pointer to a bit array */ /* * DESCRIPTION * Converts
                                                       color specifications in
                                                       an array of XcmsColor *
                                                       structures from RGBi
                                                       format to CIEXYZ format.
                                                       * * RETURNS * XcmsFailure
                                                       if failed, * XcmsSuccess
                                                       if succeeded. */
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

int XWMGeometry(
    Display *dpy,
    /* user's display connection */ int screen,
    /* screen on which to do computation */ _Xconst char *user_geom,
    /* user provided geometry spec */ _Xconst char *def_geom,
    /* default geometry spec for window */ unsigned int bwidth,
    /* border width */ XSizeHints *hints,
    /* usually WM_NORMAL_HINTS */ int *x_return,
    /* location of window */ int *y_return,
    /* location of window */ int *width_return,
    /* size of window */ int *height_return,
    /* size of window */ int *gravity_return) /* gravity of window */
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

    int widthUnits = (umask & WidthValue)
                         ? (int) uwidth
                         : ((dmask & WidthValue) ? (int) dwidth : 1);
    int heightUnits = (umask & HeightValue)
                          ? (int) uheight
                          : ((dmask & HeightValue) ? (int) dheight : 1);
    int width = widthUnits * widthInc + baseWidth;
    int height = heightUnits * heightInc + baseHeight;
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

    int x = 0;
    if (umask & XValue) {
        x = (umask & XNegative)
                ? DisplayWidth(dpy, screen) + ux - width - 2 * (int) bwidth
                : ux;
    } else if (dmask & XValue) {
        if (dmask & XNegative) {
            x = DisplayWidth(dpy, screen) + dx - width - 2 * (int) bwidth;
            rmask |= XNegative;
        } else {
            x = dx;
        }
    }

    int y = 0;
    if (umask & YValue) {
        y = (umask & YNegative)
                ? DisplayHeight(dpy, screen) + uy - height - 2 * (int) bwidth
                : uy;
    } else if (dmask & YValue) {
        if (dmask & YNegative) {
            y = DisplayHeight(dpy, screen) + dy - height - 2 * (int) bwidth;
            rmask |= YNegative;
        } else {
            y = dy;
        }
    }

    if (x_return)
        *x_return = x;
    if (y_return)
        *y_return = y;
    if (width_return)
        *width_return = width;
    if (height_return)
        *height_return = height;
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

Status XGetIconSizes(
    Display *dpy,
    Window w,
    /* typically, root */ XIconSize **size_list,
    /* RETURN */ int *count) /* RETURN number of items on the list */
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

    /* Count NUL-terminated strings inside the property. A malformed
     * payload that does not end in NUL would advance offset past nitems;
     * cap the step so the second pass cannot read past the buffer. */
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
    WARN_UNIMPLEMENTED;
}

/* Per ICCCM section 6.4, the RGB_COLOR_MAP family of properties stores
 * a list of XStandardColormap entries serialized as ten CARD32 fields
 * each in this fixed order: visualid, killid, colormap, red_max,
 * red_mult, green_max, green_mult, blue_max, blue_mult, base_pixel.
 * The C struct's field order is different from the wire order, so the
 * Get/Set helpers translate explicitly. */
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
    WARN_UNIMPLEMENTED;
    return 0;
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
    /* Overflow-safe scanline math: (width + 7) wraps before the divide if
     * width is close to UINT_MAX, so guard the add and the multiply
     * separately. */
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
    /* X11 convention names the symbols based on the file's basename; we
     * use a fixed "image" prefix to keep the writer deterministic. */
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
                    unsigned int *width,
                    /* RETURNED */ unsigned int *height,
                    /* RETURNED */ Pixmap *pixmap,
                    /* RETURNED */ int *x_hot,
                    /* RETURNED */ int *y_hot) /* RETURNED */
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
     * (width); match the trailing keyword instead of assuming an
     * underscore is present. */
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
        /* Short data: the file declared an X-by-Y bitmap but ran out of
         * hex bytes before filling it. Reporting BitmapSuccess here would
         * leave the caller drawing uninitialized regions. */
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
                        XStandardColormap **stdcmap,
                        /* RETURN */ int *count,
                        /* RETURN */ Atom property) /* XA_RGB_BEST_MAP, etc. */
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
    if (buffer && nbytes > 0) {
        snprintf(buffer, (size_t) nbytes, "%s", defaultp ? defaultp : "");
    }
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
 * UTF8_STRING when isUtf8 is True, XA_STRING otherwise. Returns True on
 * success and stores a heap-allocated value the caller must free with
 * Xfree(tp.value). */
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
    if (windowName) {
        haveWin = textPropertyFromString(dpy, windowName, isUtf8, &winProp);
    }
    if (iconName) {
        haveIcon = textPropertyFromString(dpy, iconName, isUtf8, &iconProp);
    }
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
    int count = text_len > 0 ? text_len : 0;
    if (count > buffer_size)
        count = buffer_size;
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
 * auxiliary sockets, so there is nothing to watch and nothing to drain:
 * report an empty fd set and acknowledge watcher registration without
 * recording the callback. */
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
