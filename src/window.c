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
#include "net-atoms.h"
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

static Bool moveChildToTop(Window window)
{
    Window parent = GET_PARENT(window);
    if (parent == None)
        return False;
    return moveChildToIndex(window,
                            GET_WINDOW_STRUCT(parent)->children.length - 1);
}

static Bool moveChildToBottom(Window window)
{
    return moveChildToIndex(window, 0);
}

static void postVisibilityForWindowAndSiblings(Display *display, Window window)
{
    Window parent = GET_PARENT(window);
    if (parent == None) {
        postEvent(display, window, VisibilityNotify);
        return;
    }
    Window *children = GET_CHILDREN(parent);
    for (size_t i = 0; i < GET_WINDOW_STRUCT(parent)->children.length; i++) {
        postEvent(display, children[i], VisibilityNotify);
    }
}

static Bool hasUnmappedAncestor(Window window)
{
    Window parent = GET_PARENT(window);
    while (parent != None) {
        if (GET_WINDOW_STRUCT(parent)->mapState == UnMapped) {
            return True;
        }
        parent = GET_PARENT(parent);
    }
    return False;
}

static Bool realizeTopLevelWindow(Display *display, Window window)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (windowStruct->sdlWindow)
        return True;

    Uint32 flags = SDL_WINDOW_SHOWN;
    if (windowStruct->borderWidth == 0) {
        flags |= SDL_WINDOW_BORDERLESS;
    }
    if (windowStruct->overrideRedirect) {
        flags |= SDL_WINDOW_ALWAYS_ON_TOP;
    }
    if (windowStruct->eventMask & KeyPressMask ||
        windowStruct->eventMask & KeyReleaseMask) {
        flags |= SDL_WINDOW_INPUT_FOCUS;
    }
    LOG("realizeTopLevelWindow: window=%lu pos=(%d,%d) size=(%ux%u) "
        "borderless=%d\n",
        window, windowStruct->x, windowStruct->y, windowStruct->w,
        windowStruct->h, (flags & SDL_WINDOW_BORDERLESS) != 0);
    SDL_Window *sdlWindow = SDL_CreateWindow(
        windowStruct->windowName, windowStruct->x, windowStruct->y,
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
    if (windowStruct->cursor != None) {
        XDefineCursor(display, window, windowStruct->cursor);
    }
    if (windowStruct->icon) {
        SDL_SetWindowIcon(windowStruct->sdlWindow, windowStruct->icon);
    }
    return True;
}

static void unrealizeTopLevelWindow(Window window)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!windowStruct->sdlWindow)
        return;

    deleteWindowMapping(window);
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
    if (valueMask != 0) {
        XChangeWindowAttributes(display, windowID, valueMask, attributes);
    }
    LOG("!!! XCreateWindow %lu {x = %d, y = %d, w = %d, h = %d} TYPE %d PARENT "
        "%lu\n",
        windowID, x, y, width, height, GET_XID_TYPE(windowID),
        GET_WINDOW_STRUCT(windowID)->parent);
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
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
        SDL_MinimizeWindow(GET_WINDOW_STRUCT(window)->sdlWindow);
    }
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
    if (GET_WINDOW_STRUCT(window)->mapState == Mapped) {
        return 1;
    }

    /* libx11-compat has no separate window-manager client to service
     * SubstructureRedirect requests. Some Motif paths select redirect-style
     * masks internally; if we stop at MapRequest, top-level shells never become
     * SDL windows. Keep mapping in-process.
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
         * */
        windowStruct->mapState = Mapped;

        /* On macOS a command-line launched SDL app does not get keyboard focus
         * by default even with SDL_WINDOW_INPUT_FOCUS requested, so keystrokes
         * stay in the terminal. Real X11's MapWindow puts the window on screen;
         * raising it here matches user expectation.
         */
        SDL_RaiseWindow(windowStruct->sdlWindow);
        if (windowStruct->windowName) {
            free(windowStruct->windowName);
            windowStruct->windowName = NULL;
        }
        if (windowStruct->eventMask & KeyPressMask ||
            windowStruct->eventMask & KeyReleaseMask) {
            /* Keep SDL text input off so KeyPress events are not doubled
             * by SDL_TEXTINPUT. No setKeyboardFocus here: focus is owned
             * by XSetInputFocus and its callers; XMapWindow auto-focus
             * stole focus from the active Motif dialog and prevented
             * popup shells from finishing their map sequence. */
            SDL_StopTextInput();
        }
    } else { /* Mapping a window that is not a top level window  */
        Window parent = GET_PARENT(window);
        if (GET_WINDOW_STRUCT(parent)->mapState == Mapped) {
            if (!mergeWindowDrawables(parent, window)) {
                LOG("Failed to merge the window renderer in %s: %s\n", __func__,
                    SDL_GetError());
                return 0;
            }
            GET_WINDOW_STRUCT(window)->mapState = Mapped;
            XClearArea(display, window, 0, 0, 0, 0, False);
            GET_WINDOW_STRUCT(window)->contentsMergedToParent = False;
        } else { /* Parent not mapped */
            GET_WINDOW_STRUCT(window)->mapState = MapRequested;
            return 1;
        }
    }

    postEvent(display, window, MapNotify);
    postVisibilityForWindowAndSiblings(display, window);
    mapRequestedChildren(display, window);

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    SDL_Rect exposeRect = {0, 0, windowStruct->w, windowStruct->h};
    postExposeEvent(display, window, &exposeRect, 1);
    drawWindowDataToScreen();

    // SDL_UpdateWindowSurface(GET_WINDOW_STRUCT(window)->sdlWindow);

#ifdef DEBUG_WINDOWS
    printWindowsHierarchy();
#endif
    return 1;
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
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (windowStruct->mapState == UnMapped)
        return 1;

    windowStruct->mapState = UnMapped;
    postVisibilityForWindowAndSiblings(display, window);
    if (windowStruct->sdlWindow) {
        SDL_Window *sdlWindow = windowStruct->sdlWindow;
        SDL_Renderer *sdlRenderer = windowStruct->sdlRenderer;
        windowStruct->sdlWindow = NULL;
        windowStruct->sdlRenderer = NULL;
        windowStruct->needsPresent = False;
        if (sdlRenderer) {
            invalidatePutImageStagingTexture(sdlRenderer);
            invalidateTextCacheForRenderer(sdlRenderer);
            SDL_DestroyRenderer(sdlRenderer);
        }
        SDL_DestroyWindow(sdlWindow);
    } else if (GET_WINDOW_STRUCT(GET_PARENT(window))->mapState != UnMapped) {
        postEvent(display, window, UnmapNotify, False);
        SDL_Rect exposeRect = {windowStruct->x, windowStruct->y,
                               windowStruct->w, windowStruct->h};
        postExposeEvent(display, GET_PARENT(window), &exposeRect, 1);
    }
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
        if (GET_WINDOW_STRUCT(children[i])->mapState != Mapped) {
            XMapWindow(display, children[i]);
        }
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
        if (GET_WINDOW_STRUCT(children[i - 1])->mapState != UnMapped) {
            XUnmapWindow(display, children[i - 1]);
        }
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
     * addChildToWindow(oldParent, window) reuses the slot we just vacated and
     * cannot fail under OOM.
     */
    int oldX = windowStruct->x;
    int oldY = windowStruct->y;
    removeChildFromParent(window);
    if (!addChildToWindow(parent, window)) {
        LOG("Out of memory: Failed to reattach window in XReparentWindow!\n");
        addChildToWindow(oldParent, window);
        return 0;
    }
    windowStruct->x = x;
    windowStruct->y = y;
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
        }
    }
    if (mapState != UnMapped) {
        postReparentUnmapNotify(display, window, oldParent);
    }
    postEvent(display, window, ReparentNotify, oldParent);
    if (mapState != UnMapped) {
        postEvent(display, window, MapNotify);
    }
    return 1;
}

int indexInWindowList(Window *windowList, int numWindows, Window window)
{
    int i;
    for (i = 0; i < numWindows; i++) {
        if (*windowList == window) {
            return i;
        }
        windowList++;
    }
    return -1;
}

static void windowAbsoluteOrigin(Window window, int *xReturn, int *yReturn)
{
    int x = 0;
    int y = 0;
    while (window != None && window != SCREEN_WINDOW) {
        int wx = 0;
        int wy = 0;
        GET_WINDOW_POS(window, wx, wy);
        x += wx;
        y += wy;
        window = GET_PARENT(window);
    }
    *xReturn = x;
    *yReturn = y;
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
    int x, y, width, height;
    int sourceAbsX = 0;
    int sourceAbsY = 0;
    int destAbsX = 0;
    int destAbsY = 0;
    windowAbsoluteOrigin(sourceWindow, &sourceAbsX, &sourceAbsY);
    windowAbsoluteOrigin(destinationWindow, &destAbsX, &destAbsY);
    int currX = sourceAbsX + sourceX - destAbsX;
    int currY = sourceAbsY + sourceY - destAbsY;
    *destinationXReturn = currX;
    *destinationYReturn = currY;
    if (childReturn) {
        *childReturn = None;
        // Get the first child which contains x and y
        Window *children = GET_CHILDREN(destinationWindow);
        size_t i;
        for (i = 0; i < GET_WINDOW_STRUCT(destinationWindow)->children.length;
             i++) {
            GET_WINDOW_POS(children[i], x, y);
            GET_WINDOW_DIMS(children[i], width, height);
            if (x <= currX && x + width > currX && y <= currY &&
                y + height > currY) {
                *childReturn = children[i];
                break;
            }
        }
    }
    return True;
}

static Bool isNetPropertyWithNoWindowSideEffects(Atom property)
{
    return property == _NET_WM_USER_TIME ||
           property == _NET_WM_USER_TIME_WINDOW ||
           property == _NET_WM_HANDLED_ICONS ||
           property == _NET_WM_ICON_GEOMETRY ||
           property == _NET_WM_ALLOWED_ACTIONS || property == _NET_WM_STRUT ||
           property == _NET_WM_STRUT_PARTIAL;
}

int XChangeProperty(Display *display,
                    Window window,
                    Atom property,
                    Atom type,
                    int format,
                    int mode,
                    _Xconst unsigned char *data,
                    int numberOfElements)
{
    // https://tronche.com/gui/x/xlib/window-information/XChangeProperty.html
    SET_X_SERVER_REQUEST(display, X_ChangeProperty);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (numberOfElements < 0) {
        LOG("Bad parameter: XChangeProperty got negative element count %d "
            "for property %lu (%s), type %lu (%s), format %d, mode %d.\n",
            numberOfElements, property, getAtomName(display, property), type,
            getAtomName(display, type), format, mode);
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    if (format != 8 && format != 16 && format != 32) {
        LOG("Bad parameter: XChangeProperty got invalid format %d for "
            "property %lu (%s), type %lu (%s), elements %d, mode %d.\n",
            format, property, getAtomName(display, property), type,
            getAtomName(display, type), numberOfElements, mode);
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    if (!isNetPropertyWithNoWindowSideEffects(property)) {
        LOG("Changing window property %lu (%s).\n", property,
            getAtomName(display, property));
    }
    if (!isValidAtom(property)) {
        handleError(0, display, property, 0, BadAtom, 0);
        return 0;
    }
    if (!isValidAtom(type)) {
        handleError(0, display, type, 0, BadAtom, 0);
        return 0;
    }
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    WindowProperty *windowProperty =
        findProperty(&windowStruct->properties, property, NULL);
    unsigned char *combinedData = NULL;
    unsigned char *previousData = NULL;
    unsigned int previousDataLength = 0;
    Bool propertyIsNew = !windowProperty;
    size_t dataTypeSize = format == 8
                              ? sizeof(char)
                              : (format == 16 ? sizeof(short) : sizeof(long));
    if (!propertyIsNew) {
        previousDataLength = windowProperty->dataLength;
        previousData = windowProperty->data;
    } else {
        windowProperty = malloc(sizeof(WindowProperty));
        if (!windowProperty) {
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        if (!insertArray(&windowStruct->properties, windowProperty)) {
            free(windowProperty);
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        windowProperty->dataFormat = format;
        windowProperty->data = NULL;
        windowProperty->dataLength = 0;
        windowProperty->property = property;
        windowProperty->type = type;
    }
    switch (mode) {
    case PropModeAppend:
    case PropModePrepend:
        if (format != windowProperty->dataFormat ||
            type != windowProperty->type) {
            if (propertyIsNew) {
                removeArray(&windowStruct->properties,
                            windowStruct->properties.length - 1, False);
                free(windowProperty);
            }
            handleError(0, display, None, 0, BadMatch, 0);
            return 0;
        }

        /* Checked add then checked multiply: a malicious caller can craft
         * previousDataLength + numberOfElements to wrap u32, undersizing
         * the allocation before the memcpy of dataTypeSize bytes per
         * element would write past the buffer. The element count is then
         * stored back into an unsigned int dataLength field, so it must
         * fit in UINT_MAX as well.
         */
        if ((size_t) numberOfElements >
                SIZE_MAX - (size_t) previousDataLength ||
            (size_t) previousDataLength + (size_t) numberOfElements >
                UINT_MAX ||
            (size_t) previousDataLength + (size_t) numberOfElements >
                SIZE_MAX / dataTypeSize) {
            if (propertyIsNew) {
                removeArray(&windowStruct->properties,
                            windowStruct->properties.length - 1, False);
                free(windowProperty);
            }
            handleError(0, display, None, 0, BadAlloc, 0);
            return 0;
        }
        combinedData = malloc(dataTypeSize * ((size_t) previousDataLength +
                                              (size_t) numberOfElements));
        if (!combinedData) {
            if (propertyIsNew) {
                removeArray(&windowStruct->properties,
                            windowStruct->properties.length - 1, False);
                free(windowProperty);
            }
            LOG("Out of memory: Failed to allocate space for combined data "
                "in XChangeProperty!\n");
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        if (mode == PropModeAppend) {
            if (previousData) {
                memcpy(combinedData, previousData,
                       dataTypeSize * previousDataLength);
            }
            memcpy(combinedData + dataTypeSize * previousDataLength, data,
                   dataTypeSize * numberOfElements);
        } else {
            memcpy(combinedData, data, dataTypeSize * numberOfElements);
            if (previousData) {
                memcpy(combinedData + dataTypeSize * numberOfElements,
                       previousData, dataTypeSize * previousDataLength);
            }
        }
        windowProperty->data = combinedData;
        windowProperty->dataLength = numberOfElements + previousDataLength;
        break;
    case PropModeReplace: {
        /* Same overflow surface as the append/prepend path: a large
         * numberOfElements times dataTypeSize (up to 8 for format == 32)
         * can wrap size_t before malloc, and would then drive memcpy past
         * the undersized buffer.
         *
         * Allocate into a temporary so an OOM here leaves the original
         * windowProperty->data intact instead of orphaning it with the
         * pointer overwritten to NULL. */
        unsigned char *newData = NULL;
        if (numberOfElements > 0) {
            if ((size_t) numberOfElements > SIZE_MAX / dataTypeSize) {
                if (propertyIsNew) {
                    removeArray(&windowStruct->properties,
                                windowStruct->properties.length - 1, False);
                    free(windowProperty);
                }
                handleError(0, display, None, 0, BadAlloc, 0);
                return 0;
            }
            newData = malloc(dataTypeSize * (size_t) numberOfElements);
            if (!newData) {
                if (propertyIsNew) {
                    removeArray(&windowStruct->properties,
                                windowStruct->properties.length - 1, False);
                    free(windowProperty);
                }
                LOG("Out of memory: Failed to allocate space for data in "
                    "XChangeProperty!\n");
                handleOutOfMemory(0, display, 0, 0);
                return 0;
            }
            memcpy(newData, data, dataTypeSize * (size_t) numberOfElements);
        }
        windowProperty->data = newData;
        windowProperty->dataLength = (unsigned int) numberOfElements;
        windowProperty->property = property;
        windowProperty->type = type;
        windowProperty->dataFormat = format;
        break;
    }
    default:
        if (propertyIsNew) {
            removeArray(&windowStruct->properties,
                        windowStruct->properties.length - 1, False);
            free(windowProperty);
        }
        LOG("Bad parameter: Got unknown mode %d in XChangeProperty!\n", mode);
        handleError(0, display, None, 0, BadMatch, 0);
        return 0;
    }
    if (previousData) {
        free(previousData);
    }
    if (property == _NET_WM_ICON && format == 32 && numberOfElements >= 2) {
        // Find the icon with the highest resolution
        unsigned long *pixelData = (unsigned long *) windowProperty->data;
        unsigned long *icons[20];
        int i = 0, bestIcon = 0;
        size_t offset = 0;
        unsigned long w;
        unsigned long h;
        while (i < 20 && offset + 2 <= (size_t) numberOfElements) {
            w = pixelData[0];
            h = pixelData[1];
            /* _NET_WM_ICON pixel data is CARD32 width, height, then w*h
             * BGRA pixels. Both w and h come straight from the client, so
             * w*h plus the two-word header must be guarded against
             * size_t wrap before it feeds the bounds check below.
             */
            if (w > 0 && h > (SIZE_MAX - 2) / w)
                break;
            size_t iconLength = 2 + (size_t) w * (size_t) h;
            if (offset + iconLength > (size_t) numberOfElements)
                break;
            /* SDL_CreateRGBSurfaceFrom takes width, height and pitch as
             * signed int. Reject candidates that would not fit before they
             * can win the bestIcon selection: otherwise an oversized icon
             * followed by a smaller valid icon would leave us with no
             * usable surface at all.
             */
            Bool fitsSdl = w != 0 && h != 0 && w <= (unsigned long) INT_MAX &&
                           h <= (unsigned long) INT_MAX &&
                           w <= (unsigned long) INT_MAX / 4;
            if (fitsSdl) {
                icons[i] = pixelData;
                if (i == 0 || w > icons[bestIcon][0] || h > icons[bestIcon][1])
                    bestIcon = i;
                i++;
            }
            pixelData += iconLength;
            offset += iconLength;
        }
        if (i > 0) {
            w = icons[bestIcon][0];
            h = icons[bestIcon][1];
            SDL_Surface *icon = SDL_CreateRGBSurfaceFrom(
                &icons[bestIcon][2], (int) w, (int) h, 32, (int) (w * 4),
                0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
            if (windowStruct->icon)
                SDL_FreeSurface(windowStruct->icon);
            windowStruct->icon = icon;
            if (IS_MAPPED_TOP_LEVEL_WINDOW(window))
                SDL_SetWindowIcon(windowStruct->sdlWindow, icon);
        }
    }
    /* React to _NET_WM_WINDOW_TYPE: menu/popup/tooltip/splash/dock types
     * normally come from a borderless window-manager decoration request. We
     * have no WM, so the closest equivalent is dropping SDL's border. */
    if (property == _NET_WM_WINDOW_TYPE && format == 32 &&
        IS_MAPPED_TOP_LEVEL_WINDOW(window) && windowProperty->dataLength > 0) {
        Atom *types = (Atom *) windowProperty->data;
        Bool wantBorderless = False;
        for (unsigned int t = 0; t < windowProperty->dataLength; t++) {
            Atom ty = types[t];
            if (ty == _NET_WM_WINDOW_TYPE_MENU ||
                ty == _NET_WM_WINDOW_TYPE_DROPDOWN_MENU ||
                ty == _NET_WM_WINDOW_TYPE_POPUP_MENU ||
                ty == _NET_WM_WINDOW_TYPE_TOOLTIP ||
                ty == _NET_WM_WINDOW_TYPE_SPLASH ||
                ty == _NET_WM_WINDOW_TYPE_DOCK ||
                ty == _NET_WM_WINDOW_TYPE_NOTIFICATION ||
                ty == _NET_WM_WINDOW_TYPE_COMBO) {
                wantBorderless = True;
                break;
            }
        }
        if (wantBorderless) {
            SDL_SetWindowBordered(windowStruct->sdlWindow, SDL_FALSE);
        }
    }
    /* Motif, GTK, and Qt set their window titles via XChangeProperty on
     * WM_NAME / _NET_WM_NAME rather than calling XStoreName. Route any
     * format-8 string-encoded write through XStoreName so SDL's window
     * title reflects what the client just asked for. Non-string property
     * types (e.g. atom lists) pass through untouched. */
    if (format == 8 && (property == XA_WM_NAME || property == _NET_WM_NAME)) {
        static Atom cachedUtf8 = None;
        static Atom cachedCompound = None;
        if (cachedUtf8 == None)
            cachedUtf8 = XInternAtom(display, "UTF8_STRING", False);
        if (cachedCompound == None)
            cachedCompound = XInternAtom(display, "COMPOUND_TEXT", False);
        if (type == XA_STRING || type == cachedUtf8 || type == cachedCompound) {
            /* Property bytes are not required to be NUL-terminated. */
            size_t copyLen = (size_t) numberOfElements;
            char *titleBuf = malloc(copyLen + 1);
            if (titleBuf) {
                if (copyLen > 0 && windowProperty->data) {
                    memcpy(titleBuf, windowProperty->data, copyLen);
                }
                titleBuf[copyLen] = '\0';
                XStoreName(display, window, titleBuf);
                free(titleBuf);
            }
        }
    }
    postEvent(display, window, PropertyNotify, property, PropertyNewValue);
    return 1;
}

int XDeleteProperty(Display *display, Window window, Atom property)
{
    // https://tronche.com/gui/x/xlib/window-information/XDeleteProperty.html
    SET_X_SERVER_REQUEST(display, X_DeleteProperty);
    TYPE_CHECK(window, WINDOW, display, 0);
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!isValidAtom(property)) {
        handleError(0, display, property, 0, BadAtom, 0);
        return 0;
    }
    if (property == _NET_WM_ICON) {
        if (windowStruct->icon) {
            SDL_FreeSurface(windowStruct->icon);
            windowStruct->icon = NULL;
            if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
                SDL_SetWindowIcon(windowStruct->sdlWindow, NULL);
            }
        }
    }
    size_t index;
    WindowProperty *windowProperty =
        findProperty(&windowStruct->properties, property, &index);
    if (windowProperty) {
        removeArray(&windowStruct->properties, index, False);
        freeWindowProperty(windowProperty);
        postEvent(display, window, PropertyNotify, property, PropertyDelete);
    }
    return 1;
}

int XGetWindowProperty(Display *display,
                       Window window,
                       Atom property,
                       long long_offset,
                       long long_length,
                       Bool delete,
                       Atom req_type,
                       Atom *actual_type_return,
                       int *actual_format_return,
                       unsigned long *numberOfItems_return,
                       unsigned long *bytes_after_return,
                       unsigned char **prop_return)
{
    // https://tronche.com/gui/x/xlib/window-information/XGetWindowProperty.html
    SET_X_SERVER_REQUEST(display, X_GetProperty);
    TYPE_CHECK(window, WINDOW, display, BadWindow);
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!isValidAtom(property)) {
        handleError(0, display, property, 0, BadAtom, 0);
        return BadAtom;
    }
    WindowProperty *windowProperty =
        findProperty(&windowStruct->properties, property, NULL);
    if (windowProperty) {
        *actual_type_return = windowProperty->type;
        *actual_format_return = windowProperty->dataFormat;
        size_t storageTypeSize =
            windowProperty->dataFormat == 8
                ? sizeof(char)
                : (windowProperty->dataFormat == 16 ? sizeof(short)
                                                    : sizeof(long));
        if (req_type == AnyPropertyType || req_type == windowProperty->type) {
            /* Xlib specifies long_offset and long_length in 32-bit units,
             * independent of the property format. */
            size_t wireTypeSize = (size_t) windowProperty->dataFormat / 8;
            size_t itemsPerLong = 4 / wireTypeSize;
            size_t totalItems = windowProperty->dataLength;
            if (long_offset < 0 || long_length < 0 ||
                (size_t) long_offset > SIZE_MAX / itemsPerLong ||
                (size_t) long_length > SIZE_MAX / itemsPerLong) {
                handleError(0, display, None, 0, BadValue, 0);
                return BadValue;
            }
            size_t offsetItems = (size_t) long_offset * itemsPerLong;
            if (offsetItems > totalItems) {
                handleError(0, display, None, 0, BadValue, 0);
                return BadValue;
            }
            size_t remainingItems = totalItems - offsetItems;
            size_t requestItems = (size_t) long_length * itemsPerLong;
            size_t returnItems = MIN(remainingItems, requestItems);
            /* dataReturnSize then dataReturnSize+1 must both fit in size_t. */
            if (returnItems > (SIZE_MAX - 1) / storageTypeSize) {
                handleError(0, display, None, 0, BadAlloc, 0);
                return BadAlloc;
            }
            size_t dataReturnSize = returnItems * storageTypeSize;
            *prop_return = malloc(dataReturnSize + 1);
            if (!*prop_return) {
                LOG("Out of memory: Failed to allocate space for "
                    "the return value in XGetWindowProperty!\n");
                handleOutOfMemory(0, display, 0, 0);
                return BadAlloc;
            }
            memcpy(*prop_return,
                   windowProperty->data + offsetItems * storageTypeSize,
                   dataReturnSize);
            (*prop_return)[dataReturnSize] = '\0';
            *numberOfItems_return = returnItems;
            *bytes_after_return = (remainingItems - returnItems) * wireTypeSize;
            if (delete && *bytes_after_return == 0) {
                XDeleteProperty(display, window, property);
            }
        } else {
            *bytes_after_return = (unsigned long) windowProperty->dataLength *
                                  ((size_t) windowProperty->dataFormat / 8);
            *numberOfItems_return = 0;
        }
    } else if (property == _XSETTINGS_SETTINGS_ATOM &&
               window == SCREEN_WINDOW &&
               (req_type == AnyPropertyType ||
                req_type == _XSETTINGS_SETTINGS_ATOM)) {
        /* Publish the minimum settings GTK reads: a font name, the DPI in
         * fixed-point 1024ths of a point, and an icon theme name. */
        static const char *fontName = "Sans 10";
        static const char *iconTheme = "default";
        /* Each setting:
         *   CARD8 type, CARD8 unused, CARD16 name_len, char name[name_len],
         *   pad to 4, CARD32 last_change_serial, type-specific body.
         * Header: CARD8 byte_order, 3*CARD8 unused, CARD32 serial,
         *         CARD32 n_settings. */
        size_t headerBytes = 12;
        struct {
            const char *name;
            int type; /* 0=int, 1=string */
            int intValue;
            const char *strValue;
        } items[] = {
            {.name = "Gtk/FontName",
             .type = 1,
             .intValue = 0,
             .strValue = fontName},
            {.name = "Xft/DPI",
             .type = 0,
             .intValue = 96 * 1024,
             .strValue = NULL},
            {.name = "Net/IconThemeName",
             .type = 1,
             .intValue = 0,
             .strValue = iconTheme},
        };
        const size_t nItems = sizeof(items) / sizeof(items[0]);
        size_t total = headerBytes;
        for (size_t i = 0; i < nItems; i++) {
            size_t nameLen = strlen(items[i].name);
            size_t entry = 1 + 1 + 2 + nameLen;
            entry = (entry + 3) & ~3u;
            entry += 4; /* serial */
            if (items[i].type == 0) {
                entry += 4;
            } else {
                size_t valLen = strlen(items[i].strValue);
                size_t v = 4 + valLen;
                v = (v + 3) & ~3u;
                entry += v;
            }
            total += entry;
        }
        unsigned char *out = malloc(total + 1);
        if (!out) {
            handleOutOfMemory(0, display, 0, 0);
            return BadAlloc;
        }
        unsigned char *p = out;
        *p++ = 0; /* LSBFirst */
        *p++ = 0;
        *p++ = 0;
        *p++ = 0;
        uint32_t serial = 1;
        memcpy(p, &serial, 4);
        p += 4;
        uint32_t nset = (uint32_t) nItems;
        memcpy(p, &nset, 4);
        p += 4;
        for (size_t i = 0; i < nItems; i++) {
            *p++ = (unsigned char) items[i].type;
            *p++ = 0;
            uint16_t nameLen = (uint16_t) strlen(items[i].name);
            memcpy(p, &nameLen, 2);
            p += 2;
            memcpy(p, items[i].name, nameLen);
            p += nameLen;
            size_t pad = (4 - (nameLen & 3)) & 3;
            memset(p, 0, pad);
            p += pad;
            uint32_t lastChange = 0;
            memcpy(p, &lastChange, 4);
            p += 4;
            if (items[i].type == 0) {
                int32_t iv = (int32_t) items[i].intValue;
                memcpy(p, &iv, 4);
                p += 4;
            } else {
                uint32_t valLen = (uint32_t) strlen(items[i].strValue);
                memcpy(p, &valLen, 4);
                p += 4;
                memcpy(p, items[i].strValue, valLen);
                p += valLen;
                size_t spad = (4 - (valLen & 3)) & 3;
                memset(p, 0, spad);
                p += spad;
            }
        }
        out[total] = '\0';
        *actual_type_return = _XSETTINGS_SETTINGS_ATOM;
        *actual_format_return = 8;
        *numberOfItems_return = total;
        *bytes_after_return = 0;
        *prop_return = out;
    } else if (property == _MOTIF_WM_HINTS &&
               (req_type == AnyPropertyType || req_type == _MOTIF_WM_HINTS)) {
        /* Synthesize a default hints reply when the app has not set one,
         * so Motif/Tk/AWT clients that probe this property do not abort.
         * X11 format=32 uses native long-sized client-side storage even
         * though the wire encoding is 32 bits. */
        enum {
            MWM_HINTS_FUNCTIONS = (1L << 0),
            MWM_HINTS_DECORATIONS = (1L << 1),
            MWM_HINTS_INPUT_MODE = (1L << 2),
            MWM_FUNC_ALL = (1L << 0),
            MWM_DECOR_ALL = (1L << 0),
            MWM_INPUT_MODELESS = 0,
        };
        size_t bytes = 5 * sizeof(long);
        long *values = malloc(bytes + 1);
        if (!values) {
            handleOutOfMemory(0, display, 0, 0);
            return BadAlloc;
        }
        values[0] =
            MWM_HINTS_FUNCTIONS | MWM_HINTS_DECORATIONS | MWM_HINTS_INPUT_MODE;
        values[1] = MWM_FUNC_ALL;
        values[2] = MWM_DECOR_ALL;
        values[3] = MWM_INPUT_MODELESS;
        values[4] = 0;
        *actual_type_return = _MOTIF_WM_HINTS;
        *actual_format_return = 32;
        *numberOfItems_return = 5;
        *bytes_after_return = 0;
        *prop_return = (unsigned char *) values;
    } else {
        *actual_type_return = None;
        *actual_format_return = 0;
        *bytes_after_return = 0;
        *numberOfItems_return = 0;
    }
    return Success;
}

int XRaiseWindow(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XRaiseWindow.html
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
        SDL_RaiseWindow(GET_WINDOW_STRUCT(window)->sdlWindow);
    }
    moveChildToTop(window);
    postVisibilityForWindowAndSiblings(display, window);
    return 1;
}

int XLowerWindow(Display *display, Window window)
{
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    TYPE_CHECK(window, WINDOW, display, 0);
    moveChildToBottom(window);
    postVisibilityForWindowAndSiblings(display, window);
    return 1;
}

int XRestackWindows(Display *display, Window *windows, int nwindows)
{
    SET_X_SERVER_REQUEST(display, X_ConfigureWindow);
    if (nwindows <= 0)
        return 1;
    for (int i = 0; i < nwindows; i++) {
        TYPE_CHECK(windows[i], WINDOW, display, 0);
    }
    for (int i = nwindows - 1; i >= 0; i--) {
        moveChildToTop(windows[i]);
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
    if (border_pixmap != CopyFromParent) {
        TYPE_CHECK(border_pixmap, PIXMAP, display, 0);
    }
    if (window != SCREEN_WINDOW) {
        GET_WINDOW_STRUCT(window)->borderPixmap = border_pixmap;
    }
    return 1;
}

int XSetWindowColormap(Display *display, Window window, Colormap colormap)
{
    // https://tronche.com/gui/x/xlib/window/XSetWindowColormap.html
    SET_X_SERVER_REQUEST(display, X_ChangeWindowAttributes);
    TYPE_CHECK(window, WINDOW, display, 0);
    TYPE_CHECK(colormap, COLORMAP, display, 0);
    if (window != SCREEN_WINDOW) {
        GET_WINDOW_STRUCT(window)->colormap = colormap;
    }
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
        if (HAS_VALUE(valueMask, CWBackPixel)) {
            XSetWindowBackground(display, window, attributes->background_pixel);
        }
        if (HAS_VALUE(valueMask, CWColormap)) {
            XSetWindowColormap(display, window, attributes->colormap);
        }
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
        if (HAS_VALUE(valueMask, CWBitGravity)) {
            GET_WINDOW_STRUCT(window)->bitGravity = attributes->bit_gravity;
        }
        if (HAS_VALUE(valueMask, CWWinGravity)) {
            GET_WINDOW_STRUCT(window)->winGravity = attributes->win_gravity;
        }
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
    if (window != SCREEN_WINDOW) {
        GET_WINDOW_STRUCT(window)->borderWidth = width;
        if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
            SDL_SetWindowBordered(GET_WINDOW_STRUCT(window)->sdlWindow,
                                  width == 0 ? SDL_FALSE : SDL_TRUE);
        }
    }
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
