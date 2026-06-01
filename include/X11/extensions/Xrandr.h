#ifndef _XRANDR_H_
#define _XRANDR_H_

#include <X11/Xlib.h>

extern Bool XRRQueryExtension(Display *dpy,
                              int *event_base_return,
                              int *error_base_return);

#endif /* _XRANDR_H_ */
