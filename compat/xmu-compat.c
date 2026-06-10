#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Intrinsic.h>
#include <X11/IntrinsicP.h>
#include <X11/Shell.h>
#include <X11/ShellP.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xmu/Atoms.h>
#include <X11/Xmu/Converters.h>
#include <X11/Xutil.h>

#include "util.h"

typedef struct _AtomRec {
    const char *name;
    Atom atom;
} AtomRec;

#define DEFINE_XMU_ATOM(symbol, name) \
    static AtomRec symbol##_storage = {name, None}; \
    AtomPtr symbol = &symbol##_storage

DEFINE_XMU_ATOM(_XA_ATOM_PAIR, "ATOM_PAIR");
DEFINE_XMU_ATOM(_XA_CHARACTER_POSITION, "CHARACTER_POSITION");
DEFINE_XMU_ATOM(_XA_CLASS, "CLASS");
DEFINE_XMU_ATOM(_XA_CLIENT_WINDOW, "CLIENT_WINDOW");
DEFINE_XMU_ATOM(_XA_CLIPBOARD, "CLIPBOARD");
DEFINE_XMU_ATOM(_XA_COMPOUND_TEXT, "COMPOUND_TEXT");
DEFINE_XMU_ATOM(_XA_DECNET_ADDRESS, "DECNET_ADDRESS");
DEFINE_XMU_ATOM(_XA_DELETE, "DELETE");
DEFINE_XMU_ATOM(_XA_FILENAME, "FILENAME");
DEFINE_XMU_ATOM(_XA_HOSTNAME, "HOSTNAME");
DEFINE_XMU_ATOM(_XA_IP_ADDRESS, "IP_ADDRESS");
DEFINE_XMU_ATOM(_XA_LENGTH, "LENGTH");
DEFINE_XMU_ATOM(_XA_LIST_LENGTH, "LIST_LENGTH");
DEFINE_XMU_ATOM(_XA_NAME, "NAME");
DEFINE_XMU_ATOM(_XA_NET_ADDRESS, "NET_ADDRESS");
DEFINE_XMU_ATOM(_XA_NULL, "NULL");
DEFINE_XMU_ATOM(_XA_OWNER_OS, "OWNER_OS");
DEFINE_XMU_ATOM(_XA_SPAN, "SPAN");
DEFINE_XMU_ATOM(_XA_TARGETS, "TARGETS");
DEFINE_XMU_ATOM(_XA_TEXT, "TEXT");
DEFINE_XMU_ATOM(_XA_TIMESTAMP, "TIMESTAMP");
DEFINE_XMU_ATOM(_XA_USER, "USER");
DEFINE_XMU_ATOM(_XA_UTF8_STRING, "UTF8_STRING");

static void copyISOLatin1Case(char *dst, const char *src, int size, int upper)
{
    if (!dst || !src || size == 0)
        return;
    int i = 0;
    int limit = size > 0 ? size - 1 : -1;
    while (*src && (size < 0 || i < limit)) {
        unsigned char ch = (unsigned char) *src++;
        dst[i++] = (char) (upper ? toupper(ch) : tolower(ch));
    }
    dst[i] = '\0';
}

static Widget findWMShell(Widget w)
{
    for (Widget current = w; current; current = XtParent(current)) {
        if (XtIsSubclass(current, wmShellWidgetClass))
            return current;
    }
    return NULL;
}

static Widget findApplicationShell(Widget w)
{
    for (Widget current = w; current; current = XtParent(current)) {
        if (XtIsApplicationShell(current))
            return current;
    }
    return NULL;
}

/* Populate the selection-conversion return slots with a freshly duplicated
 * STRING value. Returns True only when XtNewString succeeded. */
static Boolean returnStringSelection(const char *value,
                                     Atom *type_return,
                                     XPointer *value_return,
                                     unsigned long *length_return,
                                     int *format_return)
{
    char *dup = XtNewString(value);
    if (!dup)
        return False;
    *value_return = (XPointer) dup;
    *type_return = XA_STRING;
    *length_return = strlen(dup);
    *format_return = 8;
    return True;
}

Atom XmuInternAtom(Display *dpy, AtomPtr atom_ptr)
{
    if (!dpy || !atom_ptr || !atom_ptr->name)
        return None;
    if (atom_ptr->atom == None)
        atom_ptr->atom = XInternAtom(dpy, atom_ptr->name, False);
    return atom_ptr->atom;
}

char *XmuGetAtomName(Display *dpy, Atom atom)
{
    return XGetAtomName(dpy, atom);
}

void XmuInternStrings(Display *dpy,
                      String *names,
                      Cardinal count,
                      Atom *atoms_return)
{
    if (!dpy || !names || !atoms_return)
        return;
    for (Cardinal i = 0; i < count; i++)
        atoms_return[i] = XInternAtom(dpy, names[i], False);
}

AtomPtr XmuMakeAtom(const char *name)
{
    AtomRec *atom = calloc(1, sizeof(*atom));
    if (!atom)
        return NULL;
    if (name) {
        size_t len = strlen(name) + 1;
        char *copy = malloc(len);
        if (copy)
            memcpy(copy, name, len);
        atom->name = copy;
    }
    if (name && !atom->name) {
        free(atom);
        return NULL;
    }
    return atom;
}

char *XmuNameOfAtom(AtomPtr atom_ptr)
{
    return atom_ptr ? (char *) atom_ptr->name : NULL;
}

void XmuCopyISOLatin1Lowered(char *dst_return, const char *src)
{
    copyISOLatin1Case(dst_return, src, -1, 0);
}

void XmuCopyISOLatin1Uppered(char *dst_return, const char *src)
{
    copyISOLatin1Case(dst_return, src, -1, 1);
}

void XmuNCopyISOLatin1Lowered(char *dst_return, const char *src, int size)
{
    copyISOLatin1Case(dst_return, src, size, 0);
}

void XmuNCopyISOLatin1Uppered(char *dst_return, const char *src, int size)
{
    copyISOLatin1Case(dst_return, src, size, 1);
}

int XmuCompareISOLatin1(const char *first, const char *second)
{
    if (!first)
        first = "";
    if (!second)
        second = "";
    while (*first && *second) {
        int a = tolower((unsigned char) *first++);
        int b = tolower((unsigned char) *second++);
        if (a != b)
            return a - b;
    }
    return (unsigned char) *first - (unsigned char) *second;
}

void XmuCvtFunctionToCallback(XrmValue *args,
                              Cardinal *num_args,
                              XrmValuePtr fromVal,
                              XrmValuePtr toVal)
{
    (void) args;
    (void) num_args;
    if (!fromVal || !toVal || !fromVal->addr)
        return;

    XtCallbackRec *callbacks = calloc(2, sizeof(*callbacks));
    if (!callbacks)
        return;
    callbacks[0].callback = *(XtCallbackProc *) fromVal->addr;
    callbacks[0].closure = NULL;

    if (toVal->addr && toVal->size >= sizeof(XtCallbackList)) {
        *(XtCallbackList *) toVal->addr = callbacks;
    } else {
        toVal->addr = (XPointer) callbacks;
    }
    toVal->size = sizeof(XtCallbackList);
}

Boolean XmuConvertStandardSelection(Widget w,
                                    Time time,
                                    Atom *selection,
                                    Atom *target,
                                    Atom *type_return,
                                    XPointer *value_return,
                                    unsigned long *length_return,
                                    int *format_return)
{
    (void) selection;
    if (!w || !target || !type_return || !value_return || !length_return ||
        !format_return)
        return False;

    Display *dpy = XtDisplay(w);
    if (!dpy)
        return False;

    if (*target == XA_TIMESTAMP(dpy)) {
        Time *value = (Time *) XtMalloc(sizeof(*value));
        if (!value)
            return False;
        *value = time;
        *value_return = (XPointer) value;
        *type_return = XA_INTEGER;
        *length_return = 1;
        *format_return = 32;
        return True;
    }

    if (*target == XA_HOSTNAME(dpy)) {
        char hostname[256];
        if (gethostname(hostname, sizeof(hostname)) != 0)
            hostname[0] = '\0';
        hostname[sizeof(hostname) - 1] = '\0';
        return returnStringSelection(hostname, type_return, value_return,
                                     length_return, format_return);
    }

    if (*target == XA_USER(dpy)) {
        const char *user = getenv("USER");
        if (!user)
            user = getenv("LOGNAME");
        if (!user)
            return False;
        return returnStringSelection(user, type_return, value_return,
                                     length_return, format_return);
    }

    if (*target == XA_CLASS(dpy)) {
        Widget classShell = findApplicationShell(w);
        if (!classShell) {
            classShell = w;
            while (XtParent(classShell))
                classShell = XtParent(classShell);
        }

        const char *instance = XtName(classShell);
        if (!instance)
            instance = XtName(w);
        if (!instance)
            instance = "";

        const char *class_name = NULL;
        if (XtIsApplicationShell(classShell))
            class_name = ((ApplicationShellWidget) classShell)->application.class;
        if (!class_name)
            class_name = XtClass(classShell)->core_class.class_name;
        if (!class_name)
            class_name = instance[0] ? instance : "Widget";

        size_t instance_len = strlen(instance);
        size_t class_len = strlen(class_name);
        /* XtMalloc takes Cardinal (unsigned int); guard against size_t
         * truncation before allocating the two-NUL packed buffer. */
        if (instance_len > (size_t) INT_MAX - 2 ||
            class_len > (size_t) INT_MAX - 2 - instance_len)
            return False;
        size_t value_len = instance_len + class_len + 2;
        char *value = (char *) XtMalloc(value_len);
        if (!value)
            return False;

        memcpy(value, instance, instance_len + 1);
        memcpy(value + instance_len + 1, class_name, class_len + 1);
        *value_return = (XPointer) value;
        *type_return = XA_STRING;
        *length_return = value_len;
        *format_return = 8;
        return True;
    }

    if (*target == XA_NAME(dpy)) {
        Widget shell = findWMShell(w);
        const char *name = NULL;
        if (shell)
            XtVaGetValues(shell, XtNtitle, &name, NULL);
        if (!name && shell)
            name = XtName(shell);
        if (!name)
            name = XtName(w);
        if (!name)
            return False;
        return returnStringSelection(name, type_return, value_return,
                                     length_return, format_return);
    }

    if (*target == XA_CLIENT_WINDOW(dpy)) {
        Widget shell = findWMShell(w);
        if (!shell)
            return False;
        Window *value = (Window *) XtMalloc(sizeof(*value));
        if (!value)
            return False;
        *value = XtWindow(shell);
        *value_return = (XPointer) value;
        *type_return = XA_WINDOW;
        *length_return = 1;
        *format_return = 32;
        return True;
    }

    if (*target == XA_OWNER_OS(dpy)) {
        const char *os_name =
#if defined(__APPLE__)
            "Darwin";
#elif defined(__linux__)
            "Linux";
#else
            "Unknown";
#endif
        return returnStringSelection(os_name, type_return, value_return,
                                     length_return, format_return);
    }

    if (*target == XA_TARGETS(dpy)) {
        Atom advertised[] = {
            XA_TARGETS(dpy),  XA_TIMESTAMP(dpy),     XA_HOSTNAME(dpy),
            XA_USER(dpy),     XA_CLASS(dpy),         XA_NAME(dpy),
            XA_CLIENT_WINDOW(dpy), XA_OWNER_OS(dpy),
        };
        Atom *targets = (Atom *) XtMalloc(sizeof(advertised));
        if (!targets)
            return False;
        memcpy(targets, advertised, sizeof(advertised));
        *value_return = (XPointer) targets;
        *type_return = XA_ATOM;
        *length_return = sizeof(advertised) / sizeof(advertised[0]);
        *format_return = 32;
        return True;
    }

    return False;
}

static Bool windowHasProperty(Display *dpy, Window win, Atom property)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0;
    unsigned long bytes_after = 0;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, property, 0, 0, False, AnyPropertyType,
                           &actual_type, &actual_format, &nitems,
                           &bytes_after, &data) == Success &&
        actual_type != None) {
        XFree(data);
        return True;
    }
    XFree(data);
    return False;
}

static Window findClientWindow(Display *dpy, Window win, Atom wm_state)
{
    if (windowHasProperty(dpy, win, wm_state))
        return win;

    Window root = None;
    Window parent = None;
    Window *children = NULL;
    unsigned int child_count = 0;
    if (!XQueryTree(dpy, win, &root, &parent, &children, &child_count))
        return None;
    for (unsigned int i = 0; i < child_count; i++) {
        Window child = findClientWindow(dpy, children[i], wm_state);
        if (child != None) {
            XFree(children);
            return child;
        }
    }
    XFree(children);
    return None;
}

Window XmuClientWindow(Display *dpy, Window win)
{
    if (!dpy || win == None)
        return None;
    Atom wm_state = XInternAtom(dpy, "WM_STATE", True);
    if (wm_state == None)
        return win;
    Window client = findClientWindow(dpy, win, wm_state);
    return client == None ? win : client;
}

Screen *XmuScreenOfWindow(Display *dpy, Window w)
{
    if (!dpy || w == None)
        return NULL;
    return DefaultScreenOfDisplay(dpy);
}

Bool XmuUpdateMapHints(Display *dpy, Window win, XSizeHints *hints)
{
    if (!dpy || !hints)
        return False;
    long supplied = 0;
    return XGetWMNormalHints(dpy, win, hints, &supplied);
}

Status XmuLookupStandardColormap(Display *dpy,
                                 int screen,
                                 VisualID visualid,
                                 unsigned int depth,
                                 Atom property,
                                 Bool replace,
                                 Bool retain)
{
    /* retain across server resets isn't applicable to SDL2-backed Xlib:
     * the server lives and dies with the client. */
    (void) retain;
    if (!dpy || property == None || depth == 0)
        return False;
    Window root = RootWindow(dpy, screen);

    if (!replace) {
        XStandardColormap *existing = NULL;
        int n = 0;
        if (XGetRGBColormaps(dpy, root, &existing, &n, property) && existing) {
            Bool found = False;
            for (int i = 0; i < n; i++) {
                if (existing[i].visualid == visualid) {
                    found = True;
                    break;
                }
            }
            XFree(existing);
            if (found)
                return True;
        }
    }

    /* SDL2 backs the only visual we expose as 24bpp TrueColor RGB. The
     * client (mwm in particular) only reads this property to discover an
     * acceptable allocation; fill in the canonical TrueColor RGB cube
     * mapped 1:1 onto the pixel value. */
    XStandardColormap cmap = {0};
    cmap.colormap = DefaultColormap(dpy, screen);
    cmap.red_max = 255;
    cmap.red_mult = 0x10000;
    cmap.green_max = 255;
    cmap.green_mult = 0x100;
    cmap.blue_max = 255;
    cmap.blue_mult = 1;
    cmap.base_pixel = 0;
    cmap.visualid = visualid;
    cmap.killid = None;

    XSetRGBColormaps(dpy, root, &cmap, 1, property);
    return True;
}

int XmuGetHostname(char *buf_return, int maxlen)
{
    if (!buf_return || maxlen <= 0)
        return 0;
    if (gethostname(buf_return, (size_t) maxlen) != 0) {
        buf_return[0] = '\0';
        return 0;
    }
    buf_return[maxlen - 1] = '\0';
    return (int) strlen(buf_return);
}

int XmuSnprintf(char *str, int size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int result = vsnprintf(str, (size_t) size, fmt, ap);
    va_end(ap);
    return result;
}

/* ------------------------------------------------------------------
 * Xmu type converters. Motif registers its own converters for its own
 * resource types; the symbols below exist so any object that includes
 * <X11/Xmu/Converters.h> (which declares them) links against
 * libXmu-compat without falling back to the host libXmu. mwm and
 * Athena widgets actually exercise some of them. The simple
 * String->enum converters are implemented inline; rarely-used variants
 * (Bitmap, Cursor, ColorCursor) and the inverse "ToString" converters
 * fail conversion via WARN_UNIMPLEMENTED so users see the gap at
 * runtime if they hit it.
 *
 * Old-style converter signature (void return): set toVal->size = 0 to
 * report failure. New-style XtTypeConverter (Boolean return): return
 * False. */

static void xmuStoreConverted(XrmValuePtr toVal, const void *src, Cardinal size)
{
    if (toVal->addr && toVal->size >= size) {
        memcpy(toVal->addr, src, size);
    } else {
        static union {
            void *ptr;
            long l;
            double d;
            char bytes[256];
        } fallback;

        if (size <= sizeof(fallback)) {
            memcpy(&fallback, src, size);
            toVal->addr = (XPointer) &fallback;
        } else {
            toVal->addr = (XPointer) src;
        }
    }
    toVal->size = size;
}

static int xmuStrcasecmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char) *a++);
        int cb = tolower((unsigned char) *b++);
        if (ca != cb)
            return ca - cb;
    }
    return (unsigned char) *a - (unsigned char) *b;
}

void XmuCvtStringToBackingStore(XrmValue *args,
                                Cardinal *num_args,
                                XrmValuePtr fromVal,
                                XrmValuePtr toVal)
{
    (void) args;
    (void) num_args;
    if (!fromVal || !toVal || !fromVal->addr) {
        if (toVal)
            toVal->size = 0;
        return;
    }
    const char *s = (const char *) fromVal->addr;
    static int result;
    if (xmuStrcasecmp(s, "notUseful") == 0)
        result = NotUseful;
    else if (xmuStrcasecmp(s, "whenMapped") == 0)
        result = WhenMapped;
    else if (xmuStrcasecmp(s, "always") == 0)
        result = Always;
    else if (xmuStrcasecmp(s, "default") == 0)
        result = NotUseful;
    else {
        toVal->size = 0;
        return;
    }
    xmuStoreConverted(toVal, &result, sizeof(result));
}

Boolean XmuCvtBackingStoreToString(Display *dpy,
                                   XrmValue *args,
                                   Cardinal *num_args,
                                   XrmValuePtr fromVal,
                                   XrmValuePtr toVal,
                                   XtPointer *converter_data)
{
    (void) dpy;
    (void) args;
    (void) num_args;
    (void) converter_data;
    if (!fromVal || !toVal || !fromVal->addr)
        return False;
    static char *names[] = {(char *) "notUseful", (char *) "whenMapped",
                            (char *) "always"};
    int v = *(int *) fromVal->addr;
    if (v < NotUseful || v > Always)
        return False;
    char *s = names[v];
    xmuStoreConverted(toVal, &s, sizeof(s));
    return True;
}

void XmuCvtStringToCursor(XrmValue *args,
                          Cardinal *num_args,
                          XrmValuePtr fromVal,
                          XrmValuePtr toVal)
{
    (void) args;
    (void) num_args;
    (void) fromVal;
    WARN_UNIMPLEMENTED;
    if (toVal)
        toVal->size = 0;
}

Boolean XmuCvtStringToColorCursor(Display *dpy,
                                  XrmValue *args,
                                  Cardinal *num_args,
                                  XrmValuePtr fromVal,
                                  XrmValuePtr toVal,
                                  XtPointer *converter_data)
{
    (void) dpy;
    (void) args;
    (void) num_args;
    (void) fromVal;
    (void) toVal;
    (void) converter_data;
    WARN_UNIMPLEMENTED;
    return False;
}

void XmuCvtStringToGravity(XrmValue *args,
                           Cardinal *num_args,
                           XrmValuePtr fromVal,
                           XrmValuePtr toVal)
{
    (void) args;
    (void) num_args;
    if (!fromVal || !toVal || !fromVal->addr) {
        if (toVal)
            toVal->size = 0;
        return;
    }
    const char *s = (const char *) fromVal->addr;
    static int result;
    if (xmuStrcasecmp(s, "Forget") == 0)
        result = ForgetGravity;
    else if (xmuStrcasecmp(s, "NorthWest") == 0)
        result = NorthWestGravity;
    else if (xmuStrcasecmp(s, "North") == 0)
        result = NorthGravity;
    else if (xmuStrcasecmp(s, "NorthEast") == 0)
        result = NorthEastGravity;
    else if (xmuStrcasecmp(s, "West") == 0)
        result = WestGravity;
    else if (xmuStrcasecmp(s, "Center") == 0)
        result = CenterGravity;
    else if (xmuStrcasecmp(s, "East") == 0)
        result = EastGravity;
    else if (xmuStrcasecmp(s, "SouthWest") == 0)
        result = SouthWestGravity;
    else if (xmuStrcasecmp(s, "South") == 0)
        result = SouthGravity;
    else if (xmuStrcasecmp(s, "SouthEast") == 0)
        result = SouthEastGravity;
    else if (xmuStrcasecmp(s, "Static") == 0)
        result = StaticGravity;
    else if (xmuStrcasecmp(s, "Unmap") == 0)
        result = UnmapGravity;
    else {
        toVal->size = 0;
        return;
    }
    xmuStoreConverted(toVal, &result, sizeof(result));
}

Boolean XmuCvtGravityToString(Display *dpy,
                              XrmValue *args,
                              Cardinal *num_args,
                              XrmValuePtr fromVal,
                              XrmValuePtr toVal,
                              XtPointer *converter_data)
{
    (void) dpy;
    (void) args;
    (void) num_args;
    (void) converter_data;
    if (!fromVal || !toVal || !fromVal->addr)
        return False;
    static char *names[] = {(char *) "forget",      (char *) "northwest",
                            (char *) "north",       (char *) "northeast",
                            (char *) "west",        (char *) "center",
                            (char *) "east",        (char *) "southwest",
                            (char *) "south",       (char *) "southeast",
                            (char *) "static",      (char *) "unmap"};
    int v = *(int *) fromVal->addr;
    if (v < ForgetGravity || v > StaticGravity)
        return False;
    char *s = names[v];
    xmuStoreConverted(toVal, &s, sizeof(s));
    return True;
}

void XmuCvtStringToJustify(XrmValue *args,
                           Cardinal *num_args,
                           XrmValuePtr fromVal,
                           XrmValuePtr toVal)
{
    (void) args;
    (void) num_args;
    if (!fromVal || !toVal || !fromVal->addr) {
        if (toVal)
            toVal->size = 0;
        return;
    }
    const char *s = (const char *) fromVal->addr;
    static XtJustify result;
    if (xmuStrcasecmp(s, "left") == 0)
        result = XtJustifyLeft;
    else if (xmuStrcasecmp(s, "center") == 0)
        result = XtJustifyCenter;
    else if (xmuStrcasecmp(s, "right") == 0)
        result = XtJustifyRight;
    else {
        toVal->size = 0;
        return;
    }
    xmuStoreConverted(toVal, &result, sizeof(result));
}

Boolean XmuCvtJustifyToString(Display *dpy,
                              XrmValue *args,
                              Cardinal *num_args,
                              XrmValuePtr fromVal,
                              XrmValuePtr toVal,
                              XtPointer *converter_data)
{
    (void) dpy;
    (void) args;
    (void) num_args;
    (void) converter_data;
    if (!fromVal || !toVal || !fromVal->addr)
        return False;
    static char *names[] = {(char *) "left", (char *) "center",
                            (char *) "right"};
    int v = *(int *) fromVal->addr;
    if (v < XtJustifyLeft || v > XtJustifyRight)
        return False;
    char *s = names[v];
    xmuStoreConverted(toVal, &s, sizeof(s));
    return True;
}

void XmuCvtStringToLong(XrmValue *args,
                        Cardinal *num_args,
                        XrmValuePtr fromVal,
                        XrmValuePtr toVal)
{
    (void) args;
    (void) num_args;
    if (!fromVal || !toVal || !fromVal->addr) {
        if (toVal)
            toVal->size = 0;
        return;
    }
    char *endp = NULL;
    static long result;
    result = strtol((const char *) fromVal->addr, &endp, 0);
    if (!endp || endp == (const char *) fromVal->addr) {
        toVal->size = 0;
        return;
    }
    xmuStoreConverted(toVal, &result, sizeof(result));
}

Boolean XmuCvtLongToString(Display *dpy,
                           XrmValue *args,
                           Cardinal *num_args,
                           XrmValuePtr fromVal,
                           XrmValuePtr toVal,
                           XtPointer *converter_data)
{
    (void) dpy;
    (void) args;
    (void) num_args;
    (void) converter_data;
    if (!fromVal || !toVal || !fromVal->addr)
        return False;
    static char buf[32];
    snprintf(buf, sizeof(buf), "%ld", *(long *) fromVal->addr);
    char *p = buf;
    xmuStoreConverted(toVal, &p, sizeof(p));
    return True;
}

void XmuCvtStringToOrientation(XrmValue *args,
                               Cardinal *num_args,
                               XrmValuePtr fromVal,
                               XrmValuePtr toVal)
{
    (void) args;
    (void) num_args;
    if (!fromVal || !toVal || !fromVal->addr) {
        if (toVal)
            toVal->size = 0;
        return;
    }
    const char *s = (const char *) fromVal->addr;
    static XtOrientation result;
    if (xmuStrcasecmp(s, "horizontal") == 0)
        result = XtorientHorizontal;
    else if (xmuStrcasecmp(s, "vertical") == 0)
        result = XtorientVertical;
    else {
        toVal->size = 0;
        return;
    }
    xmuStoreConverted(toVal, &result, sizeof(result));
}

Boolean XmuCvtOrientationToString(Display *dpy,
                                  XrmValue *args,
                                  Cardinal *num_args,
                                  XrmValuePtr fromVal,
                                  XrmValuePtr toVal,
                                  XtPointer *converter_data)
{
    (void) dpy;
    (void) args;
    (void) num_args;
    (void) converter_data;
    if (!fromVal || !toVal || !fromVal->addr)
        return False;
    static char *names[] = {(char *) "horizontal", (char *) "vertical"};
    int v = *(int *) fromVal->addr;
    if (v != XtorientHorizontal && v != XtorientVertical)
        return False;
    char *s = names[v];
    xmuStoreConverted(toVal, &s, sizeof(s));
    return True;
}

void XmuCvtStringToBitmap(XrmValue *args,
                          Cardinal *num_args,
                          XrmValuePtr fromVal,
                          XrmValuePtr toVal)
{
    (void) args;
    (void) num_args;
    (void) fromVal;
    WARN_UNIMPLEMENTED;
    if (toVal)
        toVal->size = 0;
}
