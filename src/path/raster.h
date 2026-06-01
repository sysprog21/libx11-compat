/* Path raster entry points for libx11-compat
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef PATH_RASTER_H
#define PATH_RASTER_H

#include <SDL2/SDL.h>
#include <X11/Xlib.h>

#include "path.h"

Bool rasterFillPathOnRenderer(SDL_Renderer *renderer, GC gc, const Path *path);
Bool rasterFillPathOnRendererWithOptions(SDL_Renderer *renderer,
                                         GC gc,
                                         const Path *path,
                                         Bool coalesceWithPixman);
Bool rasterStrokePathOnRenderer(SDL_Renderer *renderer,
                                GC gc,
                                const Path *path);

#endif /* PATH_RASTER_H */
