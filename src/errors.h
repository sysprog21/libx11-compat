#ifndef _ERRORS_H_
#define _ERRORS_H_

#include "X11/Xlib.h"
#include "X11/Xproto.h"
#include "X11/Xutil.h"
#include "resource-types.h"

int defaultErrorHandler(Display *display, XErrorEvent *event);
void handleError(int type,
                 Display *display,
                 XID resourceId,
                 unsigned long serial,
                 unsigned char error_code,
                 unsigned char minor_code);
void handleOutOfMemory(int type,
                       Display *display,
                       unsigned long serial,
                       unsigned char minor_code);
unsigned char resourceTypeToErrorCode(XResourceType resourceType);

/* Drop the per-Display lastRequestCode entry; called from XCloseDisplay. */
void releaseLastRequestCode(Display *display);

/* Drain remaining lastRequestCode entries and destroy the side-table
 * mutex; called from XCloseDisplay's last-display-closing branch BEFORE
 * SDL_Quit so the SDL_mutex teardown reaches a still-valid SDL.
 */
void freeLastRequestStorage(void);

/* Fatal-disconnect path: invokes the client-installed XIOErrorHandler if
 * any, then exits. Used when the window manager closes a window the client
 * has not opted in to handle, or when the host signals quit. Does not
 * return.
 */
void triggerIOError(Display *display);

#endif /* _ERRORS_H_ */
