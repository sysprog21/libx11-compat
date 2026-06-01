#include "errors.h"
#include <stdio.h>
#include "display.h"

typedef int (*errorHandlerFunction)(Display *, XErrorEvent *);
errorHandlerFunction error_handler = defaultErrorHandler;

errorHandlerFunction XSetErrorHandler(errorHandlerFunction handler)
{
    // https://tronche.com/gui/x/xlib/event-handling/protocol-errors/XSetErrorHandler.html
    errorHandlerFunction prev_error_handler = error_handler;
    if (!handler) {
        handler = defaultErrorHandler;
    }
    error_handler = handler;
    return prev_error_handler;
}

int defaultErrorHandler(Display *display, XErrorEvent *event)
{
    (void) display;
    static const struct {
        unsigned char code;
        const char *format;
    } errorMessages[] = {
        {BadAlloc,
         "Out of memory: Failed to allocate memory while processing "
         "request : %d\n"},
        {BadMatch,
         "Parameter mismatch: A parameter was not valid for request %d!\n"},
        {BadValue,
         "Parameter mismatch: An int parameter was out of range for "
         "request %d!\n"},
        {BadGC,
         "Parameter mismatch: A parameter was not a GC for request %d!\n"},
        {BadDrawable,
         "Parameter mismatch: A parameter was not a Drawable for "
         "request %d!\n"},
        {BadPixmap,
         "Parameter mismatch: A parameter was not a Pixmap for request %d!\n"},
        {BadWindow,
         "Parameter mismatch: A parameter was not a Window for request %d!\n"},
        {BadName, "Parameter invalid: Got an unknown name for request %d!\n"},
        {BadAtom, "Parameter invalid: Got an invalid Atom for request %d!\n"},
        {BadFont,
         "Parameter mismatch: A parameter was not a Font for request %d!\n"},
        {BadCursor,
         "Parameter mismatch: A parameter was not a Cursor for request %d!\n"},
        {BadColor,
         "Parameter invalid: A parameter did not name a defined "
         "color for request %d!\n"},
    };
    const char *format = NULL;
    for (size_t i = 0; i < sizeof(errorMessages) / sizeof(errorMessages[0]);
         i++) {
        if (errorMessages[i].code == event->error_code) {
            format = errorMessages[i].format;
            break;
        }
    }
    if (format) {
        fprintf(stderr, format, event->request_code);
    } else {
        fprintf(stderr, "An unknown error occurred for request %d: %u\n",
                event->request_code, event->error_code);
    }
    fflush(stdout);
    fflush(stderr);
    abort();
}

inline void handleOutOfMemory(int type,
                              Display *display,
                              unsigned long serial,
                              unsigned char minor_code)
{
    handleError(type, display, None, serial, BadAlloc, minor_code);
}
void handleError(int type,
                 Display *display,
                 XID resourceId,
                 unsigned long serial,
                 unsigned char error_code,
                 unsigned char minor_code)
{
    XErrorEvent event;
    event.type = type;
    event.display = display;
    event.resourceid = resourceId;
    event.serial = serial;
    event.error_code = error_code;
    event.request_code = (unsigned char) GET_DISPLAY(display)->request;
    event.minor_code = minor_code;
    error_handler(display, &event);
}

unsigned char resourceTypeToErrorCode(XResourceType resourceType)
{
    switch (resourceType) {
    case WINDOW:
        return BadWindow;
    case DRAWABLE:
        return BadDrawable;
    case PIXMAP:
        return BadPixmap;
    case GRAPHICS_CONTEXT:
        return BadGC;
    case FONT:
        return BadFont;
    case CURSOR:
        return BadCursor;
    case COLORMAP:
        return BadColor;
    default:
        return BadMatch;
    }
}
