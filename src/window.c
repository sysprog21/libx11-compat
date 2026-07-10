#include <X11/Xlib.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "atoms.h"
#include "display.h"
#include "drawing.h"
#include "errors.h"
#include "events.h"
#include "font.h"
#include "image.h"
#include "input.h"
#include "input-method.h"
#include "net-atoms.h"
#include "replay.h"
#include "replay-target.h"
#include "visual.h"
#include "window.h"

int XDestroyWindow(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XDestroyWindow.html
    SET_X_SERVER_REQUEST(display, X_DestroyWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (window == SCREEN_WINDOW)
        return 0;
    destroyWindow(display, window, True);
    return 1;
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
        if (!ws || !ws->sdlWindow)
            continue;
        int wid = 0, hgt = 0;
        SDL_GetWindowSize(ws->sdlWindow, &wid, &hgt);
        replayTargetOfferWindow(SDL_GetWindowID(ws->sdlWindow), ws->x, ws->y,
                                wid, hgt);
        return;
    }
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

static Bool realizeTopLevelWindow(Display *display, Window window)
{
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
    flags |= SDL_WINDOW_ALLOW_HIGHDPI;
    int hostX = windowStruct->x, hostY = windowStruct->y;
    topLevelWindowHostPosition(window, windowStruct->x, windowStruct->y, &hostX,
                               &hostY);
    LOG("realizeTopLevelWindow: window=%lu pos=(%d,%d) host=(%d,%d) "
        "size=(%ux%u) "
        "borderless=%d\n",
        window, windowStruct->x, windowStruct->y, hostX, hostY, windowStruct->w,
        windowStruct->h, (flags & SDL_WINDOW_BORDERLESS) != 0);
    SDL_Window *sdlWindow =
        SDL_CreateWindow(windowStruct->windowName, hostX, hostY,
                         windowStruct->w, windowStruct->h, flags);
    if (!sdlWindow) {
        LOG("SDL_CreateWindow failed in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, None, 0, BadMatch, 0);
        return False;
    }

    registerWindowMapping(window, SDL_GetWindowID(sdlWindow));
    windowStruct->sdlWindow = sdlWindow;
    if (windowStruct->overrideRedirect) {
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
    applyTransientForRelationship(display, window);

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
                applyTransientForRelationship(display, sibling);
        }
    }
}

static void unrealizeTopLevelWindow(Window window)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!windowStruct->sdlWindow)
        return;

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
    SDL_DestroyWindow(windowStruct->sdlWindow);
    windowStruct->sdlWindow = NULL;
    windowStruct->needsPresent = False;
    windowStruct->hasPresented = False;
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
    postEvent(display, windowID, CreateNotify);
    if (valueMask != 0)
        XChangeWindowAttributes(display, windowID, valueMask, attributes);
    return windowID;
}

int XDestroySubwindows(Display *display, Window window)
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

int XConfigureWindow(Display *display,
                     Window window,
                     unsigned int value_mask,
                     XWindowChanges *values)
{
    // https://tronche.com/gui/x/xlib/window/XConfigureWindow.html
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    return configureWindow(display, window, value_mask, values);
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

Status XIconifyWindow(Display *display, Window window, int screen_number)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XIconifyWindow.html
    TYPE_CHECK(window, WINDOW, display, 0);
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window))
        SDL_MinimizeWindow(GET_WINDOW_STRUCT(window)->sdlWindow);
    return 1;
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

int XMapWindow(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XMapWindow.html
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
        windowStruct->needsPresent = True;
        drawWindowDataToScreen();
        SDL_ShowWindow(windowStruct->sdlWindow);
        SDL_RaiseWindow(windowStruct->sdlWindow);
        replayDeferredWmProperties(display, window);
    }

    // SDL_UpdateWindowSurface(GET_WINDOW_STRUCT(window)->sdlWindow);

#ifdef DEBUG_WINDOWS
    printWindowsHierarchy();
#endif
    return 1;
}

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
        SDL_Renderer *sdlRenderer = windowStruct->sdlRenderer;
        windowStruct->sdlRenderer = NULL;
        if (sdlRenderer) {
            invalidatePutImageStagingTexture(sdlRenderer);
            invalidateTextCacheForRenderer(sdlRenderer);
            SDL_DestroyRenderer(sdlRenderer);
        }
        unrealizeTopLevelWindow(window);
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

int XUnmapWindow(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XUnmapWindow.html
    SET_X_SERVER_REQUEST(display, X_UnmapWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (window == SCREEN_WINDOW) {
        handleError(0, display, window, 0, BadWindow, 0);
        return 0;
    }
    unmapWindowInternal(display, window, False);
    return 1;
}

int XMapSubwindows(Display *display, Window window)
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

int XUnmapSubwindows(Display *display, Window window)
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

Status XWithdrawWindow(Display *display, Window window, int screen_number)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XWithdrawWindow.html
    TYPE_CHECK(window, WINDOW, display, 0);
    XUnmapWindow(display, window);
    return 1;
}

int XStoreName(Display *display, Window window, _Xconst char *window_name)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XStoreName.html
    TYPE_CHECK(window, WINDOW, display, 0);
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
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
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    XWindowChanges changes;
    changes.x = x;
    changes.y = y;
    return configureWindow(display, window, CWX | CWY, &changes);
}

int XResizeWindow(Display *display,
                  Window window,
                  unsigned int width,
                  unsigned int height)
{
    // https://tronche.com/gui/x/xlib/window/XResizeWindow.html
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    XWindowChanges changes;
    changes.width = width;
    changes.height = height;
    return configureWindow(display, window, CWWidth | CWHeight, &changes);
}

int XReparentWindow(Display *display,
                    Window window,
                    Window parent,
                    int x,
                    int y)
{
    // https://tronche.com/gui/x/xlib/window-and-session-manager/XReparentWindow.html
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
            unrealizeTopLevelWindow(window);
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
                windowStruct->needsPresent = True;
                drawWindowDataToScreen();
                SDL_ShowWindow(windowStruct->sdlWindow);
                replayDeferredWmProperties(display, window);
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


int XRaiseWindow(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XRaiseWindow.html
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window))
        SDL_RaiseWindow(GET_WINDOW_STRUCT(window)->sdlWindow);
    moveChildToTop(display, window);
    postVisibilityForWindowAndSiblings(display, window);
    /* Re-offer so the replay target module sees the raise: at-root selection
     * picks the most recently offered window as topmost.
     */
    offerReplayTargetForWindow(window);
    return 1;
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
    if (XMoveWindow(display, window, x, y) &&
        XResizeWindow(display, window, width, height)) {
        return 1;
    }
    return 0;
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
    *nchildren_return = GET_WINDOW_STRUCT(window)->children.length;
    *children_return = malloc(sizeof(Window) * (*nchildren_return));
    if (!*children_return && *nchildren_return > 0)
        return 0;

    memcpy(*children_return, GET_CHILDREN(window),
           sizeof(Window) * (*nchildren_return));
    return 1;
}
