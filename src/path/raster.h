/* Path raster entry points
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef PATH_RASTER_H
#define PATH_RASTER_H

#include <SDL2/SDL.h>
#include <X11/Xlib.h>

#include "path.h"

/* Drawable d is the destination of the rasterized path. The raster
 * helpers forward it into setGcClipForIteration so sibling occlusion
 * applies inside path renders just like in the primitive paths.
 * Callers may pass None / a pixmap when no sibling clipping is
 * required.
 */
Bool rasterFillPathOnRenderer(SDL_Renderer *renderer,
                              Drawable d,
                              GC gc,
                              const Path *path);
Bool rasterFillPathOnRendererWithOptions(SDL_Renderer *renderer,
                                         Drawable d,
                                         GC gc,
                                         const Path *path,
                                         Bool coalesceWithPixman);
Bool rasterStrokePathOnRenderer(SDL_Renderer *renderer,
                                Drawable d,
                                GC gc,
                                const Path *path);

#endif /* PATH_RASTER_H */
