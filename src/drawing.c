#include <limits.h>
#include <stdint.h>
#include "X11/Xlib.h"
#include "drawing.h"
#include "errors.h"
#include "resource-types.h"
#include "window.h"
#include "display.h"
#include "util.h"
#include "gc.h"
#include "colors.h"
#include "events.h"
#include "path/raster.h"
#include <math.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void drawWindowDataToScreen()
{
    SDL_Renderer *screen = GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
    if (!screen)
        return;

    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    SDL_Texture *prevTarget = SDL_GetRenderTarget(screen);
    Bool screenTargetMutated = False;
    size_t i;
    for (i = 0; i < GET_WINDOW_STRUCT(SCREEN_WINDOW)->children.length; i++) {
        WindowStruct *child = GET_WINDOW_STRUCT(children[i]);
        if (!child->sdlWindow)
            continue;

        /* A window can enter Mapped state without ever drawing, so its backing
         * texture is still NULL. Triggering getWindowRenderer here lazily
         * allocates and background-fills the texture so the present pipeline
         * never silently skips a mapped window.
         */
        if (!child->sdlTexture) {
            (void) getWindowRenderer(children[i]);
            screenTargetMutated = True;
            if (!child->sdlTexture)
                continue;
        }

        SDL_Surface *winSurface = SDL_GetWindowSurface(child->sdlWindow);
        if (!winSurface) {
            LOG("SDL_GetWindowSurface failed in %s: %s\n", __func__,
                SDL_GetError());
            continue;
        }

        int w, h;
        GET_WINDOW_DIMS(children[i], w, h);
        /* Clamp the readback rect to all three of the X11 logical size, the
         * backing texture size, and the SDL window surface size. Window surface
         * and X11 logical w/h can diverge during resize and on high-DPI setups;
         * an unclamped read would overflow winSurface->pixels.
         */
        int texW = 0, texH = 0;
        SDL_QueryTexture(child->sdlTexture, NULL, NULL, &texW, &texH);
        int rw = w < texW ? w : texW;
        int rh = h < texH ? h : texH;
        if (rw > winSurface->w)
            rw = winSurface->w;
        if (rh > winSurface->h)
            rh = winSurface->h;
        if (rw <= 0 || rh <= 0)
            continue;
        if (SDL_SetRenderTarget(screen, child->sdlTexture) != 0) {
            LOG("SDL_SetRenderTarget(backing) failed in %s: %s\n", __func__,
                SDL_GetError());
            continue;
        }

        screenTargetMutated = True;
        Uint32 winFmt = winSurface->format->format;
        Uint32 readFmt = SDL_PIXELFORMAT_RGBA8888;
        SDL_Rect r = {
            .x = 0,
            .y = 0,
            .w = rw,
            .h = rh,
        };
        int readRc = -1;
        if (winFmt == readFmt) {
            /* Direct readback into the window's framebuffer. Saves one
             * full-image memcpy on every present.
             */
            if (SDL_LockSurface(winSurface) == 0) {
                readRc = SDL_RenderReadPixels(
                    screen, &r, readFmt, winSurface->pixels, winSurface->pitch);
                SDL_UnlockSurface(winSurface);
            }
        } else {
            SDL_Surface *staging =
                SDL_CreateRGBSurfaceWithFormat(0, rw, rh, 32, readFmt);
            if (staging) {
                readRc = SDL_RenderReadPixels(screen, &r, readFmt,
                                              staging->pixels, staging->pitch);
                if (readRc == 0) {
                    SDL_BlitSurface(staging, NULL, winSurface, NULL);
                }
                SDL_FreeSurface(staging);
            }
        }
        if (readRc == 0) {
            SDL_UpdateWindowSurface(child->sdlWindow);
        } else {
            LOG("SDL_RenderReadPixels failed in %s: %s\n", __func__,
                SDL_GetError());
        }
    }
    if (screenTargetMutated) {
        SDL_SetRenderTarget(screen, prevTarget);
        invalidateSdlDrawStateCache();
    }
#ifdef DEBUG_WINDOWS
    printWindowsHierarchy();
#endif
}

SDL_Renderer *getWindowRenderer(Window window)
{
    Window drawWindow = window;
    SDL_Rect viewPort;
    SDL_Renderer *renderer = NULL;
    int x = 0, y = 0;
    viewPort.x = 0;
    viewPort.y = 0;

    GET_WINDOW_DIMS(window, viewPort.w, viewPort.h);
    while (GET_PARENT(window) != None &&
           !GET_WINDOW_STRUCT(window)->sdlWindow &&
           GET_WINDOW_STRUCT(window)->mapState != UnMapped) {
        GET_WINDOW_POS(window, x, y);
        viewPort.x += x;
        viewPort.y += y;
        drawWindow = GET_PARENT(drawWindow);
        window = drawWindow;
    }

    renderer = GET_WINDOW_STRUCT(drawWindow)->sdlRenderer;
    Bool justCreatedRenderer = False;
    if (!renderer) {
        /* All windows draw on the SCREEN renderer with a per-window backing
         * texture as the render target. Mapped top-level windows used to own
         * a separate SDL_Renderer attached to their SDL_Window, which forced
         * every XCopyArea between two top-level windows (and every
         * Pixmap->Window copy) into a cross-renderer readback. Unifying on
         * SCREEN renderer keeps those copies in renderer space; presentation
         * to the actual SDL_Window happens in drawWindowDataToScreen.
         */
        renderer = GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
        SDL_Texture *texture = GET_WINDOW_STRUCT(drawWindow)->sdlTexture;
        if (!texture) {
            int w, h;
            GET_WINDOW_DIMS(drawWindow, w, h);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET, w, h);
            if (!texture) {
                LOG("WTF: SDL_CreateTexture failed in %s for window "
                    "0x%08lx: %s\n",
                    __func__, window, SDL_GetError());
#ifdef DEBUG_WINDOWS
                printWindowsHierarchy();
#endif
            } else {
                GET_WINDOW_STRUCT(drawWindow)->sdlTexture = texture;
                justCreatedRenderer = True;
            }
        }
    }

    /* Real X11 paints a freshly mapped window with its background pixel.
     * We allocate the SDL surface/texture lazily, so do the equivalent one-shot
     * fill here to avoid presenting uninitialized memory.
     */
    if (justCreatedRenderer) {
        SDL_Texture *prevTarget = SDL_GetRenderTarget(renderer);
        SDL_Texture *initTarget = GET_WINDOW_STRUCT(drawWindow)->sdlTexture;
        SDL_SetRenderTarget(renderer, initTarget);
        unsigned long bg = GET_WINDOW_STRUCT(drawWindow)->backgroundColor;
        SDL_SetRenderDrawColor(
            renderer, GET_RED_FROM_COLOR(bg), GET_GREEN_FROM_COLOR(bg),
            GET_BLUE_FROM_COLOR(bg), GET_ALPHA_FROM_COLOR(bg));
        SDL_RenderClear(renderer);
        SDL_SetRenderTarget(renderer, prevTarget);

        /* Direct color push above bypassed the (gc, generation) cache; drop the
         * cache so the next applySdlDrawState re-pushes state instead of
         * trusting a stale entry.
         */
        invalidateSdlDrawStateCache();
    }

    if (GET_WINDOW_STRUCT(drawWindow)->sdlTexture) {
        if (SDL_SetRenderTarget(
                renderer, GET_WINDOW_STRUCT(drawWindow)->sdlTexture) != 0) {
            LOG("SDL_SetRenderTarget failed in %s: %s\n", __func__,
                SDL_GetError());
        }
    } else if (IS_MAPPED_TOP_LEVEL_WINDOW(drawWindow) ||
               drawWindow == SCREEN_WINDOW) {
        if (SDL_SetRenderTarget(renderer, NULL) != 0) {
            LOG("SDL_SetRenderTarget(NULL) failed in %s: %s\n", __func__,
                SDL_GetError());
        }
    }
    LOG("Setting viewport to {x = %d, y = %d, w = %d, h = %d}\n", viewPort.x,
        viewPort.y, viewPort.w, viewPort.h);
    if (SDL_RenderSetViewport(renderer, &viewPort)) {
        LOG("SDL_RenderSetViewport failed in %s: %s\n", __func__,
            SDL_GetError());
    }
    return renderer;
}

SDL_Surface *getRenderSurface(SDL_Renderer *renderer)
{
    if (!renderer) {
        LOG("Got NULL renderer in %s\n", __func__);
        return NULL;
    }

    SDL_Rect rect;
    SDL_RenderGetViewport(renderer, &rect);
    SDL_Surface *surface = SDL_CreateRGBSurface(
        0, rect.w, rect.h, SDL_SURFACE_DEPTH, DEFAULT_RED_MASK,
        DEFAULT_GREEN_MASK, DEFAULT_BLUE_MASK, DEFAULT_ALPHA_MASK);
    if (!surface) {
        LOG("SDL_CreateRGBSurface failed in %s: %s\n", __func__,
            SDL_GetError());
        return NULL;
    }
    if (SDL_RenderReadPixels(renderer, &rect, SDL_PIXELFORMAT_RGBA8888,
                             surface->pixels, surface->pitch) != 0) {
        LOG("SDL_RenderReadPixels failed in %s: %s\n", __func__,
            SDL_GetError());
        SDL_FreeSurface(surface);
        return NULL;
    }
    return surface;
}

SDL_Surface *getRenderSurfaceRect(SDL_Renderer *renderer,
                                  const SDL_Rect *source)
{
    if (!renderer || !source || source->w <= 0 || source->h <= 0)
        return NULL;

    SDL_Surface *surface = SDL_CreateRGBSurface(
        0, source->w, source->h, SDL_SURFACE_DEPTH, DEFAULT_RED_MASK,
        DEFAULT_GREEN_MASK, DEFAULT_BLUE_MASK, DEFAULT_ALPHA_MASK);
    if (!surface) {
        LOG("SDL_CreateRGBSurface failed in %s: %s\n", __func__,
            SDL_GetError());
        return NULL;
    }

    SDL_FillRect(surface, NULL, 0);
    /* Clip the requested rect to the viewport. SDL_RenderReadPixels fails
     * outright if the rect leaves the render target, so an X11 caller asking
     * for a drawable-relative area that runs off the edge would otherwise
     * lose all pixels instead of getting the in-bounds portion zero-padded.
     */
    SDL_Rect viewport;
    SDL_RenderGetViewport(renderer, &viewport);
    SDL_Rect viewBounds = {
        .x = 0,
        .y = 0,
        .w = viewport.w,
        .h = viewport.h,
    };
    SDL_Rect clipped;
    if (!SDL_IntersectRect(source, &viewBounds, &clipped))
        return surface;

    SDL_Rect readRect = {
        .x = viewport.x + clipped.x,
        .y = viewport.y + clipped.y,
        .w = clipped.w,
        .h = clipped.h,
    };
    size_t dstOffsetX = (size_t) (clipped.x - source->x);
    size_t dstOffsetY = (size_t) (clipped.y - source->y);
    Uint8 *dstPixels = (Uint8 *) surface->pixels +
                       dstOffsetY * (size_t) surface->pitch +
                       dstOffsetX * (SDL_SURFACE_DEPTH / 8);
    if (SDL_RenderReadPixels(renderer, &readRect, SDL_PIXELFORMAT_RGBA8888,
                             dstPixels, surface->pitch) != 0) {
        LOG("SDL_RenderReadPixels failed in %s: %s\n", __func__,
            SDL_GetError());
        SDL_FreeSurface(surface);
        return NULL;
    }
    return surface;
}

int getGcClipIterationCount(GC gc)
{
    if (!gc)
        return 1;
    GraphicContext *gContext = GET_GC(gc);
    if (!gContext || !gContext->clipRectanglesSet)
        return 1;
    if (gContext->clipRectCount <= 0)
        return 0;
    return gContext->clipRectCount;
}

Bool setGcClipForIteration(SDL_Renderer *renderer, GC gc, int iteration)
{
    if (!renderer)
        return False;
    if (!gc) {
        SDL_RenderSetClipRect(renderer, NULL);
        return True;
    }
    GraphicContext *gContext = GET_GC(gc);
    if (!gContext || !gContext->clipRectanglesSet) {
        SDL_RenderSetClipRect(renderer, NULL);
        return True;
    }
    if (gContext->clipRectCount <= 0)
        return False;
    if (iteration < 0 || iteration >= gContext->clipRectCount)
        return False;
    XRectangle *source = &gContext->clipRects[iteration];
    if (source->width == 0 || source->height == 0)
        return False;
    SDL_Rect clip = {
        .x = gContext->clipOriginX + source->x,
        .y = gContext->clipOriginY + source->y,
        .w = source->width,
        .h = source->height,
    };
    return SDL_RenderSetClipRect(renderer, &clip) == 0 ? True : False;
}

void clearRendererClip(SDL_Renderer *renderer)
{
    if (renderer)
        SDL_RenderSetClipRect(renderer, NULL);
}

static struct {
    Bool valid;
    SDL_Renderer *renderer;
    GraphicContext *gc;
    unsigned long generation;
    SDL_BlendMode blendMode;
    unsigned long color;
} lastDrawState;

void applySdlDrawState(SDL_Renderer *renderer,
                       GC gc,
                       SDL_BlendMode blendMode,
                       unsigned long color)
{
    if (!renderer)
        return;

    GraphicContext *gContext = gc ? GET_GC(gc) : NULL;
    unsigned long generation = gContext ? gContext->generation : 0;
    if (lastDrawState.valid && lastDrawState.renderer == renderer &&
        lastDrawState.gc == gContext &&
        lastDrawState.generation == generation &&
        lastDrawState.blendMode == blendMode && lastDrawState.color == color)
        return;

    SDL_SetRenderDrawBlendMode(renderer, blendMode);
    SDL_SetRenderDrawColor(
        renderer, GET_RED_FROM_COLOR(color), GET_GREEN_FROM_COLOR(color),
        GET_BLUE_FROM_COLOR(color), GET_ALPHA_FROM_COLOR(color));
    lastDrawState.valid = True;
    lastDrawState.renderer = renderer;
    lastDrawState.gc = gContext;
    lastDrawState.generation = generation;
    lastDrawState.blendMode = blendMode;
    lastDrawState.color = color;
}

void invalidateGcStateCache(GC gc)
{
    GraphicContext *gContext = gc ? GET_GC(gc) : NULL;
    if (lastDrawState.valid && lastDrawState.gc == gContext)
        lastDrawState.valid = False;
}

void invalidateSdlDrawStateCache(void)
{
    lastDrawState.valid = False;
}

void putPixel(SDL_Surface *surface,
              unsigned int x,
              unsigned int y,
              Uint32 pixel)
{
    int bytesPerPixel = surface->format->BytesPerPixel;
    Uint8 *p =
        (Uint8 *) surface->pixels + y * surface->pitch + x * bytesPerPixel;
    switch (bytesPerPixel) {
    case 1:
        *p = pixel;
        break;
    case 2:
        *(Uint16 *) p = pixel;
        break;
    case 3:
        if (SDL_BYTEORDER == SDL_BIG_ENDIAN) {
            p[0] = (pixel >> 16) & 0xff;
            p[1] = (pixel >> 8) & 0xff;
            p[2] = pixel & 0xff;
        } else {
            p[0] = pixel & 0xff;
            p[1] = (pixel >> 8) & 0xff;
            p[2] = (pixel >> 16) & 0xff;
        }
        break;
    case 4:
        *(Uint32 *) p = pixel;
        break;
    }
}

Uint32 getPixel(SDL_Surface *surface, unsigned int x, unsigned int y)
{
    int bytesPerPixel = surface->format->BytesPerPixel;
    Uint8 *pointer =
        (Uint8 *) surface->pixels + y * surface->pitch + x * bytesPerPixel;
    switch (bytesPerPixel) {
    case 1:
        return *pointer;
    case 2:
        return *(Uint16 *) pointer;
    case 3:
        if (SDL_BYTEORDER == SDL_BIG_ENDIAN) {
            return pointer[0] << 16 | pointer[1] << 8 | pointer[2];
        } else {
            return pointer[0] | pointer[1] << 8 | pointer[2] << 16;
        }
    case 4:
        return *(Uint32 *) pointer;
    }
    return 0;
}

typedef struct {
    double x;
    int winding;
} PolygonCrossing;

static int compareCrossings(const void *left, const void *right)
{
    const PolygonCrossing *a = left;
    const PolygonCrossing *b = right;
    return (a->x > b->x) - (a->x < b->x);
}

static Bool isConvexPolygon(const SDL_Point *points, int npoints)
{
    long long sign = 0;
    for (int i = 0; i < npoints; i++) {
        SDL_Point a = points[i];
        SDL_Point b = points[(i + 1) % npoints];
        SDL_Point c = points[(i + 2) % npoints];
        long long abx = (long long) b.x - a.x;
        long long aby = (long long) b.y - a.y;
        long long bcx = (long long) c.x - b.x;
        long long bcy = (long long) c.y - b.y;
        long long cross = abx * bcy - aby * bcx;
        if (cross == 0)
            continue;
        if (sign == 0) {
            sign = cross;
        } else if ((sign > 0 && cross < 0) || (sign < 0 && cross > 0)) {
            return False;
        }
    }
    return True;
}

static Bool shouldUsePathArc(GC gc,
                             unsigned int width,
                             unsigned int height,
                             Bool fill)
{
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->function != GXcopy)
        return False;
    if (fill) {
        if (gContext->fillStyle != FillSolid)
            return False;

        /* ArcChord rendering requires honoring the chord/pie distinction,
         * which only the path accelerator does. Force the path even for small
         * arcs to avoid the legacy pie-only fallback.
         */
        if (gContext->arcMode == ArcChord)
            return True;
        return width >= 16 || height >= 16;
    }

    /* Stroke path handles wide lines and dashing; the legacy point-spray loop
     * draws disconnected dots for wide arcs and ignores LineOnOffDash /
     * LineDoubleDash entirely.
     */
    if (gContext->lineWidth > 1)
        return True;
    if (gContext->lineStyle != LineSolid)
        return True;
    return width >= 16 || height >= 16;
}

static Bool strokeLineOnRenderer(SDL_Renderer *renderer,
                                 GC gc,
                                 int x1,
                                 int y1,
                                 int x2,
                                 int y2)
{
    Path path;
    if (!pathInit(&path))
        return False;
    Bool ok = pathMoveTo(&path, x1, y1) && pathLineTo(&path, x2, y2) &&
              rasterStrokePathOnRenderer(renderer, gc, &path);
    pathFree(&path);
    return ok;
}

static Bool appendXArcPath(Path *path,
                           int x,
                           int y,
                           unsigned int width,
                           unsigned int height,
                           int angle1,
                           int angle2,
                           int arcMode)
{
    double rx = width / 2.0;
    double ry = height / 2.0;
    double cx = x + rx;
    double cy = y + ry;
    double startRad = (angle1 / 64.0) * M_PI / 180.0;
    double sweepRad = (angle2 / 64.0) * M_PI / 180.0;
    return pathAddArc(path, cx, cy, rx, ry, startRad, sweepRad, arcMode);
}

static Uint32 applyRasterFunction(int function,
                                  Uint32 planeMask,
                                  Uint32 src,
                                  Uint32 dst)
{
    Uint32 alpha = dst & 0xFF000000u;
    Uint32 colorMask = planeMask & 0x00FFFFFFu;
    Uint32 rgbResult = 0;
    switch (function) {
    case GXclear:
        rgbResult = 0;
        break;
    case GXand:
        rgbResult = src & dst;
        break;
    case GXandReverse:
        rgbResult = src & ~dst;
        break;
    case GXcopy:
        rgbResult = src;
        break;
    case GXandInverted:
        rgbResult = ~src & dst;
        break;
    case GXnoop:
        return dst;
    case GXxor:
        rgbResult = src ^ dst;
        break;
    case GXor:
        rgbResult = src | dst;
        break;
    case GXnor:
        rgbResult = ~(src | dst);
        break;
    case GXequiv:
        rgbResult = ~(src ^ dst);
        break;
    case GXinvert:
        rgbResult = ~dst;
        break;
    case GXorReverse:
        rgbResult = src | ~dst;
        break;
    case GXcopyInverted:
        rgbResult = ~src;
        break;
    case GXorInverted:
        rgbResult = ~src | dst;
        break;
    case GXnand:
        rgbResult = ~(src & dst);
        break;
    case GXset:
        rgbResult = 0x00FFFFFFu;
        break;
    default:
        rgbResult = src;
        break;
    }
    return alpha | (dst & ~colorMask & 0x00FFFFFFu) | (rgbResult & colorMask);
}

static Uint32 colorToArgb8888(unsigned long color)
{
    return ((Uint32) GET_ALPHA_FROM_COLOR(color) << 24) |
           ((Uint32) GET_RED_FROM_COLOR(color) << 16) |
           ((Uint32) GET_GREEN_FROM_COLOR(color) << 8) |
           (Uint32) GET_BLUE_FROM_COLOR(color);
}

static void rasterOpRendererRect(SDL_Renderer *renderer,
                                 const SDL_Rect *rect,
                                 int function,
                                 unsigned long planeMask,
                                 unsigned long sourceColor)
{
    if (!renderer || !rect || rect->w <= 0 || rect->h <= 0)
        return;
    if (function == GXnoop)
        return;

    SDL_Rect viewport;
    SDL_RenderGetViewport(renderer, &viewport);
    SDL_Rect bounds = {
        .x = 0,
        .y = 0,
        .w = viewport.w,
        .h = viewport.h,
    };
    SDL_Rect clipped;
    if (!SDL_IntersectRect(rect, &bounds, &clipped))
        return;

    size_t pitch = (size_t) clipped.w * 4u;
    unsigned char *pixels = malloc(pitch * (size_t) clipped.h);
    if (!pixels)
        return;

    if (SDL_RenderReadPixels(renderer, &clipped, SDL_PIXELFORMAT_ARGB8888,
                             pixels, (int) pitch) == 0) {
        Uint32 src = colorToArgb8888(sourceColor);
        for (int py = 0; py < clipped.h; py++) {
            Uint32 *row = (Uint32 *) (pixels + (size_t) py * pitch);
            for (int px = 0; px < clipped.w; px++) {
                row[px] =
                    applyRasterFunction(function, planeMask, src, row[px]);
            }
        }
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
            pixels, clipped.w, clipped.h, 32, (int) pitch,
            SDL_PIXELFORMAT_ARGB8888);
        if (surface) {
            SDL_Texture *texture =
                SDL_CreateTextureFromSurface(renderer, surface);
            if (texture) {
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
                if (SDL_RenderCopy(renderer, texture, NULL, &clipped) != 0) {
                    LOG("SDL_RenderCopy failed in %s: %s\n", __func__,
                        SDL_GetError());
                }
                SDL_DestroyTexture(texture);
            }
            SDL_FreeSurface(surface);
        }
    }

    free(pixels);
}

static void rasterOpRendererPoint(SDL_Renderer *renderer,
                                  int x,
                                  int y,
                                  int function,
                                  unsigned long planeMask,
                                  unsigned long sourceColor)
{
    SDL_Rect point = {
        .x = x,
        .y = y,
        .w = 1,
        .h = 1,
    };
    rasterOpRendererRect(renderer, &point, function, planeMask, sourceColor);
}

/* int64 math: abs(INT_MIN) and 2*err would overflow at 32-bit width.
 * Fast path reads the line's bounding box once, walks Bresenham mutating the
 * in-memory buffer, and blits back once instead of doing a full
 * SDL_RenderReadPixels round trip per pixel. The per-pixel fallback kicks in
 * for huge bboxes so we don't try to allocate gigabytes for a pathological
 * diagonal.
 */
static void rasterOpRendererLine(SDL_Renderer *renderer,
                                 int x1,
                                 int y1,
                                 int x2,
                                 int y2,
                                 int function,
                                 unsigned long planeMask,
                                 unsigned long sourceColor,
                                 Bool includeFirst)
{
    enum { BATCH_AREA_LIMIT = 1024 * 1024 };
    int xLo = x1 < x2 ? x1 : x2;
    int xHi = x1 < x2 ? x2 : x1;
    int yLo = y1 < y2 ? y1 : y2;
    int yHi = y1 < y2 ? y2 : y1;
    int64_t bboxArea = ((int64_t) xHi - xLo + 1) * ((int64_t) yHi - yLo + 1);

    int64_t dx = llabs((int64_t) x2 - (int64_t) x1);
    int64_t sx = x1 < x2 ? 1 : -1;
    int64_t dy = -llabs((int64_t) y2 - (int64_t) y1);
    int64_t sy = y1 < y2 ? 1 : -1;

    if (bboxArea > BATCH_AREA_LIMIT) {
        int64_t err = dx + dy;
        int64_t px = x1, py = y1;
        Bool first = True;
        for (;;) {
            if (includeFirst || !first) {
                rasterOpRendererPoint(renderer, (int) px, (int) py, function,
                                      planeMask, sourceColor);
            }
            if (px == x2 && py == y2)
                break;
            first = False;
            int64_t e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                px += sx;
            }
            if (e2 <= dx) {
                err += dx;
                py += sy;
            }
        }
        return;
    }

    SDL_Rect viewport;
    SDL_RenderGetViewport(renderer, &viewport);
    SDL_Rect bbox = {
        .x = xLo,
        .y = yLo,
        .w = xHi - xLo + 1,
        .h = yHi - yLo + 1,
    };
    SDL_Rect bounds = {
        .x = 0,
        .y = 0,
        .w = viewport.w,
        .h = viewport.h,
    };
    SDL_Rect clipped;
    if (!SDL_IntersectRect(&bbox, &bounds, &clipped))
        return;

    size_t pitch = (size_t) clipped.w * 4u;
    unsigned char *pixels = malloc(pitch * (size_t) clipped.h);
    if (!pixels)
        return;

    if (SDL_RenderReadPixels(renderer, &clipped, SDL_PIXELFORMAT_ARGB8888,
                             pixels, (int) pitch) == 0) {
        Uint32 src = colorToArgb8888(sourceColor);
        int64_t err = dx + dy;
        int64_t px = x1, py = y1;
        Bool first = True;
        for (;;) {
            if (includeFirst || !first) {
                int relX = (int) px - clipped.x;
                int relY = (int) py - clipped.y;
                if (relX >= 0 && relX < clipped.w && relY >= 0 &&
                    relY < clipped.h) {
                    Uint32 *p = (Uint32 *) (pixels + (size_t) relY * pitch +
                                            (size_t) relX * 4u);
                    *p = applyRasterFunction(function, planeMask, src, *p);
                }
            }
            if (px == x2 && py == y2)
                break;
            first = False;
            int64_t e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                px += sx;
            }
            if (e2 <= dx) {
                err += dx;
                py += sy;
            }
        }
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
            pixels, clipped.w, clipped.h, 32, (int) pitch,
            SDL_PIXELFORMAT_ARGB8888);
        if (surface) {
            SDL_Texture *texture =
                SDL_CreateTextureFromSurface(renderer, surface);
            if (texture) {
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
                if (SDL_RenderCopy(renderer, texture, NULL, &clipped) != 0) {
                    LOG("SDL_RenderCopy failed in %s: %s\n", __func__,
                        SDL_GetError());
                }
                SDL_DestroyTexture(texture);
            }
            SDL_FreeSurface(surface);
        }
    }
    free(pixels);
}

/* Trace a rectangle outline of (w+1) by (h+1) pixels (the X11 spec for
 * XDrawRectangle) under a non-GXcopy GC function. Each pixel must be touched
 * exactly once or raster ops like XOR cancel themselves at the corners; we
 * sequence the four edges so corner pixels appear in exactly one segment.
 */
static void rasterOpRendererRectOutline(SDL_Renderer *renderer,
                                        int x,
                                        int y,
                                        unsigned int width,
                                        unsigned int height,
                                        int function,
                                        unsigned long planeMask,
                                        unsigned long sourceColor)
{
    int xRight = x + (int) width;
    int yBot = y + (int) height;
    if (width == 0 && height == 0) {
        rasterOpRendererPoint(renderer, x, y, function, planeMask, sourceColor);
        return;
    }
    if (width == 0) {
        rasterOpRendererLine(renderer, x, y, x, yBot, function, planeMask,
                             sourceColor, True);
        return;
    }
    if (height == 0) {
        rasterOpRendererLine(renderer, x, y, xRight, y, function, planeMask,
                             sourceColor, True);
        return;
    }
    /* Top: full, including both top corners. */
    rasterOpRendererLine(renderer, x, y, xRight, y, function, planeMask,
                         sourceColor, True);
    /* Right: skip top-right corner (just drawn). */
    rasterOpRendererLine(renderer, xRight, y, xRight, yBot, function, planeMask,
                         sourceColor, False);
    /* Bottom: skip bottom-right corner (just drawn). */
    rasterOpRendererLine(renderer, xRight, yBot, x, yBot, function, planeMask,
                         sourceColor, False);
    /* Left: skip bottom-left corner (just drawn) AND stop one short of the
     * top-left corner (first edge already drew it).
     */
    rasterOpRendererLine(renderer, x, yBot, x, y + 1, function, planeMask,
                         sourceColor, False);
}

static void drawFilledSpan(SDL_Renderer *renderer,
                           const GraphicContext *gContext,
                           int x1,
                           int y,
                           int x2)
{
    if (x1 > x2)
        return;

    if (gContext->fillStyle == FillSolid && gContext->function != GXcopy) {
        SDL_Rect span = {
            .x = x1,
            .y = y,
            .w = x2 - x1 + 1,
            .h = 1,
        };
        rasterOpRendererRect(renderer, &span, gContext->function,
                             gContext->planeMask, gContext->foreground);
        return;
    }

    if (SDL_RenderDrawLine(renderer, x1, y, x2, y) != 0) {
        LOG("SDL_RenderDrawLine failed in %s: %s\n", __func__, SDL_GetError());
    }
}

int XFillPolygon(Display *display,
                 Drawable d,
                 GC gc,
                 XPoint *points,
                 int npoints,
                 int shape,
                 int mode)
{
    // https://tronche.com/gui/x/xlib/graphics/filling-areas/XFillPolygon.html
    SET_X_SERVER_REQUEST(display, X_FillPoly);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (shape != Complex && shape != Nonconvex && shape != Convex) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    if (mode != CoordModeOrigin && mode != CoordModePrevious) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    if (npoints < 3) {
        return 1;
    }

    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }

    enum { POINT_INLINE_BUDGET = 128 };
    SDL_Point inlinePoints[POINT_INLINE_BUDGET];
    SDL_Point *heapPoints = NULL;
    SDL_Point *poly = inlinePoints;
    if (npoints > POINT_INLINE_BUDGET) {
        heapPoints = malloc((size_t) npoints * sizeof(SDL_Point));
        if (!heapPoints) {
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        poly = heapPoints;
    }

    poly[0].x = points[0].x;
    poly[0].y = points[0].y;
    int minY = poly[0].y;
    int maxY = poly[0].y;
    for (int i = 1; i < npoints; i++) {
        poly[i].x = points[i].x;
        poly[i].y = points[i].y;
        if (mode == CoordModePrevious) {
            poly[i].x += poly[i - 1].x;
            poly[i].y += poly[i - 1].y;
        }
        if (poly[i].y < minY)
            minY = poly[i].y;
        if (poly[i].y > maxY)
            maxY = poly[i].y;
    }

    PolygonCrossing *crossings =
        malloc((size_t) npoints * sizeof(PolygonCrossing));
    if (!crossings) {
        free(heapPoints);
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }

    GraphicContext *gContext = GET_GC(gc);
    Bool useConvexFastPath = shape == Convex && isConvexPolygon(poly, npoints);
    if (gContext->fillStyle == FillSolid) {
        LOG("Fill_style is %s\n", "FillSolid");
        if (gContext->function == GXcopy) {
            applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE,
                              gContext->foreground);
        }
    } else if (gContext->fillStyle == FillOpaqueStippled) {
        LOG("Fill_style is %s\n", "FillOpaqueStippled");
        applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE,
                          gContext->background);
    } else {
        LOG("Fill_style is unsupported in %s: %d\n", __func__,
            gContext->fillStyle);
        free(crossings);
        free(heapPoints);
        return 1;
    }

    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        for (int y = minY; y <= maxY; y++) {
            double scanY = y + 0.5;
            int crossingCount = 0;
            for (int i = 0; i < npoints; i++) {
                SDL_Point a = poly[i];
                SDL_Point b = poly[(i + 1) % npoints];
                if (a.y == b.y)
                    continue;
                int edgeMinY = a.y < b.y ? a.y : b.y;
                int edgeMaxY = a.y > b.y ? a.y : b.y;
                if (scanY < edgeMinY || scanY >= edgeMaxY)
                    continue;
                crossings[crossingCount].x = a.x + (scanY - a.y) *
                                                       (double) (b.x - a.x) /
                                                       (double) (b.y - a.y);
                crossings[crossingCount].winding = b.y > a.y ? 1 : -1;
                crossingCount++;
            }
            if (crossingCount < 2)
                continue;
            if (useConvexFastPath) {
                double minX = crossings[0].x;
                double maxX = crossings[0].x;
                for (int i = 1; i < crossingCount; i++) {
                    if (crossings[i].x < minX)
                        minX = crossings[i].x;
                    if (crossings[i].x > maxX)
                        maxX = crossings[i].x;
                }
                int x1 = (int) ceil(minX);
                int x2 = (int) ceil(maxX) - 1;
                drawFilledSpan(renderer, gContext, x1, y, x2);
                continue;
            }
            qsort(crossings, (size_t) crossingCount, sizeof(PolygonCrossing),
                  compareCrossings);
            if (gContext->fillRule == WindingRule) {
                int winding = 0;
                int x1 = 0;
                for (int i = 0; i < crossingCount; i++) {
                    int oldWinding = winding;
                    winding += crossings[i].winding;
                    if (oldWinding == 0 && winding != 0) {
                        x1 = (int) ceil(crossings[i].x);
                    } else if (oldWinding != 0 && winding == 0) {
                        int x2 = (int) ceil(crossings[i].x) - 1;
                        drawFilledSpan(renderer, gContext, x1, y, x2);
                    }
                }
            } else {
                for (int i = 0; i + 1 < crossingCount; i += 2) {
                    int x1 = (int) ceil(crossings[i].x);
                    int x2 = (int) ceil(crossings[i + 1].x) - 1;
                    drawFilledSpan(renderer, gContext, x1, y, x2);
                }
            }
        }
    }
    clearRendererClip(renderer);
    free(crossings);
    free(heapPoints);
    return 1;
}

static int fillArcOnRenderer(SDL_Renderer *renderer,
                             GC gc,
                             int x,
                             int y,
                             unsigned int width,
                             unsigned int height,
                             int angle1,
                             int angle2)
{
    GraphicContext *gContext = GET_GC(gc);
    if (shouldUsePathArc(gc, width, height, True)) {
        Path path;
        if (pathInit(&path)) {
            Bool ok = appendXArcPath(&path, x, y, width, height, angle1, angle2,
                                     gContext->arcMode) &&
                      rasterFillPathOnRenderer(renderer, gc, &path);
            pathFree(&path);
            if (ok)
                return 1;
        }
    }
    SDL_BlendMode blendMode =
        gContext->function == GXcopy ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND;
    applySdlDrawState(renderer, gc, blendMode, gContext->foreground);
    double rx = width / 2.0;
    double ry = height / 2.0;
    if (rx <= 0.0 || ry <= 0.0)
        return 1;
    double cx = x + rx;
    double cy = y + ry;
    double start = angle1 / 64.0;
    double extent = angle2 / 64.0;
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        for (int py = y; py < y + (int) height; py++) {
            for (int px = x; px < x + (int) width; px++) {
                double nx = (px + 0.5 - cx) / rx;
                double ny = (py + 0.5 - cy) / ry;
                if (nx * nx + ny * ny > 1.0)
                    continue;
                double degrees = atan2(-ny, nx) * 180.0 / M_PI;
                if (degrees < 0.0)
                    degrees += 360.0;
                double rel = degrees - start;
                while (rel < 0.0)
                    rel += 360.0;
                Bool inArc =
                    extent >= 0.0 ? rel <= extent : rel >= 360.0 + extent;
                if (fabs(extent) >= 360.0 || inArc)
                    SDL_RenderDrawPoint(renderer, px, py);
            }
        }
    }
    clearRendererClip(renderer);
    return 1;
}

int XFillArc(Display *display,
             Drawable d,
             GC gc,
             int x,
             int y,
             unsigned int width,
             unsigned int height,
             int angle1,
             int angle2)
{
    // https://tronche.com/gui/x/xlib/graphics/filling-areas/XFillArc.html
    SET_X_SERVER_REQUEST(display, X_PolyFillArc);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    SDL_Renderer *renderer;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    return fillArcOnRenderer(renderer, gc, x, y, width, height, angle1, angle2);
}

static int drawArcOnRenderer(SDL_Renderer *renderer,
                             GC gc,
                             int x,
                             int y,
                             unsigned int width,
                             unsigned int height,
                             int angle1,
                             int angle2)
{
    GraphicContext *gContext = GET_GC(gc);
    if (shouldUsePathArc(gc, width, height, False)) {
        Path path;
        if (pathInit(&path)) {
            Bool ok = appendXArcPath(&path, x, y, width, height, angle1, angle2,
                                     -1) &&
                      rasterStrokePathOnRenderer(renderer, gc, &path);
            pathFree(&path);
            if (ok)
                return 1;
        }
    }
    applySdlDrawState(renderer, gc, SDL_BLENDMODE_BLEND, gContext->foreground);
    double rx = width / 2.0;
    double ry = height / 2.0;
    if (rx <= 0.0 || ry <= 0.0)
        return 1;
    double cx = x + rx;
    double cy = y + ry;
    double start = angle1 / 64.0;
    double extent = angle2 / 64.0;
    int steps = (int) ceil(fabs(extent));
    if (steps < 1)
        steps = 1;
    int lineWidth = gContext->lineWidth <= 0 ? 1 : gContext->lineWidth;
    int radius = lineWidth / 2;
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;

        for (int i = 0; i <= steps; i++) {
            double degrees = start + extent * (double) i / (double) steps;
            double radians = degrees * M_PI / 180.0;
            int px = (int) lround(cx + cos(radians) * rx);
            int py = (int) lround(cy - sin(radians) * ry);
            for (int oy = -radius; oy <= radius; oy++) {
                for (int ox = -radius; ox <= radius; ox++) {
                    SDL_RenderDrawPoint(renderer, px + ox, py + oy);
                }
            }
        }
    }
    clearRendererClip(renderer);
    return 1;
}

int XDrawArc(Display *display,
             Drawable d,
             GC gc,
             int x,
             int y,
             unsigned int width,
             unsigned int height,
             int angle1,
             int angle2)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawArc.html
    SET_X_SERVER_REQUEST(display, X_PolyArc);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    SDL_Renderer *renderer;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    return drawArcOnRenderer(renderer, gc, x, y, width, height, angle1, angle2);
}

int XDrawArcs(Display *display, Drawable d, GC gc, XArc *arcs, int n_arcs)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawArcs.html
    SET_X_SERVER_REQUEST(display, X_PolyArc);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (n_arcs <= 0)
        return 1;
    SDL_Renderer *renderer;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    Bool canBatchPath = True;
    for (int i = 0; i < n_arcs; i++) {
        if (!shouldUsePathArc(gc, arcs[i].width, arcs[i].height, False)) {
            canBatchPath = False;
            break;
        }
    }
    if (canBatchPath) {
        Path path;
        if (pathInit(&path)) {
            Bool ok = True;
            for (int i = 0; ok && i < n_arcs; i++) {
                ok = appendXArcPath(&path, arcs[i].x, arcs[i].y, arcs[i].width,
                                    arcs[i].height, arcs[i].angle1,
                                    arcs[i].angle2, -1);
            }
            if (ok)
                ok = rasterStrokePathOnRenderer(renderer, gc, &path);
            pathFree(&path);
            if (ok)
                return 1;
        }
    }
    for (int i = 0; i < n_arcs; i++) {
        if (!drawArcOnRenderer(renderer, gc, arcs[i].x, arcs[i].y,
                               arcs[i].width, arcs[i].height, arcs[i].angle1,
                               arcs[i].angle2)) {
            return 0;
        }
    }
    return 1;
}

int XFillArcs(Display *display, Drawable d, GC gc, XArc *arcs, int n_arcs)
{
    // https://tronche.com/gui/x/xlib/graphics/filling-areas/XFillArcs.html
    SET_X_SERVER_REQUEST(display, X_PolyFillArc);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (n_arcs <= 0)
        return 1;
    SDL_Renderer *renderer;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    for (int i = 0; i < n_arcs; i++) {
        if (!fillArcOnRenderer(renderer, gc, arcs[i].x, arcs[i].y,
                               arcs[i].width, arcs[i].height, arcs[i].angle1,
                               arcs[i].angle2)) {
            return 0;
        }
    }
    return 1;
}

int XCopyPlane(Display *display,
               Drawable src,
               Drawable dest,
               GC gc,
               int src_x,
               int src_y,
               unsigned int width,
               unsigned int height,
               int dest_x,
               int dest_y,
               unsigned long plane)
{
    // https://tronche.com/gui/x/xlib/graphics/XCopyPlane.html
    SET_X_SERVER_REQUEST(display, X_CopyPlane);
    TYPE_CHECK(src, DRAWABLE, display, 0);
    TYPE_CHECK(dest, DRAWABLE, display, 0);
    if (plane == 0 || (plane & (plane - 1)) != 0) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    if (IS_TYPE(src, WINDOW) && IS_INPUT_ONLY(src)) {
        handleError(0, display, src, 0, BadMatch, 0);
        return 0;
    }
    if (IS_TYPE(dest, WINDOW) && IS_INPUT_ONLY(dest)) {
        handleError(0, display, dest, 0, BadMatch, 0);
        return 0;
    }

    /* SDL_Rect and SDL_CreateTexture take signed ints, and the pixel scratch
     * buffer is width * height * sizeof(Uint32) bytes. Reject dimensions that
     * would overflow either signed int or size_t before touching the allocator.
     */
    if (width > (unsigned int) INT_MAX || height > (unsigned int) INT_MAX ||
        (height != 0 && width > SIZE_MAX / sizeof(Uint32) / (size_t) height)) {
        handleError(0, display, dest, 0, BadValue, 0);
        return 0;
    }

    SDL_Renderer *srcRenderer;
    GET_RENDERER(src, srcRenderer);
    if (!srcRenderer) {
        handleError(0, display, src, 0, BadDrawable, 0);
        return 0;
    }

    SDL_Rect srcRect = {
        .x = src_x,
        .y = src_y,
        .w = (int) width,
        .h = (int) height,
    };
    SDL_Surface *srcSurface = getRenderSurfaceRect(srcRenderer, &srcRect);
    if (!srcSurface) {
        handleError(0, display, src, 0, BadMatch, 0);
        return 0;
    }

    SDL_Renderer *destRenderer;
    GET_RENDERER(dest, destRenderer);
    if (!destRenderer) {
        SDL_FreeSurface(srcSurface);
        handleError(0, display, dest, 0, BadDrawable, 0);
        return 0;
    }

    GraphicContext *gContext = GET_GC(gc);
    SDL_PixelFormat *format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    if (!format) {
        SDL_FreeSurface(srcSurface);
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    Uint32 foreground =
        SDL_MapRGBA(format, GET_RED_FROM_COLOR(gContext->foreground),
                    GET_GREEN_FROM_COLOR(gContext->foreground),
                    GET_BLUE_FROM_COLOR(gContext->foreground),
                    GET_ALPHA_FROM_COLOR(gContext->foreground));
    Uint32 background =
        SDL_MapRGBA(format, GET_RED_FROM_COLOR(gContext->background),
                    GET_GREEN_FROM_COLOR(gContext->background),
                    GET_BLUE_FROM_COLOR(gContext->background),
                    GET_ALPHA_FROM_COLOR(gContext->background));
    Uint32 *pixels = malloc((size_t) width * (size_t) height * sizeof(Uint32));
    if (!pixels) {
        SDL_FreeFormat(format);
        SDL_FreeSurface(srcSurface);
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    for (unsigned int y = 0; y < height; y++) {
        for (unsigned int x = 0; x < width; x++) {
            Uint32 srcPixel = getPixel(srcSurface, x, y);
            Uint8 red = 0, green = 0, blue = 0;
            SDL_GetRGB(srcPixel, srcSurface->format, &red, &green, &blue);
            Bool bitSet =
                plane == 1 ? (red || green || blue) : ((srcPixel & plane) != 0);
            pixels[y * width + x] = bitSet ? foreground : background;
        }
    }
    SDL_FreeFormat(format);
    SDL_FreeSurface(srcSurface);

    SDL_Texture *texture =
        SDL_CreateTexture(destRenderer, SDL_PIXELFORMAT_RGBA8888,
                          SDL_TEXTUREACCESS_STATIC, (int) width, (int) height);
    if (!texture) {
        free(pixels);
        handleError(0, display, dest, 0, BadAlloc, 0);
        return 0;
    }
    if (SDL_UpdateTexture(texture, NULL, pixels,
                          (int) (width * sizeof(Uint32))) != 0) {
        SDL_DestroyTexture(texture);
        free(pixels);
        handleError(0, display, dest, 0, BadMatch, 0);
        return 0;
    }
    free(pixels);
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    SDL_Rect destRect = {
        .x = dest_x,
        .y = dest_y,
        .w = (int) width,
        .h = (int) height,
    };
    int clipCount = getGcClipIterationCount(gc);
    int rcCopy = 0;
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(destRenderer, gc, clip))
            continue;
        if (SDL_RenderCopy(destRenderer, texture, NULL, &destRect) != 0) {
            rcCopy = -1;
            break;
        }
    }
    clearRendererClip(destRenderer);
    SDL_DestroyTexture(texture);
    if (rcCopy != 0) {
        handleError(0, display, dest, 0, BadMatch, 0);
        return 0;
    }
    if (gContext->graphicsExposures)
        postEvent(display, dest, NoExpose, X_CopyPlane, 0);
    return 1;
}

int XDrawPoint(Display *display, Drawable d, GC gc, int x, int y)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawPoint.html
    SET_X_SERVER_REQUEST(display, X_PolyPoint);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->function == GXcopy) {
        applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE,
                          gContext->foreground);
    }
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (gContext->function != GXcopy) {
            rasterOpRendererPoint(renderer, x, y, gContext->function,
                                  gContext->planeMask, gContext->foreground);
        } else if (SDL_RenderDrawPoint(renderer, x, y) != 0) {
            clearRendererClip(renderer);
            LOG("SDL_RenderDrawPoint failed in %s: %s\n", __func__,
                SDL_GetError());
            handleError(0, display, d, 0, BadDrawable, 0);
            return 0;
        }
    }
    clearRendererClip(renderer);
    return 1;
}

int XDrawPoints(Display *display,
                Drawable d,
                GC gc,
                XPoint *points,
                int n_points,
                int mode)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawPoints.html
    SET_X_SERVER_REQUEST(display, X_PolyPoint);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (mode != CoordModeOrigin && mode != CoordModePrevious) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    if (n_points <= 0)
        return 1;
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->function == GXcopy) {
        applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE,
                          gContext->foreground);
    }
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        /* int64 accumulator dodges signed overflow on hostile point lists. */
        int64_t px = 0, py = 0;
        for (int i = 0; i < n_points; i++) {
            if (mode == CoordModePrevious && i > 0) {
                px += points[i].x;
                py += points[i].y;
            } else {
                px = points[i].x;
                py = points[i].y;
            }
            if (px < INT_MIN || px > INT_MAX || py < INT_MIN || py > INT_MAX)
                continue;
            if (gContext->function != GXcopy) {
                rasterOpRendererPoint(renderer, (int) px, (int) py,
                                      gContext->function, gContext->planeMask,
                                      gContext->foreground);
            } else if (SDL_RenderDrawPoint(renderer, (int) px, (int) py) != 0) {
                LOG("SDL_RenderDrawPoint failed in %s: %s\n", __func__,
                    SDL_GetError());
            }
        }
    }
    clearRendererClip(renderer);
    return 1;
}

int XDrawSegments(Display *display,
                  Drawable d,
                  GC gc,
                  XSegment *segments,
                  int nsegments)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawSegments.html
    SET_X_SERVER_REQUEST(display, X_PolySegment);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (nsegments <= 0) {
        return 1;
    }
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->function == GXcopy && gContext->lineStyle != LineSolid) {
        Bool ok = True;
        for (int i = 0; ok && i < nsegments; i++)
            ok = strokeLineOnRenderer(renderer, gc, segments[i].x1,
                                      segments[i].y1, segments[i].x2,
                                      segments[i].y2);
        if (ok)
            return 1;
        return 0;
    }
    if (gContext->function == GXcopy && gContext->lineWidth > 1) {
        Path path;
        if (pathInit(&path)) {
            Bool ok = True;
            for (int i = 0; ok && i < nsegments; i++) {
                ok = pathMoveTo(&path, segments[i].x1, segments[i].y1) &&
                     pathLineTo(&path, segments[i].x2, segments[i].y2);
            }
            if (ok)
                ok = rasterStrokePathOnRenderer(renderer, gc, &path);
            pathFree(&path);
            if (ok)
                return 1;
        }
    }
    if (gContext->function != GXcopy) {
        int clipCount = getGcClipIterationCount(gc);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip))
                continue;
            for (int i = 0; i < nsegments; i++) {
                rasterOpRendererLine(renderer, segments[i].x1, segments[i].y1,
                                     segments[i].x2, segments[i].y2,
                                     gContext->function, gContext->planeMask,
                                     gContext->foreground, True);
            }
        }
        clearRendererClip(renderer);
        return 1;
    }
    applySdlDrawState(renderer, gc, SDL_BLENDMODE_BLEND, gContext->foreground);
    /* SDL_RenderDrawLines would connect the segments; XSegment is disjoint. */
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        for (int i = 0; i < nsegments; i++) {
            if (SDL_RenderDrawLine(renderer, segments[i].x1, segments[i].y1,
                                   segments[i].x2, segments[i].y2) != 0) {
                LOG("SDL_RenderDrawLine failed in %s: %s\n", __func__,
                    SDL_GetError());
            }
        }
    }
    clearRendererClip(renderer);
    return 1;
}

int XDrawLine(Display *display,
              Drawable d,
              GC gc,
              int x1,
              int y1,
              int x2,
              int y2)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawLine.html
    SET_X_SERVER_REQUEST(display, X_PolyLine);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->function == GXcopy &&
        (gContext->lineWidth > 1 || gContext->lineStyle != LineSolid) &&
        strokeLineOnRenderer(renderer, gc, x1, y1, x2, y2)) {
        return 1;
    }
    if (gContext->function == GXcopy) {
        applySdlDrawState(renderer, gc, SDL_BLENDMODE_BLEND,
                          gContext->foreground);
    }
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (gContext->function != GXcopy) {
            rasterOpRendererLine(renderer, x1, y1, x2, y2, gContext->function,
                                 gContext->planeMask, gContext->foreground,
                                 True);
        } else if (SDL_RenderDrawLine(renderer, x1, y1, x2, y2) != 0) {
            LOG("SDL_RenderDrawLine failed in %s: %s\n", __func__,
                SDL_GetError());
        }
    }
    clearRendererClip(renderer);
    return 1;
}

int XDrawLines(Display *display,
               Drawable d,
               GC gc,
               XPoint *points,
               int npoints,
               int mode)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawLines.html
    SET_X_SERVER_REQUEST(display, X_PolyLine);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    LOG("%s: Drawing on 0x%08lx\n", __func__, d);
    if (npoints <= 1) {
        LOG("Invalid number of points in %s: %d\n", __func__, npoints);
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    if (mode != CoordModeOrigin && mode != CoordModePrevious) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }

    /* The X protocol allows arbitrarily large point counts. Stack-allocating an
     * SDL_Point[npoints] for untrusted input is a stack-overflow path, so fall
     * back to the heap once the request exceeds the small inline budget.
     */
    enum { POINT_INLINE_BUDGET = 128 };
    SDL_Point inlinePoints[POINT_INLINE_BUDGET];
    SDL_Point *heapPoints = NULL;
    SDL_Point *sdlPoints = inlinePoints;
    if (npoints > POINT_INLINE_BUDGET) {
        heapPoints = malloc((size_t) npoints * sizeof(SDL_Point));
        if (!heapPoints) {
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        sdlPoints = heapPoints;
    }
    sdlPoints[0].x = (int) points[0].x;
    sdlPoints[0].y = (int) points[0].y;
    for (int i = 1; i < npoints; i++) {
        sdlPoints[i].x = (int) points[i].x;
        sdlPoints[i].y = (int) points[i].y;
        if (mode == CoordModePrevious) {
            sdlPoints[i].x += sdlPoints[i - 1].x;
            sdlPoints[i].y += sdlPoints[i - 1].y;
        }
    }
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->function == GXcopy &&
        (gContext->lineWidth > 1 || gContext->lineStyle != LineSolid)) {
        Path path;
        if (pathInit(&path)) {
            Bool ok = pathMoveTo(&path, sdlPoints[0].x, sdlPoints[0].y);
            for (int i = 1; ok && i < npoints; i++)
                ok = pathLineTo(&path, sdlPoints[i].x, sdlPoints[i].y);
            if (ok)
                ok = rasterStrokePathOnRenderer(renderer, gc, &path);
            pathFree(&path);
            if (ok) {
                free(heapPoints);
                return 1;
            }
        }
    }
    if (gContext->function != GXcopy) {
        int clipCount = getGcClipIterationCount(gc);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip))
                continue;
            for (int i = 0; i + 1 < npoints; i++) {
                rasterOpRendererLine(renderer, sdlPoints[i].x, sdlPoints[i].y,
                                     sdlPoints[i + 1].x, sdlPoints[i + 1].y,
                                     gContext->function, gContext->planeMask,
                                     gContext->foreground, i == 0);
            }
        }
        clearRendererClip(renderer);
        free(heapPoints);
        return 1;
    }
    applySdlDrawState(renderer, gc, SDL_BLENDMODE_BLEND, gContext->foreground);
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (SDL_RenderDrawLines(renderer, &sdlPoints[0], npoints)) {
            LOG("SDL_RenderDrawLines failed in %s: %s\n", __func__,
                SDL_GetError());
        }
    }
    clearRendererClip(renderer);
    free(heapPoints);
    return 1;
}

int XClearArea(register Display *dpy,
               Window w,
               int x,
               int y,
               unsigned int width,
               unsigned int height,
               Bool exposures)
{
    // https://tronche.com/gui/x/xlib/graphics/XClearArea.html
    SET_X_SERVER_REQUEST(dpy, X_ClearArea);
    TYPE_CHECK(w, WINDOW, dpy, 0);
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(w);
    if (IS_INPUT_ONLY(w)) {
        handleError(0, dpy, w, 0, BadMatch, 0);
        return 0;
    }

    int winWidth, winHeight;
    GET_WINDOW_DIMS(w, winWidth, winHeight);
    int clearX = x;
    int clearY = y;
    int clearWidth = width == 0 ? winWidth - x : (int) width;
    int clearHeight = height == 0 ? winHeight - y : (int) height;

    if (clearX < 0) {
        clearWidth += clearX;
        clearX = 0;
    }
    if (clearY < 0) {
        clearHeight += clearY;
        clearY = 0;
    }
    if (clearX >= winWidth || clearY >= winHeight || clearWidth <= 0 ||
        clearHeight <= 0) {
        return 1;
    }
    if (clearX + clearWidth > winWidth) {
        clearWidth = winWidth - clearX;
    }
    if (clearY + clearHeight > winHeight) {
        clearHeight = winHeight - clearY;
    }

    SDL_Renderer *renderer = NULL;
    GET_RENDERER(w, renderer);
    if (!renderer) {
        handleError(0, dpy, w, 0, BadDrawable, 0);
        return 0;
    }

    SDL_Rect clearRect = {
        .x = clearX,
        .y = clearY,
        .w = clearWidth,
        .h = clearHeight,
    };
    Pixmap backgroundPixmap = windowStruct->background;
    unsigned long backgroundColor = windowStruct->backgroundColor;
    if (backgroundPixmap == (Pixmap) ParentRelative && GET_PARENT(w) != None) {
        backgroundPixmap = GET_WINDOW_STRUCT(GET_PARENT(w))->background;
        backgroundColor = GET_WINDOW_STRUCT(GET_PARENT(w))->backgroundColor;
    }
    if (backgroundPixmap != None &&
        backgroundPixmap != (Pixmap) ParentRelative &&
        IS_TYPE(backgroundPixmap, PIXMAP)) {
        SDL_Texture *background = GET_PIXMAP_TEXTURE(backgroundPixmap);
        int textureWidth = 0;
        int textureHeight = 0;
        if (background &&
            SDL_QueryTexture(background, NULL, NULL, &textureWidth,
                             &textureHeight) == 0 &&
            textureWidth > 0 && textureHeight > 0) {
            for (int tileY = clearRect.y; tileY < clearRect.y + clearRect.h;
                 tileY += textureHeight) {
                for (int tileX = clearRect.x; tileX < clearRect.x + clearRect.w;
                     tileX += textureWidth) {
                    SDL_Rect dest = {
                        .x = tileX,
                        .y = tileY,
                        .w = textureWidth,
                        .h = textureHeight,
                    };
                    SDL_IntersectRect(&dest, &clearRect, &dest);
                    SDL_Rect src = {
                        .x = 0,
                        .y = 0,
                        .w = dest.w,
                        .h = dest.h,
                    };
                    SDL_RenderCopy(renderer, background, &src, &dest);
                }
            }
        }
    } else {
        applySdlDrawState(renderer, NULL, SDL_BLENDMODE_NONE, backgroundColor);
        SDL_RenderFillRect(renderer, &clearRect);
    }

    if (exposures) {
        SDL_Rect exposeRect = {
            .x = clearX,
            .y = clearY,
            .w = clearWidth,
            .h = clearHeight,
        };
        postExposeEvent(dpy, w, &exposeRect, 1);
    }

    return 1;
}

int XCopyArea(Display *display,
              Drawable src,
              Drawable dest,
              GC gc,
              int src_x,
              int src_y,
              unsigned int width,
              unsigned int height,
              int dest_x,
              int dest_y)
{
    // https://tronche.com/gui/x/xlib/graphics/XCopyArea.html
    SET_X_SERVER_REQUEST(display, X_CopyArea);
    TYPE_CHECK(src, DRAWABLE, display, 0);
    TYPE_CHECK(dest, DRAWABLE, display, 0);
    LOG("%s: Copy area from 0x%08lx to 0x%08lx\n", __func__, src, dest);
    if (IS_TYPE(src, WINDOW)) {
        if (IS_INPUT_ONLY(src)) {
            LOG("BadMatch: Got input only window as the source in %s!\n",
                __func__);
            handleError(0, display, src, 0, BadMatch, 0);
            return 0;
        } else if (GET_WINDOW_STRUCT(src)->mapState == UnMapped &&
                   !GET_WINDOW_STRUCT(src)->sdlTexture) {
            return 0;
        }
    }
    if (IS_TYPE(dest, WINDOW)) {
        if (IS_INPUT_ONLY(dest)) {
            LOG("BadMatch: Got input only window as the destination in %s!\n",
                __func__);
            handleError(0, display, dest, 0, BadMatch, 0);
            return 0;
        }
        SDL_Renderer *destRenderer = getWindowRenderer(dest);
        if (!destRenderer) {
            handleError(0, display, dest, 0, BadDrawable, 0);
            return 0;
        }

        /* Fast path: source is a Pixmap (already an SDL_Texture) and the
         * destination is drawing on the same renderer that owns the pixmap
         * texture. Skip the readback-and-upload dance and let SDL_RenderCopy
         * stay in renderer space.
         */
        if (IS_TYPE(src, PIXMAP)) {
            SDL_Texture *pixmapTexture = GET_PIXMAP_TEXTURE(src);
            SDL_Renderer *pixmapRenderer =
                GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
            if (pixmapTexture && destRenderer == pixmapRenderer) {
                SDL_Rect fastSrc = {
                    .x = src_x,
                    .y = src_y,
                    .w = (int) width,
                    .h = (int) height,
                };
                SDL_Rect fastDest = {
                    .x = dest_x,
                    .y = dest_y,
                    .w = (int) width,
                    .h = (int) height,
                };
                SDL_SetTextureBlendMode(pixmapTexture, SDL_BLENDMODE_NONE);
                int fastClipCount = getGcClipIterationCount(gc);
                int fastRcCopy = 0;
                for (int clip = 0; clip < fastClipCount; clip++) {
                    if (!setGcClipForIteration(destRenderer, gc, clip))
                        continue;
                    if (SDL_RenderCopy(destRenderer, pixmapTexture, &fastSrc,
                                       &fastDest) != 0) {
                        LOG("SDL_RenderCopy failed in %s fast path: %s\n",
                            __func__, SDL_GetError());
                        fastRcCopy = -1;
                        break;
                    }
                }
                clearRendererClip(destRenderer);
                if (fastRcCopy != 0) {
                    handleError(0, display, src, 0, BadMatch, 0);
                    return 0;
                }
                if (GET_GC(gc)->graphicsExposures) {
                    postEvent(display, dest, NoExpose, X_CopyArea, 0);
                }
                return 1;
            }
        }
        SDL_Renderer *srcRenderer;
        GET_RENDERER(src, srcRenderer);
        if (!srcRenderer) {
            handleError(0, display, src, 0, BadDrawable, 0);
            return 0;
        }
        SDL_Rect srcRect = {
            .x = src_x,
            .y = src_y,
            .w = (int) width,
            .h = (int) height,
        };
        SDL_Rect destRect = {
            .x = dest_x,
            .y = dest_y,
            .w = (int) width,
            .h = (int) height,
        };
        SDL_Surface *srcSurface = getRenderSurfaceRect(srcRenderer, &srcRect);
        if (!srcSurface) {
            handleError(0, display, src, 0, BadMatch, 0);
            return 0;
        }
        SDL_Texture *srcTexture =
            SDL_CreateTextureFromSurface(destRenderer, srcSurface);
        SDL_FreeSurface(srcSurface);
        if (!srcTexture) {
            LOG("SDL_CreateTextureFromSurface failed in %s: %s\n", __func__,
                SDL_GetError());
            handleError(0, display, src, 0, BadMatch, 0);
            return 0;
        }
        /* XCopyArea is GXcopy: destination pixels are replaced, not
         * blended. SDL_RenderCopy ignores renderer draw color/blend
         * state, so only the texture blend mode matters here. */
        SDL_SetTextureBlendMode(srcTexture, SDL_BLENDMODE_NONE);
        srcRect.x = 0;
        srcRect.y = 0;
        int clipCount = getGcClipIterationCount(gc);
        int rcCopy = 0;
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(destRenderer, gc, clip))
                continue;
            if (SDL_RenderCopy(destRenderer, srcTexture, &srcRect, &destRect) !=
                0) {
                LOG("SDL_RenderCopy failed in %s: %s\n", __func__,
                    SDL_GetError());
                rcCopy = -1;
                break;
            }
        }
        clearRendererClip(destRenderer);
        if (rcCopy != 0) {
            handleError(0, display, src, 0, BadMatch, 0);
            SDL_DestroyTexture(srcTexture);
            return 0;
        }
        SDL_DestroyTexture(srcTexture);
    } else {
        LOG("Hit unimplemented type in %s: %d\n", __func__, GET_XID_TYPE(dest));
    }

    if (GET_GC(gc)->graphicsExposures) {
        postEvent(display, dest, NoExpose, X_CopyArea, 0);
    }
    return 1;
}

int XDrawRectangle(Display *display,
                   Drawable d,
                   GC gc,
                   int x,
                   int y,
                   unsigned int width,
                   unsigned int height)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawRectangle.html
    SET_X_SERVER_REQUEST(display, X_PolyRectangle);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    LOG("%s: Drawing on 0x%08lx\n", __func__, d);
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->function == GXcopy &&
        (gContext->lineWidth > 1 || gContext->lineStyle != LineSolid)) {
        Path path;
        if (pathInit(&path)) {
            Bool ok = pathMoveTo(&path, x, y) &&
                      pathLineTo(&path, x + (int) width, y) &&
                      pathLineTo(&path, x + (int) width, y + (int) height) &&
                      pathLineTo(&path, x, y + (int) height) &&
                      pathLineTo(&path, x, y);
            if (ok)
                ok = rasterStrokePathOnRenderer(renderer, gc, &path);
            pathFree(&path);
            if (ok)
                return 1;
        }
    }
    if (gContext->function != GXcopy) {
        int clipCount = getGcClipIterationCount(gc);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip))
                continue;
            rasterOpRendererRectOutline(renderer, x, y, width, height,
                                        gContext->function, gContext->planeMask,
                                        gContext->foreground);
        }
        clearRendererClip(renderer);
        return 1;
    }
    /* SDL_RenderDrawRect outlines a w-by-h pixel rect; the X11 spec is
     * (w+1) by (h+1) (matches what the wide-stroke path emits). */
    SDL_Rect sdlRect = {
        .x = x,
        .y = y,
        .w = (int) width + 1,
        .h = (int) height + 1,
    };
    applySdlDrawState(renderer, gc, SDL_BLENDMODE_BLEND, gContext->foreground);
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (SDL_RenderDrawRect(renderer, &sdlRect)) {
            LOG("SDL_RenderDrawRect failed in %s: %s\n", __func__,
                SDL_GetError());
        }
    }
    clearRendererClip(renderer);
    return 1;
}

int XDrawRectangles(Display *display,
                    Drawable d,
                    GC gc,
                    XRectangle *rectangles,
                    int n_rectangles)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing/XDrawRectangles.html
    SET_X_SERVER_REQUEST(display, X_PolyRectangle);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (n_rectangles <= 0)
        return 1;
    for (int i = 0; i < n_rectangles; i++) {
        if (!XDrawRectangle(display, d, gc, rectangles[i].x, rectangles[i].y,
                            rectangles[i].width, rectangles[i].height))
            return 0;
    }
    return 1;
}

int XFillRectangle(Display *dpy,
                   Drawable d,
                   GC gc,
                   int x,
                   int y,
                   unsigned int width,
                   unsigned int height)
{
    XRectangle rect = {
        .x = x,
        .y = y,
        .width = width,
        .height = height,
    };
    return XFillRectangles(dpy, d, gc, &rect, 1);
}

int XFillRectangles(Display *display,
                    Drawable d,
                    GC gc,
                    XRectangle *rectangles,
                    int nrectangles)
{
    // https://tronche.com/gui/x/xlib/graphics/filling-areas/XFillRectangles.html
    SET_X_SERVER_REQUEST(display, X_PolyFillRectangle);
    TYPE_CHECK(d, DRAWABLE, display, 0);
    LOG("%s: Drawing on 0x%08lx\n", __func__, d);
    if (nrectangles < 1) {
        LOG("Invalid number of rectangles in %s: %d\n", __func__, nrectangles);
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    /* nrectangles is caller-supplied; an unbounded VLA is a stack-overflow
     * path on hostile or buggy input. Use a small inline buffer for the
     * common case and the heap when the request is larger. */
    enum { RECT_INLINE_BUDGET = 128 };
    SDL_Rect inlineRectangles[RECT_INLINE_BUDGET];
    SDL_Rect *heapRectangles = NULL;
    SDL_Rect *sdlRectangles = inlineRectangles;
    if (nrectangles > RECT_INLINE_BUDGET) {
        heapRectangles = malloc((size_t) nrectangles * sizeof(SDL_Rect));
        if (!heapRectangles) {
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        sdlRectangles = heapRectangles;
    }
    int i;
    for (i = 0; i < nrectangles; i++) {
        sdlRectangles[i].x = (int) rectangles[i].x;
        sdlRectangles[i].y = (int) rectangles[i].y;
        sdlRectangles[i].w = (int) rectangles[i].width;
        sdlRectangles[i].h = (int) rectangles[i].height;
        LOG("{x = %d, y = %d, w = %d, h = %d}\n", sdlRectangles[i].x,
            sdlRectangles[i].y, sdlRectangles[i].w, sdlRectangles[i].h);
    }
    GraphicContext *gContext = GET_GC(gc);
    LOG("bgColor: 0x%08lx, fgColor: 0x%08lx\n", gContext->background,
        gContext->foreground);
    if (gContext->fillStyle == FillSolid) {
        LOG("Fill_style is %s\n", "FillSolid");
        if (gContext->function != GXcopy) {
            /* SDL_Renderer has no raster-op equivalent, so read the affected
             * pixels back, apply the GC function in software, and blit them
             * in place.
             */
            int clipCount = getGcClipIterationCount(gc);
            for (int clip = 0; clip < clipCount; clip++) {
                if (!setGcClipForIteration(renderer, gc, clip))
                    continue;
                for (int r = 0; r < nrectangles; r++) {
                    SDL_Rect rr = sdlRectangles[r];
                    rasterOpRendererRect(renderer, &rr, gContext->function,
                                         gContext->planeMask,
                                         gContext->foreground);
                }
            }
            clearRendererClip(renderer);
        } else {
            applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE,
                              gContext->foreground);
            int clipCount = getGcClipIterationCount(gc);
            for (int clip = 0; clip < clipCount; clip++) {
                if (!setGcClipForIteration(renderer, gc, clip))
                    continue;
                if (SDL_RenderFillRects(renderer, &sdlRectangles[0],
                                        nrectangles)) {
                    LOG("SDL_RenderFillRects failed in %s: %s\n", __func__,
                        SDL_GetError());
                }
            }
            clearRendererClip(renderer);
        }
    } else if (gContext->fillStyle == FillTiled) {
        LOG("Fill_style is %s\n", "FillTiled");
    } else if (gContext->fillStyle == FillOpaqueStippled) {
        LOG("Fill_style is %s\n", "FillOpaqueStippled");
        applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE,
                          gContext->background);
        int clipCount = getGcClipIterationCount(gc);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip))
                continue;
            if (SDL_RenderFillRects(renderer, &sdlRectangles[0], nrectangles)) {
                LOG("SDL_RenderFillRects failed in %s: %s\n", __func__,
                    SDL_GetError());
            }
        }
        clearRendererClip(renderer);
    } else if (gContext->fillStyle == FillStippled) {
        LOG("Fill_style is %s\n", "FillStippled");
    }
    free(heapRectangles);
    return 1;
}
