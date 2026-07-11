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
#include "mac-live-resize.h"
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

/* Coalesce window for a client repaint burst. A host-driven resize hands the
 * client a single ConfigureNotify at drag end; the client (e.g. xwpe) responds
 * by clearing its whole window and redrawing every cell, issuing several
 * XFlush/XSync calls mid-burst. Each of those would otherwise present a
 * half-cleared backing, showing a black flash before the redraw lands. While
 * this deadline is in the future, drawWindowDataToScreen accumulates the dirty
 * region but skips the actual present, so only the final complete frame reaches
 * the screen. endCoalesceClientRepaint (called when the client returns to its
 * idle input wait) presents once and clears the deadline; the deadline itself
 * is a safety cap so a client that never idles still repaints. Nanoseconds
 * since the monotonic clock; 0 means no coalescing in effect. Stored as two
 * 32-bit halves because SDL_atomic_t is 32-bit. */
static SDL_atomic_t coalesceDeadlineHiNs = {0};
static SDL_atomic_t coalesceDeadlineLoNs = {0};

/* Upper bound on how long a single client repaint burst may withhold presents.
 * Comfortably longer than the observed xwpe reflow (a few frames) yet short
 * enough that a misbehaving client recovers in well under a human-perceptible
 * stall. */
#define COALESCE_CLIENT_REPAINT_MAX_MS 150

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

/* Maximum number of independent dirty rects to track on a single window before
 * collapsing to fullyDirty. Keeps SDL_UpdateWindowSurfaceRects in its per-rect
 * path on every backend that has been measured; once the region exceeds this
 * cap, a single full-window readback is cheaper than per-rect bookkeeping.
 * Hard-clamped to [1, 256] when read from the env.
 */
#define DIRTY_RECT_CAP_DEFAULT 16

static int dirtyRectCap(void)
{
    static int cached = 0;
    if (cached != 0)
        return cached;
    int value = DIRTY_RECT_CAP_DEFAULT;
    const char *env = getenv("LIBX11_COMPAT_DIRTY_MAX_RECTS");
    if (env && *env) {
        char *end = NULL;
        long parsed = strtol(env, &end, 10);
        if (end != env && parsed >= 1 && parsed <= 256)
            value = (int) parsed;
    }
    cached = value;
    return cached;
}

static void unionPresentRect(WindowStruct *windowStruct, const SDL_Rect *rect)
{
    if (!windowStruct)
        return;

    if (!rect) {
        /* NULL marks the whole window dirty. The region cannot become any
         * dirtier before the next present, so the union below can short
         * circuit. The legacy bounding rect mirrors the same intent by staying
         * clear of hasPresentRect; drawWindowDataToScreen falls back to the
         * full window when hasPresentRect is False.
         */
        windowStruct->fullyDirty = True;
        return;
    }
    if (rect->w <= 0 || rect->h <= 0)
        return;
    /* Pixman validates `x + w` and `y + h` as signed-int extents and flags the
     * rect with a stderr BUG print when they overflow. Drawing primitives
     * feeding this path can synthesize wildly extreme bboxes from parent-chain
     * accumulation or unclamped arc math, so guard the addition here. A
     * degenerate-or-overflowing extent contributes no visible pixels, so
     * collapsing to fullyDirty rather than logging a pixman error is the safest
     * fallback for top-level callers that expect the present pipeline to absorb
     * anything they hand it.
     */
    if (rect->x > INT_MAX - rect->w || rect->y > INT_MAX - rect->h) {
        windowStruct->fullyDirty = True;
        return;
    }

    if (!windowStruct->hasPresentRect) {
        windowStruct->presentRect = *rect;
        windowStruct->hasPresentRect = True;
    } else {
        unionRect(&windowStruct->presentRect, rect, &windowStruct->presentRect);
    }

    /* Mirror into the new region. fullyDirty stays sticky once set; the
     * present-side path will clear it on a successful flush.
     */
    if (windowStruct->fullyDirty)
        return;

    pixman_region32_union_rect(&windowStruct->dirty, &windowStruct->dirty,
                               rect->x, rect->y, (unsigned int) rect->w,
                               (unsigned int) rect->h);
    if (pixman_region32_n_rects(&windowStruct->dirty) > dirtyRectCap())
        windowStruct->fullyDirty = True;
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

static Uint32 presentWakeTimerCallback(XC_TIMER_CALLBACK_PARAMS)
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
        /* destroyScreenWindow destroys children with freeParentData False, so
         * the array transiently holds already-destroyed ids: destroyWindow
         * leaves them typed CLOSED_WINDOW with a NULL data pointer. Skip any
         * non-WINDOW id; GET_WINDOW_STRUCT would return NULL and deref-crash.
         */
        if (!IS_TYPE(children[i], WINDOW))
            continue;
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

static uint64_t coalesceDeadlineGet(void)
{
    uint64_t hi = (uint32_t) SDL_AtomicGet(&coalesceDeadlineHiNs);
    uint64_t lo = (uint32_t) SDL_AtomicGet(&coalesceDeadlineLoNs);
    return (hi << 32) | lo;
}

static void coalesceDeadlineSet(uint64_t ns)
{
    SDL_AtomicSet(&coalesceDeadlineLoNs, (int) (uint32_t) ns);
    SDL_AtomicSet(&coalesceDeadlineHiNs, (int) (uint32_t) (ns >> 32));
}

/* True while a client repaint burst is being coalesced (deadline in the
 * future). Expires itself once the safety cap passes so a stuck client still
 * presents on the next draw. */
static Bool coalesceActive(void)
{
    uint64_t deadline = coalesceDeadlineGet();
    if (deadline == 0)
        return False;
    if (monotonicNowNs() >= deadline) {
        coalesceDeadlineSet(0);
        return False;
    }
    return True;
}

void beginCoalesceClientRepaint(void)
{
    coalesceDeadlineSet(monotonicNowNs() +
                        (uint64_t) COALESCE_CLIENT_REPAINT_MAX_MS * 1000000ULL);
}

void endCoalesceClientRepaint(void)
{
    if (coalesceDeadlineGet() == 0)
        return;
    coalesceDeadlineSet(0);
    drawWindowDataToScreen();
}

Bool libx11CompatForceSoftwarePresent(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("LIBX11_COMPAT_FORCE_SOFTWARE_PRESENT");
        cached = (env && *env && *env != '0' && *env != 'f' && *env != 'F' &&
                  *env != 'n' && *env != 'N')
                     ? 1
                     : 0;
    }
    return cached ? True : False;
}

Bool libx11CompatAcceleratedPresentUsable(void)
{
    /* The accelerated present path reads each frame back with
     * SDL_RenderReadPixels and re-presents through a per-window renderer. Some
     * drivers hand back a renderer whose readback is a no-op (SDL2's dummy
     * driver on headless CI is the motivating case): SDL_CreateRenderer
     * succeeds, so a driver-name check is not enough, but the readback never
     * reflects what was drawn, so hasPresented would never be set and mapped
     * windows would silently never present. Probe the exact capability once on
     * a throwaway hidden window: draw a known colour into a target texture,
     * read it back, and require the pixel to round-trip. Cache the verdict so
     * every realizeTopLevelWindow reuses it. A failed probe drives the
     * SDL_GetWindowSurface software present, which does not depend on renderer
     * readback and matches the pre-accelerated behaviour. */
    static int cached = -1;
    if (cached >= 0)
        return cached ? True : False;

    cached = 0; /* pessimistic default: any probe failure keeps software */
    SDL_Window *probeWin =
        SDL_CreateWindow("libx11-compat-probe", 0, 0, 4, 4, SDL_WINDOW_HIDDEN);
    if (!probeWin)
        return False;
    SDL_Renderer *probeRenderer =
        xc_CreateRenderer(probeWin, XC_RENDERER_ACCELERATED);
    if (probeRenderer) {
        SDL_Texture *probeTex =
            SDL_CreateTexture(probeRenderer, SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_TARGET, 1, 1);
        if (probeTex) {
            Uint32 readPixel = 0;
            SDL_Rect one = {0, 0, 1, 1};
            /* SDL_SetRenderTarget / SDL_RenderReadPixels are the int-normalized
             * wrappers on SDL3 (== 0 means success on both SDL2 and SDL3).
             * SetRenderDrawColor / RenderClear flip their result convention
             * between SDL2 and SDL3, so leave them unchecked as the accelerated
             * present does; the readback comparison below is the real gate.
             * Read exactly one pixel with a matching pitch so the readback
             * cannot overrun the single-Uint32 destination. */
            if (SDL_SetRenderTarget(probeRenderer, probeTex) == 0) {
                SDL_SetRenderDrawColor(probeRenderer, 0x12, 0x34, 0x56, 0xFF);
                SDL_RenderClear(probeRenderer);
                if (SDL_RenderReadPixels(probeRenderer, &one,
                                         SDL_PIXELFORMAT_RGBA8888, &readPixel,
                                         (int) sizeof(readPixel)) == 0) {
                    /* RGBA8888 packs R in the most-significant byte. Compare
                     * the colour channels only; ignore alpha so a driver that
                     * drops it still passes. */
                    cached = ((readPixel >> 24) & 0xFF) == 0x12 &&
                                     ((readPixel >> 16) & 0xFF) == 0x34 &&
                                     ((readPixel >> 8) & 0xFF) == 0x56
                                 ? 1
                                 : 0;
                }
            }
            SDL_DestroyTexture(probeTex);
        }
        SDL_DestroyRenderer(probeRenderer);
    }
    SDL_DestroyWindow(probeWin);
    if (!cached)
        LOG("accelerated present probe failed; using software present\n");
    return cached ? True : False;
}

/* Present one top-level window through its per-window accelerated renderer.
 *
 * Mirrors the software present path's dirty-region walk (the pixman region, the
 * DIRTY_RECT_BUDGET cap, and the fullyDirty / presentRect fallback) but instead
 * of reading SCREEN backing pixels into the SDL window surface it reads each
 * dirty rect into a per-window CPU scratch and uploads only those rects into a
 * STREAMING texture that persists across frames. Every frame then clears the
 * drawable to the window background and RenderCopy's the whole texture, so the
 * complete frame is always defined (the backbuffer is not persistent under
 * double/triple buffering) while only the changed pixels are read back. The
 * clear covers any right/bottom band exposed while a live resize has grown the
 * drawable ahead of the backing, so no separate margin fill is needed here.
 *
 * The caller has already set screenTargetMutated so the SCREEN render target is
 * restored once after the whole loop; this helper leaves the SCREEN target on
 * child->sdlTexture like the software path does.
 *
 * Returns True on a successful present (bookkeeping cleared), False if the
 * frame was dropped (needsPresent left set to retry next frame).
 */
static Bool presentWindowAccelerated(Window win,
                                     WindowStruct *child,
                                     SDL_Renderer *screen,
                                     uint64_t *readbackNs,
                                     uint64_t *updateNs,
                                     uint64_t *presentedPixels)
{
    int w, h;
    GET_WINDOW_DIMS(win, w, h);
    int texW = 0, texH = 0;
    SDL_QueryTexture(child->sdlTexture, NULL, NULL, &texW, &texH);
    int bw = w < texW ? w : texW;
    int bh = h < texH ? h : texH;
    if (bw <= 0 || bh <= 0)
        return False;
    SDL_Rect bounds = {0, 0, bw, bh};

    /* Lazily (re)create the streaming texture to match the current backing.
     * Single owner: no separate hook in resizeWindowTexture is needed. */
    if (!child->presentTexture || child->presentTexW != bw ||
        child->presentTexH != bh) {
        if (child->presentTexture)
            SDL_DestroyTexture(child->presentTexture);
        child->presentTexture =
            SDL_CreateTexture(child->presentRenderer, SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_STREAMING, bw, bh);
        if (!child->presentTexture) {
            LOG("SDL_CreateTexture(streaming) failed in %s: %s\n", __func__,
                SDL_GetError());
            child->presentTexW = 0;
            child->presentTexH = 0;
            return False;
        }
        child->presentTexW = bw;
        child->presentTexH = bh;
        /* A freshly (re)created streaming texture holds no prior content, so
         * the whole backing must be re-read this frame regardless of the dirty
         * region carried over from the old size. */
        child->fullyDirty = True;
    }

    /* Build the dirty-rect list exactly like the software path. */
    enum { DIRTY_RECT_BUDGET = 256 };
    SDL_Rect rects[DIRTY_RECT_BUDGET];
    int nrects = 0;
    Bool useRegion = !child->fullyDirty && child->hasPresented &&
                     child->hasPresentRect &&
                     pixman_region32_n_rects(&child->dirty) > 0;
    if (useRegion) {
        int rn = pixman_region32_n_rects(&child->dirty);
        if (rn > DIRTY_RECT_BUDGET) {
            useRegion = False;
        } else {
            pixman_box32_t *boxes =
                pixman_region32_rectangles(&child->dirty, NULL);
            for (int b = 0; b < rn; b++) {
                SDL_Rect r = {
                    .x = boxes[b].x1,
                    .y = boxes[b].y1,
                    .w = boxes[b].x2 - boxes[b].x1,
                    .h = boxes[b].y2 - boxes[b].y1,
                };
                if (!SDL_IntersectRect(&r, &bounds, &r))
                    continue;
                rects[nrects++] = r;
            }
        }
    }
    if (!useRegion || nrects == 0) {
        SDL_Rect r = {0, 0, bw, bh};
        if (!child->fullyDirty && child->hasPresented && child->hasPresentRect)
            r = child->presentRect;
        if (!SDL_IntersectRect(&r, &bounds, &r))
            return False;
        rects[0] = r;
        nrects = 1;
    }

    /* Read each dirty rect from the SCREEN backing into the CPU scratch, then
     * upload just that rect into the persistent streaming texture. The scratch
     * holds one full-width band per rect (rect->h rows at bw pitch). */
    if (SDL_SetRenderTarget(screen, child->sdlTexture) != 0) {
        LOG("SDL_SetRenderTarget(backing) failed in %s: %s\n", __func__,
            SDL_GetError());
        return False;
    }
    if (SDL_RenderSetViewport(screen, NULL) != 0 ||
        SDL_RenderSetClipRect(screen, NULL) != 0) {
        LOG("SDL_RenderSet{Viewport,ClipRect}(NULL) failed in %s: %s\n",
            __func__, SDL_GetError());
        return False;
    }

    size_t need = (size_t) bw * 4u;
    if (need > child->presentReadbackCap) {
        unsigned char *grown = realloc(child->presentReadback, need);
        if (!grown) {
            LOG("realloc(presentReadback) failed in %s\n", __func__);
            return False;
        }
        child->presentReadback = grown;
        child->presentReadbackCap = need;
    }

    uint64_t readStart = monotonicNowNs();
    uint64_t framePixels = 0;
    for (int r = 0; r < nrects; r++) {
        int rowPitch = rects[r].w * 4;
        for (int row = 0; row < rects[r].h; row++) {
            SDL_Rect line = {rects[r].x, rects[r].y + row, rects[r].w, 1};
            if (SDL_RenderReadPixels(screen, &line, SDL_PIXELFORMAT_RGBA8888,
                                     child->presentReadback, rowPitch) != 0) {
                LOG("SDL_RenderReadPixels failed in %s: %s\n", __func__,
                    SDL_GetError());
                return False;
            }
            SDL_Rect dst = {rects[r].x, rects[r].y + row, rects[r].w, 1};
            if (SDL_UpdateTexture(child->presentTexture, &dst,
                                  child->presentReadback, rowPitch) != 0) {
                /* Bail like the readback failure above: leave needsPresent and
                 * the dirty region set so the frame is retried, instead of
                 * marking it presented and stranding stale pixels on screen. */
                LOG("SDL_UpdateTexture failed in %s: %s\n", __func__,
                    SDL_GetError());
                return False;
            }
        }
        framePixels += (uint64_t) rects[r].w * (uint64_t) rects[r].h;
    }
    *readbackNs += monotonicNowNs() - readStart;

    uint64_t updateStart = monotonicNowNs();
#if defined(__APPLE__)
    if (libx11CompatInLiveResizePresent()) {
        void *nsWindow = sdlCocoaWindowHandle(child->sdlWindow);
        if (nsWindow)
            libx11CompatConfigureLiveResizeLayer(nsWindow);
    }
#endif
    unsigned long bg = resolvedWindowBackgroundColor(win);
    SDL_SetRenderDrawColor(child->presentRenderer, GET_RED_FROM_COLOR(bg),
                           GET_GREEN_FROM_COLOR(bg), GET_BLUE_FROM_COLOR(bg),
                           GET_ALPHA_FROM_COLOR(bg));
    SDL_RenderClear(child->presentRenderer);
    SDL_Rect dstCopy = {0, 0, bw, bh};
    SDL_RenderCopy(child->presentRenderer, child->presentTexture, NULL,
                   &dstCopy);
    SDL_RenderPresent(child->presentRenderer);
    *updateNs += monotonicNowNs() - updateStart;

    child->needsPresent = False;
    child->hasPresentRect = False;
    pixman_region32_clear(&child->dirty);
    child->fullyDirty = False;
    child->hasPresented = True;
    *presentedPixels += framePixels;
    timelineTapPresent(win, (size_t) nrects, framePixels);
    return True;
}

void drawWindowDataToScreen()
{
    if (!hasPendingWindowPresent()) {
        SDL_AtomicSet(&presentWakePending, False);
        return;
    }
    /* Hold intermediate presents back while a client repaint burst is in
     * flight: the dirty region stays accumulated on each WindowStruct and a
     * present wake is (re)scheduled so the final complete frame still lands
     * even if the client never returns to its idle wait before the safety cap.
     */
    if (coalesceActive()) {
        /* This pass may have been driven by the present wake event, which
         * leaves presentWakePending set. Clear it before rescheduling:
         * otherwise schedulePresentWake short-circuits on the still-set flag,
         * no new timer arms, and a burst that outlives its first wake is
         * stranded past the safety cap with no present. */
        SDL_AtomicSet(&presentWakePending, False);
        schedulePresentWake();
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
        /* A child destroyed mid-teardown lingers in the array typed
         * CLOSED_WINDOW; skip it before GET_WINDOW_STRUCT returns NULL (see
         * hasPendingWindowPresent).
         */
        if (!IS_TYPE(children[i], WINDOW))
            continue;
        WindowStruct *child = GET_WINDOW_STRUCT(children[i]);
        if (!child->sdlWindow)
            continue;

        /* A window presenting through a GLX/ANGLE CAMetalLayer drives its own
         * swap via eglSwapBuffers; the 2D software present (texture fill plus
         * SDL_UpdateWindowSurfaceRects) would fight the Metal layer for the
         * same pixels. Skip it so Metal owns the window. glxMetalView is only
         * ever set on the macOS on-screen GLX path, NULL everywhere else. Clear
         * the software-present bookkeeping (as the success path below does) so
         * a pending mark does not keep hasPendingWindowPresent waking the
         * present loop every frame, and any accumulated dirty region is
         * released.
         */
        if (child->glxMetalView) {
            if (child->needsPresent) {
                child->needsPresent = False;
                child->hasPresentRect = False;
                pixman_region32_clear(&child->dirty);
                child->fullyDirty = False;
                child->hasPresented = True;
            }
            continue;
        }

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

        /* Accelerated windows never touch SDL_GetWindowSurface (mutually
         * exclusive with the per-window renderer). The helper mutates the
         * SCREEN render target for its readback, so flag the shared restore
         * below. */
        if (!child->presentUsesSoftware && child->presentRenderer) {
            screenTargetMutated = True;
            if (presentWindowAccelerated(children[i], child, screen,
                                         &readbackNs, &updateNs,
                                         &presentedPixels))
                presentedWindows++;
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
        /* Build the read rect list. fullyDirty (sentinel set by
         * unionPresentRect on a NULL mark or rect-count overflow), or a window
         * that has never presented before, falls back to a single full-window
         * rect; otherwise walk the cached dirty region and pre-clamp each box
         * against the X11 logical size, the backing texture size, and the SDL
         * window surface size. Window surface and X11 logical w/h can diverge
         * during resize and on high-DPI setups; an unclamped read overflows
         * winSurface->pixels.
         */
        int texW = 0, texH = 0;
        SDL_QueryTexture(child->sdlTexture, NULL, NULL, &texW, &texH);
        int clampW = w;
        if (texW < clampW)
            clampW = texW;
        int clampH = h;
        if (texH < clampH)
            clampH = texH;
        if (clampW <= 0 || clampH <= 0)
            continue;
        /* Option 1b keeps the X11 backing at physical pixels, so the backing
         * texture and the host window surface are the same size in steady state
         * and the present is a 1:1 blit. During a live host resize SDL updates
         * the window surface before the app repaints the backing at the new
         * size, so winSurface can momentarily be larger (or smaller) than the
         * backing. Never bilinearly upscale that transient mismatch: it turns
         * one out-of-date frame into visibly blurred glyphs for the whole drag.
         * Instead present 1:1 into the overlapping region and leave any margin
         * as background; the app's imminent repaint (resizeWindowTexture
         * already recreated the backing at the new size and marked it fully
         * dirty) delivers the sharp full frame. Clamp to the smaller of the
         * backing and the live surface so the readback and blit stay in bounds
         * either way.
         */
        if (winSurface->w < clampW)
            clampW = winSurface->w;
        if (winSurface->h < clampH)
            clampH = winSurface->h;
        if (clampW <= 0 || clampH <= 0)
            continue;
        SDL_Rect bounds = {0, 0, clampW, clampH};
        /* One-time per-window log recording the backing vs surface sizes.
         * Under Option 1b the X11 backing is physical, so in steady state
         * winSurface and the backing texture match and the present is 1:1.
         */
        if (!child->loggedPresentScale) {
            int rendererOutputW = 0, rendererOutputH = 0;
            SDL_GetRendererOutputSize(screen, &rendererOutputW,
                                      &rendererOutputH);
            LOG("present-scale: window=%lu x11=(%dx%d) tex=(%dx%d) "
                "winSurface=(%dx%d) rendererOutput=(%dx%d)\n",
                children[i], w, h, texW, texH, winSurface->w, winSurface->h,
                rendererOutputW, rendererOutputH);
            child->loggedPresentScale = True;
        }

        enum { DIRTY_RECT_BUDGET = 256 };
        SDL_Rect rects[DIRTY_RECT_BUDGET];
        int nrects = 0;
        Bool useRegion = !child->fullyDirty && child->hasPresented &&
                         child->hasPresentRect &&
                         pixman_region32_n_rects(&child->dirty) > 0;
        if (useRegion) {
            int rn = pixman_region32_n_rects(&child->dirty);
            if (rn > DIRTY_RECT_BUDGET) {
                /* Should never trip: unionPresentRect collapses to fullyDirty
                 * once the dirtyRectCap is exceeded. Fall back to the bounding
                 * extent so a future cap bump does not silently truncate.
                 */
                useRegion = False;
            } else {
                pixman_box32_t *boxes =
                    pixman_region32_rectangles(&child->dirty, NULL);
                for (int b = 0; b < rn; b++) {
                    SDL_Rect r = {
                        .x = boxes[b].x1,
                        .y = boxes[b].y1,
                        .w = boxes[b].x2 - boxes[b].x1,
                        .h = boxes[b].y2 - boxes[b].y1,
                    };
                    if (!SDL_IntersectRect(&r, &bounds, &r))
                        continue;
                    rects[nrects++] = r;
                }
            }
        }
        if (!useRegion || nrects == 0) {
            SDL_Rect r = {0, 0, clampW, clampH};
            /* When fullyDirty is set, always read back the entire clamped
             * window. presentRect may carry over a smaller bounding extent from
             * prior partial marks in the same pending cycle; using it here
             * would under-cover the actual damage. The region-walk path above
             * already gated on !fullyDirty, so this fallback handles fullyDirty
             * + no-region + no-prior-present together.
             */
            if (!child->fullyDirty && child->hasPresented &&
                child->hasPresentRect)
                r = child->presentRect;
            if (!SDL_IntersectRect(&r, &bounds, &r))
                continue;
            rects[0] = r;
            nrects = 1;
        }
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
        Uint32 winFmt = XC_SURFACE_FMT_ENUM(winSurface);
        Uint32 readFmt = SDL_PIXELFORMAT_RGBA8888;
        int readRc = 0;
        SDL_Rect surfaceRects[DIRTY_RECT_BUDGET];
        uint64_t readStart = monotonicNowNs();
        if (winFmt == readFmt) {
            /* Direct readback into the window's framebuffer. Saves one
             * full-image memcpy on every present. SDL_RenderReadPixels accepts
             * repeated writes into a single locked surface, so locking once
             * across the per-rect loop amortizes the lock cost (which rebinds
             * GL state on some backends).
             */
            if (SDL_LockSurface(winSurface) == 0) {
                for (int r = 0; r < nrects; r++) {
                    Uint8 *pixels =
                        (Uint8 *) winSurface->pixels +
                        (size_t) rects[r].y * (size_t) winSurface->pitch +
                        (size_t) rects[r].x *
                            (size_t) XC_SURFACE_BYTESPERPIXEL(winSurface);
                    int rc = SDL_RenderReadPixels(screen, &rects[r], readFmt,
                                                  pixels, winSurface->pitch);
                    if (rc != 0) {
                        readRc = rc;
                        break;
                    }
                    surfaceRects[r] = rects[r];
                }
                SDL_UnlockSurface(winSurface);
            } else {
                readRc = -1;
            }
        } else {
            for (int r = 0; r < nrects; r++) {
                SDL_Surface *staging = SDL_CreateRGBSurfaceWithFormat(
                    0, rects[r].w, rects[r].h, 32, readFmt);
                if (!staging) {
                    readRc = -1;
                    break;
                }
                int rc = SDL_RenderReadPixels(screen, &rects[r], readFmt,
                                              staging->pixels, staging->pitch);
                if (rc == 0) {
                    SDL_Rect dst = rects[r];
                    SDL_BlitSurface(staging, NULL, winSurface, &dst);
                    surfaceRects[r] = dst;
                } else {
                    readRc = rc;
                }
                SDL_FreeSurface(staging);
                if (readRc != 0)
                    break;
            }
        }
        readbackNs += monotonicNowNs() - readStart;
        if (readRc == 0) {
            uint64_t updateStart = monotonicNowNs();
#if defined(__APPLE__)
            /* During the modal live-resize loop the observer drives this
             * present re-entrantly. By default AppKit stretches the last
             * drawable to the animating window bounds (contentsGravity =
             * kCAGravityResize), blurring glyphs until the next real frame.
             * Configure SDL's CAMetalLayer once with topLeft gravity + the
             * backing scale so AppKit pins the stale drawable top-left instead
             * of stretching it; contents is left untouched so SDL's normal
             * present keeps rendering real frames via nextDrawable (no CGImage
             * upload, no black window, no stuck resize cursor). Idempotent, so
             * calling it on each live-resize frame is cheap. No-op if the
             * handle is not a Cocoa window.
             */
            if (libx11CompatInLiveResizePresent()) {
                void *nsWindow = sdlCocoaWindowHandle(child->sdlWindow);
                if (nsWindow)
                    libx11CompatConfigureLiveResizeLayer(nsWindow);
            }
            SDL_UpdateWindowSurfaceRects(child->sdlWindow, surfaceRects,
                                         nrects);
#else
            SDL_UpdateWindowSurfaceRects(child->sdlWindow, surfaceRects,
                                         nrects);
#endif
            /* During a live host resize the window surface grows before the
             * backing is recreated, so a band of surface to the right of and
             * below the presented backing has no source pixels. On macOS the
             * client's reflow only lands once the drag settles (the modal loop
             * delivers no intermediate ConfigureNotify and the observer path
             * coalesces reflows while the drag keeps moving), so this band is
             * visible for the whole grow drag. Fill it with the window's real
             * background colour, exactly as real X11 paints a freshly exposed
             * region and as resizeWindowTexture / XClearArea clear the backing.
             *
             * An earlier version scanned inward from the content edge to guess
             * a "field colour" to extend. That is unsound: the edge row/column
             * is content, not background - the bottom function-key bar, a
             * dialog border, or leftover File Manager pixels - so the scan
             * propagated that content into the new band as a visible
             * reflection, worst of all after shrinking then re-growing (the
             * lagging backing still holds the old, taller frame at its edge). A
             * flat background fill cannot reflect content and matches the
             * surrounding cleared area.
             *
             * This runs whenever the surface is larger than the presented
             * backing, whether or not a client reflow hook is registered: a
             * client hook only repaints the whole new area on ticks where its
             * reflow actually ran, and on the intervening moving ticks the
             * backing lags the live surface (clampW/clampH are the smaller
             * backing extent). On any tick where the reflow caught up (or no
             * resize is in flight) clampW/clampH equal the surface, the
             * condition is false, and nothing runs - a no-op in steady state.
             */
            if (winSurface->w > clampW || winSurface->h > clampH) {
                unsigned long bg = resolvedWindowBackgroundColor(children[i]);
                Uint32 fill = SDL_MapRGBA(
                    XC_SURFACE_FORMAT(winSurface), GET_RED_FROM_COLOR(bg),
                    GET_GREEN_FROM_COLOR(bg), GET_BLUE_FROM_COLOR(bg),
                    GET_ALPHA_FROM_COLOR(bg));
                SDL_Rect marginRects[2];
                int marginCount = 0;
                if (winSurface->w > clampW) {
                    SDL_Rect right = {clampW, 0, winSurface->w - clampW,
                                      clampH};
                    SDL_FillRect(winSurface, &right, fill);
                    marginRects[marginCount++] = right;
                }
                if (winSurface->h > clampH) {
                    /* Full width so the bottom-right corner (below and right of
                     * the content) is covered by this pass too. */
                    SDL_Rect bottom = {0, clampH, winSurface->w,
                                       winSurface->h - clampH};
                    SDL_FillRect(winSurface, &bottom, fill);
                    marginRects[marginCount++] = bottom;
                }
                if (marginCount > 0)
                    SDL_UpdateWindowSurfaceRects(child->sdlWindow, marginRects,
                                                 marginCount);
            }
            updateNs += monotonicNowNs() - updateStart;
            child->needsPresent = False;
            child->hasPresentRect = False;
            pixman_region32_clear(&child->dirty);
            child->fullyDirty = False;
            child->hasPresented = True;
            presentedWindows++;
            uint64_t windowPixels = 0;
            for (int r = 0; r < nrects; r++) {
                windowPixels += (uint64_t) rects[r].w * (uint64_t) rects[r].h;
            }
            presentedPixels += windowPixels;
            timelineTapPresent(children[i], (size_t) nrects, windowPixels);
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

/* Composite an embedded GL child widget's read-back frame into its window. An
 * embedded GL widget (e.g. a Motif GLwDrawingArea) is a child window with no
 * native surface of its own, so its GL renders to an offscreen pbuffer; these
 * pixels (GL bottom-up, GL_RGBA byte order) are drawn through the normal
 * child-window render target so the widget appears in its toolkit shell. Called
 * from src/glx.c at glXSwapBuffers.
 */
unsigned char *glxAcquireCompositeReadback(Window window, size_t need)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (need > windowStruct->glxReadbackCap) {
        unsigned char *grown = realloc(windowStruct->glxReadbackBuf, need);
        if (!grown)
            return NULL;
        windowStruct->glxReadbackBuf = grown;
        windowStruct->glxReadbackCap = need;
    }
    return windowStruct->glxReadbackBuf;
}

void glxCompositeToWindow(Window window,
                          const unsigned char *rgba,
                          int width,
                          int height)
{
    /* Cap dimensions so the size math below and the int row pitch cannot
     * overflow; a real GL drawable is orders of magnitude smaller.
     */
    if (!rgba || width <= 0 || height <= 0 || width >= 32768 || height >= 32768)
        return;
    SDL_Renderer *renderer = getWindowRenderer(window);
    if (!renderer)
        return;
    /* ABGR8888 in SDL's little-endian byte order is R,G,B,A in memory, matching
     * glReadPixels(GL_RGBA, GL_UNSIGNED_BYTE); SDL_RenderCopy converts to the
     * window texture's own format. GL is bottom-up, so flip the rows on the CPU
     * into a scratch buffer (cheaper than wrapping SDL_RenderCopyEx).
     *
     * The staging texture and flip buffer are cached on the widget's
     * WindowStruct and reused across frames; only a size change reallocates
     * them. An animating canvas at a fixed size therefore does no per-frame
     * malloc or texture create/destroy, just the flip memcpy and the
     * upload/copy.
     */
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (width != windowStruct->glxCompositeW ||
        height != windowStruct->glxCompositeH) {
        if (windowStruct->glxCompositeTexture)
            SDL_DestroyTexture(windowStruct->glxCompositeTexture);
        free(windowStruct->glxCompositeFlip);
        windowStruct->glxCompositeTexture =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888,
                              SDL_TEXTUREACCESS_STREAMING, width, height);
        windowStruct->glxCompositeFlip = malloc((size_t) width * height * 4);
        windowStruct->glxCompositeW = width;
        windowStruct->glxCompositeH = height;
        if (!windowStruct->glxCompositeTexture ||
            !windowStruct->glxCompositeFlip) {
            /* Reset so a later frame retries the allocation cleanly. */
            if (windowStruct->glxCompositeTexture) {
                SDL_DestroyTexture(windowStruct->glxCompositeTexture);
                windowStruct->glxCompositeTexture = NULL;
            }
            free(windowStruct->glxCompositeFlip);
            windowStruct->glxCompositeFlip = NULL;
            windowStruct->glxCompositeW = 0;
            windowStruct->glxCompositeH = 0;
            return;
        }
    }
    unsigned char *flipped = windowStruct->glxCompositeFlip;
    for (int row = 0; row < height; row++)
        memcpy(flipped + (size_t) row * width * 4,
               rgba + (size_t) (height - 1 - row) * width * 4,
               (size_t) width * 4);
    SDL_UpdateTexture(windowStruct->glxCompositeTexture, NULL, flipped,
                      width * 4);
    SDL_Rect dst = {0, 0, width, height};
    SDL_RenderCopy(renderer, windowStruct->glxCompositeTexture, NULL, &dst);
    invalidateSdlDrawStateCache();

    /* Present the top-level ancestor over the widget's rect; walk up
     * accumulating the child offset the same way getWindowRenderer does.
     */
    Window top = window;
    int ox = 0, oy = 0;
    while (GET_PARENT(top) != None && !GET_WINDOW_STRUCT(top)->sdlWindow &&
           GET_WINDOW_STRUCT(top)->mapState != UnMapped) {
        int px, py;
        GET_WINDOW_POS(top, px, py);
        ox += px;
        oy += py;
        top = GET_PARENT(top);
    }
    SDL_Rect region = {ox, oy, width, height};
    markWindowNeedsPresentRect(top, &region);
}

/* Walk drawable up to its mapped top-level window, translating an optional
 * child-local rect into top-level backing coordinates along the way. Every
 * window in a tree shares the top-level's backing texture, so the present and
 * text-stamp bookkeeping all key off this single coordinate space.
 */
static Bool drawableTopLevelRect(Drawable drawable,
                                 const SDL_Rect *local,
                                 Window *topWindow,
                                 SDL_Rect *topRect)
{
    if (!IS_TYPE(drawable, WINDOW))
        return False;
    SDL_Rect r = local ? *local : (SDL_Rect) {0, 0, 0, 0};
    Window window = (Window) drawable;
    while (window != None && window != SCREEN_WINDOW) {
        if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
            *topWindow = window;
            *topRect = r;
            return True;
        }
        int x = 0, y = 0;
        GET_WINDOW_POS(window, x, y);
        r.x += x;
        r.y += y;
        window = GET_PARENT(window);
    }
    return False;
}

/* text-stamp cache (see drawing.h) */
#define TEXT_STAMP_CAPACITY 256

typedef struct {
    Window topWindow;
    SDL_Rect topRect;
    Font fontXid;
    Uint32 foreground;
    char *string;
    Uint64 lastUsed;
    Bool inUse;
} TextStampEntry;

static TextStampEntry textStamps[TEXT_STAMP_CAPACITY];
static Uint64 textStampClock = 0;
/* Live stamp count, so the invalidation hook on the hot present path can bail
 * out before scanning when nothing uses core text (atomic read needs no lock).
 */
static SDL_atomic_t textStampActive = {0};
static SDL_mutex *textStampMutex = NULL;
static SDL_SpinLock textStampMutexInitLock = 0;

static SDL_mutex *textStampEnsureMutex(void)
{
    if (!textStampMutex) {
        SDL_AtomicLock(&textStampMutexInitLock);
        if (!textStampMutex)
            textStampMutex = SDL_CreateMutex();
        SDL_AtomicUnlock(&textStampMutexInitLock);
    }
    return textStampMutex;
}

static void textStampLock(void)
{
    SDL_mutex *m = textStampEnsureMutex();
    if (m)
        SDL_LockMutex(m);
}

static void textStampUnlock(void)
{
    if (textStampMutex)
        SDL_UnlockMutex(textStampMutex);
}

static void textStampEvict(TextStampEntry *entry)
{
    if (!entry->inUse)
        return;
    free(entry->string);
    entry->string = NULL;
    entry->inUse = False;
    SDL_AtomicAdd(&textStampActive, -1);
}

/* Caller holds the stamp lock. Drop every stamp on topWindow whose cell
 * intersects rect, since those backing pixels were just overwritten.
 */
static void textStampInvalidateLocked(Window topWindow, const SDL_Rect *rect)
{
    for (int i = 0; i < TEXT_STAMP_CAPACITY; i++) {
        if (!textStamps[i].inUse || textStamps[i].topWindow != topWindow)
            continue;
        SDL_Rect overlap;
        if (SDL_IntersectRect(&textStamps[i].topRect, rect, &overlap))
            textStampEvict(&textStamps[i]);
    }
}

static void invalidateTextStampsTopRect(Window topWindow, const SDL_Rect *rect)
{
    if (!rect || rect->w <= 0 || rect->h <= 0)
        return;
    if (!SDL_AtomicGet(&textStampActive))
        return;
    textStampLock();
    textStampInvalidateLocked(topWindow, rect);
    textStampUnlock();
}

/* Drop stamps overlapping a child-local rect. Used by XClearArea (which fills
 * the backing without routing through the present choke point) and by the text
 * paths that overwrite a cell without recording a stamp of their own.
 */
void invalidateTextStampsForDrawableRect(Drawable drawable,
                                         const SDL_Rect *local)
{
    Window topWindow;
    SDL_Rect topRect;
    if (!drawableTopLevelRect(drawable, local, &topWindow, &topRect))
        return;
    invalidateTextStampsTopRect(topWindow, &topRect);
}

Bool textStampLookup(Drawable drawable,
                     const SDL_Rect *cell,
                     Font fontXid,
                     Uint32 foreground,
                     const char *string)
{
    Window topWindow;
    SDL_Rect topRect;
    if (!string || !drawableTopLevelRect(drawable, cell, &topWindow, &topRect))
        return False;
    Bool found = False;
    textStampLock();
    for (int i = 0; i < TEXT_STAMP_CAPACITY; i++) {
        TextStampEntry *e = &textStamps[i];
        if (!e->inUse || e->topWindow != topWindow || e->fontXid != fontXid ||
            e->foreground != foreground)
            continue;
        if (e->topRect.x != topRect.x || e->topRect.y != topRect.y ||
            e->topRect.w != topRect.w || e->topRect.h != topRect.h)
            continue;
        if (e->string && !strcmp(e->string, string)) {
            e->lastUsed = ++textStampClock;
            found = True;
            break;
        }
    }
    textStampUnlock();
    return found;
}

void textStampRecord(Drawable drawable,
                     const SDL_Rect *cell,
                     Font fontXid,
                     Uint32 foreground,
                     const char *string)
{
    Window topWindow;
    SDL_Rect topRect;
    if (!string || !drawableTopLevelRect(drawable, cell, &topWindow, &topRect))
        return;
    if (topRect.w <= 0 || topRect.h <= 0)
        return;
    char *dup = strdup(string);
    if (!dup)
        return;
    textStampLock();
    /* This label now owns the cell; drop any stamp it overwrites, then take a
     * free slot or evict the least-recently-used one.
     */
    textStampInvalidateLocked(topWindow, &topRect);
    int slot = -1;
    Uint64 oldest = 0;
    for (int i = 0; i < TEXT_STAMP_CAPACITY; i++) {
        if (!textStamps[i].inUse) {
            slot = i;
            break;
        }
        if (slot < 0 || textStamps[i].lastUsed < oldest) {
            oldest = textStamps[i].lastUsed;
            slot = i;
        }
    }
    if (textStamps[slot].inUse)
        textStampEvict(&textStamps[slot]);
    textStamps[slot].topWindow = topWindow;
    textStamps[slot].topRect = topRect;
    textStamps[slot].fontXid = fontXid;
    textStamps[slot].foreground = foreground;
    textStamps[slot].string = dup;
    textStamps[slot].lastUsed = ++textStampClock;
    /* Publish the count before marking the slot live so a concurrent lockless
     * early-out that observes a non-zero count is guaranteed to then take the
     * lock and scan this fully-populated entry.
     */
    SDL_AtomicAdd(&textStampActive, 1);
    textStamps[slot].inUse = True;
    textStampUnlock();
}

void flushTextStampsForWindow(Window window)
{
    if (!SDL_AtomicGet(&textStampActive))
        return;
    Window topWindow;
    SDL_Rect topRect;
    /* A geometry or stacking change to any window in the tree can move or
     * destroy the cells we stamped, so flush the whole top-level wholesale.
     */
    if (!drawableTopLevelRect(window, NULL, &topWindow, &topRect))
        topWindow = window;
    textStampLock();
    for (int i = 0; i < TEXT_STAMP_CAPACITY; i++) {
        if (textStamps[i].inUse && textStamps[i].topWindow == topWindow)
            textStampEvict(&textStamps[i]);
    }
    textStampUnlock();
}

void freeTextStamps(void)
{
    textStampLock();
    for (int i = 0; i < TEXT_STAMP_CAPACITY; i++) {
        if (textStamps[i].inUse)
            textStampEvict(&textStamps[i]);
    }
    textStampUnlock();
}

static void presentDrawableRectIfVisibleEx(Drawable drawable,
                                           const SDL_Rect *rect,
                                           Bool invalidateStamps)
{
    Bool haveRect = rect && rect->w > 0 && rect->h > 0;
    Window topWindow;
    SDL_Rect topRect;
    if (!drawableTopLevelRect(drawable, haveRect ? rect : NULL, &topWindow,
                              &topRect))
        return;
    if (haveRect) {
        if (invalidateStamps)
            invalidateTextStampsTopRect(topWindow, &topRect);
        markWindowNeedsPresentRect(topWindow, &topRect);
    } else {
        if (invalidateStamps)
            flushTextStampsForWindow(topWindow);
        markWindowNeedsPresent(topWindow);
    }
}

void presentDrawableRectIfVisible(Drawable drawable, const SDL_Rect *rect)
{
    presentDrawableRectIfVisibleEx(drawable, rect, True);
}

void presentDrawableRectIfVisibleNoStampInvalidate(Drawable drawable,
                                                   const SDL_Rect *rect)
{
    presentDrawableRectIfVisibleEx(drawable, rect, False);
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

    SDL_Texture *target = GET_WINDOW_STRUCT(drawWindow)->sdlTexture;
    if (target) {
        if (SDL_GetRenderTarget(renderer) != target &&
            SDL_SetRenderTarget(renderer, target) != 0) {
            LOG("SDL_SetRenderTarget failed in %s: %s\n", __func__,
                SDL_GetError());
        }
    } else if (IS_MAPPED_TOP_LEVEL_WINDOW(drawWindow) ||
               drawWindow == SCREEN_WINDOW) {
        if (SDL_GetRenderTarget(renderer) != NULL &&
            SDL_SetRenderTarget(renderer, NULL) != 0) {
            LOG("SDL_SetRenderTarget(NULL) failed in %s: %s\n", __func__,
                SDL_GetError());
        }
    }
    LOG("Setting viewport to {x = %d, y = %d, w = %d, h = %d}\n", viewPort.x,
        viewPort.y, viewPort.w, viewPort.h);
    SDL_Rect currentViewport;
    SDL_RenderGetViewport(renderer, &currentViewport);
    if ((currentViewport.x != viewPort.x || currentViewport.y != viewPort.y ||
         currentViewport.w != viewPort.w || currentViewport.h != viewPort.h) &&
        SDL_RenderSetViewport(renderer, &viewPort)) {
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

unsigned long x11compat_pixmap_readback_reads = 0;

/* GET_RENDERER binds a pixmap as a render target for both draws and source
 * readbacks (XCopyArea, XShape), so this over-approximates: a source read of a
 * pixmap also dirties its cache. That never returns stale pixels, it only drops
 * the cache after read-only use. Routing those source reads through
 * getPixmapSurfaceRect would recover the caching, but the win is confined to
 * apps that interleave XCopyArea-from-pixmap with XGetImage-of-the-same-pixmap,
 * so it is left out until a workload asks for it. Access is unguarded, matching
 * the single-threaded SDL renderer path the rest of the drawing code assumes.
 */
void markPixmapReadbackDirty(Drawable drawable)
{
    PixmapStruct *pixmap = GET_PIXMAP_STRUCT(drawable);
    if (pixmap)
        pixmap->readbackDirty = True;
}

void freePixmapReadback(PixmapStruct *pixmap)
{
    if (!pixmap || !pixmap->readback)
        return;
    SDL_FreeSurface(pixmap->readback);
    pixmap->readback = NULL;
    pixmap->readbackDirty = True;
}

/* Saved shared-renderer state for a pixmap readback: the target it was pointing
 * at plus its viewport and clip, so the readback can bind the pixmap texture
 * and then put everything back.
 */
typedef struct {
    SDL_Texture *target;
    SDL_Rect viewport;
    SDL_bool clipEnabled;
    SDL_Rect clip;
} SavedRendererState;

/* Bind pixmap texture as the render target for a readback, saving the shared
 * renderer's target/viewport/clip into "saved". Viewport and clip are cleared
 * only after the target bind succeeds, so a failed bind leaves the renderer
 * untouched (nothing to undo).
 *
 * Returns False on bind failure.
 */
static Bool bindPixmapTarget(SDL_Renderer *renderer,
                             SDL_Texture *texture,
                             SavedRendererState *saved)
{
    saved->target = SDL_GetRenderTarget(renderer);
    SDL_RenderGetViewport(renderer, &saved->viewport);
    saved->clipEnabled = SDL_RenderIsClipEnabled(renderer);
    if (saved->clipEnabled)
        SDL_RenderGetClipRect(renderer, &saved->clip);
    if (SDL_SetRenderTarget(renderer, texture) != 0) {
        LOG("SDL_SetRenderTarget failed in %s: %s\n", __func__, SDL_GetError());
        return False;
    }
    SDL_RenderSetViewport(renderer, NULL);
    SDL_RenderSetClipRect(renderer, NULL);
    return True;
}

/* Undo bindPixmapTarget. Restore the viewport and clip only once the target is
 * back: if the target restore fails the renderer is already wedged, and aiming
 * viewport/clip at the wrong target would only compound it.
 */
static void restorePixmapTarget(SDL_Renderer *renderer,
                                const SavedRendererState *saved)
{
    if (SDL_SetRenderTarget(renderer, saved->target) != 0) {
        LOG("SDL_SetRenderTarget restore failed in %s: %s\n", __func__,
            SDL_GetError());
        return;
    }
    SDL_RenderSetViewport(renderer, &saved->viewport);
    SDL_RenderSetClipRect(renderer, saved->clipEnabled ? &saved->clip : NULL);
}

/* Refresh pixmap->readback with the full pixmap contents when dirty. Binds the
 * pixmap texture directly rather than through GET_RENDERER so the read does not
 * re-mark itself dirty.
 *
 * Returns the cached surface (borrowed) or NULL on error.
 */
static SDL_Surface *ensurePixmapReadback(PixmapStruct *pixmap)
{
    if (pixmap->readback && !pixmap->readbackDirty)
        return pixmap->readback;

    SDL_Renderer *renderer = GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
    if (!renderer || !pixmap->texture)
        return NULL;

    if (!pixmap->readback) {
        /* Request RGBA8888 explicitly so the surface layout matches the
         * SDL_RenderReadPixels format below byte for byte on every platform,
         * rather than relying on the DEFAULT_* masks resolving to it.
         */
        pixmap->readback = SDL_CreateRGBSurfaceWithFormat(
            0, (int) pixmap->width, (int) pixmap->height, SDL_SURFACE_DEPTH,
            SDL_PIXELFORMAT_RGBA8888);
        if (!pixmap->readback) {
            LOG("SDL_CreateRGBSurfaceWithFormat failed in %s: %s\n", __func__,
                SDL_GetError());
            return NULL;
        }
        SDL_SetSurfaceBlendMode(pixmap->readback, SDL_BLENDMODE_NONE);
    }

    SavedRendererState saved;
    if (!bindPixmapTarget(renderer, pixmap->texture, &saved))
        return NULL;
    SDL_Rect fullRect = {0, 0, (int) pixmap->width, (int) pixmap->height};
    int rc =
        SDL_RenderReadPixels(renderer, &fullRect, SDL_PIXELFORMAT_RGBA8888,
                             pixmap->readback->pixels, pixmap->readback->pitch);
    restorePixmapTarget(renderer, &saved);
    if (rc != 0) {
        LOG("SDL_RenderReadPixels failed in %s: %s\n", __func__,
            SDL_GetError());
        return NULL;
    }
    x11compat_pixmap_readback_reads++;
    pixmap->readbackDirty = False;
    return pixmap->readback;
}

/* Above this many pixels a full-pixmap readback cache is not worth keeping
 * resident (a 2048x2048 RGBA surface is 16 MB). Larger pixmaps read just the
 * requested rect directly instead, matching the pre-cache memory profile. The
 * ceiling is a flat pixel count; a smarter eviction policy can replace it if a
 * real workload needs one.
 */
#define MAX_CACHED_PIXMAP_PIXELS (2048u * 2048u)

/* Read a single rect straight from the pixmap texture, bypassing the cache.
 * Binds the target directly (not via GET_RENDERER) so the read does not mark
 * the pixmap dirty, and restores the shared renderer's target, viewport and
 * clip so an in-progress draw on the previous target is not disturbed.
 */
static SDL_Surface *readPixmapRectUncached(PixmapStruct *pixmap,
                                           const SDL_Rect *source)
{
    SDL_Renderer *renderer = GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
    if (!renderer || !pixmap->texture)
        return NULL;
    SavedRendererState saved;
    if (!bindPixmapTarget(renderer, pixmap->texture, &saved))
        return NULL;
    SDL_Surface *out = getRenderSurfaceRect(renderer, source);
    restorePixmapTarget(renderer, &saved);
    return out;
}

SDL_Surface *getPixmapSurfaceRect(Pixmap pixmap, const SDL_Rect *source)
{
    PixmapStruct *pixmapStruct = GET_PIXMAP_STRUCT(pixmap);
    if (!pixmapStruct || !source || source->w <= 0 || source->h <= 0)
        return NULL;

    if ((uint64_t) pixmapStruct->width * (uint64_t) pixmapStruct->height >
        MAX_CACHED_PIXMAP_PIXELS)
        return readPixmapRectUncached(pixmapStruct, source);

    SDL_Surface *cache = ensurePixmapReadback(pixmapStruct);
    if (!cache)
        return NULL;

    SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(
        0, source->w, source->h, SDL_SURFACE_DEPTH, SDL_PIXELFORMAT_RGBA8888);
    if (!out) {
        LOG("SDL_CreateRGBSurfaceWithFormat failed in %s: %s\n", __func__,
            SDL_GetError());
        return NULL;
    }

    SDL_Rect cacheBounds = {0, 0, (int) pixmapStruct->width,
                            (int) pixmapStruct->height};
    SDL_Rect clipped;
    Bool anyInBounds = SDL_IntersectRect(source, &cacheBounds, &clipped);
    /* Zero-pad only the part of the request that runs past the pixmap edge, the
     * same contract getRenderSurfaceRect gives for out-of-bounds reads. When
     * the request lies fully inside the pixmap (the common case) the blit
     * covers every byte, so skip the full-surface fill.
     */
    if (!anyInBounds || clipped.w != source->w || clipped.h != source->h)
        SDL_FillRect(out, NULL, 0);

    if (anyInBounds) {
        SDL_Rect dst = {clipped.x - source->x, clipped.y - source->y, clipped.w,
                        clipped.h};
        SDL_BlitSurface(cache, &clipped, out, &dst);
    }
    return out;
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

/* Returns the cached visible region for "d" along with its rect count.
 *   - (NULL, 1): "d" is None or a pixmap, so no sibling occlusion applies.
 *   - (&region, N): "d" is a window; the region holds N >= 1 visible rects
 *     (top-levels see their full frame as one rect).
 *   - (&region, 0): "d" is a window fully covered by higher-stacked siblings;
 *     the caller must skip drawing.
 */
static const pixman_region32_t *drawableVisibleRegion(Drawable d,
                                                      int *visCountOut)
{
    if (d == None || !IS_TYPE(d, WINDOW)) {
        *visCountOut = 1;
        return NULL;
    }
    const pixman_region32_t *region = ensureVisibleRegion(d);
    if (!region) {
        *visCountOut = 1;
        return NULL;
    }
    *visCountOut = pixman_region32_n_rects((pixman_region32_t *) region);
    return region;
}

void invalidatePrimitiveClipCache(void)
{
    /* No-op kept as a stable exported symbol so window-internal.c can
     * unconditionally call this from invalidateVisibleRegionSubtree. The
     * previous primitive-clip cache was rolled back after benchmarks showed no
     * net win.
     */
}

/* Cartesian product of GC-clip and visible-region rect counts, capped at
 * INT_MAX in int64_t. A shape-fragmented visible region multiplied by a long GC
 * clip list can otherwise wrap negative and either skip drawing entirely or
 * desynchronize the gcIdx / visIdx decode below.
 */
static int cappedClipIterationCount(int gcCount, int visCount)
{
    return clampToInt((int64_t) gcCount * (int64_t) visCount);
}

int getGcClipIterationCount(GC gc, Drawable d)
{
    int gcCount = 1;
    if (gc) {
        GraphicContext *gContext = GET_GC(gc);
        if (gContext && gContext->clipRectanglesSet) {
            if (gContext->clipRectCount <= 0)
                return 0;
            gcCount = gContext->clipRectCount;
        }
    }
    int visCount = 1;
    (void) drawableVisibleRegion(d, &visCount);
    if (visCount <= 0)
        return 0;
    return cappedClipIterationCount(gcCount, visCount);
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

static Bool clipCopyAreaRects(Drawable src,
                              Drawable dest,
                              int srcX,
                              int srcY,
                              unsigned int width,
                              unsigned int height,
                              int destX,
                              int destY,
                              SDL_Rect *srcOut,
                              SDL_Rect *destOut)
{
    int srcWidth = 0, srcHeight = 0, destWidth = 0, destHeight = 0;
    if (!getDrawableSize(src, &srcWidth, &srcHeight) ||
        !getDrawableSize(dest, &destWidth, &destHeight))
        return False;

    SDL_Rect requestedDest = {destX, destY, (int) width, (int) height};
    SDL_Rect destBounds = {0, 0, destWidth, destHeight};
    SDL_Rect clippedDest;
    if (!SDL_IntersectRect(&requestedDest, &destBounds, &clippedDest))
        return False;

    SDL_Rect shiftedSrc = {
        .x = srcX + (clippedDest.x - destX),
        .y = srcY + (clippedDest.y - destY),
        .w = clippedDest.w,
        .h = clippedDest.h,
    };
    SDL_Rect srcBounds = {0, 0, srcWidth, srcHeight};
    SDL_Rect clippedSrc;
    if (!SDL_IntersectRect(&shiftedSrc, &srcBounds, &clippedSrc))
        return False;

    clippedDest.x += clippedSrc.x - shiftedSrc.x;
    clippedDest.y += clippedSrc.y - shiftedSrc.y;
    clippedDest.w = clippedSrc.w;
    clippedDest.h = clippedSrc.h;
    *srcOut = clippedSrc;
    *destOut = clippedDest;
    return clippedSrc.w > 0 && clippedSrc.h > 0;
}

static void postCopyAreaExposure(Display *display,
                                 Drawable src,
                                 Drawable dest,
                                 GC gc,
                                 const SDL_Rect *srcRect,
                                 const SDL_Rect *requestedDestRect,
                                 const SDL_Rect *copiedDestRect)
{
    if (!gcWantsGraphicsExposures(gc))
        return;
    if (rectInsideDrawable(src, srcRect)) {
        postEvent(display, dest, NoExpose, X_CopyArea, 0);
        return;
    }
    int destWidth = 0, destHeight = 0;
    SDL_Rect destBounds;
    SDL_Rect visibleDest;
    if (!getDrawableSize(dest, &destWidth, &destHeight))
        return;
    destBounds = (SDL_Rect) {0, 0, destWidth, destHeight};
    if (!SDL_IntersectRect(requestedDestRect, &destBounds, &visibleDest))
        return;

    SDL_Rect expose[4];
    size_t exposeCount = 0;
    if (!copiedDestRect) {
        expose[exposeCount++] = visibleDest;
    } else {
        SDL_Rect copied;
        if (!SDL_IntersectRect(copiedDestRect, &visibleDest, &copied)) {
            expose[exposeCount++] = visibleDest;
        } else {
            if (copied.y > visibleDest.y)
                expose[exposeCount++] =
                    (SDL_Rect) {visibleDest.x, visibleDest.y, visibleDest.w,
                                copied.y - visibleDest.y};
            if (copied.y + copied.h < visibleDest.y + visibleDest.h)
                expose[exposeCount++] = (SDL_Rect) {
                    visibleDest.x, copied.y + copied.h, visibleDest.w,
                    visibleDest.y + visibleDest.h - (copied.y + copied.h)};
            if (copied.x > visibleDest.x)
                expose[exposeCount++] =
                    (SDL_Rect) {visibleDest.x, copied.y,
                                copied.x - visibleDest.x, copied.h};
            if (copied.x + copied.w < visibleDest.x + visibleDest.w)
                expose[exposeCount++] = (SDL_Rect) {
                    copied.x + copied.w, copied.y,
                    visibleDest.x + visibleDest.w - (copied.x + copied.w),
                    copied.h};
        }
    }
    for (size_t i = 0; i < exposeCount; i++)
        postEvent(display, dest, GraphicsExpose, &expose[i], exposeCount - i,
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

Bool setGcClipForIteration(SDL_Renderer *renderer,
                           GC gc,
                           int iteration,
                           Drawable d)
{
    if (!renderer)
        return False;

    GraphicContext *gContext = gc ? GET_GC(gc) : NULL;
    Bool hasGcClip = gContext && gContext->clipRectanglesSet;
    int gcCount = 1;
    if (hasGcClip) {
        if (gContext->clipRectCount <= 0)
            return False;
        gcCount = gContext->clipRectCount;
    }

    int visCount = 1;
    const pixman_region32_t *region = drawableVisibleRegion(d, &visCount);
    if (visCount <= 0)
        return False;
    /* The bounds check must agree with the cap reported by
     * getGcClipIterationCount; visIdx / gcIdx stay safe because the caller only
     * iterates up to that returned int.
     */
    int total = cappedClipIterationCount(gcCount, visCount);
    if (iteration < 0 || iteration >= total)
        return False;

    int visIdx = iteration / gcCount;
    int gcIdx = iteration % gcCount;

    /* The gc clip slot determines the starting rect: an explicit gc clip rect
     * when one is installed, otherwise the parent-chain drawable clip pushed by
     * getWindowRenderer. When neither source is present, the fallback uses the
     * whole renderer target so the subsequent sibling-region intersect still
     * produces a meaningful rect.
     */
    SDL_Rect clip;
    if (hasGcClip) {
        XRectangle *source = &gContext->clipRects[gcIdx];
        if (source->width == 0 || source->height == 0)
            return False;
        clip.x = gContext->clipOriginX + source->x;
        clip.y = gContext->clipOriginY + source->y;
        clip.w = source->width;
        clip.h = source->height;
    } else if (rendererDrawableClip.valid &&
               rendererDrawableClip.renderer == renderer) {
        clip = rendererDrawableClip.clip;
    } else {
        SDL_RenderGetViewport(renderer, &clip);
        clip.x = 0;
        clip.y = 0;
    }

    if (hasGcClip && rendererDrawableClip.valid &&
        rendererDrawableClip.renderer == renderer) {
        if (!SDL_IntersectRect(&clip, &rendererDrawableClip.clip, &clip))
            return False;
    }

    /* Sibling occlusion: drop any pixels that would have landed under a
     * higher-stacked sibling on the same parent (or any ancestor's higher
     * sibling). Pixmaps and the (NULL, 1) fallback above leave "region" NULL,
     * so this branch is a no-op for them.
     */
    if (region && visIdx < visCount) {
        pixman_box32_t *boxes =
            pixman_region32_rectangles((pixman_region32_t *) region, NULL);
        pixman_box32_t b = boxes[visIdx];
        SDL_Rect visRect = {
            .x = b.x1,
            .y = b.y1,
            .w = b.x2 - b.x1,
            .h = b.y2 - b.y1,
        };
        if (visRect.w <= 0 || visRect.h <= 0)
            return False;
        if (!SDL_IntersectRect(&clip, &visRect, &clip))
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
    presentDrawableRectIfVisible(drawable, rect);
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
        SDL_GetRGB(mp, XC_SURFACE_FORMAT(mask), &r, &g, &b);
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
    int bytesPerPixel = XC_SURFACE_BYTESPERPIXEL(surface);
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
    int bytesPerPixel = XC_SURFACE_BYTESPERPIXEL(surface);
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
                                 Drawable d,
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
              rasterStrokePathOnRenderer(renderer, d, gc, &path);
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

/* Resolve the renderer's current target texture and its pixel format.
 * Returns the target texture and writes its format to *formatOut, or returns
 * NULL when there is no target or the format is not 32-bit packed
 * (applyRasterFunction operates on Uint32 pixels and the commit path uses
 * SDL_ConvertPixels which needs a known format).
 */
static SDL_Texture *rasterOpResolveTarget(SDL_Renderer *renderer,
                                          Uint32 *formatOut)
{
    *formatOut = SDL_PIXELFORMAT_ARGB8888;
    if (!renderer)
        return NULL;
    SDL_Texture *target = SDL_GetRenderTarget(renderer);
    if (!target)
        return NULL;
    Uint32 format = 0;
    if (SDL_QueryTexture(target, &format, NULL, NULL, NULL) != 0)
        return NULL;
    if (SDL_BITSPERPIXEL(format) != 32)
        return NULL;
    *formatOut = format;
    return target;
}

/* Commit a raster-op pixel buffer (in SDL_PIXELFORMAT_ARGB8888) back to the
 * renderer. When the target is a texture we own and its native format differs
 * from ARGB8888, convert into a scratch buffer first; then prefer
 * SDL_UpdateTexture, which is a direct memory blit and skips the GPU shader
 * pipeline that SDL_RenderCopy would otherwise drive. The Metal backend's
 * RenderCopy goes through float / sRGB / premultiplied-alpha conversions that
 * break bit-for-bit XOR self-inversion - that is why the previous surface +
 * texture + RenderCopy path leaked rubber-band trails on macOS. The RenderCopy
 * path stays as a last-resort fallback for the rare case where the renderer is
 * drawing straight to a window swap chain with no intermediate target texture.
 */
static void rasterOpCommitPixels(SDL_Renderer *renderer,
                                 SDL_Texture *target,
                                 const SDL_Rect *clipped,
                                 unsigned char *pixels,
                                 size_t pitch,
                                 Uint32 targetFormat)
{
    if (target) {
        if (targetFormat == SDL_PIXELFORMAT_ARGB8888) {
            if (SDL_UpdateTexture(target, clipped, pixels, (int) pitch) == 0)
                return;
        } else {
            unsigned char *converted = malloc(pitch * (size_t) clipped->h);
            if (converted) {
                if (SDL_ConvertPixels(clipped->w, clipped->h,
                                      SDL_PIXELFORMAT_ARGB8888, pixels,
                                      (int) pitch, targetFormat, converted,
                                      (int) pitch) == 0 &&
                    SDL_UpdateTexture(target, clipped, converted,
                                      (int) pitch) == 0) {
                    free(converted);
                    return;
                }
                free(converted);
            }
        }
    }
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, clipped->w, clipped->h, 32, (int) pitch,
        SDL_PIXELFORMAT_ARGB8888);
    if (!surface)
        return;
    SDL_Texture *staging = SDL_CreateTextureFromSurface(renderer, surface);
    if (staging) {
        SDL_SetTextureBlendMode(staging, SDL_BLENDMODE_NONE);
        if (SDL_RenderCopy(renderer, staging, NULL, clipped) != 0) {
            LOG("SDL_RenderCopy fallback failed in %s: %s\n", __func__,
                SDL_GetError());
        }
        SDL_DestroyTexture(staging);
    }
    SDL_FreeSurface(surface);
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

    Uint32 format;
    SDL_Texture *target = rasterOpResolveTarget(renderer, &format);
    /* Always read in ARGB8888 so applyRasterFunction's "preserve the top byte
     * as alpha" contract holds. The commit path converts back to the target's
     * native format only at the UpdateTexture boundary.
     */
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
        rasterOpCommitPixels(renderer, target, &clipped, pixels, pitch, format);
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

    Uint32 format;
    SDL_Texture *target = rasterOpResolveTarget(renderer, &format);
    /* See the matching block in rasterOpRendererRect for why this stays locked
     * to ARGB8888 - format-aware writeback happens in the commit helper, not in
     * the XOR step.
     */
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
        rasterOpCommitPixels(renderer, target, &clipped, pixels, pitch, format);
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
    /* Match the int64 clamp XDrawRectangle uses on the fast path: an
     * attacker-controlled width near UINT_MAX would otherwise wrap the
     * right-edge / bottom-edge computation before any subsequent clipping in
     * the per-edge raster-op walk could catch it.
     */
    int xRight = clampToInt((int64_t) x + (int64_t) width);
    int yBot = clampToInt((int64_t) y + (int64_t) height);
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

    int clipCount = getGcClipIterationCount(gc, d);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, d))
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
                             Drawable d,
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
                      rasterFillPathOnRenderer(renderer, d, gc, &path);
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
    int clipCount = getGcClipIterationCount(gc, d);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, d))
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
        fillArcOnRenderer(renderer, d, gc, x, y, width, height, angle1, angle2);
    shapeGuardEnd(&sg);
    if (result)
        presentDrawableRectIfVisible(d, &arcBbox);
    return result;
}

static int drawArcOnRenderer(SDL_Renderer *renderer,
                             Drawable d,
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
                      rasterStrokePathOnRenderer(renderer, d, gc, &path);
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
    int clipCount = getGcClipIterationCount(gc, d);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, d))
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
        drawArcOnRenderer(renderer, d, gc, x, y, width, height, angle1, angle2);
    shapeGuardEnd(&sg);
    if (result)
        presentDrawableRectIfVisible(d, &arcBbox);
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
                ok = rasterStrokePathOnRenderer(renderer, d, gc, &path);
            pathFree(&path);
            if (ok) {
                shapeGuardEnd(&sg);
                presentDrawableRectIfVisible(d, &arcsBbox);
                return 1;
            }
        }
    }
    for (int i = 0; i < n_arcs; i++) {
        if (!drawArcOnRenderer(renderer, d, gc, arcs[i].x, arcs[i].y,
                               arcs[i].width, arcs[i].height, arcs[i].angle1,
                               arcs[i].angle2)) {
            shapeGuardEnd(&sg);
            return 0;
        }
    }
    shapeGuardEnd(&sg);
    presentDrawableRectIfVisible(d, &arcsBbox);
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
        if (!fillArcOnRenderer(renderer, d, gc, arcs[i].x, arcs[i].y,
                               arcs[i].width, arcs[i].height, arcs[i].angle1,
                               arcs[i].angle2)) {
            shapeGuardEnd(&sg);
            return 0;
        }
    }
    shapeGuardEnd(&sg);
    presentDrawableRectIfVisible(d, &fillBbox);
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
    XcPixelFormat format = xcAllocFormat(SDL_PIXELFORMAT_RGBA8888);
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
            xcFreeFormat(format);
            SDL_FreeSurface(srcSurface);
            handleError(0, display, dest, 0, BadMatch, 0);
            return 0;
        }
    }
    Uint32 *pixels = malloc((size_t) width * (size_t) height * sizeof(Uint32));
    if (!pixels) {
        SDL_FreeSurface(destSurface);
        xcFreeFormat(format);
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
            SDL_GetRGB(srcPixel, XC_SURFACE_FORMAT(srcSurface), &red, &green,
                       &blue);
            Bool bitSet =
                plane == 1 ? (red || green || blue) : ((srcPixel & plane) != 0);
            pixels[y * width + x] = bitSet ? foreground : background;
        }
    }
    SDL_FreeSurface(destSurface);
    xcFreeFormat(format);
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
    int clipCount = getGcClipIterationCount(gc, dest);
    int rcCopy = 0;
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(destRenderer, gc, clip, dest))
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
    presentDrawableRectIfVisible(dest, &destRect);
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
    int clipCount = getGcClipIterationCount(gc, d);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, d))
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
    presentDrawableRectIfVisible(d, &pointRect);
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
    int clipCount = getGcClipIterationCount(gc, d);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, d))
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
    if (haveBbox)
        presentDrawableRectIfVisible(d, &pointsBbox);
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
            ok = strokeLineOnRenderer(renderer, d, gc, segments[i].x1,
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
                ok = rasterStrokePathOnRenderer(renderer, d, gc, &path);
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
        int clipCount = getGcClipIterationCount(gc, d);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip, d))
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
    int clipCount = getGcClipIterationCount(gc, d);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, d))
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
        strokeLineOnRenderer(renderer, d, gc, x1, y1, x2, y2)) {
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
    int clipCount = getGcClipIterationCount(gc, d);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, d))
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
                ok = rasterStrokePathOnRenderer(renderer, d, gc, &path);
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
        int clipCount = getGcClipIterationCount(gc, d);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip, d))
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
    int clipCount = getGcClipIterationCount(gc, d);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, d))
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
    /* The fill below overwrites these pixels, so any text stamp covering the
     * cleared cell is now stale. XClearArea fills the backing without routing
     * through the present choke point, so invalidate explicitly here.
     */
    invalidateTextStampsForDrawableRect(w, &clearRect);
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
                             int dest_y,
                             const SDL_Rect *exposureSrcRect,
                             const SDL_Rect *exposureDestRect)
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
    int clipCount = getGcClipIterationCount(gc, dest);
    int rc = 0;
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(destRenderer, gc, clip, dest))
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
    postCopyAreaExposure(display, src, dest, gc, exposureSrcRect,
                         exposureDestRect, &destRect);
    if (shapeOk)
        presentDrawableRectIfVisible(dest, &destRect);
    return 1;

cleanup_pixels:
    free(srcPixels);
    free(destPixels);
    return -1;
}

/* Clamp a 1:1 XCopyArea source rect to a source texture's pixel bounds,
 * shrinking the destination rect by the same trailing amount so the copy stays
 * pixel-exact. A pixmap whose backing texture is smaller than the requested
 * source region (e.g. an editor back-buffer snapped to a whole-cell grid while
 * the Expose event carries the full sub-cell window size) would otherwise have
 * its trailing row/column stretched or dropped by SDL_RenderCopy. Leading
 * offsets (src_x/src_y past the texture) collapse the rect to empty, matching
 * X11's "source region outside the drawable copies nothing" contract. No-op
 * when the texture query fails or the rect already fits.
 */
static void clampCopyAreaSrcRect(SDL_Texture *srcTexture,
                                 SDL_Rect *srcRect,
                                 SDL_Rect *destRect)
{
    if (!srcTexture || !srcRect || !destRect)
        return;
    int texW = 0, texH = 0;
    if (SDL_QueryTexture(srcTexture, NULL, NULL, &texW, &texH) != 0)
        return;
    if (srcRect->x >= texW || srcRect->y >= texH) {
        srcRect->w = 0;
        srcRect->h = 0;
        destRect->w = 0;
        destRect->h = 0;
        return;
    }
    if (srcRect->x + srcRect->w > texW) {
        int trim = srcRect->x + srcRect->w - texW;
        srcRect->w -= trim;
        destRect->w -= trim;
    }
    if (srcRect->y + srcRect->h > texH) {
        int trim = srcRect->y + srcRect->h - texH;
        srcRect->h -= trim;
        destRect->h -= trim;
    }
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
    width = (uint16_t) width;
    height = (uint16_t) height;
    if (width == 0 || height == 0) {
        handleError(0, display, src, 0, BadValue, 0);
        return 0;
    }
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
    if (IS_TYPE(dest, WINDOW) && IS_INPUT_ONLY(dest)) {
        LOG("BadMatch: Got input only window as the destination in %s!\n",
            __func__);
        handleError(0, display, dest, 0, BadMatch, 0);
        return 0;
    }

    SDL_Rect requestedSrcRect = {src_x, src_y, (int) width, (int) height};
    SDL_Rect requestedDestRect = {dest_x, dest_y, (int) width, (int) height};
    SDL_Rect srcRect;
    SDL_Rect destRect;
    if (!clipCopyAreaRects(src, dest, src_x, src_y, width, height, dest_x,
                           dest_y, &srcRect, &destRect)) {
        int destWidth = 0, destHeight = 0;
        SDL_Rect destBounds;
        if (getDrawableSize(dest, &destWidth, &destHeight)) {
            destBounds = (SDL_Rect) {0, 0, destWidth, destHeight};
            if (SDL_IntersectRect(&requestedDestRect, &destBounds, &destRect)) {
                postCopyAreaExposure(display, src, dest, gc, &requestedSrcRect,
                                     &requestedDestRect, NULL);
                return 1;
            }
        }
        if (gcWantsGraphicsExposures(gc))
            postEvent(display, dest, NoExpose, X_CopyArea, 0);
        return 1;
    }
    src_x = srcRect.x;
    src_y = srcRect.y;
    dest_x = destRect.x;
    dest_y = destRect.y;
    width = (unsigned int) destRect.w;
    height = (unsigned int) destRect.h;

    /* Non-GXcopy or masked plane masks need per-pixel arithmetic that
     * SDL_RenderCopy cannot express. xCopyAreaRasterOp does the slow
     * read-modify-write through SDL_RenderReadPixels and returns 1 if it
     * handled the request, 0 to fall through to the fast plain-blit path.
     */
    {
        int rasterRc = xCopyAreaRasterOp(display, src, dest, gc, src_x, src_y,
                                         width, height, dest_x, dest_y,
                                         &requestedSrcRect, &requestedDestRect);
        if (rasterRc != 0)
            return rasterRc > 0 ? 1 : 0;
    }
    if (IS_TYPE(dest, WINDOW)) {
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
                SDL_Rect fastSrc = srcRect;
                SDL_Rect fastDest = destRect;
                /* XCopyArea is a 1:1 blit: a source rect that runs past the
                 * source pixmap must copy only the in-bounds portion (X11
                 * leaves the rest untouched), not stretch the smaller region
                 * over the whole dest. SDL_RenderCopy would otherwise scale or
                 * clip per backend, dropping the trailing row/column. Clamp the
                 * copy rect to the texture and shrink the dest by the same
                 * amount so the copy stays pixel-exact; the exposure check
                 * below still uses the original requested rectangle.
                 */
                clampCopyAreaSrcRect(pixmapTexture, &fastSrc, &fastDest);
                if (fastSrc.w <= 0 || fastSrc.h <= 0) {
                    postCopyAreaExposure(display, src, dest, gc,
                                         &requestedSrcRect, &requestedDestRect,
                                         NULL);
                    return 1;
                }
                SDL_SetTextureBlendMode(pixmapTexture, SDL_BLENDMODE_NONE);
                ShapeGuard fastSg;
                shapeGuardBegin(&fastSg, dest, destRenderer, &fastDest);
                int fastClipCount = getGcClipIterationCount(gc, dest);
                int fastRcCopy = 0;
                for (int clip = 0; clip < fastClipCount; clip++) {
                    if (!setGcClipForIteration(destRenderer, gc, clip, dest))
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
                postCopyAreaExposure(display, src, dest, gc, &requestedSrcRect,
                                     &requestedDestRect, &fastDest);
                if (fastShapeOk)
                    presentDrawableRectIfVisible(dest, &fastDest);
                return 1;
            }
        }
        SDL_Renderer *srcRenderer;
        GET_RENDERER(src, srcRenderer);
        if (!srcRenderer) {
            handleError(0, display, src, 0, BadDrawable, 0);
            return 0;
        }
        /* Same 1:1 clamp as the fast path: for a Pixmap source, restrict the
         * copy to the pixmap texture so a src rect that runs off the pixmap
         * leaves the trailing dest row/column untouched instead of painting the
         * zero-padded (black) readback getRenderSurfaceRect would return. The
         * exposure check below still uses the original requested rectangle.
         */
        if (IS_TYPE(src, PIXMAP)) {
            clampCopyAreaSrcRect(GET_PIXMAP_TEXTURE(src), &srcRect, &destRect);
            if (srcRect.w <= 0 || srcRect.h <= 0) {
                postCopyAreaExposure(display, src, dest, gc, &requestedSrcRect,
                                     &requestedDestRect, NULL);
                return 1;
            }
        }
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
        int clipCount = getGcClipIterationCount(gc, dest);
        int rcCopy = 0;
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(destRenderer, gc, clip, dest))
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
                             &requestedDestRect, &destRect);
        if (slowShapeOk)
            presentDrawableRectIfVisible(dest, &destRect);
        return 1;
    }

    if (IS_TYPE(src, PIXMAP) && IS_TYPE(dest, PIXMAP) && src != dest) {
        SDL_Texture *pixmapTexture = GET_PIXMAP_TEXTURE(src);
        SDL_Renderer *sharedRenderer =
            GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
        SDL_Renderer *destRenderer = NULL;
        GET_RENDERER(dest, destRenderer);
        if (!destRenderer) {
            handleError(0, display, dest, 0, BadDrawable, 0);
            return 0;
        }
        if (pixmapTexture && destRenderer == sharedRenderer) {
            SDL_SetTextureBlendMode(pixmapTexture, SDL_BLENDMODE_NONE);
            int clipCount = getGcClipIterationCount(gc, dest);
            int rcCopy = 0;
            for (int clip = 0; clip < clipCount; clip++) {
                if (!setGcClipForIteration(destRenderer, gc, clip, dest))
                    continue;
                if (SDL_RenderCopy(destRenderer, pixmapTexture, &srcRect,
                                   &destRect) != 0) {
                    LOG("SDL_RenderCopy failed in %s pixmap fast path: %s\n",
                        __func__, SDL_GetError());
                    rcCopy = -1;
                    break;
                }
            }
            clearRendererClip(destRenderer);
            if (rcCopy != 0) {
                handleError(0, display, src, 0, BadMatch, 0);
                return 0;
            }
            postCopyAreaExposure(display, src, dest, gc, &requestedSrcRect,
                                 &requestedDestRect, &destRect);
            return 1;
        }
    }

    SDL_Renderer *srcRenderer;
    GET_RENDERER(src, srcRenderer);
    if (!srcRenderer) {
        handleError(0, display, src, 0, BadDrawable, 0);
        return 0;
    }
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
    SDL_SetTextureBlendMode(srcTexture, SDL_BLENDMODE_NONE);
    int clipCount = getGcClipIterationCount(gc, dest);
    int rcCopy = 0;
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(destRenderer, gc, clip, dest))
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

    postCopyAreaExposure(display, src, dest, gc, &requestedSrcRect,
                         &requestedDestRect, &destRect);
    presentDrawableRectIfVisible(dest, &destRect);
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
            /* Wide / non-solid strokes paint outside the geometric rect, so
             * inflate the damage by the stroke pad before snapshotting the
             * shape baseline and marking the present. arcDamageRect does the
             * same +1 width / +1 height correction this path needs.
             */
            SDL_Rect strokeDamage =
                arcDamageRect(x, y, width, height, arcStrokePad(gc));
            ShapeGuard sg;
            shapeGuardBegin(&sg, d, renderer, &strokeDamage);
            Bool ok = pathMoveTo(&path, x, y) && pathLineTo(&path, rightX, y) &&
                      pathLineTo(&path, rightX, bottomY) &&
                      pathLineTo(&path, x, bottomY) && pathLineTo(&path, x, y);
            if (ok)
                ok = rasterStrokePathOnRenderer(renderer, d, gc, &path);
            pathFree(&path);
            shapeGuardEnd(&sg);
            if (ok) {
                repaintMappedChildrenInRect(display, d, &strokeDamage);
                return 1;
            }
        }
    }
    ShapeGuard sg;
    shapeGuardBegin(&sg, d, renderer, &rectDamage);
    if (gContext->function != GXcopy) {
        int clipCount = getGcClipIterationCount(gc, d);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip, d))
                continue;
            rasterOpRendererRectOutline(renderer, x, y, width, height,
                                        gContext->function, gContext->planeMask,
                                        gContext->foreground);
        }
    } else {
        applySdlDrawState(renderer, gc, SDL_BLENDMODE_BLEND,
                          gContext->foreground);
        int clipCount = getGcClipIterationCount(gc, d);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip, d))
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

/* Fill a batch of rectangles under the active clip. ClipByChildren windows fill
 * one rect at a time so occluding children can be punched out per rect;
 * everything else takes SDL's batched fill.
 */
static void fillRectsClipAware(SDL_Renderer *renderer,
                               Drawable d,
                               const SDL_Rect *rects,
                               int count,
                               Bool clipByChildren)
{
    if (clipByChildren) {
        for (int r = 0; r < count; r++) {
            if (!renderFillRectClipByChildren(renderer, d, &rects[r]))
                LOG("SDL_RenderFillRect failed in %s: %s\n", __func__,
                    SDL_GetError());
        }
    } else if (SDL_RenderFillRects(renderer, rects, count)) {
        LOG("SDL_RenderFillRects failed in %s: %s\n", __func__, SDL_GetError());
    }
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
    Bool clipByChildren =
        IS_TYPE(d, WINDOW) && gContext->subWindowMode == ClipByChildren;
    /* True only once a fill path has punched children out per rect. The
     * raster-op path below cannot punch (it blits whole rects through
     * readback), so it leaves this False and forces the child-repaint post-step
     * even on a ClipByChildren window; otherwise the overdrawn children never
     * get their Expose and keep the raster-op damage.
     */
    Bool childrenPunched = False;
    if (gContext->fillStyle == FillSolid) {
        LOG("Fill_style is %s\n", "FillSolid");
        if (gContext->function != GXcopy) {
            /* SDL_Renderer has no raster-op equivalent, so read the affected
             * pixels back, apply the GC function in software, and blit them in
             * place.
             */
            int clipCount = getGcClipIterationCount(gc, d);
            for (int clip = 0; clip < clipCount; clip++) {
                if (!setGcClipForIteration(renderer, gc, clip, d))
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
            int clipCount = getGcClipIterationCount(gc, d);
            for (int clip = 0; clip < clipCount; clip++) {
                if (!setGcClipForIteration(renderer, gc, clip, d))
                    continue;
                fillRectsClipAware(renderer, d, sdlRectangles, validRectangles,
                                   clipByChildren);
            }
            clearRendererClip(renderer);
            childrenPunched = clipByChildren;
        }
    } else if (gContext->fillStyle == FillTiled) {
        LOG("Fill_style is %s\n", "FillTiled");
    } else if (gContext->fillStyle == FillOpaqueStippled) {
        LOG("Fill_style is %s\n", "FillOpaqueStippled");
        applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE,
                          gContext->background);
        int clipCount = getGcClipIterationCount(gc, d);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip, d))
                continue;
            fillRectsClipAware(renderer, d, sdlRectangles, validRectangles,
                               clipByChildren);
        }
        clearRendererClip(renderer);
        childrenPunched = clipByChildren;
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
        for (int r = 0; r < validRectangles; r++) {
            if (childrenPunched)
                presentDrawableRectIfVisible(d, &sdlRectangles[r]);
            else
                repaintMappedChildrenInRect(display, d, &sdlRectangles[r]);
        }
    }
    free(heapRectangles);
    return 1;
}
