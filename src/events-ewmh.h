/* EWMH ClientMessage dispatch: internal declarations
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef EVENTS_EWMH_H
#define EVENTS_EWMH_H

#include <X11/Xlib.h>

/* Route a root+ClientMessage EWMH wire-protocol request to its in-process SDL
 * handler. Returns True when the message was recognized and consumed; False
 * when callers should fall through to the regular acceptsEventMask walk.
 */
Bool dispatchRootEwmhClientMessage(Display *display, XEvent *event);

#endif /* EVENTS_EWMH_H */
