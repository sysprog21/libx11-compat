/* Span composition helpers
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef PATH_COMPOSE_H
#define PATH_COMPOSE_H

#include "sdl-compat.h"
#include <X11/Xlib.h>

#include "rasterize.h"

Bool pathComposeSpansToBuffer(Uint32 *buffer,
                              int width,
                              int height,
                              const PathSpanList *spans,
                              unsigned long color);

#endif /* PATH_COMPOSE_H */
