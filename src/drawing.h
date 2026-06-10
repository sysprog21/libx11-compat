#ifndef _DRAWING_H_
#define _DRAWING_H_

#include <SDL2/SDL.h>
#include <limits.h>
#include <stdint.h>
#include "resource-types.h"
#include "window.h"

#if SDL_BYTEORDER == SDL_BIG_ENDIAN || 1
#define DEFAULT_RED_MASK 0xFF000000
#define DEFAULT_GREEN_MASK 0x00FF0000
#define DEFAULT_BLUE_MASK 0x0000FF00
#define DEFAULT_ALPHA_MASK 0x000000FF
#else
#define DEFAULT_RED_MASK 0x000000FF
#define DEFAULT_GREEN_MASK 0x0000FF00
#define DEFAULT_BLUE_MASK 0x00FF0000
#define DEFAULT_ALPHA_MASK 0xFF000000
#endif
#define SDL_SURFACE_DEPTH 32

typedef struct {
    SDL_Texture *texture;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
} PixmapStruct;

#define LOCK_SURFACE(surface)  \
    if (SDL_MUSTLOCK(surface)) \
    SDL_LockSurface(surface)
#define UNLOCK_SURFACE(surface) \
    if (SDL_MUSTLOCK(surface))  \
    SDL_UnlockSurface(surface)
#define GET_PIXMAP_STRUCT(pixmap) \
    (IS_TYPE(pixmap, PIXMAP) ? ((PixmapStruct *) GET_XID_VALUE(pixmap)) : NULL)
#define GET_PIXMAP_TEXTURE(pixmap) \
    (GET_PIXMAP_STRUCT(pixmap) ? GET_PIXMAP_STRUCT(pixmap)->texture : NULL)
#define GET_RENDERER(drawable, renderer)                                       \
    renderer = NULL;                                                           \
    if (IS_TYPE(drawable, WINDOW)) {                                           \
        renderer = getWindowRenderer(drawable);                                \
    } else if (IS_TYPE(drawable, PIXMAP)) {                                    \
        renderer = GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;              \
        if (SDL_SetRenderTarget(renderer, GET_PIXMAP_TEXTURE(drawable)) !=     \
            0) {                                                               \
            fprintf(stderr,                                                    \
                    "SDL_SetRenderTarget failed while trying to get renderer " \
                    "in %s, %s, %d: %s\n",                                     \
                    __FILE__, __func__, __LINE__, SDL_GetError());             \
        }                                                                      \
        SDL_RenderSetViewport(renderer, NULL);                                 \
        setRendererDrawableClip(renderer, NULL);                               \
    } else {                                                                   \
        fprintf(stderr,                                                        \
                "Got unknown drawable type while trying to get renderer in "   \
                "%s, %s, %d\n",                                                \
                __FILE__, __func__, __LINE__);                                 \
    }

#ifdef DEBUG_WINDOWS
void drawWindowDebugView();
void drawDebugWindowSurfacePlanes();
#endif
void putPixel(SDL_Surface *surface,
              unsigned int x,
              unsigned int y,
              Uint32 pixel);
Uint32 getPixel(SDL_Surface *surface, unsigned int x, unsigned int y);
SDL_Renderer *getWindowRenderer(Window window);
SDL_Surface *getRenderSurface(SDL_Renderer *renderer);
SDL_Surface *getRenderSurfaceRect(SDL_Renderer *renderer,
                                  const SDL_Rect *source);
int getGcClipIterationCount(GC gc);
void setRendererDrawableClip(SDL_Renderer *renderer, const SDL_Rect *clip);
Bool setGcClipForIteration(SDL_Renderer *renderer, GC gc, int iteration);
void clearRendererClip(SDL_Renderer *renderer);
void drawWindowDataToScreen(void);
Bool presentWakeOwnsEventType(Uint32 eventType);
void markWindowNeedsPresent(Window window);
void markWindowNeedsPresentRect(Window window, const SDL_Rect *rect);
void presentDrawableIfVisible(Drawable drawable);
void presentDrawableRectIfVisible(Drawable drawable, const SDL_Rect *rect);

/* Single-slot (renderer, gc, generation) cache. The shim is single
 * threaded and tends to issue runs of draw calls against one renderer
 * with the same GC, so a one-deep cache catches most redundant pushes.
 * applySdlDrawState pushes the requested blend mode + color only when
 * the cached state diverges; invalidateGcStateCache is called from
 * XFreeGC so stale GC pointers don't fool a future generation match. */
void applySdlDrawState(SDL_Renderer *renderer,
                       GC gc,
                       SDL_BlendMode blendMode,
                       unsigned long color);
void invalidateGcStateCache(GC gc);
/* Call after any direct SDL_SetRenderDrawColor / SDL_SetRenderDrawBlendMode
 * that doesn't go through applySdlDrawState — otherwise the next cached
 * apply may see a "matching" cache entry and skip the SDL push even
 * though the renderer's real state has drifted. */
void invalidateSdlDrawStateCache(void);

/* Shape-mask post-process for draw primitives.
 *
 * captureShapeMaskBaseline returns NULL on the fast path (drawable is not
 * a window, or the window has no shape mask installed). Otherwise it
 * returns a freshly allocated SDL_Surface holding the pre-draw pixels
 * of `rect` so the caller can restore mask-excluded pixels afterwards.
 *
 * applyShapeMaskOverDrawnRect composites the now-drawn pixels in `rect`
 * with `baseline` using the window's installed shape mask: pixels where
 * the mask is opaque-white keep the freshly drawn value; pixels outside
 * the mask bounding box, or where the mask is black, are restored from
 * `baseline`. Callers must free `baseline` themselves.
 */
SDL_Surface *captureShapeMaskBaseline(Drawable d,
                                      SDL_Renderer *renderer,
                                      const SDL_Rect *rect);
/* Returns True on success (or when no work was needed). False means the
 * SDL readback / texture upload failed and masked pixels may still be
 * visible on screen; callers may want to skip presentDrawableIfVisible
 * or log a BadMatch in that case. */
Bool applyShapeMaskOverDrawnRect(Drawable d,
                                 SDL_Renderer *renderer,
                                 SDL_Surface *baseline,
                                 const SDL_Rect *rect);

/* RAII-style guard so each draw primitive can wrap its SDL draws with a
 * single declaration and a single end call (handling all early returns).
 *
 * Usage:
 *   ShapeGuard sg;
 *   shapeGuardBegin(&sg, d, renderer, &bbox);
 *   ... SDL draws ...
 *   if (shapeGuardEnd(&sg))
 *       presentDrawableIfVisible(d);
 *
 * If shapeGuardEnd returns False the shape composite failed; masked-out
 * pixels may still be visible, so callers should skip the present rather
 * than show that stale state (the next draw composes from a fresh
 * baseline). */
typedef struct {
    SDL_Surface *baseline;
    SDL_Rect bbox;
    Drawable d;
    SDL_Renderer *renderer;
} ShapeGuard;

static inline void shapeGuardBegin(ShapeGuard *g,
                                   Drawable d,
                                   SDL_Renderer *renderer,
                                   const SDL_Rect *bbox)
{
    g->d = d;
    g->renderer = renderer;
    g->bbox = *bbox;
    g->baseline = captureShapeMaskBaseline(d, renderer, bbox);
}

/* Returns True on success (or when nothing was captured). False signals
 * that the post-draw composite failed and masked pixels may still show;
 * callers that care can suppress the subsequent present or raise BadMatch. */
static inline Bool shapeGuardEnd(ShapeGuard *g)
{
    if (!g->baseline)
        return True;
    Bool ok =
        applyShapeMaskOverDrawnRect(g->d, g->renderer, g->baseline, &g->bbox);
    SDL_FreeSurface(g->baseline);
    g->baseline = NULL;
    return ok;
}

/* Saturating cast from int64 to int. Used wherever a 64-bit overflow-safe
 * accumulator must land back in SDL_Rect's signed-int fields. */
static inline int clampToInt(int64_t value)
{
    if (value < INT_MIN)
        return INT_MIN;
    if (value > INT_MAX)
        return INT_MAX;
    return (int) value;
}

/* SDL_UnionRect is not in our SDL wrapper shim's export list. Inline a
 * minimal equivalent: result becomes the smallest rect containing both
 * inputs (both assumed non-empty; w/h are width/height, not extents).
 * Use int64_t for extent and span math so caller-supplied coordinates near
 * INT_MAX/INT_MIN can't wrap into invalid SDL_Rects. */
static inline void unionRect(const SDL_Rect *a,
                             const SDL_Rect *b,
                             SDL_Rect *out)
{
    int64_t ax1 = a->x, ay1 = a->y;
    int64_t bx1 = b->x, by1 = b->y;
    int64_t ax2 = ax1 + a->w, ay2 = ay1 + a->h;
    int64_t bx2 = bx1 + b->w, by2 = by1 + b->h;
    int64_t x1 = ax1 < bx1 ? ax1 : bx1;
    int64_t y1 = ay1 < by1 ? ay1 : by1;
    int64_t x2 = ax2 > bx2 ? ax2 : bx2;
    int64_t y2 = ay2 > by2 ? ay2 : by2;
    out->x = clampToInt(x1);
    out->y = clampToInt(y1);
    out->w = clampToInt(x2 > x1 ? x2 - x1 : 0);
    out->h = clampToInt(y2 > y1 ? y2 - y1 : 0);
}

#endif /* _DRAWING_H_ */
