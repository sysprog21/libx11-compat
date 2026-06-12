#include <inttypes.h>
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
#include "timeline.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static SDL_atomic_t presentWakePending = {False};
static SDL_atomic_t presentWakeTimerPending = {False};
static SDL_atomic_t presentWakeEventType = {-1};

#define PRESENT_WAKE_DELAY_MS 16

static unsigned long opaqueColorIfAlphaUnset(unsigned long color);
static Bool getDrawableSize(Drawable drawable, int *width, int *height);
static void resolveWindowBackground(Window window,
                                    Pixmap *backgroundPixmap,
                                    unsigned long *backgroundColor);
/* Forward-defined here so callers earlier in the file (the shape-mask
 * snapshot/composite helpers) can stack-allocate a ShapeMaskView. The
 * resolve/sample helpers themselves live next to the rest of the shape code
 * further down.
 */
typedef struct ShapeMaskView {
    SDL_Surface *boundingMask;
    int boundingOffsetX, boundingOffsetY;
    SDL_Surface *clipMask;
    int clipOffsetX, clipOffsetY;
} ShapeMaskView;
static Bool resolveShapeMasks(const WindowStruct *window, ShapeMaskView *out);
static Bool pixelInsideShape(const ShapeMaskView *view, int64_t wx, int64_t wy);

static uint64_t monotonicNowNs(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return ((uint64_t) now.tv_sec * 1000000000ULL) + (uint64_t) now.tv_nsec;
}

static double nsToMs(uint64_t ns)
{
    return (double) ns / 1000000.0;
}

static void unionPresentRect(WindowStruct *windowStruct, const SDL_Rect *rect)
{
    if (!windowStruct || !rect || rect->w <= 0 || rect->h <= 0)
        return;
    if (!windowStruct->hasPresentRect) {
        windowStruct->presentRect = *rect;
        windowStruct->hasPresentRect = True;
        return;
    }
    unionRect(&windowStruct->presentRect, rect, &windowStruct->presentRect);
}

static FILE *renderStatsFile(void)
{
    static Bool initialized = False;
    static FILE *file = NULL;
    if (!initialized) {
        initialized = True;
        const char *path = getenv("LIBX11_COMPAT_RENDER_STATS");
        if (path && *path) {
            file = fopen(path, "w");
            if (file) {
                setvbuf(file, NULL, _IOLBF, 0);
                fprintf(file,
                        "event\telapsed_ms\treadback_ms\tupdate_"
                        "ms\twindows\tpixels\n");
            }
        }
    }
    return file;
}

static Uint32 presentWakeTimerCallback(Uint32 interval, void *param)
{
    (void) interval;
    (void) param;
    SDL_AtomicSet(&presentWakeTimerPending, False);
    int eventType = SDL_AtomicGet(&presentWakeEventType);
    if (SDL_AtomicGet(&presentWakePending) || eventType == -1)
        return 0;

    SDL_Event event;
    SDL_zero(event);
    event.type = (Uint32) eventType;
    event.user.code = PRESENT_EVENT_CODE;
    if (SDL_PushEvent(&event) == 1)
        SDL_AtomicSet(&presentWakePending, True);
    return 0;
}

Bool presentWakeOwnsEventType(Uint32 eventType)
{
    int registered = SDL_AtomicGet(&presentWakeEventType);
    return registered != -1 && eventType == (Uint32) registered;
}

static Bool hasPendingWindowPresent(void)
{
    if (SCREEN_WINDOW == None)
        return False;
    WindowStruct *screenWindow = GET_WINDOW_STRUCT(SCREEN_WINDOW);
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    for (size_t i = 0; i < screenWindow->children.length; i++) {
        WindowStruct *child = GET_WINDOW_STRUCT(children[i]);
        if (child->sdlWindow && child->needsPresent)
            return True;
    }
    return False;
}

static void schedulePresentWake(void)
{
    if (SDL_AtomicGet(&presentWakePending) ||
        SDL_AtomicGet(&presentWakeTimerPending))
        return;
    if (SDL_AtomicGet(&presentWakeEventType) == -1) {
        Uint32 eventType = SDL_RegisterEvents(1);
        if (eventType == ((Uint32) -1))
            return;
        /* Losers of the CAS already installed a different registered event
         * type; the registration this thread just won is unused but harmless,
         * and pushing it would slip past presentWakeOwnsEventType.
         */
        SDL_AtomicCAS(&presentWakeEventType, -1, (int) eventType);
    }
    SDL_AtomicSet(&presentWakeTimerPending, True);
    if (SDL_AddTimer(PRESENT_WAKE_DELAY_MS, presentWakeTimerCallback, NULL) ==
        0)
        SDL_AtomicSet(&presentWakeTimerPending, False);
}

void drawWindowDataToScreen()
{
    if (!hasPendingWindowPresent()) {
        SDL_AtomicSet(&presentWakePending, False);
        return;
    }

    SDL_Renderer *screen = GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
    if (!screen)
        return;

    uint64_t totalStart = monotonicNowNs();
    uint64_t readbackNs = 0;
    uint64_t updateNs = 0;
    uint64_t presentedPixels = 0;
    size_t presentedWindows = 0;
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
        if (!child->needsPresent)
            continue;

        SDL_Surface *winSurface = SDL_GetWindowSurface(child->sdlWindow);
        if (!winSurface) {
            LOG("SDL_GetWindowSurface failed in %s: %s\n", __func__,
                SDL_GetError());
            continue;
        }

        int w, h;
        GET_WINDOW_DIMS(children[i], w, h);
        SDL_Rect presentRect = {0, 0, w, h};
        if (child->hasPresented && child->hasPresentRect)
            presentRect = child->presentRect;
        SDL_Rect bounds = {0, 0, w, h};
        if (!SDL_IntersectRect(&presentRect, &bounds, &presentRect))
            continue;
        /* Clamp the readback rect to all three of the X11 logical size, the
         * backing texture size, and the SDL window surface size. Window surface
         * and X11 logical w/h can diverge during resize and on high-DPI setups;
         * an unclamped read would overflow winSurface->pixels.
         */
        int texW = 0, texH = 0;
        SDL_QueryTexture(child->sdlTexture, NULL, NULL, &texW, &texH);
        SDL_Rect texBounds = {0, 0, texW, texH};
        SDL_Rect surfaceBounds = {0, 0, winSurface->w, winSurface->h};
        if (!SDL_IntersectRect(&presentRect, &texBounds, &presentRect) ||
            !SDL_IntersectRect(&presentRect, &surfaceBounds, &presentRect))
            continue;
        if (SDL_SetRenderTarget(screen, child->sdlTexture) != 0) {
            LOG("SDL_SetRenderTarget(backing) failed in %s: %s\n", __func__,
                SDL_GetError());
            continue;
        }
        if (SDL_RenderSetViewport(screen, NULL) != 0) {
            LOG("SDL_RenderSetViewport(NULL) failed in %s: %s\n", __func__,
                SDL_GetError());
            continue;
        }
        if (SDL_RenderSetClipRect(screen, NULL) != 0) {
            LOG("SDL_RenderSetClipRect(NULL) failed in %s: %s\n", __func__,
                SDL_GetError());
            continue;
        }
        screenTargetMutated = True;
        Uint32 winFmt = winSurface->format->format;
        Uint32 readFmt = SDL_PIXELFORMAT_RGBA8888;
        int readRc = -1;
        uint64_t readStart = monotonicNowNs();
        if (winFmt == readFmt) {
            /* Direct readback into the window's framebuffer. Saves one
             * full-image memcpy on every present.
             */
            if (SDL_LockSurface(winSurface) == 0) {
                Uint8 *pixels =
                    (Uint8 *) winSurface->pixels +
                    (size_t) presentRect.y * (size_t) winSurface->pitch +
                    (size_t) presentRect.x *
                        (size_t) winSurface->format->BytesPerPixel;
                readRc = SDL_RenderReadPixels(screen, &presentRect, readFmt,
                                              pixels, winSurface->pitch);
                SDL_UnlockSurface(winSurface);
            }
        } else {
            SDL_Surface *staging = SDL_CreateRGBSurfaceWithFormat(
                0, presentRect.w, presentRect.h, 32, readFmt);
            if (staging) {
                readRc = SDL_RenderReadPixels(screen, &presentRect, readFmt,
                                              staging->pixels, staging->pitch);
                if (readRc == 0)
                    SDL_BlitSurface(staging, NULL, winSurface, &presentRect);
                SDL_FreeSurface(staging);
            }
        }
        readbackNs += monotonicNowNs() - readStart;
        if (readRc == 0) {
            uint64_t updateStart = monotonicNowNs();
            SDL_UpdateWindowSurfaceRects(child->sdlWindow, &presentRect, 1);
            updateNs += monotonicNowNs() - updateStart;
            child->needsPresent = False;
            child->hasPresentRect = False;
            child->hasPresented = True;
            presentedWindows++;
            uint64_t windowPixels =
                (uint64_t) presentRect.w * (uint64_t) presentRect.h;
            presentedPixels += windowPixels;
            timelineTapPresent(children[i], 1, windowPixels);
        } else {
            LOG("SDL_RenderReadPixels failed in %s: %s\n", __func__,
                SDL_GetError());
        }
    }
    if (screenTargetMutated) {
        SDL_SetRenderTarget(screen, prevTarget);
        invalidateSdlDrawStateCache();
    }
    SDL_AtomicSet(&presentWakePending, False);
#ifdef DEBUG_WINDOWS
    printWindowsHierarchy();
#endif
    FILE *stats = renderStatsFile();
    if (stats && presentedWindows > 0) {
        uint64_t totalEnd = monotonicNowNs();
        fprintf(stats,
                "drawWindowDataToScreen\t%.3f\t%.3f\t%.3f\t%zu\t%" PRIu64 "\n",
                nsToMs(totalEnd - totalStart), nsToMs(readbackNs),
                nsToMs(updateNs), presentedWindows, presentedPixels);
    }
}

void markWindowNeedsPresentRect(Window window, const SDL_Rect *rect)
{
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window) || !rect || rect->w <= 0 ||
        rect->h <= 0)
        return;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    int w, h;
    GET_WINDOW_DIMS(window, w, h);
    SDL_Rect bounds = {0, 0, w, h};
    SDL_Rect clipped;
    if (!SDL_IntersectRect(rect, &bounds, &clipped))
        return;
    windowStruct->needsPresent = True;
    unionPresentRect(windowStruct, &clipped);
    schedulePresentWake();
}

void markWindowNeedsPresent(Window window)
{
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window))
        return;
    int w, h;
    GET_WINDOW_DIMS(window, w, h);
    SDL_Rect full = {0, 0, w, h};
    markWindowNeedsPresentRect(window, &full);
}

void presentDrawableRectIfVisible(Drawable drawable, const SDL_Rect *rect)
{
    if (!IS_TYPE(drawable, WINDOW))
        return;

    Bool haveRect = rect && rect->w > 0 && rect->h > 0;
    SDL_Rect topRect = haveRect ? *rect : (SDL_Rect) {0, 0, 0, 0};
    Window window = (Window) drawable;
    while (window != None && window != SCREEN_WINDOW) {
        if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
            WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
            Bool firstPresent = !windowStruct->hasPresented;
            if (haveRect)
                markWindowNeedsPresentRect(window, &topRect);
            else
                markWindowNeedsPresent(window);
            if (firstPresent)
                drawWindowDataToScreen();
            return;
        }
        if (haveRect) {
            int x = 0, y = 0;
            GET_WINDOW_POS(window, x, y);
            topRect.x += x;
            topRect.y += y;
        }
        window = GET_PARENT(window);
    }
}

void presentDrawableIfVisible(Drawable drawable)
{
    presentDrawableRectIfVisible(drawable, NULL);
}

static struct {
    SDL_Renderer *renderer;
    SDL_Rect clip;
    Bool valid;
} rendererDrawableClip;

SDL_Renderer *getWindowRenderer(Window window)
{
    Window drawWindow = window;
    SDL_Rect viewPort;
    SDL_Rect clipAbs;
    SDL_Renderer *renderer = NULL;
    int x = 0, y = 0;
    viewPort.x = 0;
    viewPort.y = 0;

    GET_WINDOW_DIMS(window, viewPort.w, viewPort.h);
    clipAbs.x = 0;
    clipAbs.y = 0;
    clipAbs.w = viewPort.w;
    clipAbs.h = viewPort.h;
    while (GET_PARENT(window) != None &&
           !GET_WINDOW_STRUCT(window)->sdlWindow &&
           GET_WINDOW_STRUCT(window)->mapState != UnMapped) {
        GET_WINDOW_POS(window, x, y);
        viewPort.x += x;
        viewPort.y += y;
        clipAbs.x += x;
        clipAbs.y += y;
        drawWindow = GET_PARENT(drawWindow);
        window = drawWindow;
        SDL_Rect parentBounds;
        parentBounds.x = 0;
        parentBounds.y = 0;
        GET_WINDOW_DIMS(window, parentBounds.w, parentBounds.h);
        if (!SDL_IntersectRect(&clipAbs, &parentBounds, &clipAbs)) {
            clipAbs.w = 0;
            clipAbs.h = 0;
        }
    }

    renderer = GET_WINDOW_STRUCT(drawWindow)->sdlRenderer;
    Bool justCreatedRenderer = False;
    if (!renderer) {
        /* All windows draw on the SCREEN renderer with a per-window backing
         * texture as the render target. Mapped top-level windows used to own a
         * separate SDL_Renderer attached to their SDL_Window, which forced
         * every XCopyArea between two top-level windows (and every
         * Pixmap->Window copy) into a cross-renderer readback. Unifying on
         * SCREEN renderer keeps those copies in renderer space; presentation to
         * the actual SDL_Window happens in drawWindowDataToScreen.
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
                GET_WINDOW_STRUCT(drawWindow)->needsPresent = True;
                schedulePresentWake();
            }
        }
    }

    /* Real X11 paints a freshly mapped window with its background pixel.
     * libx11-compat allocates the SDL surface/texture lazily, so this path
     * performs the equivalent one-shot fill to avoid presenting uninitialized
     * memory.
     */
    if (justCreatedRenderer) {
        SDL_Texture *prevTarget = SDL_GetRenderTarget(renderer);
        SDL_Texture *initTarget = GET_WINDOW_STRUCT(drawWindow)->sdlTexture;
        SDL_SetRenderTarget(renderer, initTarget);
        Pixmap backgroundPixmap = None;
        unsigned long bg = 0;
        resolveWindowBackground(drawWindow, &backgroundPixmap, &bg);
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
    SDL_Rect drawableClip = {
        .x = clipAbs.x - viewPort.x,
        .y = clipAbs.y - viewPort.y,
        .w = clipAbs.w,
        .h = clipAbs.h,
    };
    setRendererDrawableClip(renderer, &drawableClip);
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
     * for a drawable-relative area that runs off the edge would otherwise lose
     * all pixels instead of getting the in-bounds portion zero-padded.
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
    SDL_Rect targetReadRect = readRect;

    /* When SDL has a custom render target bound (Pixmap target, shape
     * compositing), clamp the read rect to the texture's pixel bounds before
     * SDL_RenderReadPixels. Without the clamp, a read rect that the viewport
     * accepted can still run off the target, and the readback fails with
     * "invalid parameter" on the GL/Metal backends.
     */
    SDL_Texture *target = SDL_GetRenderTarget(renderer);
    if (target) {
        int targetWidth = 0, targetHeight = 0;
        if (SDL_QueryTexture(target, NULL, NULL, &targetWidth, &targetHeight) !=
            0) {
            /* Match the SDL_RenderReadPixels failure contract: an SDL error
             * here is a real failure, not a "no overlap" case. Callers
             * distinguish failure from clipped-to-zero by NULL return.
             */
            LOG("SDL_QueryTexture failed in %s: %s\n", __func__,
                SDL_GetError());
            SDL_FreeSurface(surface);
            return NULL;
        }

        SDL_Rect targetBounds = {0, 0, targetWidth, targetHeight};
        if (!SDL_IntersectRect(&targetReadRect, &targetBounds, &targetReadRect))
            return surface;
        dstOffsetX += (size_t) (targetReadRect.x - readRect.x);
        dstOffsetY += (size_t) (targetReadRect.y - readRect.y);
    }

    if (targetReadRect.x < 0) {
        int clippedPixels = -targetReadRect.x;
        if (clippedPixels >= targetReadRect.w)
            return surface;
        targetReadRect.x = 0;
        targetReadRect.w -= clippedPixels;
        dstOffsetX += (size_t) clippedPixels;
    }
    if (targetReadRect.y < 0) {
        int clippedPixels = -targetReadRect.y;
        if (clippedPixels >= targetReadRect.h)
            return surface;
        targetReadRect.y = 0;
        targetReadRect.h -= clippedPixels;
        dstOffsetY += (size_t) clippedPixels;
    }
    if (targetReadRect.w <= 0 || targetReadRect.h <= 0)
        return surface;
    Uint8 *dstPixels = (Uint8 *) surface->pixels +
                       dstOffsetY * (size_t) surface->pitch +
                       dstOffsetX * (SDL_SURFACE_DEPTH / 8);
    if (SDL_RenderReadPixels(renderer, &targetReadRect,
                             SDL_PIXELFORMAT_RGBA8888, dstPixels,
                             surface->pitch) != 0) {
        LOG("SDL_RenderReadPixels failed in %s: %s\n", __func__,
            SDL_GetError());
        SDL_FreeSurface(surface);
        return NULL;
    }
    return surface;
}

/* Clip a caller-supplied draw rect to the window's own bounds before
 * snapshotting. Pathological coordinates would otherwise ask the SDL renderer
 * to read back far outside the visible surface; the underlying
 * getRenderSurfaceRect already clips, but pre-clipping here avoids allocating a
 * multi-megabyte surface just to have most of it discarded.
 */
static Bool clipRectToWindow(WindowStruct *window,
                             const SDL_Rect *in,
                             SDL_Rect *out)
{
    if (!window || !in)
        return False;
    SDL_Rect win = {
        .x = 0,
        .y = 0,
        .w = clampToInt((int64_t) window->w),
        .h = clampToInt((int64_t) window->h),
    };
    if (win.w <= 0 || win.h <= 0)
        return False;
    return SDL_IntersectRect(in, &win, out) == SDL_TRUE;
}

SDL_Surface *captureShapeMaskBaseline(Drawable d,
                                      SDL_Renderer *renderer,
                                      const SDL_Rect *rect)
{
    if (!renderer || !rect || rect->w <= 0 || rect->h <= 0)
        return NULL;
    if (!IS_TYPE(d, WINDOW))
        return NULL;
    WindowStruct *window = GET_WINDOW_STRUCT(d);
    ShapeMaskView view;
    if (!resolveShapeMasks(window, &view))
        return NULL;
    SDL_Rect clipped;
    if (!clipRectToWindow(window, rect, &clipped))
        return NULL;
    return getRenderSurfaceRect(renderer, &clipped);
}

Bool applyShapeMaskOverDrawnRect(Drawable d,
                                 SDL_Renderer *renderer,
                                 SDL_Surface *baseline,
                                 const SDL_Rect *rect)
{
    if (!baseline || !renderer || !rect || rect->w <= 0 || rect->h <= 0)
        return True;
    if (!IS_TYPE(d, WINDOW))
        return True;
    WindowStruct *window = GET_WINDOW_STRUCT(d);
    ShapeMaskView view;
    if (!resolveShapeMasks(window, &view))
        return True;
    /* Match the clipping captureShapeMaskBaseline did so the composite surface
     * is the same size as the baseline.
     */
    SDL_Rect clipped;
    if (!clipRectToWindow(window, rect, &clipped))
        return True;
    SDL_Surface *postDraw = getRenderSurfaceRect(renderer, &clipped);
    if (!postDraw) {
        LOG("%s: readback failed; mask post-process skipped: %s\n", __func__,
            SDL_GetError());
        return False;
    }

    /* Composite: pixels that fall outside the combined shape (i.e. either mask
     * excludes them) are restored from the baseline; pixels inside keep the
     * freshly drawn value. Iterate in 64-bit so window coordinates near INT_MAX
     * can't wrap.
     */
    Bool anyChange = False;
    for (int row = 0; row < clipped.h; row++) {
        int64_t wy = (int64_t) clipped.y + row;
        for (int col = 0; col < clipped.w; col++) {
            int64_t wx = (int64_t) clipped.x + col;
            if (pixelInsideShape(&view, wx, wy))
                continue;
            Uint32 keep =
                getPixel(baseline, (unsigned int) col, (unsigned int) row);
            putPixel(postDraw, (unsigned int) col, (unsigned int) row, keep);
            anyChange = True;
        }
    }
    if (!anyChange) {
        SDL_FreeSurface(postDraw);
        return True;
    }

    Bool ok = True;
    SDL_Texture *upload = SDL_CreateTextureFromSurface(renderer, postDraw);
    if (!upload) {
        LOG("%s: CreateTextureFromSurface failed; masked pixels left visible: "
            "%s\n",
            __func__, SDL_GetError());
        ok = False;
    } else {
        SDL_SetTextureBlendMode(upload, SDL_BLENDMODE_NONE);
        if (SDL_RenderCopy(renderer, upload, NULL, &clipped) != 0) {
            LOG("%s: RenderCopy failed; masked pixels left visible: %s\n",
                __func__, SDL_GetError());
            ok = False;
        }
        SDL_DestroyTexture(upload);
    }
    SDL_FreeSurface(postDraw);
    invalidateSdlDrawStateCache();
    return ok;
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

/* Centralize the graphics-exposures lookup so every XCopyArea exit path
 * tolerates a NULL gc the same way the clip helpers already do.
 */
static Bool gcWantsGraphicsExposures(GC gc)
{
    if (!gc)
        return False;
    GraphicContext *gContext = GET_GC(gc);
    return gContext ? gContext->graphicsExposures : False;
}

static Bool rectInsideDrawable(Drawable drawable, const SDL_Rect *rect)
{
    if (!rect)
        return False;
    int width = 0;
    int height = 0;
    if (!getDrawableSize(drawable, &width, &height))
        return False;
    return rect->x >= 0 && rect->y >= 0 && rect->w >= 0 && rect->h >= 0 &&
           rect->x <= width && rect->y <= height &&
           rect->w <= width - rect->x && rect->h <= height - rect->y;
}

static void postCopyAreaExposure(Display *display,
                                 Drawable src,
                                 Drawable dest,
                                 GC gc,
                                 const SDL_Rect *srcRect,
                                 const SDL_Rect *destRect)
{
    if (!gcWantsGraphicsExposures(gc))
        return;
    if (rectInsideDrawable(src, srcRect)) {
        postEvent(display, dest, NoExpose, X_CopyArea, 0);
        return;
    }
    postEvent(display, dest, GraphicsExpose, (SDL_Rect *) destRect, (size_t) 1,
              X_CopyArea, 0);
}

void setRendererDrawableClip(SDL_Renderer *renderer, const SDL_Rect *clip)
{
    if (!renderer || !clip) {
        if (!renderer || rendererDrawableClip.renderer == renderer) {
            rendererDrawableClip.renderer = NULL;
            rendererDrawableClip.valid = False;
        }
        if (renderer)
            SDL_RenderSetClipRect(renderer, NULL);
        return;
    }
    rendererDrawableClip.renderer = renderer;
    rendererDrawableClip.clip = *clip;
    rendererDrawableClip.valid = True;
    SDL_RenderSetClipRect(renderer, clip);
}

Bool setGcClipForIteration(SDL_Renderer *renderer, GC gc, int iteration)
{
    if (!renderer)
        return False;
    if (!gc) {
        clearRendererClip(renderer);
        return True;
    }
    GraphicContext *gContext = GET_GC(gc);
    if (!gContext || !gContext->clipRectanglesSet) {
        clearRendererClip(renderer);
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
    if (rendererDrawableClip.valid &&
        rendererDrawableClip.renderer == renderer) {
        if (!SDL_IntersectRect(&clip, &rendererDrawableClip.clip, &clip))
            return False;
    }
    return SDL_RenderSetClipRect(renderer, &clip) == 0 ? True : False;
}

void clearRendererClip(SDL_Renderer *renderer)
{
    if (!renderer)
        return;
    if (rendererDrawableClip.valid && rendererDrawableClip.renderer == renderer)
        SDL_RenderSetClipRect(renderer, &rendererDrawableClip.clip);
    else
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

    /* Core X11 pixels have no alpha; the upper byte is conventionally zero
     * (e.g., BlackPixel = 0x00000000, WhitePixel = 0x00FFFFFF). SDL2 treats
     * alpha=0 as fully transparent, so passing those pixels through verbatim
     * makes everything drawn with the default GC foreground invisible. Promote
     * at the single entry point so every primitive routed through
     * applySdlDrawState() inherits the fix.
     */
    color = colorWithOpaqueDefault(color);

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

static void repaintMappedChildrenInRect(Display *display,
                                        Drawable drawable,
                                        const SDL_Rect *rect)
{
    if (rect && IS_TYPE(drawable, WINDOW))
        postExposeEventsForMappedChildren(display, drawable, rect, 1);
    presentDrawableIfVisible(drawable);
}

static int compareInts(const void *a, const void *b)
{
    int lhs = *(const int *) a;
    int rhs = *(const int *) b;
    return (lhs > rhs) - (lhs < rhs);
}

/* Splits and removals can produce more outputs than inputs at any given index,
 * so the helper cannot write back into "segments" in place: a write to
 * segments[outCount] when outCount > i would overwrite an unread input segment.
 * Output through a caller-provided scratch buffer sized for the worst case
 * (every segment splits in two), then copy back.
 */
static void subtractSegment(SDL_Rect *segments,
                            SDL_Rect *scratch,
                            int *segmentCount,
                            int removeX1,
                            int removeX2)
{
    int outCount = 0;
    int count = *segmentCount;
    for (int i = 0; i < count; i++) {
        /* Extents in int64 so a segment near INT_MAX cannot wrap when width is
         * added. Width differences are clamped back into int before writing
         * into SDL_Rect.w (caller-side checks guarantee the post-clamp values
         * are well-formed).
         */
        int64_t segX1 = segments[i].x;
        int64_t segX2 = (int64_t) segments[i].x + (int64_t) segments[i].w;
        if ((int64_t) removeX2 <= segX1 || (int64_t) removeX1 >= segX2) {
            scratch[outCount++] = segments[i];
            continue;
        }
        if ((int64_t) removeX1 > segX1) {
            int64_t w = (int64_t) removeX1 - segX1;
            if (w > INT_MAX)
                w = INT_MAX;
            scratch[outCount++] = (SDL_Rect) {
                .x = (int) segX1,
                .y = segments[i].y,
                .w = (int) w,
                .h = segments[i].h,
            };
        }
        if ((int64_t) removeX2 < segX2) {
            int64_t w = segX2 - (int64_t) removeX2;
            if (w > INT_MAX)
                w = INT_MAX;
            scratch[outCount++] = (SDL_Rect) {
                .x = removeX2,
                .y = segments[i].y,
                .w = (int) w,
                .h = segments[i].h,
            };
        }
    }
    if (outCount > 0)
        memcpy(segments, scratch, sizeof(SDL_Rect) * (size_t) outCount);
    *segmentCount = outCount;
}

static Bool renderFillRectClipByChildren(SDL_Renderer *renderer,
                                         Window window,
                                         const SDL_Rect *rect)
{
    if (!renderer || !rect || rect->w <= 0 || rect->h <= 0)
        return True;
    if (!IS_TYPE(window, WINDOW) ||
        GET_WINDOW_STRUCT(window)->children.length == 0) {
        return SDL_RenderFillRect(renderer, rect) == 0 ? True : False;
    }

    WindowStruct *parent = GET_WINDOW_STRUCT(window);
    size_t childCount = parent->children.length;
    /* Bound the allocation so a pathological window tree can't wrap the malloc
     * size and bypass the OOM guard.
     */
    size_t childLimit = SIZE_MAX / sizeof(SDL_Rect) / 4;
    if (childCount > childLimit)
        return SDL_RenderFillRect(renderer, rect) == 0 ? True : False;
    int *edges = malloc(sizeof(int) * (childCount * 2 + 2));
    SDL_Rect *segments = malloc(sizeof(SDL_Rect) * (childCount + 1));
    SDL_Rect *segScratch = malloc(sizeof(SDL_Rect) * (childCount + 1) * 2);
    if (!edges || !segments || !segScratch) {
        free(edges);
        free(segments);
        free(segScratch);
        return SDL_RenderFillRect(renderer, rect) == 0 ? True : False;
    }

    /* Compute rect extents in int64_t so a rect at the int range cannot wrap
     * before clamping.
     */
    int64_t rectX1_64 = rect->x;
    int64_t rectX2_64 = (int64_t) rect->x + (int64_t) rect->w;
    int64_t rectY2_64 = (int64_t) rect->y + (int64_t) rect->h;
    if (rectX2_64 > INT_MAX)
        rectX2_64 = INT_MAX;
    if (rectY2_64 > INT_MAX)
        rectY2_64 = INT_MAX;
    int rectY2 = (int) rectY2_64;

    int edgeCount = 0;
    edges[edgeCount++] = rect->y;
    edges[edgeCount++] = rectY2;
    Window *children = GET_CHILDREN(window);
    for (size_t i = 0; i < childCount; i++) {
        Window child = children[i];
        if (child == None || !IS_TYPE(child, WINDOW))
            continue;
        WindowStruct *childStruct = GET_WINDOW_STRUCT(child);
        if (childStruct->mapState != Mapped)
            continue;
        SDL_Rect childRect = {
            .x = childStruct->x,
            .y = childStruct->y,
            .w = clampToInt(childStruct->w),
            .h = clampToInt(childStruct->h),
        };
        SDL_Rect overlap;
        if (!SDL_IntersectRect(rect, &childRect, &overlap))
            continue;
        int64_t overlapY2 = (int64_t) overlap.y + (int64_t) overlap.h;
        if (overlapY2 > INT_MAX)
            overlapY2 = INT_MAX;
        edges[edgeCount++] = overlap.y;
        edges[edgeCount++] = (int) overlapY2;
    }
    qsort(edges, (size_t) edgeCount, sizeof(int), compareInts);

    Bool ok = True;
    for (int e = 0; e + 1 < edgeCount; e++) {
        int bandY1 = edges[e], bandY2 = edges[e + 1];
        if (bandY1 == bandY2)
            continue;
        int segmentCount = 1;
        segments[0] = (SDL_Rect) {
            .x = rect->x,
            .y = bandY1,
            .w = rect->w,
            .h = bandY2 - bandY1,
        };
        for (size_t i = 0; i < childCount && segmentCount > 0; i++) {
            Window child = children[i];
            if (child == None || !IS_TYPE(child, WINDOW))
                continue;
            WindowStruct *childStruct = GET_WINDOW_STRUCT(child);
            if (childStruct->mapState != Mapped)
                continue;
            int64_t childY1_64 = childStruct->y;
            int64_t childY2_64 =
                (int64_t) childStruct->y + (int64_t) clampToInt(childStruct->h);
            if (childY2_64 <= bandY1 || childY1_64 >= bandY2)
                continue;
            int64_t childX1_64 = childStruct->x;
            int64_t childX2_64 =
                (int64_t) childStruct->x + (int64_t) clampToInt(childStruct->w);
            if (childX2_64 <= rectX1_64 || childX1_64 >= rectX2_64)
                continue;
            if (childX1_64 < rectX1_64)
                childX1_64 = rectX1_64;
            if (childX2_64 > rectX2_64)
                childX2_64 = rectX2_64;
            subtractSegment(segments, segScratch, &segmentCount,
                            (int) childX1_64, (int) childX2_64);
        }
        for (int i = 0; i < segmentCount; i++) {
            if (segments[i].w <= 0 || segments[i].h <= 0)
                continue;
            if (SDL_RenderFillRect(renderer, &segments[i]) != 0)
                ok = False;
        }
    }

    free(edges);
    free(segments);
    free(segScratch);
    return ok;
}

static Bool getDrawableSize(Drawable drawable, int *width, int *height)
{
    if (IS_TYPE(drawable, WINDOW)) {
        unsigned int w = 0, h = 0;
        GET_WINDOW_DIMS(drawable, w, h);
        *width = clampToInt(w);
        *height = clampToInt(h);
        return True;
    }
    if (IS_TYPE(drawable, PIXMAP)) {
        PixmapStruct *pixmap = GET_PIXMAP_STRUCT(drawable);
        if (!pixmap)
            return False;
        *width = clampToInt(pixmap->width);
        *height = clampToInt(pixmap->height);
        return True;
    }
    return False;
}

/* Cap per-side stroke padding so the bbox arithmetic can't overflow signed int
 * when an X client passes an extreme lineWidth. SDL_Rect is signed int wide;
 * values larger than this practically can't draw anyway.
 */
#define DAMAGE_PAD_CAP 16384

static int64_t arcStrokePad(GC gc)
{
    GraphicContext *gContext = GET_GC(gc);
    int64_t pad = gContext->lineWidth <= 1 ? 1 : (int64_t) gContext->lineWidth;
    if (pad > DAMAGE_PAD_CAP)
        pad = DAMAGE_PAD_CAP;
    return pad;
}

static SDL_Rect arcDamageRect(int x,
                              int y,
                              unsigned int width,
                              unsigned int height,
                              int64_t strokePad)
{
    SDL_Rect rect = {
        .x = clampToInt((int64_t) x - strokePad),
        .y = clampToInt((int64_t) y - strokePad),
        .w = clampToInt((int64_t) width + 1 + 2 * strokePad),
        .h = clampToInt((int64_t) height + 1 + 2 * strokePad),
    };
    return rect;
}

static SDL_Rect arcsUnionBbox(const XArc *arcs, int n_arcs, int64_t strokePad)
{
    SDL_Rect bbox = arcDamageRect(arcs[0].x, arcs[0].y, arcs[0].width,
                                  arcs[0].height, strokePad);
    for (int i = 1; i < n_arcs; i++) {
        SDL_Rect a = arcDamageRect(arcs[i].x, arcs[i].y, arcs[i].width,
                                   arcs[i].height, strokePad);
        unionRect(&bbox, &a, &bbox);
    }
    return bbox;
}

static SDL_Rect lineDamageRect(int x1, int y1, int x2, int y2, int lineWidth)
{
    int64_t minX = x1 < x2 ? x1 : x2, minY = y1 < y2 ? y1 : y2;
    int64_t maxX = x1 > x2 ? x1 : x2, maxY = y1 > y2 ? y1 : y2;
    int64_t pad = lineWidth > 1 ? ((int64_t) lineWidth + 1) / 2 : 1;
    if (pad > DAMAGE_PAD_CAP)
        pad = DAMAGE_PAD_CAP;
    int64_t outX = minX - pad, outY = minY - pad;
    int64_t w = (maxX + pad) - outX + 1, h = (maxY + pad) - outY + 1;
    SDL_Rect rect = {
        .x = clampToInt(outX),
        .y = clampToInt(outY),
        .w = clampToInt(w < 0 ? 0 : w),
        .h = clampToInt(h < 0 ? 0 : h),
    };
    return rect;
}

static SDL_Rect polylineDamageRect(const SDL_Point *points,
                                   int npoints,
                                   int lineWidth)
{
    SDL_Rect rect = lineDamageRect(points[0].x, points[0].y, points[1].x,
                                   points[1].y, lineWidth);
    for (int i = 2; i < npoints; i++) {
        SDL_Rect segment = lineDamageRect(points[i - 1].x, points[i - 1].y,
                                          points[i].x, points[i].y, lineWidth);
        unionRect(&rect, &segment, &rect);
    }
    return rect;
}

static void repaintSegmentDamage(Display *display,
                                 Drawable d,
                                 const XSegment *segments,
                                 int nsegments,
                                 int lineWidth)
{
    for (int i = 0; i < nsegments; i++) {
        SDL_Rect damage =
            lineDamageRect(segments[i].x1, segments[i].y1, segments[i].x2,
                           segments[i].y2, lineWidth);
        repaintMappedChildrenInRect(display, d, &damage);
    }
}

static SDL_Rect segmentsUnionBbox(const XSegment *segments,
                                  int nsegments,
                                  int lineWidth)
{
    SDL_Rect bbox = lineDamageRect(segments[0].x1, segments[0].y1,
                                   segments[0].x2, segments[0].y2, lineWidth);
    for (int i = 1; i < nsegments; i++) {
        SDL_Rect d_i =
            lineDamageRect(segments[i].x1, segments[i].y1, segments[i].x2,
                           segments[i].y2, lineWidth);
        unionRect(&bbox, &d_i, &bbox);
    }
    return bbox;
}

static unsigned long opaqueColorIfAlphaUnset(unsigned long color)
{
    if ((color & (0xFFul << ALPHA_SHIFT)) == 0)
        return color | (0xFFul << ALPHA_SHIFT);
    return color;
}

static void resolveWindowBackground(Window window,
                                    Pixmap *backgroundPixmap,
                                    unsigned long *backgroundColor)
{
    Pixmap pixmap = None;
    unsigned long color = 0;
    Window current = window;
    while (current != None && IS_TYPE(current, WINDOW)) {
        WindowStruct *windowStruct = GET_WINDOW_STRUCT(current);
        pixmap = windowStruct->background;
        color = windowStruct->backgroundColor;
        if (pixmap != (Pixmap) ParentRelative)
            break;
        current = GET_PARENT(current);
    }
    if (backgroundPixmap)
        *backgroundPixmap = pixmap;
    if (backgroundColor)
        *backgroundColor = opaqueColorIfAlphaUnset(color);
}

/* X Shape semantics intersect the two masks: a pixel is "inside" the window iff
 * it is inside both the bounding region (defaults to all-ones when no bounding
 * mask is installed) and the clip region (same default). resolveShapeMasks
 * loads both pointers + their offsets once; pixelInsideShape samples each
 * non-NULL mask in mask-local coordinates and ANDs the results.
 */
static Bool resolveShapeMasks(const WindowStruct *window, ShapeMaskView *out)
{
    if (!window || !out)
        return False;
    out->boundingMask = window->shapeBoundingMask;
    out->boundingOffsetX = window->shapeBoundingOffsetX;
    out->boundingOffsetY = window->shapeBoundingOffsetY;
    out->clipMask = window->shapeClipMask;
    out->clipOffsetX = window->shapeClipOffsetX;
    out->clipOffsetY = window->shapeClipOffsetY;
    return out->boundingMask != NULL || out->clipMask != NULL;
}

/* True when (wx, wy) is "inside" the combined shape: present in every installed
 * mask (a pixel outside a mask's rect, or hitting a black mask pixel, counts as
 * excluded). With no masks installed the caller has no work to do
 * (resolveShapeMasks returns False); callers must consult that first.
 */
static Bool pixelInsideShape(const ShapeMaskView *view, int64_t wx, int64_t wy)
{
    SDL_Surface *masks[2] = {view->boundingMask, view->clipMask};
    int offX[2] = {view->boundingOffsetX, view->clipOffsetX};
    int offY[2] = {view->boundingOffsetY, view->clipOffsetY};
    for (int i = 0; i < 2; i++) {
        SDL_Surface *mask = masks[i];
        if (!mask)
            continue;
        int64_t mx = wx - (int64_t) offX[i], my = wy - (int64_t) offY[i];
        if (mx < 0 || my < 0 || mx >= mask->w || my >= mask->h)
            return False;
        Uint32 mp = getPixel(mask, (unsigned int) mx, (unsigned int) my);
        Uint8 r = 0, g = 0, b = 0;
        SDL_GetRGB(mp, mask->format, &r, &g, &b);
        if (!(r || g || b))
            return False;
    }
    return True;
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
        long long abx = (long long) b.x - a.x, aby = (long long) b.y - a.y;
        long long bcx = (long long) c.x - b.x, bcy = (long long) c.y - b.y;
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

        /* ArcChord rendering requires honoring the chord/pie distinction, which
         * only the path accelerator does. Force the path even for small arcs to
         * avoid the legacy pie-only fallback.
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
    double rx = width / 2.0, ry = height / 2.0;
    double cx = x + rx, cy = y + ry;
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

    /* Guard the row * height multiplication so an oversize intersect cannot
     * wrap size_t before malloc; also keep the pitch within
     * SDL_RenderReadPixels' int pitch argument.
     */
    if (clipped.w <= 0 || clipped.h <= 0 ||
        (size_t) clipped.w > SIZE_MAX / 4u ||
        (size_t) clipped.h > SIZE_MAX / ((size_t) clipped.w * 4u) ||
        (size_t) clipped.w * 4u > (size_t) INT_MAX)
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

/* int64 math: abs(INT_MIN) and 2*err would overflow at 32-bit width. Fast path
 * reads the line's bounding box once, walks Bresenham mutating the in-memory
 * buffer, and blits back once instead of doing a full SDL_RenderReadPixels
 * round trip per pixel. The per-pixel fallback kicks in for huge bboxes so the
 * fast path does not try to allocate gigabytes for a pathological diagonal.
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
    int xLo = x1 < x2 ? x1 : x2, xHi = x1 < x2 ? x2 : x1;
    int yLo = y1 < y2 ? y1 : y2, yHi = y1 < y2 ? y2 : y1;
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

    /* Guard the row * height multiplication so an oversize intersect cannot
     * wrap size_t before malloc; also keep the pitch within
     * SDL_RenderReadPixels' int pitch argument.
     */
    if (clipped.w <= 0 || clipped.h <= 0 ||
        (size_t) clipped.w > SIZE_MAX / 4u ||
        (size_t) clipped.h > SIZE_MAX / ((size_t) clipped.w * 4u) ||
        (size_t) clipped.w * 4u > (size_t) INT_MAX)
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
                int relX = (int) px - clipped.x, relY = (int) py - clipped.y;
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
 * exactly once or raster ops like XOR cancel themselves at the corners; the
 * helper sequences the four edges so corner pixels appear in exactly one
 * segment.
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

    if (SDL_RenderDrawLine(renderer, x1, y, x2, y) != 0)
        LOG("SDL_RenderDrawLine failed in %s: %s\n", __func__, SDL_GetError());
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
    if (npoints < 3)
        return 1;

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

    /* Accumulate CoordModePrevious in int64 so a hostile delta sequence can't
     * signed-overflow before the bbox math sees the point. SDL_Point fields are
     * int; clampToInt saturates so an out-of-range running sum degrades to the
     * boundary instead of wrapping into bogus geometry.
     */
    int64_t accX = points[0].x, accY = points[0].y;
    poly[0].x = clampToInt(accX);
    poly[0].y = clampToInt(accY);
    int minX = poly[0].x, maxX = poly[0].x, minY = poly[0].y, maxY = poly[0].y;
    for (int i = 1; i < npoints; i++) {
        if (mode == CoordModePrevious) {
            accX += points[i].x;
            accY += points[i].y;
        } else {
            accX = points[i].x;
            accY = points[i].y;
        }
        poly[i].x = clampToInt(accX);
        poly[i].y = clampToInt(accY);
        if (poly[i].x < minX)
            minX = poly[i].x;
        if (poly[i].x > maxX)
            maxX = poly[i].x;
        if (poly[i].y < minY)
            minY = poly[i].y;
        if (poly[i].y > maxY)
            maxY = poly[i].y;
    }
    SDL_Rect polyBbox = {
        .x = minX,
        .y = minY,
        .w = clampToInt((int64_t) maxX - minX + 1),
        .h = clampToInt((int64_t) maxY - minY + 1),
    };

    PolygonCrossing *crossings =
        malloc((size_t) npoints * sizeof(PolygonCrossing));
    if (!crossings) {
        free(heapPoints);
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }

    /* Open the shape guard only after both allocations have succeeded so the
     * OOM paths above don't leak the captured baseline.
     */
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &polyBbox);

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
        shapeGuardEnd(&sg);
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
                int x1 = (int) ceil(minX), x2 = (int) ceil(maxX) - 1;
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
    shapeGuardEnd(&sg);
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
    double rx = width / 2.0, ry = height / 2.0;
    if (rx <= 0.0 || ry <= 0.0)
        return 1;
    double cx = x + rx, cy = y + ry;
    double start = angle1 / 64.0;
    double extent = angle2 / 64.0;
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        for (int py = y; py < y + (int) height; py++) {
            for (int px = x; px < x + (int) width; px++) {
                double nx = (px + 0.5 - cx) / rx, ny = (py + 0.5 - cy) / ry;
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
    SDL_Rect arcBbox = arcDamageRect(x, y, width, height, 0);
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &arcBbox);
    int result =
        fillArcOnRenderer(renderer, gc, x, y, width, height, angle1, angle2);
    shapeGuardEnd(&sg);
    if (result)
        presentDrawableIfVisible(d);
    return result;
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
    double rx = width / 2.0, ry = height / 2.0;
    if (rx <= 0.0 || ry <= 0.0)
        return 1;
    double cx = x + rx, cy = y + ry;
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
                for (int ox = -radius; ox <= radius; ox++)
                    SDL_RenderDrawPoint(renderer, px + ox, py + oy);
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
    SDL_Rect arcBbox = arcDamageRect(x, y, width, height, arcStrokePad(gc));
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &arcBbox);
    int result =
        drawArcOnRenderer(renderer, gc, x, y, width, height, angle1, angle2);
    shapeGuardEnd(&sg);
    if (result)
        presentDrawableIfVisible(d);
    return result;
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
    SDL_Rect arcsBbox = arcsUnionBbox(arcs, n_arcs, arcStrokePad(gc));
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &arcsBbox);
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
            if (ok) {
                shapeGuardEnd(&sg);
                presentDrawableIfVisible(d);
                return 1;
            }
        }
    }
    for (int i = 0; i < n_arcs; i++) {
        if (!drawArcOnRenderer(renderer, gc, arcs[i].x, arcs[i].y,
                               arcs[i].width, arcs[i].height, arcs[i].angle1,
                               arcs[i].angle2)) {
            shapeGuardEnd(&sg);
            return 0;
        }
    }
    shapeGuardEnd(&sg);
    presentDrawableIfVisible(d);
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
    SDL_Rect fillBbox = arcsUnionBbox(arcs, n_arcs, 0);
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &fillBbox);
    for (int i = 0; i < n_arcs; i++) {
        if (!fillArcOnRenderer(renderer, gc, arcs[i].x, arcs[i].y,
                               arcs[i].width, arcs[i].height, arcs[i].angle1,
                               arcs[i].angle2)) {
            shapeGuardEnd(&sg);
            return 0;
        }
    }
    shapeGuardEnd(&sg);
    presentDrawableIfVisible(d);
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
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
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
    unsigned long foregroundColor =
        opaqueColorIfAlphaUnset(gContext->foreground);
    unsigned long backgroundColor =
        opaqueColorIfAlphaUnset(gContext->background);
    SDL_PixelFormat *format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    if (!format) {
        SDL_FreeSurface(srcSurface);
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    Uint32 foreground = SDL_MapRGBA(format, GET_RED_FROM_COLOR(foregroundColor),
                                    GET_GREEN_FROM_COLOR(foregroundColor),
                                    GET_BLUE_FROM_COLOR(foregroundColor),
                                    GET_ALPHA_FROM_COLOR(foregroundColor));
    Uint32 background = SDL_MapRGBA(format, GET_RED_FROM_COLOR(backgroundColor),
                                    GET_GREEN_FROM_COLOR(backgroundColor),
                                    GET_BLUE_FROM_COLOR(backgroundColor),
                                    GET_ALPHA_FROM_COLOR(backgroundColor));
    SDL_Rect destRect = {
        .x = dest_x,
        .y = dest_y,
        .w = (int) width,
        .h = (int) height,
    };
    SDL_Surface *destSurface = NULL;
    WindowStruct *destWindow =
        IS_TYPE(dest, WINDOW) ? GET_WINDOW_STRUCT(dest) : NULL;
    ShapeMaskView shapeView;
    Bool hasShape = resolveShapeMasks(destWindow, &shapeView);
    if (hasShape) {
        /* Shape composite needs the destination's prior pixels for
         * mask-excluded positions. Fail rather than mix synthetic GC background
         * into preserved pixels.
         */
        destSurface = getRenderSurfaceRect(destRenderer, &destRect);
        if (!destSurface) {
            SDL_FreeFormat(format);
            SDL_FreeSurface(srcSurface);
            handleError(0, display, dest, 0, BadMatch, 0);
            return 0;
        }
    }
    Uint32 *pixels = malloc((size_t) width * (size_t) height * sizeof(Uint32));
    if (!pixels) {
        SDL_FreeSurface(destSurface);
        SDL_FreeFormat(format);
        SDL_FreeSurface(srcSurface);
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    for (unsigned int y = 0; y < height; y++) {
        for (unsigned int x = 0; x < width; x++) {
            if (hasShape) {
                /* 64-bit math avoids signed overflow when dest_x/dest_y are
                 * near INT_MAX.
                 */
                int64_t wx = (int64_t) dest_x + (int64_t) x;
                int64_t wy = (int64_t) dest_y + (int64_t) y;
                if (!pixelInsideShape(&shapeView, wx, wy)) {
                    pixels[y * width + x] =
                        destSurface ? getPixel(destSurface, x, y) : background;
                    continue;
                }
            }
            Uint32 srcPixel = getPixel(srcSurface, x, y);
            Uint8 red = 0, green = 0, blue = 0;
            SDL_GetRGB(srcPixel, srcSurface->format, &red, &green, &blue);
            Bool bitSet =
                plane == 1 ? (red || green || blue) : ((srcPixel & plane) != 0);
            pixels[y * width + x] = bitSet ? foreground : background;
        }
    }
    SDL_FreeSurface(destSurface);
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
    presentDrawableIfVisible(dest);
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
    SDL_Rect pointRect = {.x = x, .y = y, .w = 1, .h = 1};
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &pointRect);
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (gContext->function != GXcopy) {
            rasterOpRendererPoint(renderer, x, y, gContext->function,
                                  gContext->planeMask, gContext->foreground);
        } else if (SDL_RenderDrawPoint(renderer, x, y) != 0) {
            clearRendererClip(renderer);
            shapeGuardEnd(&sg);
            LOG("SDL_RenderDrawPoint failed in %s: %s\n", __func__,
                SDL_GetError());
            handleError(0, display, d, 0, BadDrawable, 0);
            return 0;
        }
    }
    clearRendererClip(renderer);
    shapeGuardEnd(&sg);
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
    /* Compute the bbox of all points (post-CoordMode resolution) so the shape
     * guard can capture once. An empty/degenerate bbox (no in-range points)
     * yields w/h == 0; shapeGuardBegin treats that as a no-op.
     */
    SDL_Rect pointsBbox = {0, 0, 0, 0};
    int64_t px = 0, py = 0;
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
    Bool haveBbox = False;
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
        int ix = (int) px, iy = (int) py;
        if (!haveBbox) {
            minX = maxX = ix;
            minY = maxY = iy;
            haveBbox = True;
        } else {
            if (ix < minX)
                minX = ix;
            if (ix > maxX)
                maxX = ix;
            if (iy < minY)
                minY = iy;
            if (iy > maxY)
                maxY = iy;
        }
    }
    if (haveBbox) {
        pointsBbox.x = minX;
        pointsBbox.y = minY;
        pointsBbox.w = clampToInt((int64_t) maxX - minX + 1);
        pointsBbox.h = clampToInt((int64_t) maxY - minY + 1);
    }
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &pointsBbox);
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
    shapeGuardEnd(&sg);
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
    if (nsegments <= 0)
        return 1;
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    GraphicContext *gContext = GET_GC(gc);
    SDL_Rect segUnion =
        segmentsUnionBbox(segments, nsegments, gContext->lineWidth);
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &segUnion);
    if (gContext->function == GXcopy && gContext->lineStyle != LineSolid) {
        Bool ok = True;
        for (int i = 0; ok && i < nsegments; i++)
            ok = strokeLineOnRenderer(renderer, gc, segments[i].x1,
                                      segments[i].y1, segments[i].x2,
                                      segments[i].y2);
        shapeGuardEnd(&sg);
        if (ok) {
            repaintSegmentDamage(display, d, segments, nsegments,
                                 gContext->lineWidth);
            return 1;
        }
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
            if (ok) {
                shapeGuardEnd(&sg);
                repaintSegmentDamage(display, d, segments, nsegments,
                                     gContext->lineWidth);
                return 1;
            }
        }
        /* Wide-stroke path failed (most likely pathInit OOM). The fallback
         * below renders each segment with SDL_RenderDrawLine, which is
         * single-pixel only; flag the silent lineWidth downgrade.
         */
        LOG("%s: wide-stroke rasterizer failed; falling back to 1-pixel "
            "lines (lineWidth=%d)\n",
            __func__, gContext->lineWidth);
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
        shapeGuardEnd(&sg);
        repaintSegmentDamage(display, d, segments, nsegments,
                             gContext->lineWidth);
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
    shapeGuardEnd(&sg);
    repaintSegmentDamage(display, d, segments, nsegments, gContext->lineWidth);
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
    SDL_Rect damage = lineDamageRect(x1, y1, x2, y2, gContext->lineWidth);
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &damage);
    Bool wideStrokeWanted =
        gContext->function == GXcopy &&
        (gContext->lineWidth > 1 || gContext->lineStyle != LineSolid);
    if (wideStrokeWanted &&
        strokeLineOnRenderer(renderer, gc, x1, y1, x2, y2)) {
        shapeGuardEnd(&sg);
        repaintMappedChildrenInRect(display, d, &damage);
        return 1;
    }
    /* Wide / non-solid stroke wanted but the path rasterizer failed (typically
     * pathInit OOM). The fallback below uses SDL_RenderDrawLine which is
     * single-pixel only; flag the silent lineWidth downgrade so a regression in
     * the path rasterizer isn't invisible.
     */
    if (wideStrokeWanted)
        LOG("%s: wide-stroke rasterizer failed; falling back to 1-pixel "
            "line (lineWidth=%d, style=%d)\n",
            __func__, gContext->lineWidth, gContext->lineStyle);
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
    shapeGuardEnd(&sg);
    repaintMappedChildrenInRect(display, d, &damage);
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
    /* CoordModePrevious accumulates relative deltas in int64 so a hostile delta
     * sequence can't signed-overflow into bogus coordinates fed to
     * polylineDamageRect; clampToInt saturates back to the SDL_Point int.
     */
    int64_t accX = points[0].x, accY = points[0].y;
    sdlPoints[0].x = clampToInt(accX);
    sdlPoints[0].y = clampToInt(accY);
    for (int i = 1; i < npoints; i++) {
        if (mode == CoordModePrevious) {
            accX += points[i].x;
            accY += points[i].y;
        } else {
            accX = points[i].x;
            accY = points[i].y;
        }
        sdlPoints[i].x = clampToInt(accX);
        sdlPoints[i].y = clampToInt(accY);
    }
    GraphicContext *gContext = GET_GC(gc);
    SDL_Rect lineDamage =
        polylineDamageRect(sdlPoints, npoints, gContext->lineWidth);
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &lineDamage);
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
                shapeGuardEnd(&sg);
                repaintMappedChildrenInRect(display, d, &lineDamage);
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
        shapeGuardEnd(&sg);
        repaintMappedChildrenInRect(display, d, &lineDamage);
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
    shapeGuardEnd(&sg);
    repaintMappedChildrenInRect(display, d, &lineDamage);
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
    if (IS_INPUT_ONLY(w)) {
        handleError(0, dpy, w, 0, BadMatch, 0);
        return 0;
    }

    int winWidth, winHeight;
    GET_WINDOW_DIMS(w, winWidth, winHeight);
    int clearX = x, clearY = y;
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
    if (clearX + clearWidth > winWidth)
        clearWidth = winWidth - clearX;
    if (clearY + clearHeight > winHeight)
        clearHeight = winHeight - clearY;
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
    Pixmap backgroundPixmap = None;
    unsigned long backgroundColor = 0;
    resolveWindowBackground(w, &backgroundPixmap, &backgroundColor);
    ShapeGuard sg;
    shapeGuardBegin(&sg, w, renderer, &clearRect);
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
        if (!renderFillRectClipByChildren(renderer, w, &clearRect)) {
            clearRendererClip(renderer);
            shapeGuardEnd(&sg);
            LOG("SDL_RenderFillRect failed in %s: %s\n", __func__,
                SDL_GetError());
            handleError(0, dpy, w, 0, BadDrawable, 0);
            return 0;
        }
    }
    shapeGuardEnd(&sg);
    repaintMappedChildrenInRect(dpy, w, &clearRect);

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

/* Read-modify-write XCopyArea for non-GXcopy/masked GCs.
 *
 * Returns:
 *   0 - GC is GXcopy with full plane mask; fall through to the fast
 *       plain-blit path.
 *   1 - request handled successfully (read, applied function per
 *       pixel, wrote back).
 * -1 - request failed (intermediate alloc/readback returned an
 *       error). XCopyArea should report failure to the caller.
 */
static int xCopyAreaRasterOp(Display *display,
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
    if (!gc)
        return 0;
    GraphicContext *gContext = GET_GC(gc);
    if (!gContext)
        return 0;
    int gcFunction = gContext->function;
    unsigned long gcPlaneMask = gContext->planeMask;
    if (gcFunction == GXcopy && (gcPlaneMask & 0x00FFFFFFul) == 0x00FFFFFFul)
        return 0;
    if (width == 0 || height == 0)
        return 1;
    if (width > (unsigned int) (INT_MAX / (int) sizeof(Uint32))) {
        handleError(0, display, src, 0, BadValue, 0);
        return -1;
    }

    /* Resolve the source renderer FIRST and read its pixels before the
     * destination renderer is touched. GET_RENDERER can lazily attach an
     * SDL_Texture render target to the drawable, which switches the active
     * target. Resolving dest first and then reading from src would, for a
     * same-renderer src/dest pair, read destination pixels into srcPixels and
     * silently corrupt the GXxor/GXand result.
     */
    SDL_Renderer *srcRenderer = NULL;
    GET_RENDERER(src, srcRenderer);
    if (!srcRenderer) {
        handleError(0, display, src, 0, BadDrawable, 0);
        return -1;
    }
    int destWidth = 0;
    int destHeight = 0;
    if (!getDrawableSize(dest, &destWidth, &destHeight)) {
        handleError(0, display, dest, 0, BadDrawable, 0);
        return -1;
    }
    SDL_Rect requestedDestRect = {dest_x, dest_y, (int) width, (int) height};
    SDL_Rect destBounds = {0, 0, destWidth, destHeight};
    SDL_Rect destRect;
    if (!SDL_IntersectRect(&requestedDestRect, &destBounds, &destRect)) {
        if (gcWantsGraphicsExposures(gc))
            postEvent(display, dest, NoExpose, X_CopyArea, 0);
        return 1;
    }
    int64_t srcReadX = (int64_t) src_x + ((int64_t) destRect.x - dest_x);
    int64_t srcReadY = (int64_t) src_y + ((int64_t) destRect.y - dest_y);
    if (srcReadX < INT_MIN || srcReadX > INT_MAX || srcReadY < INT_MIN ||
        srcReadY > INT_MAX) {
        handleError(0, display, src, 0, BadValue, 0);
        return -1;
    }
    SDL_Rect requestedSrcRect = {src_x, src_y, (int) width, (int) height};
    SDL_Rect srcRect = {(int) srcReadX, (int) srcReadY, destRect.w, destRect.h};

    size_t pixelCount = (size_t) destRect.w * (size_t) destRect.h;
    if (pixelCount > SIZE_MAX / sizeof(Uint32))
        return -1;
    /* calloc instead of malloc: some SDL_RenderReadPixels backends silently
     * partial-fill when the requested rect intersects out-of-bounds; the rest
     * stays zero rather than feeding uninitialized heap into
     * applyRasterFunction.
     */
    Uint32 *srcPixels = calloc(pixelCount, sizeof(Uint32));
    Uint32 *destPixels = calloc(pixelCount, sizeof(Uint32));
    if (!srcPixels || !destPixels) {
        free(srcPixels);
        free(destPixels);
        handleOutOfMemory(0, display, 0, 0);
        return -1;
    }

    /* Free path is unified: all error exits jump to cleanup_pixels, which frees
     * both pixel buffers. srcPixels is no longer touched after the pixel-math
     * loop, but holding onto it until the single cleanup site is far simpler
     * than threading separate free sites.
     */
    size_t pitchSize = (size_t) destRect.w * sizeof(Uint32);
    int pitch = (int) pitchSize;
    SDL_Texture *texture = NULL;
    SDL_Renderer *destRenderer = NULL;
    /* SDL_RenderReadPixels reads absolute render-target coordinates and ignores
     * the active viewport, while the caller passes srcRect / destRect in
     * drawable-local (viewport-relative) coordinates. For a child window whose
     * renderer is offset, raw read would pull from the wrong pixels. Translate
     * by each renderer's viewport origin the same way getRenderSurfaceRect does
     * for its readback.
     */
    SDL_Rect srcViewport;
    SDL_RenderGetViewport(srcRenderer, &srcViewport);
    SDL_Rect srcReadRect = {srcRect.x + srcViewport.x,
                            srcRect.y + srcViewport.y, srcRect.w, srcRect.h};
    if (SDL_RenderReadPixels(srcRenderer, &srcReadRect,
                             SDL_PIXELFORMAT_ARGB8888, srcPixels, pitch) != 0) {
        LOG("SDL_RenderReadPixels src failed in %s: %s\n", __func__,
            SDL_GetError());
        goto cleanup_pixels;
    }
    GET_RENDERER(dest, destRenderer);
    if (!destRenderer) {
        handleError(0, display, dest, 0, BadDrawable, 0);
        goto cleanup_pixels;
    }
    SDL_Rect destViewport;
    SDL_RenderGetViewport(destRenderer, &destViewport);
    SDL_Rect destReadRect = {destRect.x + destViewport.x,
                             destRect.y + destViewport.y, destRect.w,
                             destRect.h};
    if (SDL_RenderReadPixels(destRenderer, &destReadRect,
                             SDL_PIXELFORMAT_ARGB8888, destPixels,
                             pitch) != 0) {
        LOG("SDL_RenderReadPixels dest failed in %s: %s\n", __func__,
            SDL_GetError());
        goto cleanup_pixels;
    }

    for (size_t i = 0; i < pixelCount; i++) {
        destPixels[i] = applyRasterFunction(gcFunction, (Uint32) gcPlaneMask,
                                            srcPixels[i], destPixels[i]);
    }

    SDL_Surface *surface =
        SDL_CreateRGBSurfaceWithFormatFrom(destPixels, destRect.w, destRect.h,
                                           32, pitch, SDL_PIXELFORMAT_ARGB8888);
    if (!surface)
        goto cleanup_pixels;
    texture = SDL_CreateTextureFromSurface(destRenderer, surface);
    SDL_FreeSurface(surface);
    if (!texture)
        goto cleanup_pixels;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);

    ShapeGuard sg;
    shapeGuardBegin(&sg, dest, destRenderer, &destRect);
    int clipCount = getGcClipIterationCount(gc);
    int rc = 0;
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(destRenderer, gc, clip))
            continue;
        if (SDL_RenderCopy(destRenderer, texture, NULL, &destRect) != 0) {
            LOG("SDL_RenderCopy failed in %s raster path: %s\n", __func__,
                SDL_GetError());
            rc = -1;
            break;
        }
    }
    clearRendererClip(destRenderer);
    Bool shapeOk = shapeGuardEnd(&sg);
    SDL_DestroyTexture(texture);
    free(srcPixels);
    free(destPixels);
    if (rc != 0) {
        handleError(0, display, src, 0, BadMatch, 0);
        return -1;
    }
    postCopyAreaExposure(display, src, dest, gc, &requestedSrcRect, &destRect);
    if (shapeOk)
        presentDrawableIfVisible(dest);
    return 1;

cleanup_pixels:
    free(srcPixels);
    free(destPixels);
    return -1;
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
    /* Reject anything whose extents would overflow signed int. Downstream
     * SDL_Rect math uses int x/y/w/h and would wrap; capping width/height alone
     * is not enough because src_x + width or dest_x + width can still overflow
     * with a negative-leaning coordinate.
     */
    if (width > (unsigned int) INT_MAX || height > (unsigned int) INT_MAX ||
        (int64_t) src_x + (int64_t) width > INT_MAX ||
        (int64_t) src_y + (int64_t) height > INT_MAX ||
        (int64_t) dest_x + (int64_t) width > INT_MAX ||
        (int64_t) dest_y + (int64_t) height > INT_MAX) {
        handleError(0, display, src, 0, BadValue, 0);
        return 0;
    }
    /* Non-GXcopy or masked plane masks need per-pixel arithmetic that
     * SDL_RenderCopy cannot express. xCopyAreaRasterOp does the slow
     * read-modify-write through SDL_RenderReadPixels and returns 1 if it
     * handled the request, 0 to fall through to the fast plain-blit path.
     */
    {
        int rasterRc = xCopyAreaRasterOp(display, src, dest, gc, src_x, src_y,
                                         width, height, dest_x, dest_y);
        if (rasterRc != 0)
            return rasterRc > 0 ? 1 : 0;
    }
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
                ShapeGuard fastSg;
                shapeGuardBegin(&fastSg, dest, destRenderer, &fastDest);
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
                Bool fastShapeOk = shapeGuardEnd(&fastSg);
                if (fastRcCopy != 0) {
                    handleError(0, display, src, 0, BadMatch, 0);
                    return 0;
                }
                postCopyAreaExposure(display, src, dest, gc, &fastSrc,
                                     &fastDest);
                if (fastShapeOk)
                    presentDrawableIfVisible(dest);
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
        SDL_Rect requestedSrcRect = srcRect;
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
        destRenderer = getWindowRenderer(dest);
        if (!destRenderer) {
            SDL_DestroyTexture(srcTexture);
            handleError(0, display, dest, 0, BadDrawable, 0);
            return 0;
        }
        /* XCopyArea is GXcopy: destination pixels are replaced, not blended.
         * SDL_RenderCopy ignores renderer draw color/blend state, so only the
         * texture blend mode matters here.
         */
        SDL_SetTextureBlendMode(srcTexture, SDL_BLENDMODE_NONE);
        srcRect.x = 0;
        srcRect.y = 0;
        ShapeGuard sg;
        shapeGuardBegin(&sg, dest, destRenderer, &destRect);
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
        Bool slowShapeOk = shapeGuardEnd(&sg);
        if (rcCopy != 0) {
            handleError(0, display, src, 0, BadMatch, 0);
            SDL_DestroyTexture(srcTexture);
            return 0;
        }
        SDL_DestroyTexture(srcTexture);
        postCopyAreaExposure(display, src, dest, gc, &requestedSrcRect,
                             &destRect);
        if (slowShapeOk)
            presentDrawableIfVisible(dest);
        return 1;
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
    SDL_Texture *srcTexture =
        SDL_CreateTextureFromSurface(destRenderer, srcSurface);
    SDL_FreeSurface(srcSurface);
    if (!srcTexture) {
        LOG("SDL_CreateTextureFromSurface failed in %s pixmap path: %s\n",
            __func__, SDL_GetError());
        handleError(0, display, src, 0, BadMatch, 0);
        return 0;
    }

    SDL_Rect copySrc = {
        .x = 0,
        .y = 0,
        .w = (int) width,
        .h = (int) height,
    };
    SDL_Rect destRect = {
        .x = dest_x,
        .y = dest_y,
        .w = (int) width,
        .h = (int) height,
    };
    SDL_SetTextureBlendMode(srcTexture, SDL_BLENDMODE_NONE);
    int clipCount = getGcClipIterationCount(gc);
    int rcCopy = 0;
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(destRenderer, gc, clip))
            continue;
        if (SDL_RenderCopy(destRenderer, srcTexture, &copySrc, &destRect) !=
            0) {
            LOG("SDL_RenderCopy failed in %s pixmap path: %s\n", __func__,
                SDL_GetError());
            rcCopy = -1;
            break;
        }
    }
    clearRendererClip(destRenderer);
    SDL_DestroyTexture(srcTexture);
    if (rcCopy != 0) {
        handleError(0, display, src, 0, BadMatch, 0);
        return 0;
    }

    postCopyAreaExposure(display, src, dest, gc, &srcRect, &destRect);
    presentDrawableIfVisible(dest);
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
    /* SDL_RenderDrawRect outlines a w-by-h pixel rect; the X11 spec is (w+1) by
     * (h+1). Clamp in int64 so an unsigned width near UINT_MAX can't wrap into
     * a negative SDL_Rect.w or wrap the path corners.
     */
    SDL_Rect rectDamage = {
        .x = x,
        .y = y,
        .w = clampToInt((int64_t) width + 1),
        .h = clampToInt((int64_t) height + 1),
    };
    int rightX = clampToInt((int64_t) x + width);
    int bottomY = clampToInt((int64_t) y + height);
    if (gContext->function == GXcopy &&
        (gContext->lineWidth > 1 || gContext->lineStyle != LineSolid)) {
        Path path;
        if (pathInit(&path)) {
            ShapeGuard sg;
            shapeGuardBegin(&sg, d, renderer, &rectDamage);
            Bool ok = pathMoveTo(&path, x, y) && pathLineTo(&path, rightX, y) &&
                      pathLineTo(&path, rightX, bottomY) &&
                      pathLineTo(&path, x, bottomY) && pathLineTo(&path, x, y);
            if (ok)
                ok = rasterStrokePathOnRenderer(renderer, gc, &path);
            pathFree(&path);
            shapeGuardEnd(&sg);
            if (ok) {
                repaintMappedChildrenInRect(display, d, &rectDamage);
                return 1;
            }
        }
    }
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &rectDamage);
    if (gContext->function != GXcopy) {
        int clipCount = getGcClipIterationCount(gc);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip))
                continue;
            rasterOpRendererRectOutline(renderer, x, y, width, height,
                                        gContext->function, gContext->planeMask,
                                        gContext->foreground);
        }
    } else {
        applySdlDrawState(renderer, gc, SDL_BLENDMODE_BLEND,
                          gContext->foreground);
        int clipCount = getGcClipIterationCount(gc);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip))
                continue;
            if (SDL_RenderDrawRect(renderer, &rectDamage)) {
                LOG("SDL_RenderDrawRect failed in %s: %s\n", __func__,
                    SDL_GetError());
            }
        }
    }
    clearRendererClip(renderer);
    shapeGuardEnd(&sg);
    repaintMappedChildrenInRect(display, d, &rectDamage);
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
    if (nrectangles < 0) {
        LOG("Invalid number of rectangles in %s: %d\n", __func__, nrectangles);
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    if (nrectangles == 0)
        return 1;
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(d, renderer);
    if (!renderer) {
        LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, d, 0, BadDrawable, 0);
        return 0;
    }
    /* nrectangles is caller-supplied; an unbounded VLA is a stack-overflow path
     * on hostile or buggy input. Use a small inline buffer for the common case
     * and the heap when the request is larger.
     */
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
    int validRectangles = 0;
    for (i = 0; i < nrectangles; i++) {
        if (rectangles[i].width == 0 || rectangles[i].height == 0)
            continue;
        sdlRectangles[validRectangles].x = (int) rectangles[i].x;
        sdlRectangles[validRectangles].y = (int) rectangles[i].y;
        sdlRectangles[validRectangles].w = (int) rectangles[i].width;
        sdlRectangles[validRectangles].h = (int) rectangles[i].height;
        LOG("{x = %d, y = %d, w = %d, h = %d}\n",
            sdlRectangles[validRectangles].x, sdlRectangles[validRectangles].y,
            sdlRectangles[validRectangles].w, sdlRectangles[validRectangles].h);
        validRectangles++;
    }
    if (validRectangles == 0) {
        free(heapRectangles);
        return 1;
    }
    GraphicContext *gContext = GET_GC(gc);
    LOG("bgColor: 0x%08lx, fgColor: 0x%08lx\n", gContext->background,
        gContext->foreground);
    /* Snapshot the union of all rectangles before drawing so shape-mask
     * post-processing can restore mask-excluded pixels in one pass.
     */
    SDL_Rect shapeUnion = sdlRectangles[0];
    for (int r = 1; r < validRectangles; r++)
        unionRect(&shapeUnion, &sdlRectangles[r], &shapeUnion);
    SDL_Surface *shapeBase = captureShapeMaskBaseline(d, renderer, &shapeUnion);
    if (gContext->fillStyle == FillSolid) {
        LOG("Fill_style is %s\n", "FillSolid");
        if (gContext->function != GXcopy) {
            /* SDL_Renderer has no raster-op equivalent, so read the affected
             * pixels back, apply the GC function in software, and blit them in
             * place.
             */
            int clipCount = getGcClipIterationCount(gc);
            for (int clip = 0; clip < clipCount; clip++) {
                if (!setGcClipForIteration(renderer, gc, clip))
                    continue;
                for (int r = 0; r < validRectangles; r++) {
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
                                        validRectangles)) {
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
            if (SDL_RenderFillRects(renderer, &sdlRectangles[0],
                                    validRectangles)) {
                LOG("SDL_RenderFillRects failed in %s: %s\n", __func__,
                    SDL_GetError());
            }
        }
        clearRendererClip(renderer);
    } else if (gContext->fillStyle == FillStippled) {
        LOG("Fill_style is %s\n", "FillStippled");
    }
    Bool shapeOk = True;
    if (shapeBase) {
        shapeOk =
            applyShapeMaskOverDrawnRect(d, renderer, shapeBase, &shapeUnion);
        SDL_FreeSurface(shapeBase);
    }
    if (shapeOk) {
        for (int r = 0; r < validRectangles; r++)
            repaintMappedChildrenInRect(display, d, &sdlRectangles[r]);
    }
    free(heapRectangles);
    return 1;
}
