#include <X11/Xlib.h>
#include <dlfcn.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "atoms.h"
#include "display.h"
#include "drawing.h"
#include "errors.h"
#include "events.h"
#include "input.h"
#include "input-method.h"
#include "main-dispatch.h"
#include "net-atoms.h"
#include "replay.h"
#include "replay-target.h"
#include "visual.h"
#include "window.h"

/* requireMainEventThread is declared in window-internal.h (pulled in via
 * window.h) so realizeTopLevelWindow above its definition, and configureWindow
 * in window-internal.c, share the one guard.
 */

static void replayDeferredWmProperties(Display *display, Window window);

#if defined(__APPLE__)
struct DarwinProcessSerialNumber {
    uint32_t highLongOfPSN;
    uint32_t lowLongOfPSN;
};

typedef void *(*ObjcGetClassFn)(const char *);
typedef void *(*SelRegisterNameFn)(const char *);
typedef void *(*ObjcMsgSendIdFn)(void *, void *);
typedef void (*ObjcMsgSendVoidFn)(void *, void *);
typedef void (*ObjcMsgSendVoidIdFn)(void *, void *, void *);
typedef void (*ObjcMsgSendVoidIntFn)(void *, void *, long);
typedef void (*ObjcMsgSendVoidBoolFn)(void *, void *, signed char);
typedef signed char (*ObjcMsgSendBoolIntFn)(void *, void *, unsigned long);

struct DarwinObjcRuntime {
    ObjcGetClassFn getClass;
    SelRegisterNameFn sel;
    void *msgSend;
};

static struct DarwinObjcRuntime cachedObjcRuntime;
static Bool cachedObjcRuntimeOk;
static pthread_once_t objcRuntimeOnce = PTHREAD_ONCE_INIT;

static void resolveObjcRuntimeOnce(void)
{
    (void) dlopen("/System/Library/Frameworks/AppKit.framework/AppKit",
                  RTLD_LAZY | RTLD_LOCAL);
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_LOCAL);

    struct DarwinObjcRuntime *runtime = &cachedObjcRuntime;
    runtime->getClass = (ObjcGetClassFn) dlsym(RTLD_DEFAULT, "objc_getClass");
    runtime->sel = (SelRegisterNameFn) dlsym(RTLD_DEFAULT, "sel_registerName");
    runtime->msgSend = dlsym(RTLD_DEFAULT, "objc_msgSend");
    if (objc) {
        if (!runtime->getClass)
            runtime->getClass = (ObjcGetClassFn) dlsym(objc, "objc_getClass");
        if (!runtime->sel)
            runtime->sel = (SelRegisterNameFn) dlsym(objc, "sel_registerName");
        if (!runtime->msgSend)
            runtime->msgSend = dlsym(objc, "objc_msgSend");
    }
    cachedObjcRuntimeOk = runtime->getClass && runtime->sel && runtime->msgSend;
}

static Bool loadObjcRuntime(struct DarwinObjcRuntime *runtime)
{
    /* Callers hit this on every top-level map (activate, order-front), and the
     * resolved runtime never changes, so resolve the frameworks and symbols
     * once. Repeating the dlopen per map churned handle refcounts for no gain.
     */
    pthread_once(&objcRuntimeOnce, resolveObjcRuntimeOnce);
    *runtime = cachedObjcRuntime;
    return cachedObjcRuntimeOk;
}

static void activateHostApplication(void)
{
    static Bool transformed = False;

    if (!transformed) {
        void *applicationServices = dlopen(
            "/System/Library/Frameworks/ApplicationServices.framework/"
            "ApplicationServices",
            RTLD_LAZY | RTLD_LOCAL);
        if (applicationServices) {
            typedef int (*GetCurrentProcessFn)(
                struct DarwinProcessSerialNumber *);
            typedef int (*TransformProcessTypeFn)(
                const struct DarwinProcessSerialNumber *, unsigned int);
            typedef int (*SetFrontProcessFn)(
                const struct DarwinProcessSerialNumber *);

            GetCurrentProcessFn getCurrentProcess = (GetCurrentProcessFn) dlsym(
                applicationServices, "GetCurrentProcess");
            TransformProcessTypeFn transformProcessType =
                (TransformProcessTypeFn) dlsym(applicationServices,
                                               "TransformProcessType");
            SetFrontProcessFn setFrontProcess = (SetFrontProcessFn) dlsym(
                applicationServices, "SetFrontProcess");

            if (getCurrentProcess && transformProcessType && setFrontProcess) {
                struct DarwinProcessSerialNumber psn;
                if (getCurrentProcess(&psn) == 0) {
                    if (transformProcessType(&psn, 1) == 0)
                        transformed = True;
                    (void) setFrontProcess(&psn);
                }
            }

            /* The resolved symbols are only used above, so drop the handle
             * rather than leak a reference on every activation. Gated on
             * !transformed so a successful transform never reopens it.
             */
            dlclose(applicationServices);
        }
    }

    struct DarwinObjcRuntime objc;
    if (!loadObjcRuntime(&objc))
        return;

    void *nsApplication = objc.getClass("NSApplication");
    if (!nsApplication)
        return;

    void *app = ((ObjcMsgSendIdFn) objc.msgSend)(nsApplication,
                                                 objc.sel("sharedApplication"));
    if (app) {
        ((ObjcMsgSendVoidIntFn) objc.msgSend)(
            app, objc.sel("setActivationPolicy:"), 0);
        ((ObjcMsgSendVoidFn) objc.msgSend)(app, objc.sel("finishLaunching"));
        ((ObjcMsgSendVoidIdFn) objc.msgSend)(app, objc.sel("unhide:"), NULL);
        ((ObjcMsgSendVoidBoolFn) objc.msgSend)(
            app, objc.sel("activateIgnoringOtherApps:"), 1);
    }

    void *nsRunningApplication = objc.getClass("NSRunningApplication");
    if (!nsRunningApplication)
        return;

    void *currentApplication = ((ObjcMsgSendIdFn) objc.msgSend)(
        nsRunningApplication, objc.sel("currentApplication"));
    if (!currentApplication)
        return;

    (void) ((ObjcMsgSendBoolIntFn) objc.msgSend)(
        currentApplication, objc.sel("activateWithOptions:"), 3);
}

static void orderNativeWindowFront(SDL_Window *sdlWindow)
{
    struct DarwinObjcRuntime objc;
    if (!loadObjcRuntime(&objc))
        return;

    void *nativeWindow = sdlCocoaWindowHandle(sdlWindow);
    if (!nativeWindow)
        return;

    ((ObjcMsgSendVoidIdFn) objc.msgSend)(
        nativeWindow, objc.sel("makeKeyAndOrderFront:"), NULL);
    ((ObjcMsgSendVoidFn) objc.msgSend)(nativeWindow,
                                       objc.sel("orderFrontRegardless"));
}
#endif

/* destroyWindow tears down the SDL window/renderer (SDL_DestroyWindow,
 * SDL_DestroyRenderer), main-thread-only on macOS, and reads the window tree,
 * so XDestroyWindow/XDestroySubwindows route their whole body to the main
 * thread when called off it. Shared args struct; on the main thread it is a
 * direct call.
 */
struct destroyArgs {
    Display *display;
    Window window;
    int rc;
};

static int destroyWindowEntry(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XDestroyWindow.html
    SET_X_SERVER_REQUEST(display, X_DestroyWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (window == SCREEN_WINDOW)
        return 0;
    destroyWindow(display, window, True);
    return 1;
}

static void destroyWindowOnMain(void *p)
{
    struct destroyArgs *a = (struct destroyArgs *) p;
    a->rc = destroyWindowEntry(a->display, a->window);
}

int XDestroyWindow(Display *display, Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        struct destroyArgs a = {display, window, 0};
        runOnMainThread(destroyWindowOnMain, &a);
        return a.rc;
    }
    return destroyWindowEntry(display, window);
}

static Bool moveChildToTop(Display *display, Window window)
{
    Window parent = GET_PARENT(window);
    if (parent == None)
        return False;
    return moveChildToIndexAndExpose(
        display, window, GET_WINDOW_STRUCT(parent)->children.length - 1);
}

static Bool moveChildToBottom(Display *display, Window window)
{
    return moveChildToIndexAndExpose(display, window, 0);
}

static void offerReplayTargetForWindow(Window window)
{
    for (Window current = window; current != None;
         current = GET_PARENT(current)) {
        WindowStruct *ws = GET_WINDOW_STRUCT(current);
        if (!ws || !ws->sdlWindow || ws->borrowedSdlWindow)
            continue;
        int wid = 0, hgt = 0;
        SDL_GetWindowSize(ws->sdlWindow, &wid, &hgt);
        replayTargetOfferWindow(SDL_GetWindowID(ws->sdlWindow), ws->x, ws->y,
                                wid, hgt);
        return;
    }
}

static void showTopLevelWindow(Display *display, Window window, Bool raise)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!windowStruct || !windowStruct->sdlWindow)
        return;

    windowStruct->needsPresent = True;

    /* Release any coalesce gate a prior host-resize burst left armed so this
     * mandatory present-before-show is not swallowed by
     * drawWindowDataToScreen's coalesce early-return, which would leave the
     * freshly mapped window unpresented. No-op when nothing is coalescing.
     */
    endCoalesceClientRepaint();
    drawWindowDataToScreen();
    if (windowStruct->borrowedSdlWindow) {
        replayDeferredWmProperties(display, window);
        return;
    }
#if defined(__APPLE__)
    if (raise)
        activateHostApplication();
#endif

    /* The map paths that call this already post an explicit MapNotify; arm the
     * one-shot so the MapNotify the coming SDL SHOWN echo would synthesize is
     * dropped in convertEvent rather than delivered a second time.
     */
    SDL_AtomicSet(&windowStruct->suppressSdlShowMap, 1);
    SDL_ShowWindow(windowStruct->sdlWindow);
    if (raise)
        SDL_RaiseWindow(windowStruct->sdlWindow);
#if defined(__APPLE__)
    if (raise)
        orderNativeWindowFront(windowStruct->sdlWindow);
#endif
    pumpEventsSafe();
#if defined(__APPLE__)
    if (raise)
        activateHostApplication();
#endif

    /* macOS may not retain an SDL_UpdateWindowSurface done while the native
     * window was hidden. Present once more after ShowWindow so the first mapped
     * frame reaches the compositor without waiting for an expose or alt-tab.
     */
    markWindowNeedsPresent(window);
    drawWindowDataToScreen();
    replayDeferredWmProperties(display, window);
}

static void postVisibilityForWindowAndSiblings(Display *display, Window window)
{
    Window parent = GET_PARENT(window);
    if (parent == None) {
        postEvent(display, window, VisibilityNotify);
        return;
    }
    Window *children = GET_CHILDREN(parent);
    for (size_t i = 0; i < GET_WINDOW_STRUCT(parent)->children.length; i++)
        postEvent(display, children[i], VisibilityNotify);
}

static Bool hasUnmappedAncestor(Window window)
{
    Window parent = GET_PARENT(window);
    while (parent != None) {
        if (GET_WINDOW_STRUCT(parent)->mapState == UnMapped)
            return True;
        parent = GET_PARENT(parent);
    }
    return False;
}

/* Whether to promote a top-level's X11 geometry from logical points to physical
 * pixels on a HiDPI backing (see the promotion block in realizeTopLevelWindow).
 * Fixed-pixel clients draw at literal X pixel coordinates, so the default path
 * leaves their X geometry logical and uniformly upscales the finished backing.
 * Toolkits such as Tk and Motif lay widgets out in logical points, so promoting
 * their X11 geometry behind their back double-scales and the layout settles
 * wrong. They still use a HiDPI SDL backing; the present path scales their
 * logical backing to it.
 *
 * The physical-pixel promotion path is off by default (opt-in via
 * LIBX11_COMPAT_HIDPI_PROMOTE) because it changes the X11 dimensions clients
 * observe; fixed-size clients then keep painting only their requested logical
 * area and leave the rest of the Retina backing blank.
 *
 * The geometry-promotion branch below and core-font scaling in font.c both gate
 * on compatHiDpiPromoteToolkits, so they cannot drift: an app that is not
 * promoted keeps 1x core fonts too, or the font scale and the present upscale
 * compound and the text comes out twice too large.
 */
static Bool realizeTopLevelWindow(Display *display, Window window)
{
    /* SDL_CreateWindow and the renderer setup below are main-thread-only on
     * macOS. Reached only from mapWindowImpl and XReparentWindow, both of which
     * route to the main thread; this asserts that invariant at the SDL source.
     */
    requireMainEventThread("realizeTopLevelWindow");
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (windowStruct->sdlWindow)
        return True;

    /* Start hidden so XMapWindow controls the show timing. Without this,
     * SDL_CreateWindow flashes an empty window before drawWindowDataToScreen
     * renders content; the SDL_ShowWindow call at the end of XMapWindow brings
     * it on screen with the first frame already drawn.
     */
    Uint32 flags = SDL_WINDOW_HIDDEN;
    if (windowStruct->overrideRedirect) {
        flags |= SDL_WINDOW_BORDERLESS;
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;

#ifndef LIBX11_COMPAT_SDL3
        /* SDL2 path only: the SDL3 backend routes override-redirect popups
         * through xc_CreatePopupWindow above. On SDL2, BORDERLESS alone leaves
         * the window WM-managed, so a real window manager (FVWM) reparents it,
         * draws a title bar on every menu, and its map/unmap leaves ghosting.
         * The TOOLTIP window type makes SDL2's X11 driver set
         * override_redirect, so the WM ignores the menu entirely. Supported
         * since SDL 2.0.5, so it is safe on the 2.0.10 floor.
         */
        flags |= SDL_WINDOW_TOOLTIP;
#endif
    }
    if (windowStruct->eventMask & KeyPressMask ||
        windowStruct->eventMask & KeyReleaseMask) {
        flags |= SDL_WINDOW_INPUT_FOCUS;
    }

    /* Ask SDL to create a HiDPI-aware backing on hosts that report a fractional
     * or 2x display scale. Without this on Apple Silicon the OS upscales the
     * SDL framebuffer at display time (the "fuzzy non-Retina look" -
     * XDrawString output and Xft text both come out blurry).
     * drawWindowDataToScreen detects the resulting size mismatch between the 1x
     * X11 backing texture and the 2x window surface and SDL_BlitScales the
     * readback so the present matches the actual backing resolution. SDL
     * silently ignores the flag on displays without high-DPI, so leaving it
     * always-on is safe.
     */
    Bool promoteHiDpi = compatHiDpiPromoteToolkits();
    flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    int hostX = windowStruct->x, hostY = windowStruct->y;
    topLevelWindowHostPosition(window, windowStruct->x, windowStruct->y, &hostX,
                               &hostY);
    LOG("realizeTopLevelWindow: window=%lu pos=(%d,%d) host=(%d,%d) "
        "size=(%ux%u) "
        "borderless=%d\n",
        window, windowStruct->x, windowStruct->y, hostX, hostY, windowStruct->w,
        windowStruct->h, (flags & SDL_WINDOW_BORDERLESS) != 0);
    compatTrace(
        "realizeTopLevelWindow: window=%lu requestedSize=(%ux%u) "
        "globalScale=%.3f\n",
        window, windowStruct->w, windowStruct->h, compatGlobalHiDpiScale());
    int createW = (int) windowStruct->w;
    int createH = (int) windowStruct->h;
    SDL_Window *sdlWindow = NULL;

#ifdef __EMSCRIPTEN__
    if (SCREEN_WINDOW != None) {
        WindowStruct *screen = GET_WINDOW_STRUCT(SCREEN_WINDOW);
        Window *children = GET_CHILDREN(SCREEN_WINDOW);
        for (size_t i = 0; i < screen->children.length; i++) {
            Window sibling = children[i];
            if (sibling == window || !IS_TYPE(sibling, WINDOW))
                continue;
            WindowStruct *sws = GET_WINDOW_STRUCT(sibling);
            if (sws && sws->sdlWindow && !sws->borrowedSdlWindow &&
                sws->mapState == Mapped && !sws->overrideRedirect) {
                windowStruct->sdlWindow = sws->sdlWindow;
                windowStruct->borrowedSdlWindow = True;
                windowStruct->hiDpiScaleX = sws->hiDpiScaleX;
                windowStruct->hiDpiScaleY = sws->hiDpiScaleY;
                windowStruct->hiDpiPromoted = False;
                windowStruct->presentUsesSoftware = True;
                windowStruct->needsPresent = True;
                windowStruct->hasPresented = False;
                (void) getWindowRenderer(window);
                return True;
            }
        }
    }
#endif

#ifdef LIBX11_COMPAT_SDL3
    /* A Wayland client cannot self-position a top-level, so an
     * override-redirect popup created as an ordinary window floats wherever the
     * compositor drops it, detached from the menu bar that spawned it. When a
     * mapped parent contains the popup's anchor, create it as an xdg_popup
     * anchored to that parent at a relative offset instead, so it lands where
     * the toolkit asked. BORDERLESS/ALWAYS_ON_TOP are implied by and invalid
     * for a popup, so drop them; keep HIDDEN, high-density, and input-focus
     * intent from flags.
     */
    if (windowStruct->overrideRedirect) {
        int offsetX = 0, offsetY = 0;
        Window popupParent =
            findPopupParentToplevel(window, &offsetX, &offsetY);
        WindowStruct *parentStruct =
            popupParent != None ? GET_WINDOW_STRUCT(popupParent) : NULL;
        if (parentStruct && parentStruct->sdlWindow) {
            Uint64 popupFlags =
                SDL_WINDOW_POPUP_MENU |
                (flags & ~((Uint64) (SDL_WINDOW_BORDERLESS |
                                     SDL_WINDOW_ALWAYS_ON_TOP)));
            sdlWindow =
                xc_CreatePopupWindow(parentStruct->sdlWindow, offsetX, offsetY,
                                     createW, createH, popupFlags);
            if (sdlWindow) {
                windowStruct->popupParent = popupParent;
                windowStruct->popupAnchorX = windowStruct->x;
                windowStruct->popupAnchorY = windowStruct->y;
                LOG("realizeTopLevelWindow: window=%lu popup parent=%lu "
                    "offset=(%d,%d)\n",
                    window, popupParent, offsetX, offsetY);
            } else {
                LOG("SDL_CreatePopupWindow failed for %lu: %s; using "
                    "absolute top-level\n",
                    window, SDL_GetError());
            }
        }
    }
#endif
    if (!sdlWindow)
        sdlWindow = SDL_CreateWindow(windowStruct->windowName, hostX, hostY,
                                     createW, createH, flags);
    if (!sdlWindow) {
        LOG("SDL_CreateWindow failed in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, None, 0, BadMatch, 0);
        return False;
    }

    registerWindowMapping(window, SDL_GetWindowID(sdlWindow));
    windowStruct->sdlWindow = sdlWindow;
    windowStruct->borrowedSdlWindow = False;
    windowStruct->hiDpiPromoted = promoteHiDpi;

    /* Decide the present path for this window's lifetime, before anything calls
     * SDL_GetWindowSurface: SDL_GetWindowSurface binds a hidden framebuffer
     * renderer to the window and SDL_CreateRenderer then fails ("renderer
     * already associated"), so the accelerated renderer must be created first.
     * On success the window presents through presentRenderer for its whole
     * life; if creation fails (dummy/headless driver) or the kill switch forces
     * it, the window keeps the SDL_GetWindowSurface software path unchanged.
     *
     * Some drivers still hand back a (software) renderer whose
     * SDL_RenderReadPixels cannot read the backing (SDL2's dummy driver on
     * headless CI), so the accelerated readback path would never present and
     * hasPresented would stay False. libx11CompatAcceleratedPresentUsable
     * probes the readback capability once; when it reports the pipeline is not
     * usable, force the software present path so headless/CI runs behave like a
     * real driver.
     */
    if (!libx11CompatAcceleratedPresentForcedForTest() &&
        (libx11CompatForceSoftwarePresent() ||
         !libx11CompatAcceleratedPresentUsable())) {
        windowStruct->presentUsesSoftware = True;
    } else {
        windowStruct->presentRenderer =
            xc_CreateRenderer(sdlWindow, XC_RENDERER_ACCELERATED);
        if (windowStruct->presentRenderer) {
            SDL_RenderSetScale(windowStruct->presentRenderer, 1.0f, 1.0f);
            SDL_RenderSetViewport(windowStruct->presentRenderer, NULL);
            SDL_RenderSetClipRect(windowStruct->presentRenderer, NULL);
            windowStruct->presentUsesSoftware = False;
        } else {
            LOG("xc_CreateRenderer(accelerated) failed for window %lu: %s; "
                "using software present\n",
                window, SDL_GetError());
            windowStruct->presentUsesSoftware = True;
        }
    }

    /* A parent-anchored popup is always on top of its parent by construction;
     * always-on-top is invalid for an xdg_popup and was masked out of its
     * create flags, so only force it on an absolutely positioned
     * override-redirect top-level.
     */
    if (windowStruct->overrideRedirect && windowStruct->popupParent == None) {
#if SDL_VERSION_ATLEAST(2, 0, 16)
        SDL_SetWindowAlwaysOnTop(sdlWindow, SDL_TRUE);
#endif
    }
    windowStruct->needsPresent = True;
    windowStruct->hasPresented = False;
    if (windowStruct->cursor != None)
        XDefineCursor(display, window, windowStruct->cursor);
    if (windowStruct->icon)
        SDL_SetWindowIcon(windowStruct->sdlWindow, windowStruct->icon);

    /* Option 1b: promote X11 geometry to physical pixels. SDL created the
     * window from logical points; on Retina the actual backing is 2x. The
     * physical size is the size drawWindowDataToScreen presents against:
     * SDL_GetRendererOutputSize for accelerated windows (the renderer
     * drawable), or the SDL window surface for software windows (which cannot
     * use a renderer). This must happen before getWindowRenderer creates the X
     * backing texture; otherwise popup/dialog backing starts at 1x and only
     * later gets stretched to the physical window. Falls through as a no-op
     * when the query fails or the host is non-HiDPI (ratio 1.0, e.g. the CI
     * dummy driver). Self-scaling toolkits still record the ratio for the
     * present path but leave X11 geometry logical.
     */
    {
        int physW = 0, physH = 0;
        int logicalW = 0, logicalH = 0;
        Bool haveSize = False;
        SDL_GetWindowSize(sdlWindow, &logicalW, &logicalH);

        /* Prefer SDL_GetWindowSizeInPixels: it reports the window's physical
         * backing size directly and does not depend on a renderer or the
         * SDL_GetWindowSurface software binding. On sdl2-compat over SDL3 the
         * SDL_GetRendererOutputSize thunk fills the size correctly but can
         * return nonzero on a still-hidden window, which would skip the
         * promotion and leave xwpe reading the logical (1x) size from
         * XGetWindowAttributes -- the initial File Manager dialog then comes up
         * at the wrong grid size. Treat any query that yields a sane (>0) size
         * as usable regardless of its int return code, and fall back through
         * the renderer / window-surface paths so older SDL and the CI dummy
         * driver keep working.
         */
        if (compatSdlHasWindowSizeInPixels()) {
#if SDL_VERSION_ATLEAST(2, 26, 0)
            SDL_GetWindowSizeInPixels(sdlWindow, &physW, &physH);
            if (physW > 0 && physH > 0)
                haveSize = True;
#endif
        }
        if (!haveSize && !windowStruct->presentUsesSoftware &&
            windowStruct->presentRenderer) {
            SDL_GetRendererOutputSize(windowStruct->presentRenderer, &physW,
                                      &physH);
            if (physW > 0 && physH > 0)
                haveSize = True;
        }
        if (!haveSize && windowStruct->presentUsesSoftware) {
            /* Software-present windows have no accelerated renderer, so the
             * SDL_GetWindowSurface binding is safe here. Skip it for
             * accelerated windows: it would bind a framebuffer renderer that
             * collides with presentRenderer.
             */
            SDL_Surface *winSurface = SDL_GetWindowSurface(sdlWindow);
            if (winSurface) {
                physW = winSurface->w;
                physH = winSurface->h;
                haveSize = (physW > 0 && physH > 0);
            }
        }
        if (haveSize && physW > 0 && physH > 0 && logicalW > 0 &&
            logicalH > 0) {
            windowStruct->hiDpiScaleX = (double) physW / (double) logicalW;
            windowStruct->hiDpiScaleY = (double) physH / (double) logicalH;

            /* The font engine scaled glyphs and metrics by the display-global
             * HiDPI factor probed at XOpenDisplay. That factor and this
             * per-window ratio come from the same host display, so on a
             * single-scale setup they agree and the grid math lines up
             * (promoted pixel geometry / scaled font size == the original
             * grid). A divergence would only arise across monitors with
             * different scales, which the font size (loaded once, globally)
             * cannot track; surface this so such a mismatch is diagnosable.
             */
            LOG("realizeTopLevelWindow: window=%lu surfaceScale=(%.3f,%.3f) "
                "globalFontScale=%.3f\n",
                window, windowStruct->hiDpiScaleX, windowStruct->hiDpiScaleY,
                compatGlobalHiDpiScale());
            compatTrace(
                "realizeTopLevelWindow: window=%lu phys=(%dx%d) "
                "requested=(%ux%u) surfaceScale=(%.3f,%.3f) globalScale=%.3f\n",
                window, physW, physH, windowStruct->w, windowStruct->h,
                windowStruct->hiDpiScaleX, windowStruct->hiDpiScaleY,
                compatGlobalHiDpiScale());
            if (promoteHiDpi && (physW != (int) windowStruct->w ||
                                 physH != (int) windowStruct->h)) {
                windowStruct->w = (unsigned int) physW;
                windowStruct->h = (unsigned int) physH;

                /* A parent-anchored popup owns no absolute host position; its
                 * offset is relative to the parent surface, so leave it alone.
                 */
                if (windowStruct->overrideRedirect &&
                    windowStruct->popupParent == None) {
                    int scaledHostX = windowStruct->x;
                    int scaledHostY = windowStruct->y;
                    topLevelWindowHostPosition(window, windowStruct->x,
                                               windowStruct->y, &scaledHostX,
                                               &scaledHostY);
                    compatTrace(
                        "realizeTopLevelWindow: window=%lu reposition "
                        "override host=(%d,%d)\n",
                        window, scaledHostX, scaledHostY);
                    SDL_SetWindowPosition(sdlWindow, scaledHostX, scaledHostY);
                }
                if (windowStruct->sdlTexture)
                    resizeWindowTexture(window);
                compatTrace(
                    "realizeTopLevelWindow: window=%lu PROMOTED to "
                    "(%ux%u)\n",
                    window, windowStruct->w, windowStruct->h);
            }
        }
    }

    /* Prime the X backing store while the native window is still hidden.
     * XMapWindow will present and show it after map/expose state is ready.
     */
    (void) getWindowRenderer(window);

    return True;
}

static void replayDeferredWmProperties(Display *display, Window window)
{
    /* Re-apply deferred WM hints after the top-level transition has made the
     * window mapped. Motif and Xt write WM_TRANSIENT_FOR / _MOTIF_WM_HINTS
     * during widget realization, well before the shell turns into a real native
     * window. The WM helpers intentionally no-op until the top-level window is
     * both realized and mapped.
     */
    applyMotifWmHintsFromProperty(window);
    applyNormalHintsResizableFromProperty(window);
    applyNetWmStateFromProperty(window);
    applyTransientForRelationship(window);

    /* This window may itself be the deferred parent for one or more
     * earlier-realized modal children. Walk SCREEN_WINDOW's top-level siblings
     * and re-resolve any whose deferredTransientParent points here so the modal
     * pairing finally binds.
     */
    if (SCREEN_WINDOW != None && IS_TYPE(SCREEN_WINDOW, WINDOW)) {
        Window *siblings = GET_CHILDREN(SCREEN_WINDOW);
        size_t count = GET_WINDOW_STRUCT(SCREEN_WINDOW)->children.length;
        for (size_t i = 0; i < count; i++) {
            Window sibling = siblings[i];
            if (sibling == window || !IS_TYPE(sibling, WINDOW))
                continue;

            if (GET_WINDOW_STRUCT(sibling)->deferredTransientParent == window)
                applyTransientForRelationship(sibling);
        }
    }
}

/* Tear down every SDL3 xdg_popup anchored to window before its SDL surface is
 * destroyed, so nothing is stranded with a dangling child surface. A still
 * mapped popup is unmapped through the full path (grab release, mapState flip,
 * UnmapNotify), which unrealizes it. But a popup already flipped to UnMapped by
 * an SDL hide event still holds its anchored sdlWindow, and unmapWindowInternal
 * returns early for an already-unmapped window without unrealizing it, so force
 * the unrealize whenever a collected child still owns an sdlWindow. That both
 * frees the surface before the parent's is destroyed and guarantees progress:
 * every collected child ends with sdlWindow cleared, so collectPopupChildren
 * stops matching it and the loop converges. Recursion through unmap/unrealize
 * drains cascade submenus. Collect first, then tear down outside the
 * mapping-list lock collectPopupChildren holds, draining in batches so the
 * fixed snapshot is not a teardown cap.
 */
void drainAnchoredPopups(Display *display, Window window)
{
    Window popupChildren[32];
    size_t popupChildCount;
    do {
        popupChildCount = collectPopupChildren(
            window, popupChildren,
            sizeof popupChildren / sizeof popupChildren[0]);
        for (size_t i = 0; i < popupChildCount; i++) {
            unmapWindowInternal(display, popupChildren[i], False);
            WindowStruct *ws = GET_WINDOW_STRUCT(popupChildren[i]);
            if (ws && ws->sdlWindow)
                unrealizeTopLevelWindow(display, popupChildren[i]);
        }
    } while (popupChildCount > 0);
}

/* Re-run an anchored popup's xdg_positioner against its parent so the
 * compositor recomputes the popup's geometry from the window's current size and
 * relative offset. Used after a resize (SDL_SetWindowSize grows only the
 * surface buffer, not the immutable popup geometry) so a menu resized while
 * mapped stops rendering clipped to its creation rectangle. The offset is the
 * same parent-relative delta the realize and move paths use; both windows carry
 * X11 logical coordinates, so their difference is the intended offset.
 */
void repositionAnchoredPopup(Window window)
{
    WindowStruct *ws = GET_WINDOW_STRUCT(window);
    if (!ws || !ws->sdlWindow || ws->popupParent == None)
        return;
    WindowStruct *parent = GET_WINDOW_STRUCT(ws->popupParent);
    if (!parent || !parent->sdlWindow)
        return;

    /* Widen to int64 before the subtraction so a hostile anchor cannot wrap the
     * parent-relative offset, matching findPopupParentToplevel and the geometry
     * math in window-internal.c.
     */
    int64_t relX = (int64_t) ws->popupAnchorX - popupAnchorOriginX(parent);
    int64_t relY = (int64_t) ws->popupAnchorY - popupAnchorOriginY(parent);
    SDL_SetWindowPosition(ws->sdlWindow, clampToIntRange(relX),
                          clampToIntRange(relY));
}

void unrealizeTopLevelWindow(Display *display, Window window)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!windowStruct->sdlWindow)
        return;

    if (windowStruct->borrowedSdlWindow) {
        windowStruct->popupParent = None;
        windowStruct->popupAnchorX = windowStruct->x;
        windowStruct->popupAnchorY = windowStruct->y;
        drainAnchoredPopups(display, window);
        windowStruct->sdlWindow = NULL;
        windowStruct->borrowedSdlWindow = False;
        windowStruct->needsPresent = False;
        windowStruct->hasPresented = False;
        return;
    }

    /* Clear this window's own anchor before draining its popup children. The
     * anchor is recomputed at each realize, so a window remapped later must not
     * carry a stale parent; clearing it here also breaks any anchor cycle (a
     * remapped popup anchored to a peer that anchors back) so the recursive
     * drain below cannot loop forever.
     */
    windowStruct->popupParent = None;
    windowStruct->popupAnchorX = windowStruct->x;
    windowStruct->popupAnchorY = windowStruct->y;
    drainAnchoredPopups(display, window);

    /* If this top-level was the replay/XTest injection target, retire the
     * cached ID so the next mapped shell can take its place.
     */
    Uint32 destroyedId = SDL_GetWindowID(windowStruct->sdlWindow);
    replayTargetForgetWindow(destroyedId);

    deleteWindowMapping(window);

    /* Drop any cached GLX/EGL surface bound to this window before its SDL
     * window (and, on Linux, the native X11 window behind it) is destroyed, so
     * an EGL window surface never outlives the window it renders to. A later
     * remap rebuilds the surface. The hook is NULL when the GLX layer is not
     * loaded.
     */
    extern void (*glxDrawableDestroyedHook)(Window drawable);
    if (glxDrawableDestroyedHook)
        glxDrawableDestroyedHook(window);

    /* Tear down the GLX Metal view (if any) before its SDL window, and clear
     * the cache so a later remap creates a fresh one instead of reusing a stale
     * view. Metal surface is macOS + SDL 2.0.12; elsewhere glxMetalView stays
     * NULL. The guard matches src/glx.c and src/wrapper/sdl-wrapper.c.
     */
#if defined(__APPLE__) && SDL_VERSION_ATLEAST(2, 0, 12)
    if (windowStruct->glxMetalView) {
        SDL_Metal_DestroyView(windowStruct->glxMetalView);
        windowStruct->glxMetalView = NULL;
    }
#endif

    /* Tear down the per-window accelerated present resources before the SDL
     * window they are bound to. realizeTopLevelWindow rebuilds presentRenderer
     * on the next map, so leaving these live would leak the renderer and tie
     * its streaming texture to a destroyed SDL_Window. Texture first (owned by
     * the renderer), then the renderer, then the CPU readback scratch.
     */
    if (windowStruct->presentTexture) {
        SDL_DestroyTexture(windowStruct->presentTexture);
        windowStruct->presentTexture = NULL;
    }
    if (windowStruct->presentRenderer) {
        SDL_DestroyRenderer(windowStruct->presentRenderer);
        windowStruct->presentRenderer = NULL;
    }
    free(windowStruct->presentReadback);
    windowStruct->presentReadback = NULL;
    windowStruct->presentReadbackCap = 0;
    windowStruct->presentTexW = 0;
    windowStruct->presentTexH = 0;
    windowStruct->presentUsesSoftware = False;
    SDL_DestroyWindow(windowStruct->sdlWindow);
    windowStruct->sdlWindow = NULL;
    windowStruct->needsPresent = False;
    windowStruct->hasPresented = False;

    /* Demote the promoted physical-pixel geometry back to logical points before
     * the SDL window is gone. realizeTopLevelWindow creates the next SDL window
     * from ws->w/h as *logical* dimensions and then re-promotes to pixels;
     * leaving the physical size here would feed physical dims in as logical on
     * remap and grow the window by the HiDPI factor on every unmap/map cycle.
     * The next realize re-derives the scale, so reset it to 1.0. No-op at scale
     * 1.0 (Linux / CI dummy).
     */
    if (windowStruct->hiDpiPromoted && windowStruct->hiDpiScaleX > 0.0 &&
        windowStruct->hiDpiScaleY > 0.0 &&
        (windowStruct->hiDpiScaleX != 1.0 ||
         windowStruct->hiDpiScaleY != 1.0)) {
        windowStruct->w = (unsigned int) lround((double) windowStruct->w /
                                                windowStruct->hiDpiScaleX);
        windowStruct->h = (unsigned int) lround((double) windowStruct->h /
                                                windowStruct->hiDpiScaleY);
    }
    windowStruct->hiDpiScaleX = 1.0;
    windowStruct->hiDpiScaleY = 1.0;
    windowStruct->hiDpiPromoted = False;
}

Window XCreateSimpleWindow(Display *display,
                           Window parent,
                           int x,
                           int y,
                           unsigned int width,
                           unsigned int height,
                           unsigned int border_width,
                           unsigned long border,
                           unsigned long background)
{
    XSetWindowAttributes attributes;
    attributes.border_pixel = border;
    attributes.background_pixel = background;
    return XCreateWindow(display, parent, x, y, width, height, border_width,
                         CopyFromParent, CopyFromParent, CopyFromParent,
                         CWBackPixel | CWBorderPixel, &attributes);
}

static Window createWindowImpl(Display *display,
                               Window parent,
                               int x,
                               int y,
                               unsigned int width,
                               unsigned int height,
                               unsigned int border_width,
                               int depth,
                               unsigned int clazz,
                               Visual *visual,
                               unsigned long valueMask,
                               XSetWindowAttributes *attributes,
                               Bool internalWindow)
{
    // https://tronche.com/gui/x/xlib/window/XCreateWindow.html
    SET_X_SERVER_REQUEST(display, X_CreateWindow);
    TYPE_CHECK(parent, WINDOW, display, None);
    if (valueMask != 0 && !attributes) {
        handleError(0, display, None, 0, BadValue, 0);
        return None;
    }

    Bool inputOnly = (clazz == InputOnly ||
                      (clazz == CopyFromParent && IS_INPUT_ONLY(parent)));
    if (inputOnly && border_width != 0) {
        LOG("Bad argument: Given class is InputOnly but border_with is not 0 "
            "in XCreateWindow!\n");
        handleError(0, display, None, 0, BadMatch, 0);
        return None;
    }

    Window windowID = ALLOC_XID();
    if (windowID == None) {
        LOG("Out of memory: Could not allocate the window id in "
            "XCreateWindow!\n");
        handleOutOfMemory(0, display, 0, 0);
        return None;
    }

    WindowStruct *windowStruct = malloc(sizeof(WindowStruct));
    if (!windowStruct) {
        LOG("Out of memory: Could not allocate the window struct in "
            "XCreateWindow!\n");
        handleOutOfMemory(0, display, 0, 0);
        FREE_XID(windowID);
        return None;
    }

    SET_XID_TYPE(windowID, WINDOW);
    SET_XID_VALUE(windowID, windowStruct);
    initWindowStruct(windowStruct, x, y, width, height, visual, None, inputOnly,
                     0, None);
    windowStruct->internal = internalWindow;
    compatTrace(
        "XCreateWindow: id=%lu parent=%lu topLevel=%d req=(%d,%d %ux%u) "
        "globalScale=%.3f\n",
        windowID, parent, parent == SCREEN_WINDOW, x, y, width, height,
        compatGlobalHiDpiScale());
    windowStruct->depth = depth;
    windowStruct->borderWidth = border_width;
    if (!addChildToWindow(parent, windowID)) {
        LOG("Out of memory: Could not increase size of parent's child list in "
            "XCreateWindow!\n");
        handleOutOfMemory(0, display, 0, 0);
        free(windowStruct);
        FREE_XID(windowID);
        return None;
    }
    if (visual == CopyFromParent)
        visual = getDefaultVisual(0);
    if (!visual) {
        handleError(0, display, None, 0, BadMatch, 0);
        removeChildFromParent(windowID);
        free(windowStruct);
        FREE_XID(windowID);
        return None;
    }

    int visualClass = visual->CLASS_ATTRIBUTE;
    windowStruct->colormap = (Colormap) XCreateColormap(
        display, windowID, visual,
        visualClass == StaticGray || visualClass == StaticColor ||
                visualClass == TrueColor
            ? AllocNone
            : AllocAll);
    SET_X_SERVER_REQUEST(display, X_CreateWindow);
    if (windowStruct->colormap == None) {
        LOG("Out of memory: Could not allocate the window colormap in "
            "XCreateWindow!\n");
        handleOutOfMemory(0, display, 0, 0);
        removeChildFromParent(windowID);
        free(windowStruct);
        FREE_XID(windowID);
        return None;
    }

    /* Register event masks before CreateNotify so interested clients can
     * observe the window with its initial selection state.
     */
    if (HAS_VALUE(valueMask, CWEventMask))
        windowStruct->eventMask = attributes->event_mask;
    if (!windowStruct->internal)
        postEvent(display, windowID, CreateNotify);
    if (valueMask != 0)
        XChangeWindowAttributes(display, windowID, valueMask, attributes);
    return windowID;
}

Window XCreateWindow(Display *display,
                     Window parent,
                     int x,
                     int y,
                     unsigned int width,
                     unsigned int height,
                     unsigned int border_width,
                     int depth,
                     unsigned int clazz,
                     Visual *visual,
                     unsigned long valueMask,
                     XSetWindowAttributes *attributes)
{
    return createWindowImpl(display, parent, x, y, width, height, border_width,
                            depth, clazz, visual, valueMask, attributes, False);
}

Window createInternalWindow(Display *display,
                            Window parent,
                            int x,
                            int y,
                            unsigned int width,
                            unsigned int height,
                            unsigned int border_width,
                            int depth,
                            unsigned int clazz,
                            Visual *visual,
                            unsigned long valueMask,
                            XSetWindowAttributes *attributes)
{
    return createWindowImpl(display, parent, x, y, width, height, border_width,
                            depth, clazz, visual, valueMask, attributes, True);
}

static int destroySubwindowsImpl(Display *display, Window window)
{
    SET_X_SERVER_REQUEST(display, X_DestroyWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    while (GET_WINDOW_STRUCT(window)->children.length > 0) {
        Window child = GET_CHILDREN(
            window)[GET_WINDOW_STRUCT(window)->children.length - 1];
        destroyWindow(display, child, True);
    }
    return 1;
}

static void destroySubwindowsOnMain(void *p)
{
    struct destroyArgs *a = (struct destroyArgs *) p;
    a->rc = destroySubwindowsImpl(a->display, a->window);
}

int XDestroySubwindows(Display *display, Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        struct destroyArgs a = {display, window, 0};
        runOnMainThread(destroySubwindowsOnMain, &a);
        return a.rc;
    }
    return destroySubwindowsImpl(display, window);
}

/* Off-main-thread router for the configure choke. configureWindow touches SDL
 * (SDL_SetWindowPosition/Size) and pumps, so a worker thread runs it on the
 * main thread. XConfigureWindow / XMoveWindow / XResizeWindow all funnel here;
 * XMoveResizeWindow inherits via XMoveWindow + XResizeWindow. Internal
 * event-driven callers already run on the main thread and call configureWindow
 * directly. The XWindowChanges pointer lives on the caller's stack, valid for
 * the whole exchange because the worker blocks until the handoff completes.
 */
struct configureArgs {
    Display *display;
    Window window;
    unsigned int mask;
    XWindowChanges *values;
    Bool rc;
};

static void configureOnMain(void *p)
{
    struct configureArgs *a = (struct configureArgs *) p;
    a->rc = configureWindow(a->display, a->window, a->mask, a->values);
}

static Bool configureWindowRouted(Display *display,
                                  Window window,
                                  unsigned int mask,
                                  XWindowChanges *values)
{
    if (!libx11CompatOnMainEventThread()) {
        struct configureArgs a = {display, window, mask, values, False};
        runOnMainThread(configureOnMain, &a);
        return a.rc;
    }
    return configureWindow(display, window, mask, values);
}

int XConfigureWindow(Display *display,
                     Window window,
                     unsigned int value_mask,
                     XWindowChanges *values)
{
    // https://tronche.com/gui/x/xlib/window/XConfigureWindow.html
    /* TYPE_CHECK runs inside configureWindow on the main thread (see there),
     * not here on a possibly-worker thread.
     */
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    return configureWindowRouted(display, window, value_mask, values);
}

Status XReconfigureWMWindow(Display *display,
                            Window window,
                            int screen_number,
                            unsigned int value_mask,
                            XWindowChanges *values)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XReconfigureWMWindow.html
    XConfigureWindow(display, window, value_mask, values);
    return 1;
}

static Status iconifyWindowImpl(Display *display,
                                Window window,
                                int screen_number)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XIconifyWindow.html
    (void) screen_number;
    TYPE_CHECK(window, WINDOW, display, 0);
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
        requireMainEventThread("XIconifyWindow");
        SDL_MinimizeWindow(GET_WINDOW_STRUCT(window)->sdlWindow);
    }
    return 1;
}

/* Off-main-thread router: XIconifyWindow calls SDL_MinimizeWindow. */
struct iconifyArgs {
    Display *display;
    Window window;
    int screen_number;
    Status rc;
};

static void iconifyOnMain(void *p)
{
    struct iconifyArgs *a = (struct iconifyArgs *) p;
    a->rc = iconifyWindowImpl(a->display, a->window, a->screen_number);
}

Status XIconifyWindow(Display *display, Window window, int screen_number)
{
    if (!libx11CompatOnMainEventThread()) {
        struct iconifyArgs a = {display, window, screen_number, 0};
        runOnMainThread(iconifyOnMain, &a);
        return a.rc;
    }
    return iconifyWindowImpl(display, window, screen_number);
}

Status XSetWMColormapWindows(Display *display,
                             Window window,
                             Window *colormap_windows,
                             int count)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XSetWMColormapWindows.html
    TYPE_CHECK(window, WINDOW, display, 0);
    if (count < 0 || (count > 0 && !colormap_windows)) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }

    for (int i = 0; i < count; i++)
        TYPE_CHECK(colormap_windows[i], WINDOW, display, 0);
    Window *copy = NULL;
    if (count > 0) {
        copy = malloc(sizeof(Window) * (size_t) count);
        if (!copy) {
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        memcpy(copy, colormap_windows, sizeof(Window) * (size_t) count);
    }

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    free(windowStruct->colormapWindows);
    windowStruct->colormapWindowsCount = count;
    windowStruct->colormapWindows = copy;
    return 1;
}

Status XGetWMColormapWindows(Display *display,
                             Window window,
                             Window **colormap_windows_return,
                             int *count_return)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XGetWMColormapWindows.html
    TYPE_CHECK(window, WINDOW, display, 0);
    if (!count_return || !colormap_windows_return)
        return 0;
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (windowStruct->colormapWindowsCount > -1) {
        *count_return = windowStruct->colormapWindowsCount;
        if (windowStruct->colormapWindowsCount == 0) {
            *colormap_windows_return = NULL;
            return 1;
        }
        *colormap_windows_return = malloc(
            sizeof(Window) * (size_t) windowStruct->colormapWindowsCount);
        if (!*colormap_windows_return) {
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        memcpy(*colormap_windows_return, windowStruct->colormapWindows,
               sizeof(Window) * (size_t) windowStruct->colormapWindowsCount);
        return 1;
    }
    return 0;
}

int XSetTransientForHint(Display *display, Window window, Window prop_window)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XSetTransientForHint.html
    return XChangeProperty(display, window, XA_WM_TRANSIENT_FOR, XA_WINDOW, 32,
                           PropModeReplace, (unsigned char *) &prop_window, 1);
}

int XMapRaised(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XMapRaised.html
    TYPE_CHECK(window, WINDOW, display, 0);
    int result = XMapWindow(display, window);
    XRaiseWindow(display, window);
    return result;
}

/* Enforce the "SDL work runs on the main event thread" invariant
 * unconditionally (not via assert, which NDEBUG would compile out, turning a
 * controlled failure into a silent off-thread SDL crash on macOS). A trip means
 * an SDL-touching entry point reached a worker thread without routing through
 * runOnMainThread.
 */
void requireMainEventThread(const char *who)
{
    if (!libx11CompatOnMainEventThread()) {
        LOG("%s: SDL work reached a non-main thread; aborting instead of "
            "crashing off-main\n",
            who);
        abort();
    }
}

static int mapWindowImpl(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XMapWindow.html
    /* Creates/shows/raises SDL windows and calls drawWindowDataToScreen, all
     * main-thread-only on macOS; the router below guarantees we are on the main
     * thread here.
     */
    requireMainEventThread("XMapWindow");
    SET_X_SERVER_REQUEST(display, X_MapWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (GET_WINDOW_STRUCT(window)->mapState == Mapped)
        return 1;

    /* Map adds this window to the occlusion graph; lower siblings now see less.
     * Mark stale before the actual map so neither branch needs to remember.
     */
    invalidateVisibleRegionForTopLevel(window);

    /* libx11-compat has no separate window-manager client to service
     * SubstructureRedirect requests. Some Motif paths select redirect-style
     * masks internally; stopping at MapRequest would leave top-level shells
     * from ever becoming SDL windows. Keep mapping in-process.
     */
    if (IS_TOP_LEVEL(window)) {
        if (IS_MAPPED_TOP_LEVEL_WINDOW(window))
            return 1;

        LOG("Mapping Window %lu\n", window);
        WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
        if (!realizeTopLevelWindow(display, window))
            return 0;

        /* Every window draws on the SCREEN renderer with a per-window backing
         * texture as render target. The unmapped state already holds that
         * texture in windowStruct->sdlTexture, so transitioning to mapped is
         * just associating the SDL_Window; the texture is unchanged and stays
         * on the same renderer. Presentation to the actual SDL_Window happens
         * later in drawWindowDataToScreen.
         */
        windowStruct->mapState = Mapped;

        /* On macOS a command-line launched SDL app does not get keyboard focus
         * by default even with SDL_WINDOW_INPUT_FOCUS requested, so keystrokes
         * stay in the terminal. Real X11's MapWindow puts the window on screen;
         * raising it here matches user expectation.
         */
        if (windowStruct->windowName) {
            free(windowStruct->windowName);
            windowStruct->windowName = NULL;
        }
        if ((windowStruct->eventMask & KeyPressMask ||
             windowStruct->eventMask & KeyReleaseMask) &&
            !inputMethodHasFocusedIC()) {
            /* Keep SDL text input off so KeyPress events are not doubled by
             * SDL_TEXTINPUT. No setKeyboardFocus here: focus is owned by
             * XSetInputFocus and its callers; XMapWindow auto-focus stole focus
             * from the active Motif dialog and prevented popup shells from
             * finishing their map sequence.
             */
            inputMethodNoteTextInputStopped();
            SDL_StopTextInput();
        }

        /* First top-level mapping is the canonical "window is up" signal for an
         * external test script. Snapshot the SDL window ID now, on the main
         * thread, so XTest's off-thread injection has a stable target without
         * scanning the live window tree. Then arm the replay engine.
         */
        offerReplayTargetForWindow(window);
        replayStartIfRequested(display);
    } else { /* Mapping a window that is not a top level window  */
        Window parent = GET_PARENT(window);
        if (GET_WINDOW_STRUCT(parent)->mapState == Mapped) {
            if (!mergeWindowDrawables(parent, window)) {
                LOG("Failed to merge the window renderer in %s: %s\n", __func__,
                    SDL_GetError());
                return 0;
            }
            WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
            windowStruct->mapState = Mapped;
            if (!windowStruct->inputOnly)
                XClearArea(display, window, 0, 0, 0, 0, False);
            windowStruct->contentsMergedToParent = False;
            replayStartIfRequested(display);
        } else { /* Parent not mapped */
            GET_WINDOW_STRUCT(window)->mapState = MapRequested;
            return 1;
        }
    }

    postEvent(display, window, MapNotify);
    postVisibilityForWindowAndSiblings(display, window);
    mapRequestedChildren(display, window);

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);

    /* Under a real window manager the post-map ConfigureNotify carries a
     * WM-adjusted geometry that differs from what the shell requested, and Xt
     * uses that size change to run its post-realize layout pass, reconfiguring
     * managed children and firing their resize procs. An embedded
     * GLwDrawingArea only gets its initial XmNresizeCallback (where GL clients
     * set glViewport/glFrustum) from that pass. With no window manager under
     * SDL it never happens, so a GL widget renders with no viewport. Emulate it
     * with a one-pixel size wobble on the shell: Xt ignores a same-size
     * configure, so report width-1 then the true width. Both configures precede
     * the Expose below, so no frame is drawn at the interim size (no flicker);
     * the flexible drawing-area child absorbs the delta so the change reaches
     * it. Top-level windows only; children are sized by their own managers. The
     * event size is snapshotted from the struct at post time (see
     * ConfigureNotify in src/events.c), hence the temporary size swap.
     */
    if (IS_TOP_LEVEL(window) && windowStruct->w > 1 && windowStruct->h > 1) {
        unsigned int realW = windowStruct->w, realH = windowStruct->h;
        windowStruct->w = realW - 1;
        windowStruct->h = realH - 1;
        postEvent(display, window, ConfigureNotify);
        windowStruct->w = realW;
        windowStruct->h = realH;
        postEvent(display, window, ConfigureNotify);
    }
    if (!windowStruct->inputOnly) {
        SDL_Rect exposeRect = {0, 0, windowStruct->w, windowStruct->h};
        postExposeEvent(display, window, &exposeRect, 1);
    }
    if (windowStruct->sdlWindow) {
        /* Render first into the hidden SDL_Window so the user never sees an
         * empty frame, then show + raise.
         */
        showTopLevelWindow(display, window, True);
    }

    // SDL_UpdateWindowSurface(GET_WINDOW_STRUCT(window)->sdlWindow);

#ifdef DEBUG_WINDOWS
    printWindowsHierarchy();
#endif
    return 1;
}

/* Off-main-thread router: XMapWindow creates and shows SDL windows and mutates
 * the window tree, so a client worker thread runs the whole body on the main
 * thread. On the main thread it is a direct call. Internal recursive maps
 * (XMapSubwindows, mapRequestedChildren) already run on the main thread and so
 * hit the direct path with no extra round-trip.
 */
struct mapWindowArgs {
    Display *display;
    Window window;
    int rc;
};

static void mapWindowOnMain(void *p)
{
    struct mapWindowArgs *a = (struct mapWindowArgs *) p;
    a->rc = mapWindowImpl(a->display, a->window);
}

int XMapWindow(Display *display, Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        struct mapWindowArgs a = {display, window, 0};
        runOnMainThread(mapWindowOnMain, &a);
        return a.rc;
    }
    return mapWindowImpl(display, window);
}

#ifndef LIBX11_COMPAT_SDL3

/* Mark every mapped top-level other than exclude that overlaps rect for a
 * present. Under SDL2 talking to a backing/remote X server (Xvnc), the
 * cross-window Expose that a real X server sends to windows below a torn-down
 * popup or a moved-away window is not reliably delivered to a sibling
 * top-level's SDL event queue, so the vacated region keeps the old pixels as
 * residue. Forcing a present makes those siblings redraw. Root children are the
 * top-levels, and rect plus each sibling rect share the same X11 logical space,
 * so a plain rectangle overlap decides who needs it. Not needed on SDL3, where
 * the Wayland compositor repaints below a dismissed popup on its own.
 */
void repaintTopLevelsOverlappingRect(Window exclude, SDL_Rect rect)
{
    /* Walks the screen child list and marks siblings for present; the callers
     * (unmap/destroy/move teardown) all run on the main event thread, and
     * markWindowNeedsPresent touches SDL. Assert the invariant so a future
     * off-main caller fails loudly instead of racing the child list.
     */
    requireMainEventThread("repaintTopLevelsOverlappingRect");
    if (SCREEN_WINDOW == None)
        return;
    WindowStruct *screen = GET_WINDOW_STRUCT(SCREEN_WINDOW);
    if (!screen)
        return;
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    for (size_t i = 0; i < screen->children.length; i++) {
        Window w = children[i];

        /* A child torn down mid-teardown lingers in the array typed
         * CLOSED_WINDOW with a NULL struct (destroyScreenWindowImpl destroys
         * the top-levels with freeParentData=False, so they stay listed).
         * Filter it on the type slot before IS_MAPPED_TOP_LEVEL_WINDOW
         * dereferences the struct, matching drawWindowDataToScreen.
         */
        if (w == exclude || !IS_TYPE(w, WINDOW) ||
            !IS_MAPPED_TOP_LEVEL_WINDOW(w))
            continue;
        WindowStruct *ws = GET_WINDOW_STRUCT(w);

        /* Plain rectangle overlap in int64 so a hostile window dimension cannot
         * overflow the signed sum, matching this file's geometry discipline. No
         * SDL_* call here, so no wrapper thunk is needed on the SDL2 backend.
         */
        int64_t sx = ws->x, sy = ws->y, sw = ws->w, sh = ws->h;
        int64_t rx = rect.x, ry = rect.y, rw = rect.w, rh = rect.h;
        if (sw <= 0 || sh <= 0 || rw <= 0 || rh <= 0)
            continue;
        if (rx < sx + sw && rx + rw > sx && ry < sy + sh && ry + rh > sy)
            markWindowNeedsPresent(w);
    }
}
#endif

/* Core unmap bookkeeping shared by XUnmapWindow and the
 * win_gravity=UnmapGravity path. fromConfigure stamps UnmapNotify: true when
 * the unmap is a side effect of a parent resize (UnmapGravity), false for a
 * direct client unmap. This does not touch the request counter, so gravity
 * unmaps generated inside a configure do not masquerade as a separate client
 * X_UnmapWindow request.
 */
void unmapWindowInternal(Display *display, Window window, Bool fromConfigure)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (windowStruct->mapState == UnMapped)
        return;

    windowStruct->mapState = UnMapped;
    flushTextStampsForWindow(window);

    /* The window is now unviewable; drop any active grab it (or an ancestor)
     * held so a Motif menu shell does not keep swallowing pointer input after
     * it unmaps.
     */
    releaseActiveGrabsForUnviewableWindow(display, window);

    /* Unmapping clears this window's contribution to the sibling occlusion
     * graph; lower siblings may now have more visible area. Mark the top-level
     * subtree stale so the next draw recomputes.
     */
    invalidateVisibleRegionForTopLevel(window);
    postVisibilityForWindowAndSiblings(display, window);
    if (windowStruct->sdlWindow) {
        /* SDL_DestroyRenderer / unrealizeTopLevelWindow are main-thread-only on
         * macOS. Guard at the SDL source, not just the XUnmapWindow router: the
         * win_gravity=UnmapGravity configure path also reaches here and does
         * not route (broader off-thread configure is deferred), so this catches
         * it too if it ever runs off-main.
         */
        requireMainEventThread("XUnmapWindow");

        /* Unmap any anchored SDL3 xdg_popup children first, so their pointer
         * grabs are released, their mapState flips to UnMapped, and their
         * UnmapNotify is posted before this window's SDL surface (which owns
         * theirs) is destroyed. Without this, unrealizing the parent would
         * strand a popup logically mapped with no surface, so a later
         * XMapWindow could not recreate it and a live menu grab could keep
         * routing input to an invisible shell.
         */
        drainAnchoredPopups(display, window);

        /* Report the unmap before tearing down the SDL window. A client that
         * selected StructureNotifyMask (Tk wraps every toplevel, and its
         * WaitForEvent blocks up to 2 seconds for the UnmapNotify when it
         * withdraws a window, e.g. closing a menu) otherwise never learns the
         * window went away and stalls on that timeout. The non-SDL child branch
         * below already posts it; the top-level path must too.
         */
        postEvent(display, window, UnmapNotify, fromConfigure);
#ifndef LIBX11_COMPAT_SDL3
        SDL_Rect closedRect = {windowStruct->x, windowStruct->y,
                               clampToIntRange((int64_t) windowStruct->w),
                               clampToIntRange((int64_t) windowStruct->h)};
#endif
        SDL_Renderer *sdlRenderer = windowStruct->sdlRenderer;
        windowStruct->sdlRenderer = NULL;
        destroyWindowRenderer(sdlRenderer);
        unrealizeTopLevelWindow(display, window);
#ifndef LIBX11_COMPAT_SDL3

        /* This top-level's SDL window is gone; repaint whatever it covered so
         * its pixels do not linger on the siblings below. Any top-level counts,
         * not just a menu popup: closing an editor window uncovers the same
         * way, and Xvnc delivers those siblings no Expose either.
         */
        repaintTopLevelsOverlappingRect(window, closedRect);
        drawWindowDataToScreen();
#endif
    } else if (GET_WINDOW_STRUCT(GET_PARENT(window))->mapState != UnMapped) {
        postEvent(display, window, UnmapNotify, fromConfigure);
        SDL_Rect exposeRect = {windowStruct->x, windowStruct->y,
                               windowStruct->w, windowStruct->h};
        XClearArea(display, GET_PARENT(window), exposeRect.x, exposeRect.y,
                   (unsigned int) exposeRect.w, (unsigned int) exposeRect.h,
                   False);
        postExposeEvent(display, GET_PARENT(window), &exposeRect, 1);
    }
}

static int unmapWindowImpl(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XUnmapWindow.html
    /* The main-thread guard lives in unmapWindowInternal, at the SDL teardown
     * itself, so it also protects the win_gravity=UnmapGravity caller.
     */
    SET_X_SERVER_REQUEST(display, X_UnmapWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (window == SCREEN_WINDOW) {
        handleError(0, display, window, 0, BadWindow, 0);
        return 0;
    }
    unmapWindowInternal(display, window, False);
    return 1;
}

/* Off-main-thread router, the teardown counterpart to XMapWindow: an app that
 * maps a top-level from a worker thread will unmap it from the same thread, so
 * both must run on the main thread. Routed off-thread today: map, unmap, raise,
 * store-name. Broader ops (resize, move, configure, reparent) stay
 * main-thread-only pending the wider lock integration in T1 phase 2; no current
 * workload drives them off-thread.
 */
struct unmapWindowArgs {
    Display *display;
    Window window;
    int rc;
};

static void unmapWindowOnMain(void *p)
{
    struct unmapWindowArgs *a = (struct unmapWindowArgs *) p;
    a->rc = unmapWindowImpl(a->display, a->window);
}

int XUnmapWindow(Display *display, Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        struct unmapWindowArgs a = {display, window, 0};
        runOnMainThread(unmapWindowOnMain, &a);
        return a.rc;
    }
    return unmapWindowImpl(display, window);
}

/* XMap/XUnmapSubwindows walk the parent's child array and (un)map each child.
 * The walk (children.length + a memcpy of GET_CHILDREN) reads the tree, so off
 * a worker thread it would race a concurrent main-thread reallocation of that
 * same array (add/remove child). Route the whole body to the main thread so the
 * walk and the per-child (un)maps run there, serialized with tree mutation; on
 * the main thread it is a direct call and each child hop takes the inline path.
 */
static int mapSubwindowsImpl(Display *display, Window window)
{
    SET_X_SERVER_REQUEST(display, X_MapWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    size_t count = GET_WINDOW_STRUCT(window)->children.length;
    Window *children = malloc(sizeof(Window) * count);
    if (!children && count > 0) {
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    memcpy(children, GET_CHILDREN(window), sizeof(Window) * count);
    for (size_t i = 0; i < count; i++) {
        if (GET_WINDOW_STRUCT(children[i])->mapState != Mapped)
            XMapWindow(display, children[i]);
    }
    free(children);
    return 1;
}

static int unmapSubwindowsImpl(Display *display, Window window)
{
    SET_X_SERVER_REQUEST(display, X_UnmapWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    size_t count = GET_WINDOW_STRUCT(window)->children.length;
    Window *children = malloc(sizeof(Window) * count);
    if (!children && count > 0) {
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    memcpy(children, GET_CHILDREN(window), sizeof(Window) * count);
    for (size_t i = count; i > 0; i--) {
        if (GET_WINDOW_STRUCT(children[i - 1])->mapState != UnMapped)
            XUnmapWindow(display, children[i - 1]);
    }
    free(children);
    return 1;
}

struct subwindowsArgs {
    Display *display;
    Window window;
    int rc;
};

static void mapSubwindowsOnMain(void *p)
{
    struct subwindowsArgs *a = (struct subwindowsArgs *) p;
    a->rc = mapSubwindowsImpl(a->display, a->window);
}

static void unmapSubwindowsOnMain(void *p)
{
    struct subwindowsArgs *a = (struct subwindowsArgs *) p;
    a->rc = unmapSubwindowsImpl(a->display, a->window);
}

int XMapSubwindows(Display *display, Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        struct subwindowsArgs a = {display, window, 0};
        runOnMainThread(mapSubwindowsOnMain, &a);
        return a.rc;
    }
    return mapSubwindowsImpl(display, window);
}

int XUnmapSubwindows(Display *display, Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        struct subwindowsArgs a = {display, window, 0};
        runOnMainThread(unmapSubwindowsOnMain, &a);
        return a.rc;
    }
    return unmapSubwindowsImpl(display, window);
}

Status XWithdrawWindow(Display *display, Window window, int screen_number)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XWithdrawWindow.html
    TYPE_CHECK(window, WINDOW, display, 0);
    XUnmapWindow(display, window);
    return 1;
}

static int storeNameImpl(Display *display,
                         Window window,
                         _Xconst char *window_name)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XStoreName.html
    TYPE_CHECK(window, WINDOW, display, 0);
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
        /* SDL_SetWindowTitle is a main-thread-only call on macOS; the router
         * below guarantees we are on the main thread by the time we get here.
         */
        requireMainEventThread("XStoreName");
        SDL_SetWindowTitle(GET_WINDOW_STRUCT(window)->sdlWindow, window_name);
    } else {
        size_t len = strlen(window_name) + 1;
        char *windowName = malloc(len);
        if (!windowName) {
            handleError(0, display, window, 0, BadAlloc, 0);
            return 0;
        }
        memcpy(windowName, window_name, len);
        free(GET_WINDOW_STRUCT(window)->windowName);
        GET_WINDOW_STRUCT(window)->windowName = windowName;
    }
    return 1;
}

/* Off-main-thread router: XStoreName touches SDL (SDL_SetWindowTitle) and reads
 * the window tree, so a client worker thread runs the whole body on the main
 * thread. On the main thread it is a direct call.
 */
struct storeNameArgs {
    Display *display;
    Window window;
    _Xconst char *window_name;
    int rc;
};

static void storeNameOnMain(void *p)
{
    struct storeNameArgs *a = (struct storeNameArgs *) p;
    a->rc = storeNameImpl(a->display, a->window, a->window_name);
}

int XStoreName(Display *display, Window window, _Xconst char *window_name)
{
    if (!libx11CompatOnMainEventThread()) {
        struct storeNameArgs a = {display, window, window_name, 0};
        runOnMainThread(storeNameOnMain, &a);
        return a.rc;
    }
    return storeNameImpl(display, window, window_name);
}

int XSetIconName(Display *display, Window window, _Xconst char *icon_name)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XSetIconName.html
    /* No icon-name display in SDL; accept and discard. */
    (void) display;
    (void) window;
    (void) icon_name;
    return 1;
}

int XMoveWindow(Display *display, Window window, int x, int y)
{
    // https://tronche.com/gui/x/xlib/window/XMoveWindow.html
    /* TYPE_CHECK runs inside configureWindow on the main thread, not here. */
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    XWindowChanges changes;
    changes.x = x;
    changes.y = y;
    return configureWindowRouted(display, window, CWX | CWY, &changes);
}

int XResizeWindow(Display *display,
                  Window window,
                  unsigned int width,
                  unsigned int height)
{
    // https://tronche.com/gui/x/xlib/window/XResizeWindow.html
    /* TYPE_CHECK runs inside configureWindow on the main thread, not here. */
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    XWindowChanges changes;
    changes.width = width;
    changes.height = height;
    return configureWindowRouted(display, window, CWWidth | CWHeight, &changes);
}

static int reparentWindowImpl(Display *display,
                              Window window,
                              Window parent,
                              int x,
                              int y)
{
    // https://tronche.com/gui/x/xlib/window-and-session-manager/XReparentWindow.html
    /* May realize/unrealize the SDL window and show it (SDL_CreateWindow,
     * SDL_ShowWindow, drawWindowDataToScreen), main-thread-only on macOS; the
     * router below guarantees we are on the main thread here.
     */
    requireMainEventThread("XReparentWindow");
    SET_X_SERVER_REQUEST(display, X_ReparentWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    TYPE_CHECK(parent, WINDOW, display, 0);
    if (window == parent) {
        LOG("Invalid parameter: Can not add window to itself in "
            "XReparentWindow!\n");
        handleError(0, display, window, 0, BadMatch, 0);
        return 0;
    } else if (IS_INPUT_ONLY(parent) && !IS_INPUT_ONLY(window)) {
        LOG("Invalid parameter: Can not add InputOutput window "
            "to InputOnly window in XReparentWindow!\n");
        handleError(0, display, window, 0, BadMatch, 0);
        return 0;
    } else if (isParent(window, parent)) {
        LOG("Invalid parameter: Can not add window to one of it's childs in "
            "XReparentWindow!\n");
        handleError(0, display, window, 0, BadMatch, 0);
        return 0;
    }

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    MapState mapState = windowStruct->mapState;
    Window oldParent = GET_PARENT(window);
    Bool wasTopLevel = IS_TOP_LEVEL(window);

    /* Snapshot pre-mutation geometry so every failure path can restore the
     * window to its original parent, position, and map state. removeArray
     * shrinks length but never reduces capacity, so the subsequent
     * addChildToWindow(oldParent, window) reuses the slot just vacated and
     * cannot fail under OOM.
     */
    int oldX = windowStruct->x, oldY = windowStruct->y;

    /* Reparenting relocates the window's cells in (and possibly between) shared
     * top-level backings, so drop its text stamps for both the old and the new
     * top-level. Flush before detach to resolve the old top-level.
     */
    flushTextStampsForWindow(window);
    removeChildFromParent(window);
    if (!addChildToWindow(parent, window)) {
        LOG("Out of memory: Failed to reattach window in XReparentWindow!\n");
        addChildToWindow(oldParent, window);
        return 0;
    }

    windowStruct->x = x;
    windowStruct->y = y;
    flushTextStampsForWindow(window);

    /* The new parent's siblings of "window" (and their descendants) and
     * "window"'s own descendants all need their cached visible regions
     * recomputed against the new parent chain. removeChildFromParent above
     * already invalidated the old top-level subtree; this call picks up the new
     * one.
     */
    invalidateVisibleRegionForTopLevel(window);

    /* Reparenting a mapped window under an unmapped parent makes it unviewable
     * without an XUnmapWindow, so release any active grab it held here too.
     */
    if (mapState != UnMapped && !isWindowEffectivelyViewable(window))
        releaseActiveGrabsForUnviewableWindow(display, window);
    if (mapState != UnMapped) {
        if (wasTopLevel && parent != SCREEN_WINDOW) {
            if (!mergeWindowDrawables(parent, window)) {
                removeChildFromParent(window);
                addChildToWindow(oldParent, window);
                windowStruct->x = oldX;
                windowStruct->y = oldY;
                return 0;
            }
            unrealizeTopLevelWindow(display, window);
        } else if (!wasTopLevel && parent == SCREEN_WINDOW) {
            if (!realizeTopLevelWindow(display, window)) {
                removeChildFromParent(window);
                addChildToWindow(oldParent, window);
                windowStruct->x = oldX;
                windowStruct->y = oldY;
                windowStruct->mapState = mapState;
                return 0;
            }

            /* A child whose pre-reparent ancestor was unmapped sat at
             * MapRequested. Its new parent (root) is always mapped, so promote
             * it to Mapped now so XGetWindowAttributes and the rest of the
             * map-state machinery agree that this top-level is viewable.
             */
            windowStruct->mapState = Mapped;

            /* realizeTopLevelWindow creates the SDL_Window hidden so XMapWindow
             * can control show timing. The reparent path doesn't go through
             * XMapWindow, so the window stays hidden. Show it now since the old
             * child was already considered mapped before reparent.
             */
            if (windowStruct->sdlWindow) {
                showTopLevelWindow(display, window, False);
            }
        }
    }
    if (mapState != UnMapped)
        postReparentUnmapNotify(display, window, oldParent);
    postEvent(display, window, ReparentNotify, oldParent);
    if (mapState != UnMapped)
        postEvent(display, window, MapNotify);
    return 1;
}

/* Off-main-thread router: XReparentWindow can realize a new SDL top-level and
 * show it, so a worker thread runs the whole body on the main thread.
 */
struct reparentArgs {
    Display *display;
    Window window;
    Window parent;
    int x;
    int y;
    int rc;
};

static void reparentOnMain(void *p)
{
    struct reparentArgs *a = (struct reparentArgs *) p;
    a->rc = reparentWindowImpl(a->display, a->window, a->parent, a->x, a->y);
}

int XReparentWindow(Display *display,
                    Window window,
                    Window parent,
                    int x,
                    int y)
{
    if (!libx11CompatOnMainEventThread()) {
        struct reparentArgs a = {display, window, parent, x, y, 0};
        runOnMainThread(reparentOnMain, &a);
        return a.rc;
    }
    return reparentWindowImpl(display, window, parent, x, y);
}

int indexInWindowList(Window *windowList, int numWindows, Window window)
{
    int i;
    for (i = 0; i < numWindows; i++) {
        if (*windowList == window)
            return i;
        windowList++;
    }
    return -1;
}

Bool XTranslateCoordinates(Display *display,
                           Window sourceWindow,
                           Window destinationWindow,
                           int sourceX,
                           int sourceY,
                           int *destinationXReturn,
                           int *destinationYReturn,
                           Window *childReturn)
{
    // https://tronche.com/gui/x/xlib/window-information/XTranslateCoordinates.html
    SET_X_SERVER_REQUEST(display, X_TranslateCoords);
    TYPE_CHECK(sourceWindow, WINDOW, display, False);
    TYPE_CHECK(destinationWindow, WINDOW, display, False);
    int currX = 0, currY = 0;
    translateWindowPoint(sourceWindow, destinationWindow, sourceX, sourceY,
                         &currX, &currY);
    *destinationXReturn = currX;
    *destinationYReturn = currY;
    if (childReturn) {
        *childReturn =
            getDirectChildContainingPoint(destinationWindow, currX, currY);
    }
    return True;
}

/* XChangeProperty / XGetWindowProperty / XDeleteProperty live in
 * src/window-property.c. The WM-state helpers (applyMotifWmHintsFromProperty,
 * windowIsModal, applyTransientForRelationship, clearTransientForRelationship,
 * applyNetWmStateAction) live in src/window-wm.c. Both are declared in
 * window-internal.h which window.c already pulls in via window.h.
 */

static int raiseWindowImpl(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XRaiseWindow.html
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
        requireMainEventThread("XRaiseWindow");
        SDL_RaiseWindow(GET_WINDOW_STRUCT(window)->sdlWindow);
    }
    moveChildToTop(display, window);
    postVisibilityForWindowAndSiblings(display, window);

    /* Re-offer so the replay target module sees the raise: at-root selection
     * picks the most recently offered window as topmost.
     */
    offerReplayTargetForWindow(window);
    return 1;
}

/* Off-main-thread router. XMapRaised maps then raises, so leaving XRaiseWindow
 * unrouted would make an off-thread XMapRaised map safely and then crash in
 * SDL_RaiseWindow on the worker; route it too so the map/raise pair is
 * coherent.
 */
struct raiseWindowArgs {
    Display *display;
    Window window;
    int rc;
};

static void raiseWindowOnMain(void *p)
{
    struct raiseWindowArgs *a = (struct raiseWindowArgs *) p;
    a->rc = raiseWindowImpl(a->display, a->window);
}

int XRaiseWindow(Display *display, Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        struct raiseWindowArgs a = {display, window, 0};
        runOnMainThread(raiseWindowOnMain, &a);
        return a.rc;
    }
    return raiseWindowImpl(display, window);
}

int XLowerWindow(Display *display, Window window)
{
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    moveChildToBottom(display, window);
    postVisibilityForWindowAndSiblings(display, window);
    return 1;
}

int XRestackWindows(Display *display, Window *windows, int nwindows)
{
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    if (nwindows <= 0)
        return 1;
    for (int i = 0; i < nwindows; i++)
        TYPE_CHECK(windows[i], WINDOW, display, 0);
    for (int i = nwindows - 1; i >= 0; i--) {
        Window parent = GET_PARENT(windows[i]);
        if (parent != None) {
            moveChildToIndexAndExpose(
                display, windows[i],
                GET_WINDOW_STRUCT(parent)->children.length - 1);
        }
        postVisibilityForWindowAndSiblings(display, windows[i]);
    }
    return 1;
}

Status XGetWindowAttributes(Display *display,
                            Window window,
                            XWindowAttributes *window_attributes_return)
{
    // https://tronche.com/gui/x/xlib/window-information/XGetWindowAttributes.html
    SET_X_SERVER_REQUEST(display, X_GetWindowAttributes);
    TYPE_CHECK(window, WINDOW, display, 0);
    window_attributes_return->root = SCREEN_WINDOW;
    window_attributes_return->visual = GET_VISUAL(window);
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    window_attributes_return->x = windowStruct->x;
    window_attributes_return->y = windowStruct->y;
    window_attributes_return->width = windowStruct->w;
    window_attributes_return->height = windowStruct->h;
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
        SDL_Window *sdlWindow = GET_WINDOW_STRUCT(window)->sdlWindow;
        Uint32 flags = SDL_GetWindowFlags(sdlWindow);
        if (windowStruct->mapState == UnMapped) {
            window_attributes_return->map_state = IsUnmapped;
        } else if (HAS_VALUE(flags, SDL_WINDOW_MINIMIZED)) {
            window_attributes_return->map_state = IsUnviewable;
        } else if (HAS_VALUE(flags, SDL_WINDOW_HIDDEN)) {
            window_attributes_return->map_state = IsUnmapped;
        } else {
            window_attributes_return->map_state = IsViewable;
        }
    } else {
        if (windowStruct->mapState == UnMapped) {
            window_attributes_return->map_state = IsUnmapped;
        } else if (hasUnmappedAncestor(window)) {
            window_attributes_return->map_state = IsUnviewable;
        } else {
            window_attributes_return->map_state = IsViewable;
        }
    }
    window_attributes_return->depth =
        windowStruct->depth == CopyFromParent
            ? DefaultDepth(display, DefaultScreen(display))
            : windowStruct->depth;
    window_attributes_return->colormap = GET_WINDOW_STRUCT(window)->colormap;
    window_attributes_return->override_redirect =
        GET_WINDOW_STRUCT(window)->overrideRedirect;
    window_attributes_return->your_event_mask =
        GET_WINDOW_STRUCT(window)->eventMask;
    return 1;
}

int XSetWindowBackground(Display *display,
                         Window window,
                         unsigned long background_pixel)
{
    // https://tronche.com/gui/x/xlib/window/XSetWindowBackground.html
    SET_X_SERVER_REQUEST(display, X_ChangeWindowAttributes);
    if (window != SCREEN_WINDOW) {
        if (IS_INPUT_ONLY(window)) {
            LOG("Invalid parameter: Can not change the background of an "
                "InputOnly "
                "window in XChangeWindowAttributes!\n");
            handleError(0, display, window, 0, BadMatch, 0);
            return 0;
        }
        GET_WINDOW_STRUCT(window)->backgroundColor = background_pixel;
    }
    return 1;
}

int XSetWindowBackgroundPixmap(Display *display,
                               Window window,
                               Pixmap background_pixmap)
{
    // https://tronche.com/gui/x/xlib/window/XSetWindowBackgroundPixmap.html
    SET_X_SERVER_REQUEST(display, X_ChangeWindowAttributes);
    if (window != SCREEN_WINDOW) {
        WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
        if (IS_INPUT_ONLY(window)) {
            LOG("Invalid parameter: Can not change the background Pixmap of an "
                "InputOnly window in XChangeWindowAttributes!\n");
            handleError(0, display, window, 0, BadMatch, 0);
            return 0;
        }
        if (background_pixmap == None ||
            background_pixmap == (Pixmap) ParentRelative) {
            windowStruct->background = background_pixmap;
        } else {
            TYPE_CHECK(background_pixmap, PIXMAP, display, 0);
            windowStruct->background = background_pixmap;
        }
    }
    return 1;
}

int XSetWindowBorder(Display *display,
                     Window window,
                     unsigned long border_pixel)
{
    // https://tronche.com/gui/x/xlib/window/XSetWindowBorder.html
    SET_X_SERVER_REQUEST(display, X_ChangeWindowAttributes);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (window != SCREEN_WINDOW) {
        GET_WINDOW_STRUCT(window)->borderPixel = border_pixel;
        GET_WINDOW_STRUCT(window)->borderPixmap = None;
    }
    return 1;
}

int XSetWindowBorderPixmap(Display *display,
                           Window window,
                           Pixmap border_pixmap)
{
    // https://tronche.com/gui/x/xlib/window/XSetWindowBorderPixmap.html
    SET_X_SERVER_REQUEST(display, X_ChangeWindowAttributes);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (border_pixmap != CopyFromParent)
        TYPE_CHECK(border_pixmap, PIXMAP, display, 0);
    if (window != SCREEN_WINDOW)
        GET_WINDOW_STRUCT(window)->borderPixmap = border_pixmap;
    return 1;
}

int XSetWindowColormap(Display *display, Window window, Colormap colormap)
{
    // https://tronche.com/gui/x/xlib/window/XSetWindowColormap.html
    SET_X_SERVER_REQUEST(display, X_ChangeWindowAttributes);
    TYPE_CHECK(window, WINDOW, display, 0);
    TYPE_CHECK(colormap, COLORMAP, display, 0);
    if (window != SCREEN_WINDOW)
        GET_WINDOW_STRUCT(window)->colormap = colormap;
    return 1;
}

int XChangeWindowAttributes(Display *display,
                            Window window,
                            unsigned long valueMask,
                            XSetWindowAttributes *attributes)
{
    // https://tronche.com/gui/x/xlib/window/XChangeWindowAttributes.html
    SET_X_SERVER_REQUEST(display, X_ChangeWindowAttributes);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (window != SCREEN_WINDOW) {
        if (HAS_VALUE(valueMask, CWCursor)) {
            if (!XDefineCursor(display, window, attributes->cursor))
                return 0;
        }
        if (HAS_VALUE(valueMask, CWBackPixmap)) {
            XSetWindowBackgroundPixmap(display, window,
                                       attributes->background_pixmap);
        }
        if (HAS_VALUE(valueMask, CWBackPixel))
            XSetWindowBackground(display, window, attributes->background_pixel);
        if (HAS_VALUE(valueMask, CWColormap))
            XSetWindowColormap(display, window, attributes->colormap);
        if (HAS_VALUE(valueMask, CWEventMask)) {
            LOG("Change window attributes event: window=%lu mask=0x%lx "
                "exposure=0x%lx structure=0x%lx substructure=0x%lx\n",
                window, attributes->event_mask,
                attributes->event_mask & ExposureMask,
                attributes->event_mask & StructureNotifyMask,
                attributes->event_mask & SubstructureNotifyMask);
            GET_WINDOW_STRUCT(window)->eventMask = attributes->event_mask;
        }
        if (HAS_VALUE(valueMask, CWOverrideRedirect)) {
            GET_WINDOW_STRUCT(window)->overrideRedirect =
                attributes->override_redirect ? True : False;
        }
        if (HAS_VALUE(valueMask, CWBitGravity))
            GET_WINDOW_STRUCT(window)->bitGravity = attributes->bit_gravity;
        if (HAS_VALUE(valueMask, CWWinGravity))
            GET_WINDOW_STRUCT(window)->winGravity = attributes->win_gravity;
    }
    return 1;
}

int XMoveResizeWindow(Display *display,
                      Window window,
                      int x,
                      int y,
                      unsigned int width,
                      unsigned int height)
{
    // https://tronche.com/gui/x/xlib/window/XMoveResizeWindow.html
    /* One combined configure, not XMoveWindow + XResizeWindow: real X applies
     * the geometry atomically (a single ConfigureNotify). Two calls also meant
     * two main-thread handoffs off-thread, exposing a half-applied
     * position/size between them.
     */
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    XWindowChanges changes;
    changes.x = x;
    changes.y = y;
    changes.width = width;
    changes.height = height;
    return configureWindowRouted(display, window,
                                 CWX | CWY | CWWidth | CWHeight, &changes);
}

int XSetWindowBorderWidth(Display *display, Window window, unsigned int width)
{
    // https://tronche.com/gui/x/xlib/window/XSetWindowBorderWidth.html
    SET_X_SERVER_REQUEST(display, X_ChangeWindowAttributes);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (window != SCREEN_WINDOW)
        GET_WINDOW_STRUCT(window)->borderWidth = width;
    return 1;
}

Status XQueryTree(Display *display,
                  Window window,
                  Window *root_return,
                  Window *parent_return,
                  Window **children_return,
                  unsigned int *nchildren_return)
{
    // https://tronche.com/gui/x/xlib/window-information/XQueryTree.html
    SET_X_SERVER_REQUEST(display, X_QueryTree);
    TYPE_CHECK(window, WINDOW, display, 0);
    *root_return = SCREEN_WINDOW;
    *parent_return = GET_PARENT(window);
    size_t total = GET_WINDOW_STRUCT(window)->children.length;
    Window *source = GET_CHILDREN(window);

    /* Internal windows (the hidden clipboard requestor) are library plumbing
     * and must not surface to a client walking the tree. Copy only the real
     * ones.
     */
    Window *out = malloc(sizeof(Window) * total);
    if (!out && total > 0)
        return 0;

    unsigned int kept = 0;
    for (size_t i = 0; i < total; i++) {
        if (IS_TYPE(source[i], WINDOW) &&
            GET_WINDOW_STRUCT(source[i])->internal)
            continue;
        out[kept++] = source[i];
    }
    *children_return = out;
    *nchildren_return = kept;
    return 1;
}
