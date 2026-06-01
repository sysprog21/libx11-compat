#ifndef _XDAMAGE_H_
#define _XDAMAGE_H_

#include <X11/Xlib.h>

extern Bool XDamageQueryExtension(Display *dpy,
                                  int *event_base_return,
                                  int *error_base_return);

#endif /* _XDAMAGE_H_ */
