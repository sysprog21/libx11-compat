#ifndef _XFIXES_H_
#define _XFIXES_H_

#include <X11/Xlib.h>

extern Bool XFixesQueryExtension(Display *dpy,
                                 int *event_base_return,
                                 int *error_base_return);

#endif /* _XFIXES_H_ */
