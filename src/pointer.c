#include "X11/Xlib.h"
#include <SDL2/SDL.h>
#include <string.h>
#include "window.h"
#include "events.h"
#include "display.h"

unsigned int currentEventMask = ~0;
Bool mouseFrozen = False;
Bool keyboardFrozen = False;

typedef struct {
    Bool active;
    Window grab_window;
    Bool owner_events;
    unsigned int event_mask;
    Window confine_to;
    Cursor cursor;
    int pointer_mode;
    int keyboard_mode;
} PointerGrabState;

static PointerGrabState pointerGrab;

int XWarpPointer(Display *display,
                 Window src_window,
                 Window dest_window,
                 int src_x,
                 int src_y,
                 unsigned int src_width,
                 unsigned int src_height,
                 int dest_x,
                 int dest_y)
{
    // https://tronche.com/gui/x/xlib/input/XWarpPointer.html
    SET_X_SERVER_REQUEST(display, X_WarpPointer);
    int curr_x = 0, curr_y = 0;
    if (dest_window == None) {
#if SDL_VERSION_ATLEAST(2, 0, 4)
        SDL_GetGlobalMouseState(&curr_x, &curr_y);
#else
        SDL_GetMouseState(&curr_x, &curr_y);
#endif
    } else {
        if (src_window != None) {
            int gx, gy;
#if SDL_VERSION_ATLEAST(2, 0, 4)
            SDL_GetGlobalMouseState(&gx, &gy);
#else
            SDL_GetMouseState(&gx, &gy);
#endif
            /* Spec: the source rectangle is in src_window coordinates, and a
             * zero width/height means "to the edge of src_window". */
            int local_x, local_y;
            XTranslateCoordinates(display, SCREEN_WINDOW, src_window, gx, gy,
                                  &local_x, &local_y, NULL);
            unsigned int w = src_width, h = src_height;
            if (w == 0 || h == 0) {
                unsigned int win_w, win_h;
                GET_WINDOW_DIMS(src_window, win_w, win_h);
                if (w == 0)
                    w = (src_x >= 0 && (unsigned int) src_x < win_w)
                            ? win_w - (unsigned int) src_x
                            : 0;
                if (h == 0)
                    h = (src_y >= 0 && (unsigned int) src_y < win_h)
                            ? win_h - (unsigned int) src_y
                            : 0;
            }
            /* X rect containment is half-open: x <= p < x + width. Use an
             * unsigned-difference compare to avoid signed overflow when
             * src_width approaches INT_MAX. */
            if (local_x < src_x || (unsigned int) (local_x - src_x) >= w ||
                local_y < src_y || (unsigned int) (local_y - src_y) >= h) {
                return 1;
            }
        }
        if (dest_window == SCREEN_WINDOW) {
#if SDL_VERSION_ATLEAST(2, 0, 4)
            SDL_GetGlobalMouseState(&curr_x, &curr_y);
#else
            SDL_GetMouseState(&curr_x, &curr_y);
#endif
        } else {
            XTranslateCoordinates(display, SCREEN_WINDOW, dest_window, 0, 0,
                                  &curr_x, &curr_y, NULL);
        }
    }
#if SDL_VERSION_ATLEAST(2, 0, 4)
    if (SDL_WarpMouseGlobal(curr_x + dest_x, curr_y + dest_y) != 0) {
        LOG("Warning: SDL_WarpMouseGlobal failed: %s", SDL_GetError());
    }
#else
    SDL_WarpMouseInWindow(SDL_GetMouseFocus(), curr_x + dest_x,
                          curr_y + dest_y);
#endif
    return 1;
}

Bool XQueryPointer(Display *display,
                   Window window,
                   Window *root_return,
                   Window *child_return,
                   int *root_x_return,
                   int *root_y_return,
                   int *win_x_return,
                   int *win_y_return,
                   unsigned int *mask_return)
{
    // https://tronche.com/gui/x/xlib/window-information/XQueryPointer.html
    SET_X_SERVER_REQUEST(display, X_QueryPointer);
    *root_return = SCREEN_WINDOW;
    SDL_GetMouseState(root_x_return, root_y_return);
    XTranslateCoordinates(display, SCREEN_WINDOW, window, *root_x_return,
                          *root_y_return, win_x_return, win_y_return,
                          child_return);
    *mask_return = convertModifierState(SDL_GetModState());
    return True;
}

int XGrabPointer(Display *display,
                 Window grab_window,
                 Bool owner_events,
                 unsigned int event_mask,
                 int pointer_mode,
                 int keyboard_mode,
                 Window confine_to,
                 Cursor cursor,
                 Time time)
{
    // https://tronche.com/gui/x/xlib/input/XGrabPointer.html
    SET_X_SERVER_REQUEST(display, X_GrabPointer);
    /* Xlib status codes: GrabSuccess == 0, AlreadyGrabbed == 1,
     * GrabInvalidTime == 2, GrabNotViewable == 3, GrabFrozen == 4.
     * Returning a bare `1` for success would be read by Xlib clients as
     * AlreadyGrabbed and route them down the wrong recovery path.
     * SDL_SetRelativeMouseMode failure is benign here: dummy/headless
     * drivers reject it, and the X grab state still applies to event
     * routing whether or not SDL captures the pointer.
     */
    if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0) {
        LOG("SDL_SetRelativeMouseMode failed in XGrabPointer: %s",
            SDL_GetError());
    }
    pointerGrab.active = True;
    pointerGrab.grab_window = grab_window;
    pointerGrab.owner_events = owner_events;
    pointerGrab.event_mask = event_mask;
    pointerGrab.confine_to = confine_to;
    pointerGrab.cursor = cursor;
    pointerGrab.pointer_mode = pointer_mode;
    pointerGrab.keyboard_mode = keyboard_mode;
    currentEventMask = event_mask;
    mouseFrozen = pointer_mode == GrabModeSync;
    keyboardFrozen = keyboard_mode == GrabModeSync;
    if (confine_to != None && IS_TYPE(confine_to, WINDOW) &&
        IS_MAPPED_TOP_LEVEL_WINDOW(confine_to))
        SDL_SetWindowGrab(GET_WINDOW_STRUCT(confine_to)->sdlWindow, SDL_TRUE);
    if (cursor != None)
        XDefineCursor(display, grab_window, cursor);
    postCrossingEvent(display, grab_window, EnterNotify, NotifyGrab,
                      NotifyAncestor, convertModifierState(SDL_GetModState()));
    return GrabSuccess;
}

int XUngrabPointer(Display *display, Time time)
{
    // https://tronche.com/gui/x/xlib/input/XUngrabPointer.html
    SET_X_SERVER_REQUEST(display, X_UngrabPointer);
    if (SDL_GetRelativeMouseMode() == SDL_TRUE) {
        if (SDL_SetRelativeMouseMode(SDL_FALSE) != 0) {
            LOG("SDL_SetRelativeMouseMode failed in XUngrabPointer: %s",
                SDL_GetError());
            return 0;
        }
    }
    if (pointerGrab.confine_to != None &&
        IS_TYPE(pointerGrab.confine_to, WINDOW) &&
        IS_MAPPED_TOP_LEVEL_WINDOW(pointerGrab.confine_to)) {
        SDL_SetWindowGrab(GET_WINDOW_STRUCT(pointerGrab.confine_to)->sdlWindow,
                          SDL_FALSE);
    }
    if (pointerGrab.active) {
        postCrossingEvent(display, pointerGrab.grab_window, LeaveNotify,
                          NotifyUngrab, NotifyAncestor,
                          convertModifierState(SDL_GetModState()));
    }
    memset(&pointerGrab, 0, sizeof(pointerGrab));
    currentEventMask = ~0;
    mouseFrozen = False;
    keyboardFrozen = False;
    return 1;
}
