#ifndef LIBX11_COMPAT_XMU_STDCMAP_H
#define LIBX11_COMPAT_XMU_STDCMAP_H

#include <X11/Xfuncproto.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

_XFUNCPROTOBEGIN
Status XmuLookupStandardColormap(Display *dpy,
                                 int screen,
                                 VisualID visualid,
                                 unsigned int depth,
                                 Atom property,
                                 Bool replace,
                                 Bool retain);
_XFUNCPROTOEND

#endif
