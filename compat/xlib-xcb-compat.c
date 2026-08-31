#include <X11/Xlib-xcb.h>
#include "xcb-compat-private.h"

xcb_connection_t *XGetXCBConnection(Display *display)
{
    return xcbCompatConnectionForDisplay(display);
}

void XSetEventQueueOwner(Display *display, enum XEventQueueOwner owner)
{
    xcbCompatSetQueueOwner(xcbCompatConnectionForDisplay(display), owner);
}
