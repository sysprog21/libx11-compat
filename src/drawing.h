#ifndef _DRAWING_H_
#define _DRAWING_H_

#include <limits.h>
#include <stdint.h>

#include "sdl-compat.h"
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
    unsigned int width, height;
    unsigned int depth;

    /* Lazy readback cache. Pixmaps are texture-only, so without this a run of
     * XGetImage calls on an unchanged pixmap issues one SDL_RenderReadPixels (a
     * GPU-to-CPU stall) per call. readback holds the whole pixmap surface,
     * populated on demand; readbackDirty is raised whenever a draw op binds the
     * pixmap as a render target (see GET_RENDERER), forcing the next read to
     * refresh. Windows keep their own pixman dirty tracking and are not cached
     * here.
     */
    SDL_Surface *readback;
    Bool readbackDirty;

    /* Cached stipple stamp. A pixmap used as a GC stipple gets turned into a
     * foreground/background-colored, alpha-keyed tile texture in
     * renderStippleRects; magic reuses the same stipple across many fills, so
     * memoize it here keyed by the resolved colors, opaque flag, and the
     * renderer it was built for. SDL textures are owned by one renderer, so a
     * stipple filled into a second top-level window (a different renderer) must
     * rebuild rather than reuse a texture the other renderer cannot draw. Tied
     * to the pixmap lifetime, so invalidation is just "drop it whenever the
     * pixmap content is marked dirty or the pixmap is freed" - no separate
     * cache to keep coherent.
     */
    SDL_Texture *stippleStamp;
    SDL_Renderer *stampRenderer;
    unsigned long stampFg, stampBg;
    Bool stampOpaque;
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
        markPixmapReadbackDirty(drawable);                                     \
        renderer = GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;              \
        SDL_Texture *target = GET_PIXMAP_TEXTURE(drawable);                    \
        if (SDL_GetRenderTarget(renderer) != target &&                         \
            SDL_SetRenderTarget(renderer, target) != 0) {                      \
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

/* True if point (x, y), in the mask's coordinate space offset by (offsetX,
 * offsetY), lands on an active (non-black) mask pixel. Out-of-range points are
 * outside the shape. Shared by shape rendering and event hit-testing so both
 * agree on which pixels belong to a shaped window.
 */
Bool shapeSurfaceContains(SDL_Surface *mask,
                          int offsetX,
                          int offsetY,
                          int64_t x,
                          int64_t y);
SDL_Renderer *getWindowRenderer(Window window);
unsigned char *glxAcquireCompositeReadback(Window window, size_t need);
void glxCompositeToWindow(Window window,
                          const unsigned char *rgba,
                          int width,
                          int height);
SDL_Surface *getRenderSurface(SDL_Renderer *renderer);
SDL_Surface *getRenderSurfaceRect(SDL_Renderer *renderer,
                                  const SDL_Rect *source);

/* Read a rect from a Pixmap through its lazy readback cache.
 *
 * Returns a freshly allocated RGBA8888 surface (caller frees) matching
 * getRenderSurfaceRect's contract, but at most one SDL_RenderReadPixels per
 * dirty cycle.
 */
SDL_Surface *getPixmapSurfaceRect(Pixmap pixmap, const SDL_Rect *source);

void markPixmapReadbackDirty(Drawable drawable);
void freePixmapReadback(PixmapStruct *pixmap);
void freePixmapStippleStamp(PixmapStruct *pixmap);
void invalidateStippleStampsForRenderer(SDL_Renderer *renderer);

/* Test hook: count of SDL_RenderReadPixels issued to refresh pixmap caches. */
extern unsigned long x11compat_pixmap_readback_reads;

/* Replace the present dirty rects with their bounding box when that costs fewer
 * readback bytes, returning the new count. The rects must already be clipped to
 * the drawable, so the box stays inside it.
 *
 * callOverhead is what one saved readback call is worth in bytes of extra
 * contiguous pixels. It is a parameter rather than a constant read inside, so a
 * test can pin a rate without inheriting production tuning. The one input not
 * passed in is the readback scratch cap, which decides how many calls a rect
 * bands into: that is read from the same place the read loop reads it, on
 * purpose, since a model that banded differently from the loop would price
 * merges that do not exist. Only rects larger than the cap (8 MiB by default)
 * are affected by it at all.
 */
int coalescePresentDirtyRects(SDL_Rect *rects,
                              int nrects,
                              uint64_t callOverhead);

/* Number of clip iterations to perform when drawing into "d" through "gc". Each
 * iteration is one (gc clip rect) x (visible-region rect) pair.
 *
 * Returns 0 when the gc clip excludes all pixels or "d"'s sibling occlusion
 * fully covers it, so callers can short-circuit the draw. "d" may be None or a
 * Pixmap, in which case only gc clipping is considered; sibling occlusion only
 * applies to windows.
 */
int getGcClipIterationCount(GC gc, Drawable d);

void setRendererDrawableClip(SDL_Renderer *renderer, const SDL_Rect *clip);

/* Pair "iteration" of getGcClipIterationCount: intersect the gc clip rect, the
 * cached parent-chain drawable clip, and "d"'s sibling visible region (when "d"
 * is a window), then push the result through SDL_RenderSetClipRect.
 * Returns False when the intersected region is empty so the caller skips draw
 * work for this iteration.
 */
Bool setGcClipForIteration(SDL_Renderer *renderer,
                           GC gc,
                           int iteration,
                           Drawable d);

void clearRendererClip(SDL_Renderer *renderer);

/* Flush the per-primitive cache that pairs a Drawable with its resolved visible
 * region and rect count. Called from invalidateVisibleRegionSubtree so the
 * cached pointer cannot outlive the underlying pixman rect storage across a
 * region recomputation.
 */
void invalidatePrimitiveClipCache(void);

void drawWindowDataToScreen(void);

#ifdef __EMSCRIPTEN__
/* Force a full recomposite of the single wasm DOM canvas. A borrowed overlay (a
 * menu or popup sharing the host window's SDL surface) composites straight onto
 * the host surface, so when one unmaps or moves its pixels must be overpainted
 * by the host backing. Marking the host fully dirty makes the next present
 * repaint it whole; the surviving overlays then redraw on top. Call after a
 * borrowed overlay is torn down.
 */
void invalidateWasmCanvasComposite(void);
#endif

/* Runtime kill switch for the per-window accelerated present path. When
 * LIBX11_COMPAT_FORCE_SOFTWARE_PRESENT is set to a truthy value every top-level
 * window keeps the SDL_GetWindowSurface software present, so the accelerated
 * path can be bisected out without a rebuild. Read once and cached.
 */
Bool libx11CompatForceSoftwarePresent(void);

/* True when the per-window accelerated present pipeline is actually usable on
 * this host. Some drivers (SDL2's dummy driver on headless CI) return a
 * renderer whose SDL_RenderReadPixels never reflects what was drawn, so a
 * driver-name check is insufficient; this probes the readback capability once
 * on a throwaway window and caches the verdict. realizeTopLevelWindow forces
 * the SDL_GetWindowSurface software present when this returns False.
 */
Bool libx11CompatAcceleratedPresentUsable(void);

/* Test hooks for the accelerated-present software fallback (see tests/check.c
 * test_present_software_fallback). ForceAcceleratedPresentForTest makes
 * realizeTopLevelWindow put a window on the accelerated path even where the
 * probe reports it unusable (the headless CI driver), and FailAccelerated-
 * PresentOnceForTest makes the next accelerated present fail, so the mid-run
 * demotion to the software path can be exercised. Both default off.
 */
void libx11CompatForceAcceleratedPresentForTest(Bool on);
Bool libx11CompatAcceleratedPresentForcedForTest(void);
void libx11CompatFailAcceleratedPresentOnceForTest(void);
int libx11CompatSoftwareDemotionCountForTest(void);

/* Coalesce a client repaint burst so its intermediate XFlush/XSync presents do
 * not reach the screen (see the deadline notes in drawing.c). begin arms the
 * hold at the point a host-resize ConfigureNotify is produced; end presents the
 * final frame once and disarms, called when the client returns to its idle
 * input wait.
 */
void beginCoalesceClientRepaint(void);
void endCoalesceClientRepaint(void);

/* Pure coalesce-gate decision, exposed for the zero-clock regression test. See
 * src/drawing.c: a clock reading of 0 (monotonic clock unavailable) counts as
 * an expired gate so a stuck deadline cannot suppress presents forever.
 */
Bool coalesceGateExpired(uint64_t now, uint64_t deadline);
Bool presentWakeOwnsEventType(Uint32 eventType);
void cancelPresentWakeTimer(void);
void markWindowNeedsPresent(Window window);
void markWindowNeedsPresentRect(Window window, const SDL_Rect *rect);
void presentDrawableIfVisible(Drawable drawable);
void presentDrawableRectIfVisible(Drawable drawable, const SDL_Rect *rect);

/* Same as presentDrawableRectIfVisible but leaves the text-stamp cache
 * untouched. The core-text path calls this so marking its own freshly drawn
 * cell present does not evict the stamp it is about to record.
 */
void presentDrawableRectIfVisibleNoStampInvalidate(Drawable drawable,
                                                   const SDL_Rect *rect);

/* Text-stamp cache: records that an exact anti-aliased label (font, color,
 * string) is already painted at a cell so a Motif expose/arm redraw that
 * re-issues the identical XDrawString can skip the non-idempotent re-blend.
 * Cells are keyed in top-level backing coordinates; any other draw over the
 * region, or a structural change, drops the stamp. See src/drawing.c.
 */
Bool textStampLookup(Drawable drawable,
                     const SDL_Rect *cell,
                     Font fontXid,
                     Uint32 foreground,
                     const char *string);
void textStampRecord(Drawable drawable,
                     const SDL_Rect *cell,
                     Font fontXid,
                     Uint32 foreground,
                     const char *string);
void invalidateTextStampsForDrawableRect(Drawable drawable,
                                         const SDL_Rect *local);
void flushTextStampsForWindow(Window window);
void freeTextStamps(void);

/* Single-slot (renderer, gc, generation) cache. The shim is single threaded and
 * tends to issue runs of draw calls against one renderer with the same GC, so a
 * one-deep cache catches most redundant pushes. applySdlDrawState pushes the
 * requested blend mode + color only when the cached state diverges;
 * invalidateGcStateCache is called from XFreeGC so stale GC pointers don't fool
 * a future generation match.
 */
void applySdlDrawState(SDL_Renderer *renderer,
                       GC gc,
                       SDL_BlendMode blendMode,
                       unsigned long color);
void invalidateGcStateCache(GC gc);

/* XClearArea with an extra exceptChild: that mapped child of w is not clipped
 * out of the background fill. Used to erase a moved child's vacated footprint
 * from the shared top-level backing, where the ordinary clip-by-children would
 * protect the child at its new position and leave a stale ghost in the overlap.
 * Pass None for plain XClearArea semantics.
 */
int compatClearWindowAreaExceptChild(Display *dpy,
                                     Window w,
                                     int x,
                                     int y,
                                     unsigned int width,
                                     unsigned int height,
                                     Bool exposures,
                                     Window exceptChild);

/* Call after any direct SDL_SetRenderDrawColor / SDL_SetRenderDrawBlendMode
 * that does not go through applySdlDrawState. Otherwise the next cached apply
 * may see a "matching" cache entry and skip the SDL push even though the
 * renderer's real state has drifted.
 */
void invalidateSdlDrawStateCache(void);

/* Shape-mask post-process for draw primitives.
 *
 * captureShapeMaskBaseline returns NULL on the fast path (drawable is not a
 * window, or the window has no shape mask installed). Otherwise it returns a
 * freshly allocated SDL_Surface holding the pre-draw pixels of `rect` so the
 * caller can restore mask-excluded pixels afterwards.
 *
 * applyShapeMaskOverDrawnRect composites the now-drawn pixels in `rect` with
 * `baseline` using the window's installed shape mask: pixels where the mask is
 * opaque-white keep the freshly drawn value; pixels outside the mask bounding
 * box, or where the mask is black, are restored from `baseline`. Callers must
 * free `baseline` themselves.
 */
SDL_Surface *captureShapeMaskBaseline(Drawable d,
                                      SDL_Renderer *renderer,
                                      const SDL_Rect *rect);

/* Returns True on success (or when no work was needed). False means the SDL
 * readback / texture upload failed and masked pixels may still be visible on
 * screen; callers may want to skip presentDrawableIfVisible or log a BadMatch
 * in that case.
 */
Bool applyShapeMaskOverDrawnRect(Drawable d,
                                 SDL_Renderer *renderer,
                                 SDL_Surface *baseline,
                                 const SDL_Rect *rect);

/* RAII-style guard so each draw primitive can wrap its SDL draws with a single
 * declaration and a single end call (handling all early returns).
 *
 * Usage:
 *   ShapeGuard sg;
 *   shapeGuardBegin(&sg, d, renderer, &bbox);
 *   ... SDL draws ...
 *   if (shapeGuardEnd(&sg))
 *       presentDrawableIfVisible(d);
 *
 * If shapeGuardEnd returns False the shape composite failed; masked-out pixels
 * may still be visible, so callers should skip the present rather than show
 * that stale state (the next draw composes from a fresh baseline).
 */
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

/* Returns True on success (or when nothing was captured). False signals that
 * the post-draw composite failed and masked pixels may still show; callers that
 * care can suppress the subsequent present or raise BadMatch.
 */
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
 * accumulator must land back in SDL_Rect's signed-int fields.
 */
static inline int clampToInt(int64_t value)
{
    if (value < INT_MIN)
        return INT_MIN;
    if (value > INT_MAX)
        return INT_MAX;
    return (int) value;
}

/* SDL_UnionRect is not in the SDL wrapper shim's export list. The inline
 * equivalent below produces the smallest rect containing both inputs (both
 * assumed non-empty; w/h are width/height, not extents). The extent and span
 * math runs in int64_t so caller-supplied coordinates near INT_MAX/INT_MIN
 * cannot wrap into invalid SDL_Rects.
 */
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
