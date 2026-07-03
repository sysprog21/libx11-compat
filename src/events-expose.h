/*
 * Expose-event helpers shared between events.c and events-expose.c
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

#ifndef EVENTS_EXPOSE_H
#define EVENTS_EXPOSE_H

#include <X11/Xlib.h>

void clearWindowTreeWithoutExpose(Display *display, Window window);
void postFullWindowExpose(Display *display, Window window);

#endif /* EVENTS_EXPOSE_H */
