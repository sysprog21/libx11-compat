/* libx11-compat XTest extension public surface.
 *
 * Mirrors the API from libXtst (xorg). On real X11, XTest is implemented
 * server-side and an external client uses it to inject input events that
 * the server then distributes to every connected client. libx11-compat is
 * single-process (no IPC), so XTest here injects directly into the
 * calling process's SDL event queue, which is the same queue the local
 * vw / Motif application drains. Functionally equivalent: the fake event
 * is indistinguishable from a real user click on the way through
 * convertEvent().
 */

#ifndef _XTEST_H_
#define _XTEST_H_

#include <X11/Xlib.h>
#include <X11/Xfuncproto.h>
#include <X11/extensions/xtestconst.h>

/* Forward-declare XDevice as opaque. Callers wanting device-aware
 * XTestFake* variants must include XInput's <X11/extensions/XInput.h>
 * themselves. libx11-compat treats device-event entry points as stubs.
 * Guard against the typedef colliding with XInput.h, which defines the
 * same name. The xorgproto convention is to gate it on _XTYPEDEF_XDEVICE.
 */
#ifndef _XTYPEDEF_XDEVICE
#define _XTYPEDEF_XDEVICE
struct _XDevice;
typedef struct _XDevice XDevice;
#endif

_XFUNCPROTOBEGIN

extern Bool XTestQueryExtension(Display *display,
                                int *event_basep,
                                int *error_basep,
                                int *majorp,
                                int *minorp);

extern Bool XTestCompareCursorWithWindow(Display *display,
                                         Window window,
                                         Cursor cursor);

extern Bool XTestCompareCurrentCursorWithWindow(Display *display,
                                                Window window);

extern int XTestFakeKeyEvent(Display *display,
                             unsigned int keycode,
                             Bool is_press,
                             unsigned long delay);

extern int XTestFakeButtonEvent(Display *display,
                                unsigned int button,
                                Bool is_press,
                                unsigned long delay);

extern int XTestFakeMotionEvent(Display *display,
                                int screen_number,
                                int x,
                                int y,
                                unsigned long delay);

extern int XTestFakeRelativeMotionEvent(Display *display,
                                        int x,
                                        int y,
                                        unsigned long delay);

extern int XTestFakeDeviceKeyEvent(Display *display,
                                   XDevice *dev,
                                   unsigned int keycode,
                                   Bool is_press,
                                   int *axes,
                                   int n_axes,
                                   unsigned long delay);

extern int XTestFakeDeviceButtonEvent(Display *display,
                                      XDevice *dev,
                                      unsigned int button,
                                      Bool is_press,
                                      int *axes,
                                      int n_axes,
                                      unsigned long delay);

extern int XTestFakeProximityEvent(Display *display,
                                   XDevice *dev,
                                   Bool in_prox,
                                   int *axes,
                                   int n_axes,
                                   unsigned long delay);

extern int XTestFakeDeviceMotionEvent(Display *display,
                                      XDevice *dev,
                                      Bool is_relative,
                                      int first_axis,
                                      int *axes,
                                      int n_axes,
                                      unsigned long delay);

extern int XTestGrabControl(Display *display, Bool impervious);

extern void XTestSetGContextOfGC(GC gc, GContext gid);
extern void XTestSetVisualIDOfVisual(Visual *visual, VisualID visualid);
extern Status XTestDiscard(Display *display);

_XFUNCPROTOEND

#endif /* _XTEST_H_ */
