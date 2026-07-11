#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/xtestconst.h>
#include "extension.h"
#include "util.h"

/* Built-in extensions get fixed protocol codes assigned by hand in
 * XQueryExtension below: SHAPE (opcode 129, event 64, error 128), MIT-SHM
 * (opcode 130, event 128, error 129), XTEST (opcode 132), plus synthetic XKB
 * from missing.c (opcode 135, event 85, error 137). XAddExtension hands
 * app-registered extensions auto-assigned codes; base them past the highest
 * built-in in each class so the two schemes never overlap. Bump these if a new
 * built-in is added above them.
 */
#define BUILTIN_EXT_OPCODE_MAX 135
#define BUILTIN_EXT_EVENT_MAX 128
#define BUILTIN_EXT_ERROR_MAX 137

static _XExtension *findExtension(Display *display, int extension)
{
    for (_XExtension *ext = display->ext_procs; ext; ext = ext->next) {
        if (ext->codes.extension == extension)
            return ext;
    }
    return NULL;
}

XExtCodes *XAddExtension(Display *display)
{
    /* Protocol codes are byte-sized on the wire (CARD8, 0-255) even though
     * XExtCodes stores them as int. Each code is <reserve base> + ext, so the
     * largest reserve base is the first field to exceed 255; refuse once the
     * next id would not fit. Computing the max here keeps the guard correct no
     * matter which class holds the highest built-in. Clamping would hand out
     * duplicate codes, worse than failing; callers already tolerate a NULL
     * return (see XInitExtension).
     */
    int reserveMax = BUILTIN_EXT_OPCODE_MAX;
    if (BUILTIN_EXT_EVENT_MAX > reserveMax)
        reserveMax = BUILTIN_EXT_EVENT_MAX;
    if (BUILTIN_EXT_ERROR_MAX > reserveMax)
        reserveMax = BUILTIN_EXT_ERROR_MAX;
    if (display->ext_number >= 255 - reserveMax)
        return NULL;
    _XExtension *ext = calloc(1, sizeof(_XExtension));
    if (!ext)
        return NULL;
    ext->codes.extension = ++display->ext_number;
    ext->codes.major_opcode = BUILTIN_EXT_OPCODE_MAX + ext->codes.extension;
    ext->codes.first_event = BUILTIN_EXT_EVENT_MAX + ext->codes.extension;
    ext->codes.first_error = BUILTIN_EXT_ERROR_MAX + ext->codes.extension;
    ext->next = display->ext_procs;
    display->ext_procs = ext;
    return &ext->codes;
}

XExtCodes *XInitExtension(Display *display, _Xconst char *name)
{
    int majorOpcode = 0;
    int firstEvent = 0;
    int firstError = 0;
    if (!XQueryExtension(display, name, &majorOpcode, &firstEvent,
                         &firstError)) {
        return NULL;
    }

    XExtCodes *codes = XAddExtension(display);
    if (!codes)
        return NULL;
    codes->major_opcode = majorOpcode;
    codes->first_event = firstEvent;
    codes->first_error = firstError;
    _XExtension *ext = findExtension(display, codes->extension);
    if (ext && name)
        ext->name = strdup(name);
    return codes;
}

Bool XQueryExtension(Display *display,
                     _Xconst char *name,
                     int *major_opcode_return,
                     int *first_event_return,
                     int *first_error_return)
{
    (void) display;
    /* Deliberately do NOT advertise "GLX" here. Apps that want our GLX layer
     * use glXQueryExtension (which is provider-aware). Reporting GLX present
     * through the generic probe steers real toolkits into a GLX-protocol path:
     * on any host with real GL/GLX loaded in-process (node11 has a system
     * libEGL) that path issues X_GLXGetVisualConfigs and dies. Never break
     * userspace.
     */
    if (name && !strcmp(name, SHAPENAME)) {
        if (major_opcode_return)
            *major_opcode_return = 129;
        if (first_event_return)
            *first_event_return = 64;
        if (first_error_return)
            *first_error_return = 128;
        return True;
    }
    if (name && !strcmp(name, XTestExtensionName)) {
        if (major_opcode_return)
            *major_opcode_return = 132;
        if (first_event_return)
            *first_event_return = 0;
        if (first_error_return)
            *first_error_return = 0;
        return True;
    }
    /* Stay coherent with the per-extension entry points in src/xshm.c:
     * XShmQueryExtension reports MIT-SHM present, so probing the generic name
     * must agree. The event base matches XShmGetEventBase (128) so
     * ShmCompletion derives to one value from either path. The error base is
     * advisory (this shim never dispatches extension errors), but must stay
     * distinct from SHAPE's 128, so MIT-SHM gets 129.
     */
    if (name && !strcmp(name, "MIT-SHM")) {
        if (major_opcode_return)
            *major_opcode_return = 130;
        if (first_event_return)
            *first_event_return = 128;
        if (first_error_return)
            *first_error_return = 129;
        return True;
    }

    LOG("Ignoring unsupported extension probe: %s\n", name ? name : "(null)");
    if (major_opcode_return)
        *major_opcode_return = 0;
    if (first_event_return)
        *first_event_return = 0;
    if (first_error_return)
        *first_error_return = 0;
    return False;
}

Bool XFixesQueryExtension(Display *dpy,
                          int *event_base_return,
                          int *error_base_return)
{
    (void) dpy;
    if (event_base_return)
        *event_base_return = 0;
    if (error_base_return)
        *error_base_return = 0;
    return False;
}

Bool XDamageQueryExtension(Display *dpy,
                           int *event_base_return,
                           int *error_base_return)
{
    (void) dpy;
    if (event_base_return)
        *event_base_return = 0;
    if (error_base_return)
        *error_base_return = 0;
    return False;
}

Bool XRRQueryExtension(Display *dpy,
                       int *event_base_return,
                       int *error_base_return)
{
    (void) dpy;
    if (event_base_return)
        *event_base_return = 0;
    if (error_base_return)
        *error_base_return = 0;
    return False;
}

/* Each XESet* hook is a pointer-typed get-and-swap on a different field of
 * _XExtension. The differences are: (1) hook type, (2) struct member, and (3)
 * function name -- everything else is identical.
 */
#define DEFINE_XESET_HOOK(funcName, hookType, field)                  \
    hookType funcName(Display *display, int extension, hookType proc) \
    {                                                                 \
        _XExtension *ext = findExtension(display, extension);         \
        if (!ext)                                                     \
            return NULL;                                              \
        hookType previous = ext->field;                               \
        ext->field = proc;                                            \
        return previous;                                              \
    }

DEFINE_XESET_HOOK(XESetCreateGC, CreateGCType, create_GC)
DEFINE_XESET_HOOK(XESetCopyGC, CopyGCType, copy_GC)
DEFINE_XESET_HOOK(XESetFlushGC, FlushGCType, flush_GC)
DEFINE_XESET_HOOK(XESetFreeGC, FreeGCType, free_GC)
DEFINE_XESET_HOOK(XESetCreateFont, CreateFontType, create_Font)
DEFINE_XESET_HOOK(XESetFreeFont, FreeFontType, free_Font)
DEFINE_XESET_HOOK(XESetCloseDisplay, CloseDisplayType, close_display)
DEFINE_XESET_HOOK(XESetError, ErrorType, error)
DEFINE_XESET_HOOK(XESetErrorString, ErrorStringType, error_string)
DEFINE_XESET_HOOK(XESetPrintErrorValues, PrintErrorType, error_values)
DEFINE_XESET_HOOK(XESetBeforeFlush, BeforeFlushType, before_flush)

#undef DEFINE_XESET_HOOK

Bool (*XESetWireToEvent(Display *display,
                        int event_number,
                        Bool (*proc)(Display *, XEvent *, xEvent *)))(Display *,
                                                                      XEvent *,
                                                                      xEvent *)
{
    (void) display;
    (void) event_number;
    (void) proc;
    return NULL;
}

Bool (*XESetWireToEventCookie(Display *display,
                              int extension,
                              Bool (*proc)(Display *,
                                           XGenericEventCookie *,
                                           xEvent *)))(Display *,
                                                       XGenericEventCookie *,
                                                       xEvent *)
{
    (void) display;
    (void) extension;
    (void) proc;
    return NULL;
}

Bool (*XESetCopyEventCookie(
    Display *display,
    int extension,
    Bool (*proc)(Display *, XGenericEventCookie *, XGenericEventCookie *)))(
    Display *,
    XGenericEventCookie *,
    XGenericEventCookie *)
{
    (void) display;
    (void) extension;
    (void) proc;
    return NULL;
}

Status (*XESetEventToWire(Display *display,
                          int event_number,
                          Status (*proc)(Display *, XEvent *, xEvent *)))(
    Display *,
    XEvent *,
    xEvent *)
{
    (void) display;
    (void) event_number;
    (void) proc;
    return NULL;
}

Bool (*XESetWireToError(Display *display,
                        int error_number,
                        Bool (*proc)(Display *, XErrorEvent *, xError *)))(
    Display *,
    XErrorEvent *,
    xError *)
{
    (void) display;
    (void) error_number;
    (void) proc;
    return NULL;
}

void freeExtensionStorage(Display *display)
{
    _XExtension *ext = display->ext_procs;
    while (ext) {
        _XExtension *next = ext->next;
        if (ext->close_display)
            ext->close_display(display, &ext->codes);
        free(ext->name);
        free(ext);
        ext = next;
    }
    display->ext_procs = NULL;
}
