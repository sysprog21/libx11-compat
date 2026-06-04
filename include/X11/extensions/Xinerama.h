#ifndef LIBX11_COMPAT_XINERAMA_H
#define LIBX11_COMPAT_XINERAMA_H

#include <X11/Xlib.h>
#include <X11/Xfuncproto.h>

typedef struct {
    int screen_number;
    short x_org;
    short y_org;
    short width;
    short height;
} XineramaScreenInfo;

_XFUNCPROTOBEGIN
Bool XineramaQueryExtension(Display *dpy,
                            int *event_base_return,
                            int *error_base_return);
Status XineramaQueryVersion(Display *dpy,
                            int *major_version_return,
                            int *minor_version_return);
Bool XineramaIsActive(Display *dpy);
XineramaScreenInfo *XineramaQueryScreens(Display *dpy, int *number_return);
_XFUNCPROTOEND

#endif /* LIBX11_COMPAT_XINERAMA_H */
