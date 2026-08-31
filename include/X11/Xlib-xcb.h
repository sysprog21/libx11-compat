/* Minimal libX11-xcb compatibility API. */
#ifndef _X11_XLIB_XCB_H_
#define _X11_XLIB_XCB_H_
#include <X11/Xfuncproto.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
_XFUNCPROTOBEGIN
xcb_connection_t *XGetXCBConnection(Display *display);
enum XEventQueueOwner { XlibOwnsEventQueue = 0, XCBOwnsEventQueue };
void XSetEventQueueOwner(Display *display, enum XEventQueueOwner owner);
_XFUNCPROTOEND
#endif
