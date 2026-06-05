#ifndef LIBX11_COMPAT_XMU_WINUTIL_H
#define LIBX11_COMPAT_XMU_WINUTIL_H

#include <X11/Xfuncproto.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

_XFUNCPROTOBEGIN
Window XmuClientWindow(Display *dpy, Window win);
Bool XmuUpdateMapHints(Display *dpy, Window win, XSizeHints *hints);
Screen *XmuScreenOfWindow(Display *dpy, Window w);
_XFUNCPROTOEND

#endif
