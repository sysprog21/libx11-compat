#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <X11/X.h>
#include <X11/Xlibint.h>
#include <X11/Xutil.h>

#include "sdl-compat.h"
#include "sdl-ttf-compat.h"
#include "window.h"
#include "errors.h"
#include "events.h"
#include "main-dispatch.h"
#include "replay.h"
#include "colors.h"
#include "drawing.h"
#include "display.h"
#include "gc.h"
#include "atoms.h"
#include "visual.h"
#include "font.h"
#include "extension.h"
#include "selection.h"
#include "image.h"
#include "input-method.h"
#include "mac-live-resize.h"

#ifndef SDL_HINT_VIDEO_X11_XKB
#define SDL_HINT_VIDEO_X11_XKB "SDL_VIDEO_X11_XKB"
#endif

/* Toolkits like Tk read $DISPLAY (and abort with "no display name and no
 * $DISPLAY environment variable") before ever calling into this layer, which
 * then ignores the value and drives SDL in process. Default DISPLAY to :0 at
 * library load, before any such check runs, so a client works with no DISPLAY
 * set. XOpenDisplay does the same setenv, but that is too late for the
 * toolkit's own pre-flight check. Only sets it when unset (setenv overwrite
 * flag 0).
 */
__attribute__((constructor)) static void libx11CompatDefaultDisplay(void)
{
    setenv("DISPLAY", ":0", 0);
}

/* Lock hooks installed by libX11's locking.c when XInitThreads runs.
 * libx11-compat holds the function-pointer storage so display open/close can
 * invoke them without dragging in upstream XlibInt.c. The storage is
 * LIBX11_COMPAT_HIDDEN so a system libX11.so.6 loaded alongside libx11-compat
 * cannot reach in and overwrite it; see util.h for the rationale.
 *
 * These two function pointers are the entire interface libx11-compat needs from
 * upstream locking, so declare them here directly rather than pulling in the
 * staged upstream locking.h. That header lives under build/upstream/src and is
 * only visible through the upstream sync staging, so including it coupled this
 * first-party object to that staging order for no real gain.
 */
LIBX11_COMPAT_HIDDEN int (*_XInitDisplayLock_fn)(Display *dpy) = NULL;
LIBX11_COMPAT_HIDDEN void (*_XFreeDisplayLock_fn)(Display *dpy) = NULL;

#define InitDisplayLock(d) \
    (_XInitDisplayLock_fn ? (*_XInitDisplayLock_fn)(d) : Success)
#define FreeDisplayLock(d)    \
    if (_XFreeDisplayLock_fn) \
    (*_XFreeDisplayLock_fn)(d)

/* Display locking under XInitThreads. Upstream locking.c allocates
 * dpy->lock_fns at display open (only when a client called XInitThreads, which
 * installs _XInitDisplayLock_fn) and points lock_display at its own
 * non-recursive per-display mutex. That mutex self-deadlocks the moment one
 * locked path re-enters another: an app-level XLockDisplay followed by any
 * internal LockDisplay, or one public Xlib entry calling another. We repoint
 * the hooks at a single process-global recursive mutex instead, so all such
 * nesting is safe. Single-threaded clients never install lock_fns, so
 * LockDisplay stays a no-op and this lock is never touched.
 *
 * One global lock, not per-display. Simultaneous Displays are rare here and a
 * coarse lock is correct; if multi-display contention ever shows up, key the
 * mutex off dpy. Handoff-aware recursive display lock. Replaces a plain
 * PTHREAD_MUTEX_RECURSIVE so a worker that must hand SDL work to the main
 * thread (runOnMainThread) can shed the lock while it blocks and have the main
 * thread pick it up to run the handoff, without a third thread slipping into
 * the shedding worker's critical section. A bolt-on gate over a recursive mutex
 * has a check-then-act race (pass the gate, then acquire, and a fresh handoff
 * wedges the main thread), so the gate is folded into ownership here: every
 * ownership transition happens under mutex, and while a handoff is pending only
 * the main thread (to run the thunk) and the shedding worker (to reacquire) may
 * take the free lock. Single-threaded clients never install the hooks, so none
 * of this is reached and LockDisplay stays a no-op.
 */
typedef struct DisplayLock {
    pthread_mutex_t mutex;     /* guards every field below */
    pthread_cond_t cond;       /* signalled on any ownership/handoff change */
    SDL_threadID owner;        /* current holder, 0 when free */
    int depth;                 /* owner's recursion depth */
    SDL_threadID handoffOwner; /* worker mid-handoff, 0 when none */
} DisplayLock;

static DisplayLock displayLock;
static pthread_once_t displayLockOnce = PTHREAD_ONCE_INIT;

static void initDisplayLock(void)
{
    /* pthread_once cannot report failure, and a display lock that silently
     * no-ops would let threads corrupt shared state undetected. Fail loudly.
     */
    if (pthread_mutex_init(&displayLock.mutex, NULL) != 0 ||
        pthread_cond_init(&displayLock.cond, NULL) != 0) {
        fprintf(stderr, "libX11-compat: failed to init display lock\n");
        abort();
    }
    displayLock.owner = 0;
    displayLock.depth = 0;
    displayLock.handoffOwner = 0;
}

/* True when tid may take the currently-free lock given the handoff state.
 * During a handoff only the main thread (runs the thunk) and the handoff owner
 * (reacquires) are allowed; everyone else waits so the shedding worker's
 * critical section is not interleaved. Called with displayLock.mutex held;
 * libx11CompatOnMainEventThread is a lock-free atomic read.
 */
static Bool mayTakeFreeLock(SDL_threadID tid)
{
    return displayLock.handoffOwner == 0 || displayLock.handoffOwner == tid ||
           libx11CompatOnMainEventThread();
}

static void compatLockDisplay(Display *dpy)
{
    (void) dpy;
    SDL_threadID tid = SDL_ThreadID();
    pthread_mutex_lock(&displayLock.mutex);
    for (;;) {
        if (displayLock.owner == tid) { /* recursive re-entry always allowed */
            displayLock.depth++;
            break;
        }
        if (displayLock.owner == 0 && mayTakeFreeLock(tid)) {
            displayLock.owner = tid;
            displayLock.depth = 1;
            break;
        }
        pthread_cond_wait(&displayLock.cond, &displayLock.mutex);
    }
    pthread_mutex_unlock(&displayLock.mutex);
}

static void compatUnlockDisplay(Display *dpy)
{
    (void) dpy;
    SDL_threadID tid = SDL_ThreadID();

    pthread_mutex_lock(&displayLock.mutex);
    /* Only the owner may unlock, and only at a positive depth. The replaced
     * PTHREAD_MUTEX_RECURSIVE returned EPERM on a non-owner or over-unlock; the
     * monitor mirrors that by ignoring it, rather than decrementing another
     * thread's depth, clearing owner mid-critical-section, or underflowing.
     */
    if (displayLock.owner == tid && displayLock.depth > 0) {
        if (--displayLock.depth == 0) {
            displayLock.owner = 0;
            pthread_cond_broadcast(&displayLock.cond);
        }
    }
    pthread_mutex_unlock(&displayLock.mutex);
}

/* Fully release this thread's hold on the display lock (all recursion levels)
 * and mark a handoff pending, so the main thread can take the lock to run the
 * routed work while other lock-takers wait.
 *
 * Returns the shed depth, or 0 (a no-op) when this thread holds no lock,
 * including single-threaded clients that never installed the hooks.
 */
int displayLockReleaseForHandoff(void)
{
    /* Reached from runOnMainThread, not the lock hooks, so it can be the first
     * monitor access when a client spawns threads without XInitThreads (the
     * hooks, and thus initDisplayLock, are only installed under XInitThreads).
     * Ensure the mutex/cond are initialized before touching them.
     */
    pthread_once(&displayLockOnce, initDisplayLock);
    SDL_threadID tid = SDL_ThreadID();
    int depth = 0;
    pthread_mutex_lock(&displayLock.mutex);
    if (displayLock.owner == tid) {
        depth = displayLock.depth;
        displayLock.depth = 0;
        displayLock.owner = 0;
        displayLock.handoffOwner = tid;
        pthread_cond_broadcast(&displayLock.cond);
    }
    pthread_mutex_unlock(&displayLock.mutex);
    return depth;
}

Bool displayLockHandoffPending(void)
{
    pthread_once(&displayLockOnce, initDisplayLock);
    pthread_mutex_lock(&displayLock.mutex);
    Bool pending = displayLock.handoffOwner != 0;
    pthread_mutex_unlock(&displayLock.mutex);
    return pending;
}

/* Reacquire the lock at the shed depth once the handoff has run, then clear the
 * handoff so any threads gated in compatLockDisplay proceed. Waits out the main
 * thread still holding the lock from running the thunk.
 */
void displayLockReacquire(int depth)
{
    if (depth <= 0)
        return;

    /* depth > 0 implies a prior displayLockReleaseForHandoff already ran the
     * once-init, but keep it symmetric and robust.
     */
    pthread_once(&displayLockOnce, initDisplayLock);
    SDL_threadID tid = SDL_ThreadID();
    pthread_mutex_lock(&displayLock.mutex);
    while (displayLock.owner != 0)
        pthread_cond_wait(&displayLock.cond, &displayLock.mutex);
    displayLock.owner = tid;
    displayLock.depth = depth;
    displayLock.handoffOwner = 0;
    pthread_cond_broadcast(&displayLock.cond);
    pthread_mutex_unlock(&displayLock.mutex);
}

/* Redirect the just-initialized lock hooks at the recursive monitor. Call right
 * after InitDisplayLock; a no-op when threads were not initialized.
 */
static void installRecursiveDisplayLock(Display *display)
{
    if (!display->lock_fns)
        return;
    pthread_once(&displayLockOnce, initDisplayLock);
    display->lock_fns->lock_display = compatLockDisplay;
    display->lock_fns->unlock_display = compatUnlockDisplay;
}

int numDisplaysOpen = 0;

/* Vendor reports the SDL version this library was compiled against. The active
 * video driver can vary by environment at runtime, while this string is stable
 * and useful for compatibility probes.
 */
static char *vendor = "SDL " TO_STRING(SDL_MAJOR_VERSION) "." TO_STRING(
    SDL_MINOR_VERSION) "." TO_STRING(SDL_PATCHLEVEL);
static const int releaseVersion = 1;
static const int supportedDepths[] = {1, 16, 24, 32};
#define COMPAT_LOGICAL_DPI 96.0f

/* See compatGlobalHiDpiScale in display.h. Defaults to 1.0 so every path stays
 * a no-op until the probe measures a real Retina/HiDPI backing.
 */
static double globalHiDpiScale = 1.0;

double compatGlobalHiDpiScale(void)
{
    return globalHiDpiScale;
}

void compatSetGlobalHiDpiScale(double scale)
{
    if (scale > 0.0)
        globalHiDpiScale = scale;
}

void compatPublishHiDpiScaleProperty(Display *display)
{
    if (!display || SCREEN_WINDOW == None)
        return;
    Atom scaleAtom = internalInternAtom(HIDPI_SCALE_PROPERTY_NAME);
    if (scaleAtom == None)
        return;
    long fixed = lround(globalHiDpiScale * HIDPI_SCALE_PROPERTY_FIXED_POINT);
    if (fixed < 0)
        fixed = 0;

    /* Format-32 properties are stored and returned as an array of long (the
     * documented Xlib client convention, which this shim follows), so publish a
     * long rather than a fixed-width 32-bit integer.
     */
    XChangeProperty(display, SCREEN_WINDOW, scaleAtom, XA_CARDINAL, 32,
                    PropModeReplace, (const unsigned char *) &fixed, 1);
}

Bool compatSdlHasWindowSizeInPixels(void)
{
#if defined(LIBX11_COMPAT_SDL3)
    /* SDL3 always provides SDL_GetWindowSizeInPixels. */
    return True;
#elif SDL_VERSION_ATLEAST(2, 26, 0)
    const char *force = getenv("LIBX11_COMPAT_NO_SIZE_IN_PIXELS");
    if (force && force[0] == '1')
        return False;
    SDL_version linked;
    SDL_GetVersion(&linked);
    return SDL_VERSIONNUM(linked.major, linked.minor, linked.patch) >=
                   SDL_VERSIONNUM(2, 26, 0)
               ? True
               : False;
#else
    return False;
#endif
}

/* Measure the host HiDPI backing scale from a hidden throwaway window. SDL only
 * exposes the Retina backing ratio through a live window (SDL_GetDisplayDPI is
 * unreliable on macOS and SDL_GetDisplayContentScale reports 1.0), so create a
 * 1x1 hidden ALLOW_HIGHDPI window, compare its pixel size to its point size,
 * and destroy it. Leaves the scale at its 1.0 default on any failure or on the
 * CI dummy driver where the two sizes match.
 */
static void probeGlobalHiDpiScale(void)
{
    /* Reset first so a later 1x session cannot inherit a prior Retina scale:
     * globalHiDpiScale is process-lifetime, and the probe below only overwrites
     * it on a pixel!=point mismatch, so without this a Retina-then-1x sequence
     * (or the probe-failure early return) would keep the stale 2.0.
     */
    globalHiDpiScale = 1.0;
    SDL_Window *probe =
        SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                         1, 1, SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!probe) {
        LOG("probeGlobalHiDpiScale: SDL_CreateWindow failed: %s\n",
            SDL_GetError());
        return;
    }
    int pointW = 0, pointH = 0, pixelW = 0, pixelH = 0;
    SDL_GetWindowSize(probe, &pointW, &pointH);
    if (compatSdlHasWindowSizeInPixels()) {
#if SDL_VERSION_ATLEAST(2, 26, 0)
        SDL_GetWindowSizeInPixels(probe, &pixelW, &pixelH);
#endif
    }
#if !defined(LIBX11_COMPAT_SDL3)
    else {
        /* SDL_GetWindowSizeInPixels is unavailable (SDL2 older than 2.26).
         * Derive the backing size from a throwaway renderer's output size,
         * which reports physical pixels on every SDL2 that ships a renderer.
         * SDL3 always has SizeInPixels, so this fallback is never compiled
         * there (its SDL2-only renderer API would not build).
         */
        SDL_Renderer *probeRenderer =
            SDL_CreateRenderer(probe, -1, SDL_RENDERER_ACCELERATED);
        if (!probeRenderer)
            probeRenderer = SDL_CreateRenderer(probe, -1, 0);
        if (probeRenderer) {
            SDL_GetRendererOutputSize(probeRenderer, &pixelW, &pixelH);
            SDL_DestroyRenderer(probeRenderer);
        }
    }
#endif
    if (pointW > 0 && pixelW > 0 && pixelW != pointW)
        compatSetGlobalHiDpiScale((double) pixelW / (double) pointW);
    else if (pointH > 0 && pixelH > 0 && pixelH != pointH)
        compatSetGlobalHiDpiScale((double) pixelH / (double) pointH);
    LOG("probeGlobalHiDpiScale: point=(%dx%d) pixel=(%dx%d) scale=%.3f\n",
        pointW, pointH, pixelW, pixelH, globalHiDpiScale);
    SDL_DestroyWindow(probe);
}

static void applyScreenGeometryOverride(int *width, int *height)
{
    const char *value = getenv("LIBX11_COMPAT_SCREEN_GEOMETRY");
    if (!value || !*value)
        return;

    int parsedWidth = 0;
    int parsedHeight = 0;
    char tail = '\0';
    if (sscanf(value, "%dx%d%c", &parsedWidth, &parsedHeight, &tail) == 2 &&
        parsedWidth > 0 && parsedHeight > 0) {
        *width = parsedWidth;
        *height = parsedHeight;
    }
}

/* Queue of Display* handles closed re-entrantly from the live-resize observer;
 * the real closes run after the observer frame unwinds via
 * libx11CompatRunDeferredDisplayClose. A single slot would drop earlier closes
 * if a reflow callback tore down more than one Display in the same frame, so
 * keep them all. Main-thread-only, so no locking is needed.
 */
static Display **pendingCloseDisplays = NULL;
static size_t pendingCloseCount = 0;
static size_t pendingCloseCapacity = 0;

/* Fixed fallback used only when the dynamic queue above cannot grow (realloc
 * failure). Deferring is mandatory for correctness here -- closing
 * synchronously from inside the observer crashes -- so under OOM the close must
 * still be recorded somewhere rather than silently dropped while XCloseDisplay
 * reports success. A handful of slots covers any plausible per-frame teardown
 * burst.
 */
#define PENDING_CLOSE_FALLBACK_SLOTS 8
static Display *pendingCloseFallback[PENDING_CLOSE_FALLBACK_SLOTS];
static size_t pendingCloseFallbackCount = 0;

int XCloseDisplay(Display *display)
{
    // https://tronche.com/gui/x/xlib/display/XCloseDisplay.html
    if (libx11CompatInLiveResizePresent()) {
        /* Re-entered from the CFRunLoop live-resize observer (client reflow ran
         * XCloseDisplay). Tearing down SDL / removing the run-loop observer
         * from inside the observer's own callback crashes; defer the real close
         * until the present frame unwinds. Queue the Display* and return
         * cleanly.
         */
        if (pendingCloseCount == pendingCloseCapacity) {
            size_t newCap = pendingCloseCapacity ? pendingCloseCapacity * 2 : 4;
            Display **grown = realloc(pendingCloseDisplays,
                                      newCap * sizeof(*pendingCloseDisplays));
            if (grown) {
                pendingCloseDisplays = grown;
                pendingCloseCapacity = newCap;
            }
        }
        if (pendingCloseCount < pendingCloseCapacity) {
            pendingCloseDisplays[pendingCloseCount++] = display;
        } else if (pendingCloseFallbackCount < PENDING_CLOSE_FALLBACK_SLOTS) {
            /* Dynamic queue is full and could not grow; keep the close in the
             * static fallback so the deferred drain still tears it down.
             */
            pendingCloseFallback[pendingCloseFallbackCount++] = display;
        } else {
            /* Both queues are exhausted under sustained OOM. Dropping the close
             * leaks the Display, but the alternative (a synchronous close from
             * inside the observer) crashes, so leak rather than crash.
             */
            LOG("XCloseDisplay: deferred-close queues exhausted, leaking "
                "Display %p\n",
                (void *) display);
        }
        return 0;
    }
    freeExtensionStorage(display);
    freeSelectionStorage(display);
    int screenIndex;

    /* XOpenDisplay can call XCloseDisplay for cleanup after
     * SDL_GetNumVideoDisplays succeeds but the screens calloc fails: nscreens
     * is then >= 0 while screens is NULL. Guard the GC teardown so partial-init
     * paths do not segfault.
     */
    if (display->screens) {
        for (screenIndex = 0; screenIndex < display->nscreens; screenIndex++) {
            Screen *screen = &display->screens[screenIndex];
            if (screen->default_gc) {
                XFreeGC(display, screen->default_gc);
                screen->default_gc = NULL;
            }
        }
    }

    /* Teardown order before SDL_Quit:
     *   releaseLastRequestCode -> replayStop -> destroyScreenWindow
     *   -> closeEventPipe -> SDL_Quit.
     * All four touch SDL primitives (mutexes, timers, the event filter) that
     * must still exist. replayStop precedes closeEventPipe so the worker cannot
     * push XTest events through a queue with no filter behind it.
     * destroyScreenWindow precedes closeEventPipe because destroyWindow
     * recurses into discardQueuedEventsForWindow() and
     * postEvent(DestroyNotify), both of which take per-display event mutexes
     * that closeEventPipe destroys. The numDisplaysOpen guard scopes
     * replay/screen teardown to the last close so secondary displays opened by
     * Motif/Xt probes do not tear down shared state.
     */
    releaseLastRequestCode(display);
    if (numDisplaysOpen == 1) {
        inputMethodReset();
        replayStop();
        freeImageStorage();
        destroyScreenWindow(display);
    }
    closeEventPipe(display);
    if (numDisplaysOpen == 1) {
        /* Remove any pending present-wake timer immediately before SDL_Quit,
         * after all rendering teardown so nothing re-arms it below. Its
         * callback runs on SDL's timer thread and pushes a PRESENT event; left
         * live across SDL_Quit it can push into a half-torn-down event
         * subsystem (and race the next display's SDL_Init), which corrupts the
         * SDL allocator heap. cancelPresentWakeTimer blocks a callback already
         * running, so once it returns no present-wake push can overlap the
         * teardown. XOpenDisplay cancels on the reinit side; this closes the
         * matching teardown side.
         */
        cancelPresentWakeTimer();
        freeAtomStorage();
        resetSelectionAtomCache();
        freeFontStorage();
        freeColorStorage();
        freeVisuals();
        releaseMainEventThread();
        freeLastRequestStorage();
        TTF_Quit();
        SDL_Quit();
    }
    if (numDisplaysOpen > 0)
        numDisplaysOpen--;

    if (GET_DISPLAY(display)->screens) {
        for (screenIndex = 0; screenIndex < GET_DISPLAY(display)->nscreens;
             screenIndex++) {
            free(GET_DISPLAY(display)->screens[screenIndex].depths);
            GET_DISPLAY(display)->screens[screenIndex].depths = NULL;
        }
        free(GET_DISPLAY(display)->screens);
    }

    /* Release the upstream per-display lock allocation (lock, lock_fns, and the
     * mutex/condvars _XInitDisplayLock created). Only allocated under
     * XInitThreads; the macro no-ops otherwise. Safe even though we repointed
     * lock_fns->lock_display: _XFreeDisplayLock frees dpy->lock and
     * dpy->lock_fns without invoking them, and never touches the shared
     * process-global displayLock monitor.
     */
    FreeDisplayLock(display);
    free(display);
    return 0;
}

/* Deferred-close drain (option i): run the real XCloseDisplay immediately after
 * the live-resize present frame unwinds, in the SAME observer tick but after
 * the in-present flag/atomic are cleared (so libx11CompatInLiveResizePresent()
 * reads false and the full teardown runs). This is safe because
 * libx11CompatStopLiveResizeObserver now defers CFRunLoopRemoveObserver when
 * the observer callback is still on the stack (see mac-live-resize.c). Called
 * from libx11CompatPresentDuringLiveResize as its last action. No-op otherwise.
 */
void libx11CompatRunDeferredDisplayClose(void)
{
    if (pendingCloseCount == 0 && pendingCloseFallbackCount == 0)
        return;

    /* Detach both queues before draining so any re-entrant XCloseDisplay during
     * a teardown starts fresh lists instead of mutating the ones being walked.
     */
    Display **pending = pendingCloseDisplays;
    size_t count = pendingCloseCount;
    pendingCloseDisplays = NULL;
    pendingCloseCount = 0;
    pendingCloseCapacity = 0;
    Display *fallback[PENDING_CLOSE_FALLBACK_SLOTS];
    size_t fallbackCount = pendingCloseFallbackCount;
    for (size_t i = 0; i < fallbackCount; i++)
        fallback[i] = pendingCloseFallback[i];
    pendingCloseFallbackCount = 0;
    for (size_t i = 0; i < count; i++)
        XCloseDisplay(
            pending[i]); /* outside the present frame -> full teardown */
    for (size_t i = 0; i < fallbackCount; i++)
        XCloseDisplay(fallback[i]);
    free(pending);
}

/* Set only while this library drives SDL_Init(VIDEO) from XOpenDisplay. When a
 * real X server is present, host SDL's x11 video driver opens the display
 * through XOpenDisplay, which this library interposes; without this guard that
 * re-enters XOpenDisplay and recurses into SDL_Init until the stack overflows.
 * This must be thread-local: concurrent client XOpenDisplay calls are valid,
 * only same-thread recursion from SDL's probe should be refused.
 */
static __thread Bool sdlVideoInitInProgress = False;

Display *XOpenDisplay(_Xconst char *display_name)
{
    /* Refuse the re-entrant open from SDL's x11 driver so SDL reports x11
     * unavailable and falls back to a headless video driver instead of
     * recursing. A genuine second client XOpenDisplay never nests inside our
     * own SDL_Init, so this only ever rejects the driver's recursion.
     */
    if (sdlVideoInitInProgress)
        return NULL;

    setenv("DISPLAY", ":0", 0);

    // https://tronche.com/gui/x/xlib/display/opening.html
    /* calloc zeroes every field, which matches the explicit reset of the many
     * _Xprivate Display members the protocol expects to start at NULL/0
     * (lock_meaning == NoSymbol == 0, cursor_font == None == 0, etc.).
     */
    Display *display = calloc(1, sizeof(Display));
    if (!display) {
        fprintf(stderr, "libX11-compat: failed to allocate Display struct\n");
        return NULL;
    }

    /* Track whether the open path owns the SDL/TTF init so the failure paths
     * only tear down what this call brought up. An embedding application that
     * pre-initialized SDL would otherwise lose its video subsystem.
     */
    Bool sdlOwned = False;
    Bool ttfOwned = False;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        /* libX11-compat initializes SDL from XOpenDisplay, not SDL_main. Tell
         * SDL its platform main bootstrap has already been handled before
         * SDL_Init runs.
         */
        SDL_SetMainReady();
        SDL_SetHint(SDL_HINT_VIDEO_X11_XKB, "0");
        SDL_SetHint("SDL_IME_SHOW_UI", "1");
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "0");
        SDL_SetHint("SDL_WINDOW_ACTIVATE_WHEN_SHOWN", "1");
        SDL_SetHint("SDL_WINDOW_ACTIVATE_WHEN_RAISED", "1");

        /* SDL3 posts SDL_EVENT_QUIT when the last window closes. In an
         * in-process X layer the client owns its windows and its lifetime: a
         * toolkit routinely leaves zero mapped windows for an instant (Tk
         * withdraws and recreates a toplevel while posting a modal dialog such
         * as tk_getOpenFile), and a spurious quit there tore the whole client
         * down mid-dialog. The client still exits through XCloseDisplay, an
         * explicit quit, or a real window-close (handled separately), so hold
         * this off. Named literal: the macro is SDL3-only.
         */
        SDL_SetHint("SDL_QUIT_ON_LAST_WINDOW_CLOSE", "0");

        /* On macOS, the click that activates a background window is consumed by
         * activation and never seen by the app. Click-through routes that first
         * click to the app as a real button event, matching X11 semantics. Has
         * no effect on Accessibility/TCC gating of synthetic input (cliclick,
         * CGEvent).
         */
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        /* Cancel any present-wake timer a previous display left pending so its
         * callback cannot push an SDL event while this SDL_Init touches the
         * event subsystem; ThreadSanitizer flags that overlap as a race.
         */
        cancelPresentWakeTimer();
        sdlVideoInitInProgress = True;
        /* Init the timer subsystem alongside video so this display owns its
         * refcount and the matching SDL_Quit joins the SDL timer thread. The
         * present-wake and Xt pump-wake timers otherwise start that thread
         * lazily through SDL_AddTimer, outside SDL's init refcount, so SDL_Quit
         * leaves it running; a later XOpenDisplay's SDL_Init then re-inits the
         * tick base under the still-live thread, which ThreadSanitizer reports
         * as a data race. SDL_INIT_TIMER is a no-op bit on the SDL3 backend,
         * where timers need no subsystem.
         */
        int sdlInitResult = SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER);
        sdlVideoInitInProgress = False;
        /* SDL2 returns 0 on success or a negative error code on failure, and on
         * the SDL3 backend SDL_Init resolves to xc_Init (sdl-compat.h), which
         * maps SDL3's bool to that same 0/negative convention. Test for any
         * negative so an SDL3 init failure runs the error path below rather
         * than continuing on uninitialized SDL state.
         */
        if (sdlInitResult < 0) {
            /* Diagnose intermittent XOpenDisplay -> NULL failures observed
             * under load on the remote Xvfb differential test. The Xt error
             * "Can't open display" is generic; without this line the actual SDL
             * backend error is lost when DEBUG_LIBX11_COMPAT is off. Cache
             * getenv() results in locals: passing them through ternaries inline
             * evaluates getenv() twice per arg, and a NULL result the second
             * time around would be undefined behavior in fprintf %s.
             */
            const char *displayEnv = getenv("DISPLAY");
            const char *driverEnv = getenv("SDL_VIDEODRIVER");
            fprintf(stderr,
                    "libX11-compat: SDL_Init(VIDEO) failed: %s "
                    "(DISPLAY=%s SDL_VIDEODRIVER=%s)\n",
                    SDL_GetError(), displayEnv ? displayEnv : "",
                    driverEnv ? driverEnv : "");
            free(display);
            return NULL;
        }
        sdlOwned = True;
    }
    if (!TTF_WasInit()) {
        if (TTF_Init() == -1) {
            fprintf(stderr, "libX11-compat: TTF_Init failed: %s\n",
                    TTF_GetError());
            if (sdlOwned)
                SDL_Quit();
            free(display);
            return NULL;
        }
        ttfOwned = True;
    }
    if (numDisplaysOpen == 0) {
        /* Probe the HiDPI backing scale before font storage initializes so the
         * first XLoadFont already opens glyphs at the physical-pixel size.
         */
        probeGlobalHiDpiScale();
        if (!(initVisuals() && initColorStorage() && initFontStorage())) {
            fprintf(stderr,
                    "libX11-compat: visuals/colors/fonts init failed\n");
            freeFontStorage();
            freeColorStorage();
            freeVisuals();
            if (ttfOwned)
                TTF_Quit();
            if (sdlOwned)
                SDL_Quit();
            free(display);
            return NULL;
        }
    }
    numDisplaysOpen++;

    display->next_event_serial_num = 1;
    int eventFd = initEventPipe(display);
    if (eventFd < 0) {
        fprintf(stderr, "libX11-compat: initEventPipe failed\n");
        display->nscreens = 0;
        XCloseDisplay(display);
        return NULL;
    }
    display->fd = eventFd;
    display->proto_major_version = X_PROTOCOL;
    display->proto_minor_version = X_PROTOCOL_REVISION;
    display->vendor = vendor;
    display->release = releaseVersion;
    display->request = 0;
    display->last_request_read = 0;
    setLastRequestCode(display, X_NoOperation);
    display->min_keycode = 8;
    display->max_keycode = 255;
    display->display_name = (char *) display_name;
    display->byte_order = SDL_BYTEORDER == SDL_BIG_ENDIAN ? MSBFirst : LSBFirst;
    display->bitmap_unit = 32;
    display->bitmap_pad = 32;
    display->bitmap_bit_order = display->byte_order;
    /* Keep the Xlib default screen stable at 0. SDL_GetCurrentVideoDisplay is
     * window-relative and can change after windows move between displays.
     */
    display->default_screen = 0;
    display->nscreens = SDL_GetNumVideoDisplays();
    if (display->nscreens < 0) {
        fprintf(stderr, "libX11-compat: SDL_GetNumVideoDisplays failed: %s\n",
                SDL_GetError());
        XCloseDisplay(display);
        return NULL;
    }
    display->screens = calloc((size_t) display->nscreens, sizeof(Screen));
    if (!display->screens) {
        fprintf(stderr, "libX11-compat: failed to allocate %d screen records\n",
                display->nscreens);
        XCloseDisplay(display);
        return NULL;
    }

    /* Initialize the display lock */
    if (InitDisplayLock(display) != 0) {
        fprintf(stderr, "libX11-compat: InitDisplayLock failed\n");
        XCloseDisplay(display);
        return NULL;
    }
    installRecursiveDisplayLock(display);

    if (!(display->free_funcs = Xcalloc(1, sizeof(_XFreeFuncRec)))) {
        fprintf(stderr, "libX11-compat: failed to allocate free_funcs\n");
        XCloseDisplay(display);
        return NULL;
    }

    int screenIndex;
    for (screenIndex = 0; screenIndex < display->nscreens; screenIndex++) {
        Screen *screen = &display->screens[screenIndex];
        SDL_DisplayMode displayMode;
        if (SDL_GetDesktopDisplayMode(screenIndex, &displayMode) != 0) {
            fprintf(stderr,
                    "libX11-compat: SDL_GetDesktopDisplayMode(%d) failed: %s\n",
                    screenIndex, SDL_GetError());
            XCloseDisplay(display);
            return NULL;
        }
        screen->display = display;
        applyScreenGeometryOverride(&displayMode.w, &displayMode.h);
        screen->width = displayMode.w;
        screen->height = displayMode.h;

        /* Motif converts resolution-independent dimensions from the screen's
         * reported physical size. Host physical DPI, especially Retina/HiDPI,
         * makes those resources expand by the monitor scale factor while
         * XGetDefault/_XSETTINGS still advertise 96 DPI. Report a stable
         * logical X server size so toolkit layout and font defaults agree.
         */
        screen->mwidth =
            (int) roundf(((float) displayMode.w * 25.4f) / COMPAT_LOGICAL_DPI);
        screen->mheight =
            (int) roundf(((float) displayMode.h * 25.4f) / COMPAT_LOGICAL_DPI);
        screen->root = SCREEN_WINDOW;
        screen->root_visual = getDefaultVisual(screenIndex);

        /* RGB32 visuals have 24 significant bits with the high byte ignored for
         * alpha. Matches the (depth 24, bpp 32) entry from XListPixmapFormats.
         */
        screen->root_depth = 24;
        screen->ndepths = ARRAY_LENGTH(supportedDepths);
        screen->depths = calloc(ARRAY_LENGTH(supportedDepths), sizeof(Depth));
        if (!screen->depths) {
            fprintf(stderr,
                    "libX11-compat: failed to allocate depths for screen %d\n",
                    screenIndex);
            XCloseDisplay(display);
            return NULL;
        }
        for (size_t depthIndex = 0; depthIndex < ARRAY_LENGTH(supportedDepths);
             depthIndex++) {
            screen->depths[depthIndex].depth = supportedDepths[depthIndex];
        }

        /* Default pixels are 24-bit TrueColor values, not full internal ARGB
         * draw colors. Some legacy clients index tables sized by 1 <<
         * DefaultDepth using BlackPixel/WhitePixel.
         */
        screen->white_pixel = 0x00FFFFFF;
        screen->black_pixel = 0x00000000;
        screen->cmap = REAL_COLOR_COLORMAP;
        screen->min_maps = 1;
        screen->max_maps = 1;
        screen->backing_store = NotUseful;
        screen->save_unders = False;
        screen->root_input_mask = NoEventMask;
    }
    if (SCREEN_WINDOW == None) {
        if (initScreenWindow(display) != True) {
            fprintf(stderr, "libX11-compat: initScreenWindow failed: %s\n",
                    SDL_GetError());
            XCloseDisplay(display);
            return NULL;
        }
    }
    for (screenIndex = 0; screenIndex < display->nscreens; screenIndex++) {
        display->screens[screenIndex].root = SCREEN_WINDOW;
        display->screens[screenIndex].default_gc =
            XCreateGC(display, RootWindow(display, 0), 0, 0);
        if (!display->screens[screenIndex].default_gc) {
            fprintf(stderr, "libX11-compat: XCreateGC failed for screen %d\n",
                    screenIndex);
            XCloseDisplay(display);
            return NULL;
        }
        GraphicContext *defaultGc =
            GET_GC(display->screens[screenIndex].default_gc);
        defaultGc->foreground = display->screens[screenIndex].black_pixel;
        defaultGc->background = display->screens[screenIndex].white_pixel;
    }

    if (numDisplaysOpen == 1) {
        /* Init the font search path */
        XSetFontPath(display, NULL, 0);
    }

    /* Advertise the probed HiDPI backing scale on the root window so clients
     * that render at a fixed point size can scale their glyphs to match the
     * promoted physical-pixel geometry. Kept current on a monitor move by the
     * SDL_WINDOWEVENT_DISPLAY_CHANGED handler.
     */
    compatPublishHiDpiScaleProperty(display);
    return display;
}

int XBell(Display *display, int percent)
{
    // https://tronche.com/gui/x/xlib/input/XBell.html
    SET_X_SERVER_REQUEST(display, X_Bell);
    if (-100 > percent || 100 < percent) {
        handleError(0, display, None, 0, BadValue, 0);
    } else {
        /* Terminal bell only: portable SDL audio/haptic feedback would need
         * device setup and user-visible side effects beyond XBell's hint.
         */
        printf("\a");
        fflush(stdout);
    }
    return 1;
}

int XSync(Display *display, Bool discard)
{
    // https://tronche.com/gui/x/xlib/event-handling/XSync.html
    // WARN_UNIMPLEMENTED;
    drawWindowDataToScreen();
    return 1;
}

/* XConvertSelection / XSetSelectionOwner live in src/selection.c. */

int XNoOp(Display *display)
{
    // https://tronche.com/gui/x/xlib/display/XNoOp.html
    SET_X_SERVER_REQUEST(display, X_NoOperation);
    return 1;
}

int XGrabServer(Display *display)
{
    // https://tronche.com/gui/x/xlib/window-and-session-manager/XGrabServer.html
    SET_X_SERVER_REQUEST(display, X_GrabServer);
    /* No-op: this backend has no separate X server or competing clients to
     * exclude while a client updates global state.
     */
    return 1;
}

int XUngrabServer(Display *display)
{
    // https://tronche.com/gui/x/xlib/window-and-session-manager/XUngrabServer.html
    SET_X_SERVER_REQUEST(display, X_UngrabServer);
    return 1;
}

XHostAddress *XListHosts(Display *display,
                         int *nhosts_return,
                         Bool *state_return)
{
    // https://tronche.com/gui/x/xlib/window-and-session-manager/controlling-host-access/XListHosts.html
    SET_X_SERVER_REQUEST(display, X_ListHosts);
    const static char *LOCAL_HOST = "127.0.0.1";
    *state_return = True;
    XHostAddress *host = malloc(sizeof(XHostAddress));
    if (!host) {
        *nhosts_return = 0;
        return NULL;
    }
    *nhosts_return = 1;
    host->address = (char *) LOCAL_HOST;
    host->length = strlen(LOCAL_HOST);
    host->family = FamilyInternet;
    return host;
}

#define BOOL long
#define SIGNEDINT long
#define UNSIGNEDINT unsigned long
#define RESOURCEID unsigned long

/* this structure may be extended, but do not change the order */
typedef struct {
    UNSIGNEDINT flags;
    BOOL input;             /* need to convert */
    SIGNEDINT initialState; /* need to cvt */
    RESOURCEID iconPixmap;
    RESOURCEID iconWindow;
    SIGNEDINT iconX; /* need to cvt */
    SIGNEDINT iconY; /* need to cvt */
    RESOURCEID iconMask;
    UNSIGNEDINT windowGroup;
} xPropWMHints;
#define NumPropWMHintsElements 9 /* number of elements in this structure */

#undef BOOL
#undef SIGNEDINT
#undef UNSIGNEDINT
#undef RESOURCEID

int XSetWMHints(Display *dpy, Window w, XWMHints *wmhints)
{
    xPropWMHints prop;
    memset(&prop, 0, sizeof(prop));
    prop.flags = wmhints->flags;
    if (wmhints->flags & InputHint)
        prop.input = (wmhints->input == True ? 1 : 0);
    if (wmhints->flags & StateHint)
        prop.initialState = wmhints->initial_state;
    if (wmhints->flags & IconPixmapHint)
        prop.iconPixmap = wmhints->icon_pixmap;
    if (wmhints->flags & IconWindowHint)
        prop.iconWindow = wmhints->icon_window;
    if (wmhints->flags & IconPositionHint) {
        prop.iconX = wmhints->icon_x;
        prop.iconY = wmhints->icon_y;
    }
    if (wmhints->flags & IconMaskHint)
        prop.iconMask = wmhints->icon_mask;
    if (wmhints->flags & WindowGroupHint)
        prop.windowGroup = wmhints->window_group;
    return XChangeProperty(dpy, w, XA_WM_HINTS, XA_WM_HINTS, 32,
                           PropModeReplace, (unsigned char *) &prop,
                           NumPropWMHintsElements);
}

XWMHints *XGetWMHints(Display *dpy, Window w)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *property = NULL;
    long propertyLength = NumPropWMHintsElements * (long) sizeof(long) / 4;
    if (XGetWindowProperty(dpy, w, XA_WM_HINTS, 0, propertyLength, False,
                           XA_WM_HINTS, &actualType, &actualFormat, &nitems,
                           &bytesAfter, &property) != Success ||
        actualType != XA_WM_HINTS || actualFormat != 32 ||
        nitems < NumPropWMHintsElements || !property) {
        if (property)
            XFree(property);
        return NULL;
    }

    xPropWMHints *prop = (xPropWMHints *) property;
    XWMHints *hints = XAllocWMHints();
    if (!hints) {
        XFree(property);
        return NULL;
    }
    hints->flags = prop->flags;
    hints->input = prop->input ? True : False;
    hints->initial_state = prop->initialState;
    hints->icon_pixmap = prop->iconPixmap;
    hints->icon_window = prop->iconWindow;
    hints->icon_x = prop->iconX;
    hints->icon_y = prop->iconY;
    hints->icon_mask = prop->iconMask;
    hints->window_group = prop->windowGroup;
    XFree(property);
    return hints;
}

int XSetCommand(Display *display, Window w, char **argv, int argc)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-session-manager/XSetCommand.html
    if (argc < 0) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }

    /* XChangeProperty takes the element count as int, and the ICCCM payload
     * size is bounded by the X protocol; cap aggregate WM_COMMAND bytes at
     * INT_MAX. Check bytes first so the subtraction in the second clause cannot
     * wrap when bytes is already at the cap.
     */
    size_t bytes = 0;
    for (int i = 0; i < argc; i++) {
        size_t len = argv && argv[i] ? strlen(argv[i]) : 0;
        if (bytes >= (size_t) INT_MAX || len > (size_t) INT_MAX - 1 - bytes) {
            handleError(0, display, None, 0, BadAlloc, 0);
            return 0;
        }
        bytes += len + 1;
    }
    unsigned char *data = bytes ? malloc(bytes) : NULL;
    if (bytes && !data) {
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    unsigned char *p = data;
    for (int i = 0; i < argc; i++) {
        size_t len = argv && argv[i] ? strlen(argv[i]) : 0;
        if (len)
            memcpy(p, argv[i], len);
        p[len] = '\0';
        p += len + 1;
    }
    int ok = XChangeProperty(display, w, XA_WM_COMMAND, XA_STRING, 8,
                             PropModeReplace, data, (int) bytes);
    free(data);
    return ok;
}

#define BOOL long
#define SIGNEDINT long
#define UNSIGNEDINT unsigned long
#define RESOURCEID unsigned long

/* this structure may be extended, but do not change the order */
typedef struct {
    UNSIGNEDINT flags;
    SIGNEDINT x, y, width, height;    /* need to cvt; only for pre-ICCCM */
    SIGNEDINT minWidth, minHeight;    /* need to cvt */
    SIGNEDINT maxWidth, maxHeight;    /* need to cvt */
    SIGNEDINT widthInc, heightInc;    /* need to cvt */
    SIGNEDINT minAspectX, minAspectY; /* need to cvt */
    SIGNEDINT maxAspectX, maxAspectY; /* need to cvt */
    SIGNEDINT baseWidth, baseHeight;  /* need to cvt; ICCCM version 1 */
    SIGNEDINT winGravity;             /* need to cvt; ICCCM version 1 */
} xPropSizeHints;
#define OldNumPropSizeElements 15 /* pre-ICCCM */
#define NumPropSizeElements 18    /* ICCCM version 1 */

#undef BOOL
#undef SIGNEDINT
#undef UNSIGNEDINT
#undef RESOURCEID

void XSetWMSizeHints(Display *dpy, Window w, XSizeHints *hints, Atom prop)
{
    xPropSizeHints data;

    memset(&data, 0, sizeof(data));
    data.flags = (hints->flags &
                  (USPosition | USSize | PPosition | PSize | PMinSize |
                   PMaxSize | PResizeInc | PAspect | PBaseSize | PWinGravity));

    /* The x, y, width, and height fields are obsolete; but, applications that
     * want to work with old window managers might set them.
     */
    if (hints->flags & (USPosition | PPosition)) {
        data.x = hints->x;
        data.y = hints->y;
    }
    if (hints->flags & (USSize | PSize)) {
        data.width = hints->width;
        data.height = hints->height;
    }

    if (hints->flags & PMinSize) {
        data.minWidth = hints->min_width;
        data.minHeight = hints->min_height;
    }
    if (hints->flags & PMaxSize) {
        data.maxWidth = hints->max_width;
        data.maxHeight = hints->max_height;
    }
    if (hints->flags & PResizeInc) {
        data.widthInc = hints->width_inc;
        data.heightInc = hints->height_inc;
    }
    if (hints->flags & PAspect) {
        data.minAspectX = hints->min_aspect.x;
        data.minAspectY = hints->min_aspect.y;
        data.maxAspectX = hints->max_aspect.x;
        data.maxAspectY = hints->max_aspect.y;
    }
    if (hints->flags & PBaseSize) {
        data.baseWidth = hints->base_width;
        data.baseHeight = hints->base_height;
    }
    if (hints->flags & PWinGravity)
        data.winGravity = hints->win_gravity;

    /* The resize-increment cache is refreshed from the stored property on the
     * XChangeProperty success path (window-property.c calls
     * cacheResizeIncrementsFromNormalHintsProperty for
     * WM_NORMAL_HINTS/format-32 there), so it stays correct without a second
     * inline decode here that would also run even when the property write
     * failed.
     */
    XChangeProperty(dpy, w, prop, XA_WM_SIZE_HINTS, 32, PropModeReplace,
                    (unsigned char *) &data, NumPropSizeElements);
}

/* Apply size hints to the SDL window so user resizes are clamped. X11
 * 'border_width' lives inside the window's geometry, so SDL minimum / maximum
 * can use min/max_width/height directly without adding border to either side.
 * Runs on the main event thread only (XSetWMNormalHints routes it there).
 */
static void applyWmNormalHintsSdl(Window w, XSizeHints *hints)
{
    SDL_Window *sdlWindow = GET_WINDOW_STRUCT(w)->sdlWindow;

    /* Clear stale constraints when a flag disappears: passing 0 tells SDL there
     * is no bound. Otherwise a window that switches from fixed-size hints
     * (PMaxSize with max == min) to ranged hints (no PMaxSize) would keep the
     * old maximum and stay effectively fixed even after resize is enabled.
     */
    if (hints->flags & PMinSize) {
        SDL_SetWindowMinimumSize(sdlWindow, hints->min_width,
                                 hints->min_height);
    } else {
        SDL_SetWindowMinimumSize(sdlWindow, 0, 0);
    }
    if (hints->flags & PMaxSize) {
        SDL_SetWindowMaximumSize(sdlWindow, hints->max_width,
                                 hints->max_height);
    } else {
        SDL_SetWindowMaximumSize(sdlWindow, 0, 0);
    }
    applyNormalHintsResizableFromProperty(w);
}

struct wmNormalHintsArgs {
    Window window;
    XSizeHints *hints;
};

static void wmNormalHintsSdlOnMain(void *p)
{
    struct wmNormalHintsArgs *a = (struct wmNormalHintsArgs *) p;
    applyWmNormalHintsSdl(a->window, a->hints);
}

void XSetWMNormalHints(Display *dpy, Window w, XSizeHints *hints)
{
    XSetWMSizeHints(dpy, w, hints, XA_WM_NORMAL_HINTS);

    if (!hints || !IS_MAPPED_TOP_LEVEL_WINDOW(w))
        return;

    /* The hints pointer stays valid for the whole call: an off-main worker
     * blocks in runOnMainThread until the main thread has read it.
     */
    if (!libx11CompatOnMainEventThread()) {
        struct wmNormalHintsArgs a = {w, hints};
        runOnMainThread(wmNormalHintsSdlOnMain, &a);
        return;
    }
    applyWmNormalHintsSdl(w, hints);
}

Bool topLevelResizableFromNormalHints(Window window)
{
    if (!IS_TYPE(window, WINDOW))
        return False;

    /* A client that speaks Motif owns the resizable decision through
     * _MOTIF_WM_HINTS (MWM_FUNC_RESIZE). Only speak for the WM_NORMAL_HINTS
     * default when it has not, so the two paths compose instead of fighting.
     */
    if (topLevelHasMotifFunctionHints(window))
        return False;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    WindowProperty *prop =
        findProperty(&windowStruct->properties, XA_WM_NORMAL_HINTS, NULL);
    if (!prop || prop->dataFormat != 32 || prop->type != XA_WM_SIZE_HINTS ||
        prop->dataLength < OldNumPropSizeElements || !prop->data)
        return False;

    xPropSizeHints *sizeHints = (xPropSizeHints *) prop->data;

    /* Mirror a real WM: a top-level is resizable unless the client pinned it to
     * a fixed size (both PMinSize and PMaxSize present with min == max). xwpe
     * advertises PMinSize/PResizeInc/PBaseSize without capping max, so it stays
     * resizable; fixed-size dialogs (min == max) do not.
     */
    Bool fixedSize = (sizeHints->flags & PMinSize) &&
                     (sizeHints->flags & PMaxSize) &&
                     sizeHints->minWidth == sizeHints->maxWidth &&
                     sizeHints->minHeight == sizeHints->maxHeight;
    return fixedSize ? False : True;
}

void applyNormalHintsResizableFromProperty(Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        runWindowOpOnMain(applyNormalHintsResizableFromProperty, window);
        return;
    }
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window))
        return;

    /* Defer to Motif when it governs functions: the decoder returns False for
     * that case, but so does a genuinely non-resizable window, so gate the SDL
     * call on the Motif check here to avoid forcing non-resizable over Motif.
     */
    if (topLevelHasMotifFunctionHints(window))
        return;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    WindowProperty *prop =
        findProperty(&windowStruct->properties, XA_WM_NORMAL_HINTS, NULL);
    if (!prop || prop->dataFormat != 32 || prop->type != XA_WM_SIZE_HINTS ||
        prop->dataLength < OldNumPropSizeElements || !prop->data)
        return;

    Bool resizable = topLevelResizableFromNormalHints(window);
#if SDL_VERSION_ATLEAST(2, 0, 5)
    SDL_SetWindowResizable(windowStruct->sdlWindow,
                           resizable ? SDL_TRUE : SDL_FALSE);
#else
    (void) resizable;
#endif
}

void cacheResizeIncrementsFromNormalHintsProperty(Window window)
{
    if (!IS_TYPE(window, WINDOW))
        return;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    WindowProperty *prop =
        findProperty(&windowStruct->properties, XA_WM_NORMAL_HINTS, NULL);
    if (!prop || prop->dataFormat != 32 || prop->type != XA_WM_SIZE_HINTS ||
        prop->dataLength < OldNumPropSizeElements || !prop->data) {
        windowStruct->hasResizeInc = False;
        return;
    }

    xPropSizeHints *sizeHints = (xPropSizeHints *) prop->data;

    /* Mirror the cache the XSetWMSizeHints path populates from the XSizeHints
     * struct (see the PResizeInc block there): only WM_NORMAL_HINTS governs
     * live sizing and only PResizeInc requests cell snapping. ICCCM: when
     * PBaseSize is absent the minimum size stands in for the base, so fall back
     * to min_width/height (not 0) as the grid origin; a missing PMinSize
     * defaults to 0 ("no minimum"). The base fields live only in the 18-element
     * ICCCM-v1 payload (indices 15-16); a legacy 15-element write reaches here
     * with dataLength == OldNumPropSizeElements, so treat PBaseSize as absent
     * unless the full payload is present, or the read runs past the store.
     */
    Bool haveBaseFields = (sizeHints->flags & PBaseSize) &&
                          prop->dataLength >= NumPropSizeElements;
    if ((sizeHints->flags & PResizeInc) && sizeHints->widthInc > 0 &&
        sizeHints->heightInc > 0) {
        windowStruct->hasResizeInc = True;
        windowStruct->widthInc = (int) sizeHints->widthInc;
        windowStruct->heightInc = (int) sizeHints->heightInc;
        windowStruct->baseWidth =
            haveBaseFields
                ? (int) sizeHints->baseWidth
                : ((sizeHints->flags & PMinSize) ? (int) sizeHints->minWidth
                                                 : 0);
        windowStruct->baseHeight =
            haveBaseFields
                ? (int) sizeHints->baseHeight
                : ((sizeHints->flags & PMinSize) ? (int) sizeHints->minHeight
                                                 : 0);
        windowStruct->minWidth =
            (sizeHints->flags & PMinSize) ? (int) sizeHints->minWidth : 0;
        windowStruct->minHeight =
            (sizeHints->flags & PMinSize) ? (int) sizeHints->minHeight : 0;
    } else {
        windowStruct->hasResizeInc = False;
    }
}

Status XGetWMSizeHints(Display *dpy,
                       Window w,
                       XSizeHints *hints,
                       long *supplied,
                       Atom property)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *propertyData = NULL;
    long propertyLength = NumPropSizeElements * (long) sizeof(long) / 4;
    if (XGetWindowProperty(dpy, w, property, 0, propertyLength, False,
                           XA_WM_SIZE_HINTS, &actualType, &actualFormat,
                           &nitems, &bytesAfter, &propertyData) != Success ||
        actualType != XA_WM_SIZE_HINTS || actualFormat != 32 ||
        nitems < OldNumPropSizeElements || !propertyData) {
        if (propertyData)
            XFree(propertyData);
        return 0;
    }

    xPropSizeHints *prop = (xPropSizeHints *) propertyData;
    memset(hints, 0, sizeof(*hints));
    hints->flags = prop->flags;
    hints->x = prop->x;
    hints->y = prop->y;
    hints->width = prop->width;
    hints->height = prop->height;
    hints->min_width = prop->minWidth;
    hints->min_height = prop->minHeight;
    hints->max_width = prop->maxWidth;
    hints->max_height = prop->maxHeight;
    hints->width_inc = prop->widthInc;
    hints->height_inc = prop->heightInc;
    hints->min_aspect.x = prop->minAspectX;
    hints->min_aspect.y = prop->minAspectY;
    hints->max_aspect.x = prop->maxAspectX;
    hints->max_aspect.y = prop->maxAspectY;
    if (nitems >= NumPropSizeElements) {
        hints->base_width = prop->baseWidth;
        hints->base_height = prop->baseHeight;
        hints->win_gravity = prop->winGravity;
    }
    if (supplied)
        *supplied = hints->flags;
    XFree(propertyData);
    return 1;
}

Status XGetWMNormalHints(Display *dpy,
                         Window w,
                         XSizeHints *hints,
                         long *supplied)
{
    return XGetWMSizeHints(dpy, w, hints, supplied, XA_WM_NORMAL_HINTS);
}

int XSetClassHint(Display *display, Window w, XClassHint *class_hints)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XSetClassHint.html
    if (!class_hints)
        return 0;
    const char *resName = class_hints->res_name ? class_hints->res_name : "";
    const char *resClass = class_hints->res_class ? class_hints->res_class : "";
    size_t nameLen = strlen(resName);
    size_t classLen = strlen(resClass);

    /* XChangeProperty takes int element count; both NUL terminators must fit
     * along with nameLen + classLen, so cap the combined size below INT_MAX.
     * Check nameLen first so the subtraction in the second clause cannot wrap
     * when nameLen is already at the cap.
     */
    if (nameLen > (size_t) INT_MAX - 2 ||
        classLen > (size_t) INT_MAX - 2 - nameLen) {
        handleError(0, display, None, 0, BadAlloc, 0);
        return 0;
    }
    size_t bytes = nameLen + 1 + classLen + 1;
    unsigned char *data = malloc(bytes);
    if (!data) {
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    memcpy(data, resName, nameLen + 1);
    memcpy(data + nameLen + 1, resClass, classLen + 1);
    int ok = XChangeProperty(display, w, XA_WM_CLASS, XA_STRING, 8,
                             PropModeReplace, data, (int) bytes);
    free(data);
    return ok;
}

Status XStringListToTextProperty(char **list,
                                 int count,
                                 XTextProperty *text_prop_return)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XStringListToTextProperty.html
    /* Per ICCCM: value holds NUL-separated strings, nitems is the byte count
     * excluding the trailing NUL. NULL entries become empty strings.
     */
    text_prop_return->encoding = XA_STRING;
    text_prop_return->format = 8;
    text_prop_return->value = NULL;
    text_prop_return->nitems = 0;

    size_t nbytes = 0;
    for (int i = 0; i < count; i++) {
        size_t len = list[i] ? strlen(list[i]) : 0;
        /* Guard against size_t wrap from a malicious or absurdly large list. */
        if (len + 1 > SIZE_MAX - nbytes)
            return 0;
        nbytes += len + 1;
    }

    if (nbytes == 0) {
        text_prop_return->value = malloc(1);
        if (!text_prop_return->value)
            return 0;
        text_prop_return->value[0] = '\0';
        return 1;
    }

    text_prop_return->value = malloc(nbytes);
    if (!text_prop_return->value)
        return 0;

    char *buf = (char *) text_prop_return->value;
    for (int i = 0; i < count; i++) {
        if (list[i]) {
            size_t n = strlen(list[i]) + 1;
            memcpy(buf, list[i], n);
            buf += n;
        } else {
            *buf++ = '\0';
        }
    }
    text_prop_return->nitems = nbytes - 1;
    return 1;
}

Status XGetTextProperty(Display *display,
                        Window window,
                        XTextProperty *tp,
                        Atom property)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *value = NULL;
    if (XGetWindowProperty(display, window, property, 0, 1000000, False,
                           AnyPropertyType, &actualType, &actualFormat, &nitems,
                           &bytesAfter, &value) != Success ||
        actualType == None) {
        if (value)
            XFree(value);
        tp->value = NULL;
        tp->encoding = None;
        tp->format = 0;
        tp->nitems = 0;
        return 0;
    }
    tp->value = value;
    tp->encoding = actualType;
    tp->format = actualFormat;
    tp->nitems = nitems;
    return 1;
}

void XSetWMClientMachine(Display *dpy, Window w, XTextProperty *tp)
{
    XSetTextProperty(dpy, w, tp, XA_WM_CLIENT_MACHINE);
}

#define safestrlen(s) ((s) ? strlen(s) : 0)

int XSetSizeHints(/* old routine */
                  Display *dpy,
                  Window w,
                  XSizeHints *hints,
                  Atom property)
{
    xPropSizeHints prop;
    memset(&prop, 0, sizeof(prop));
    prop.flags = (hints->flags & (USPosition | USSize | PAllHints));
    if (hints->flags & (USPosition | PPosition)) {
        prop.x = hints->x;
        prop.y = hints->y;
    }
    if (hints->flags & (USSize | PSize)) {
        prop.width = hints->width;
        prop.height = hints->height;
    }
    if (hints->flags & PMinSize) {
        prop.minWidth = hints->min_width;
        prop.minHeight = hints->min_height;
    }
    if (hints->flags & PMaxSize) {
        prop.maxWidth = hints->max_width;
        prop.maxHeight = hints->max_height;
    }
    if (hints->flags & PResizeInc) {
        prop.widthInc = hints->width_inc;
        prop.heightInc = hints->height_inc;
    }
    if (hints->flags & PAspect) {
        prop.minAspectX = hints->min_aspect.x;
        prop.minAspectY = hints->min_aspect.y;
        prop.maxAspectX = hints->max_aspect.x;
        prop.maxAspectY = hints->max_aspect.y;
    }
    return XChangeProperty(dpy, w, property, XA_WM_SIZE_HINTS, 32,
                           PropModeReplace, (unsigned char *) &prop,
                           OldNumPropSizeElements);
}

Status XGetSizeHints(Display *dpy, Window w, XSizeHints *hints, Atom property)
{
    long supplied = 0;
    return XGetWMSizeHints(dpy, w, hints, &supplied, property);
}

/* XSetNormalHints sets the property
 * 	WM_NORMAL_HINTS 	type: WM_SIZE_HINTS format: 32
 */

int XSetNormalHints(/* old routine */
                    Display *dpy,
                    Window w,
                    XSizeHints *hints)
{
    return XSetSizeHints(dpy, w, hints, XA_WM_NORMAL_HINTS);
}

Status XGetNormalHints(Display *dpy, Window w, XSizeHints *hints)
{
    long supplied = 0;
    return XGetWMNormalHints(dpy, w, hints, &supplied);
}

/* XSetStandardProperties sets the following properties:
 * 	WM_NAME		  type: STRING		format: 8
 * 	WM_ICON_NAME	  type: STRING		format: 8
 * 	WM_HINTS	  type: WM_HINTS	format: 32
 * 	WM_COMMAND	  type: STRING
 * 	WM_NORMAL_HINTS	  type: WM_SIZE_HINTS 	format: 32
 */

int XSetStandardProperties(
    Display *dpy,
    Window w /* window to decorate */,
    _Xconst char *name /* name of application */,
    _Xconst char *icon_string /* name string for icon */,
    Pixmap icon_pixmap /* pixmap to use as icon, or None */,
    char **argv /* command to be used to restart application */,
    int argc /* count of arguments */,
    XSizeHints *hints /* size hints for window in its normal state */)
{
    XWMHints phints;
    phints.flags = 0;

    if (name)
        XStoreName(dpy, w, name);

    if (safestrlen(icon_string) >= USHRT_MAX)
        return 1;
    if (icon_string) {
        XChangeProperty(dpy, w, XA_WM_ICON_NAME, XA_STRING, 8, PropModeReplace,
                        (_Xconst unsigned char *) icon_string,
                        (int) safestrlen(icon_string));
    }

    if (icon_pixmap != None) {
        phints.icon_pixmap = icon_pixmap;
        phints.flags |= IconPixmapHint;
    }
    if (argv)
        XSetCommand(dpy, w, argv, argc);

    if (hints)
        XSetNormalHints(dpy, w, hints);

    if (phints.flags != 0)
        XSetWMHints(dpy, w, &phints);

    return 1;
}
