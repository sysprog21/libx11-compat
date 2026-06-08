#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/xtestconst.h>
#include "extension.h"
#include "util.h"

static _XExtension *findExtension(Display *display, int extension)
{
    for (_XExtension *ext = display->ext_procs; ext; ext = ext->next) {
        if (ext->codes.extension == extension) {
            return ext;
        }
    }
    return NULL;
}

XExtCodes *XAddExtension(Display *display)
{
    _XExtension *ext = calloc(1, sizeof(_XExtension));
    if (!ext) {
        return NULL;
    }
    ext->codes.extension = ++display->ext_number;
    ext->codes.major_opcode = 128 + ext->codes.extension;
    ext->codes.first_event = 64 + ext->codes.extension;
    ext->codes.first_error = 128 + ext->codes.extension;
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
    if (!codes) {
        return NULL;
    }
    codes->major_opcode = majorOpcode;
    codes->first_event = firstEvent;
    codes->first_error = firstError;
    _XExtension *ext = findExtension(display, codes->extension);
    if (ext && name) {
        ext->name = strdup(name);
    }
    return codes;
}

Bool XQueryExtension(Display *display,
                     _Xconst char *name,
                     int *major_opcode_return,
                     int *first_event_return,
                     int *first_error_return)
{
    (void) display;
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

    LOG("Ignoring unsupported extension probe: %s\n", name ? name : "(null)");
    if (major_opcode_return) {
        *major_opcode_return = 0;
    }
    if (first_event_return) {
        *first_event_return = 0;
    }
    if (first_error_return) {
        *first_error_return = 0;
    }
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
 * _XExtension. The differences are: (1) hook type, (2) struct member, and
 * (3) function name -- everything else is identical. */
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
        if (ext->close_display) {
            ext->close_display(display, &ext->codes);
        }
        free(ext->name);
        free(ext);
        ext = next;
    }
    display->ext_procs = NULL;
}
