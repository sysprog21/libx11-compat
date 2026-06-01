/* Span composition helpers for libx11-compat
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef PATH_COMPOSE_H
#define PATH_COMPOSE_H

#include <SDL2/SDL.h>
#include <X11/Xlib.h>

#include "rasterize.h"

Bool pathComposeSpansToBuffer(Uint32 *buffer,
                              int width,
                              int height,
                              const PathSpanList *spans,
                              unsigned long color);

#endif /* PATH_COMPOSE_H */
