#ifndef _LIBX11_COMPAT_REPLAY_H_
#define _LIBX11_COMPAT_REPLAY_H_

#include <X11/Xlib.h>

/* If $LIBX11_COMPAT_REPLAY names a script file, spawn a background thread that
 * injects its contents via XTest after the first top-level window is mapped.
 * Idempotent: only the first invocation arms the thread. Pass the Display the
 * script should target.
 */
void replayStartIfRequested(Display *display);

/* Signal the replay thread to exit at the next command boundary. Called from
 * XCloseDisplay so a script with queued delay commands cannot keep firing into
 * a torn-down Display.
 */
void replayStop(void);

#endif
