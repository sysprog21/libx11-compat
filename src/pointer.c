#include "X11/Xlib.h"
#include <SDL2/SDL.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "window-internal.h"
#include "window.h"
#include "events.h"
#include "display.h"
#include "errors.h"

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
    Cursor savedCursor; /* cursor before XDefineCursor override */
    Bool cursorWasOverridden;
    int pointer_mode;
    int keyboard_mode;
    Bool passive;
} PointerGrabState;

static PointerGrabState pointerGrab;

typedef struct ButtonGrab {
    unsigned int button;
    unsigned int modifiers;
    Window grab_window;
    Bool owner_events;
    unsigned int event_mask;
    int pointer_mode;
    int keyboard_mode;
    Window confine_to;
    Cursor cursor;
    struct ButtonGrab *next;
} ButtonGrab;

static ButtonGrab *buttonGrabs = NULL;

Window getGrabbedPointerWindow(void)
{
    return pointerGrab.active ? pointerGrab.grab_window : None;
}

Bool getPointerGrabOwnerEvents(void)
{
    return pointerGrab.active ? pointerGrab.owner_events : False;
}

unsigned int getPointerGrabEventMask(void)
{
    return pointerGrab.active ? pointerGrab.event_mask : 0;
}

static Bool windowContainsDescendant(Window ancestor, Window descendant)
{
    while (descendant != None && IS_TYPE(descendant, WINDOW)) {
        if (descendant == ancestor)
            return True;
        descendant = GET_PARENT(descendant);
    }
    return False;
}

static void installPointerGrab(Display *display,
                               Window grab_window,
                               Bool owner_events,
                               unsigned int event_mask,
                               int pointer_mode,
                               int keyboard_mode,
                               Window confine_to,
                               Cursor cursor,
                               Bool passive)
{
    pointerGrab.active = True;
    pointerGrab.grab_window = grab_window;
    pointerGrab.owner_events = owner_events;
    pointerGrab.event_mask = event_mask;
    pointerGrab.confine_to = confine_to;
    pointerGrab.cursor = cursor;
    pointerGrab.pointer_mode = pointer_mode;
    pointerGrab.keyboard_mode = keyboard_mode;
    pointerGrab.passive = passive;
    currentEventMask = event_mask;
    mouseFrozen = pointer_mode == GrabModeSync;
    keyboardFrozen = keyboard_mode == GrabModeSync;
    if (confine_to != None && IS_TYPE(confine_to, WINDOW) &&
        IS_MAPPED_TOP_LEVEL_WINDOW(confine_to))
        SDL_SetWindowGrab(GET_WINDOW_STRUCT(confine_to)->sdlWindow, SDL_TRUE);
    pointerGrab.cursorWasOverridden = False;
    pointerGrab.savedCursor = None;
    if (cursor != None && IS_TYPE(grab_window, WINDOW)) {
        /* Capture the window's previous cursor so XUngrabPointer can restore
         * it; XDefineCursor otherwise persists the grab cursor after ungrab.
         */
        pointerGrab.savedCursor = GET_WINDOW_STRUCT(grab_window)->cursor;
        pointerGrab.cursorWasOverridden = True;
        XDefineCursor(display, grab_window, cursor);
    }
    postCrossingEvent(display, grab_window, EnterNotify, NotifyGrab,
                      NotifyAncestor, convertModifierState(SDL_GetModState()));
}

Bool activatePassiveButtonGrab(Display *display,
                               Window root,
                               int root_x,
                               int root_y,
                               unsigned int button,
                               unsigned int state)
{
    if (pointerGrab.active)
        return False;

    (void) display;
    Window containing = getContainingWindow(root, root_x, root_y);

    unsigned int modifiers =
        state & (ShiftMask | LockMask | ControlMask | Mod1Mask | Mod2Mask |
                 Mod3Mask | Mod4Mask | Mod5Mask);
    for (ButtonGrab *grab = buttonGrabs; grab; grab = grab->next) {
        Bool buttonMatch = grab->button == AnyButton || grab->button == button;
        Bool modMatch =
            grab->modifiers == AnyModifier || grab->modifiers == modifiers;
        if (buttonMatch && modMatch &&
            windowContainsDescendant(grab->grab_window, containing)) {
            installPointerGrab(display, grab->grab_window, grab->owner_events,
                               grab->event_mask, grab->pointer_mode,
                               grab->keyboard_mode, grab->confine_to,
                               grab->cursor, True);
            return True;
        }
    }
    return False;
}

Bool pointerGrabIsPassive(void)
{
    return pointerGrab.active && pointerGrab.passive;
}

static void clearPointerGrab(Display *display)
{
    if (pointerGrab.confine_to != None &&
        IS_TYPE(pointerGrab.confine_to, WINDOW) &&
        IS_MAPPED_TOP_LEVEL_WINDOW(pointerGrab.confine_to)) {
        SDL_SetWindowGrab(GET_WINDOW_STRUCT(pointerGrab.confine_to)->sdlWindow,
                          SDL_FALSE);
    }
    if (pointerGrab.cursorWasOverridden && pointerGrab.grab_window != None &&
        IS_TYPE(pointerGrab.grab_window, WINDOW)) {
        XDefineCursor(display, pointerGrab.grab_window,
                      pointerGrab.savedCursor);
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
    clearActivePointerWindow();
}

static void replacePointerGrab(Display *display,
                               Window grab_window,
                               Bool owner_events,
                               unsigned int event_mask,
                               int pointer_mode,
                               int keyboard_mode,
                               Window confine_to,
                               Cursor cursor)
{
    if (pointerGrab.grab_window != grab_window) {
        clearPointerGrab(display);
        installPointerGrab(display, grab_window, owner_events, event_mask,
                           pointer_mode, keyboard_mode, confine_to, cursor,
                           False);
        return;
    }

    if (pointerGrab.confine_to != confine_to) {
        if (pointerGrab.confine_to != None &&
            IS_TYPE(pointerGrab.confine_to, WINDOW) &&
            IS_MAPPED_TOP_LEVEL_WINDOW(pointerGrab.confine_to)) {
            SDL_SetWindowGrab(
                GET_WINDOW_STRUCT(pointerGrab.confine_to)->sdlWindow,
                SDL_FALSE);
        }
        if (confine_to != None && IS_TYPE(confine_to, WINDOW) &&
            IS_MAPPED_TOP_LEVEL_WINDOW(confine_to)) {
            SDL_SetWindowGrab(GET_WINDOW_STRUCT(confine_to)->sdlWindow,
                              SDL_TRUE);
        }
    }
    if (cursor != None && !pointerGrab.cursorWasOverridden) {
        pointerGrab.savedCursor = GET_WINDOW_STRUCT(grab_window)->cursor;
        pointerGrab.cursorWasOverridden = True;
    }
    if (cursor != None) {
        XDefineCursor(display, grab_window, cursor);
    } else if (pointerGrab.cursorWasOverridden) {
        XDefineCursor(display, grab_window, pointerGrab.savedCursor);
        pointerGrab.cursorWasOverridden = False;
        pointerGrab.savedCursor = None;
    }

    pointerGrab.owner_events = owner_events;
    pointerGrab.event_mask = event_mask;
    pointerGrab.confine_to = confine_to;
    pointerGrab.cursor = cursor;
    pointerGrab.pointer_mode = pointer_mode;
    pointerGrab.keyboard_mode = keyboard_mode;
    pointerGrab.passive = False;
    currentEventMask = event_mask;
    mouseFrozen = pointer_mode == GrabModeSync;
    keyboardFrozen = keyboard_mode == GrabModeSync;
}

void releasePassivePointerGrab(Display *display)
{
    if (pointerGrabIsPassive())
        clearPointerGrab(display);
}

static void queryPointerRootPosition(Display *display, int *root_x, int *root_y)
{
    SDL_Window *focus = SDL_GetMouseFocus();
    if (focus) {
        Window focusWindow = getWindowFromId(SDL_GetWindowID(focus));
        if (focusWindow != None && IS_TYPE(focusWindow, WINDOW)) {
            int local_x = 0, local_y = 0;
            SDL_GetMouseState(&local_x, &local_y);
            Window child = None;
            if (XTranslateCoordinates(display, focusWindow, SCREEN_WINDOW,
                                      local_x, local_y, root_x, root_y,
                                      &child)) {
                return;
            }
        }
    }

#if SDL_VERSION_ATLEAST(2, 0, 4)
    SDL_GetGlobalMouseState(root_x, root_y);
#else
    SDL_GetMouseState(root_x, root_y);
#endif
}

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
             * zero width/height means "to the edge of src_window".
             */
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
            /* X rect containment is half-open: x <= p < x + width. The
             * difference runs through int64 so a local_x of INT_MIN and src_x
             * of INT_MAX cannot wrap before the unsigned compare.
             */
            int64_t dx = (int64_t) local_x - (int64_t) src_x;
            int64_t dy = (int64_t) local_y - (int64_t) src_y;
            if (dx < 0 || dx >= (int64_t) w || dy < 0 || dy >= (int64_t) h)
                return 1;
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
    /* Compute final pointer in int64 and clamp to SDL's int arg range so
     * callers cannot drive an overflow into the warp call.
     */
    int64_t finalX = (int64_t) curr_x + (int64_t) dest_x;
    int64_t finalY = (int64_t) curr_y + (int64_t) dest_y;
    if (finalX > INT_MAX)
        finalX = INT_MAX;
    if (finalX < INT_MIN)
        finalX = INT_MIN;
    if (finalY > INT_MAX)
        finalY = INT_MAX;
    if (finalY < INT_MIN)
        finalY = INT_MIN;
#if SDL_VERSION_ATLEAST(2, 0, 4)
    if (SDL_WarpMouseGlobal((int) finalX, (int) finalY) != 0)
        LOG("Warning: SDL_WarpMouseGlobal failed: %s", SDL_GetError());
#else
    SDL_WarpMouseInWindow(SDL_GetMouseFocus(), (int) finalX, (int) finalY);
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
    TYPE_CHECK(window, WINDOW, display, False);
    if (root_return)
        *root_return = SCREEN_WINDOW;
    int root_x = 0, root_y = 0;
    queryPointerRootPosition(display, &root_x, &root_y);
    if (root_x_return)
        *root_x_return = root_x;
    if (root_y_return)
        *root_y_return = root_y;
    int win_x = 0, win_y = 0;
    Window child = None;
    XTranslateCoordinates(display, SCREEN_WINDOW, window, root_x, root_y,
                          &win_x, &win_y, &child);
    if (win_x_return)
        *win_x_return = win_x;
    if (win_y_return)
        *win_y_return = win_y;
    if (child_return)
        *child_return = child;
    if (mask_return) {
        /* Real XQueryPointer reports modifier state in the low byte and pressed
         * button state in the high byte (Button[1-5]Mask). SDL_GetMouseState
         * returns a per-button bitmask; project each SDL button bit onto the
         * matching X mask.
         */
        unsigned int mask = convertModifierState(SDL_GetModState());
        Uint32 sdlButtons = SDL_GetMouseState(NULL, NULL);
        if (sdlButtons & SDL_BUTTON(SDL_BUTTON_LEFT))
            mask |= Button1Mask;
        if (sdlButtons & SDL_BUTTON(SDL_BUTTON_MIDDLE))
            mask |= Button2Mask;
        if (sdlButtons & SDL_BUTTON(SDL_BUTTON_RIGHT))
            mask |= Button3Mask;
        if (sdlButtons & SDL_BUTTON(SDL_BUTTON_X1))
            mask |= Button4Mask;
        if (sdlButtons & SDL_BUTTON(SDL_BUTTON_X2))
            mask |= Button5Mask;
        *mask_return = mask;
    }
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
    /* Xlib status codes: GrabSuccess == 0, AlreadyGrabbed == 1, GrabInvalidTime
     * == 2, GrabNotViewable == 3, GrabFrozen == 4.
     *
     * Do not use SDL relative mouse mode for an X pointer grab. SDL relative
     * mode hides the system cursor and reports relative deltas; an X grab only
     * redirects/freezes pointer events while preserving normal cursor
     * visibility. Motif pulldown menus rely on that distinction.
     */
    if (grab_window == None || !IS_TYPE(grab_window, WINDOW))
        return BadWindow;
    if (!isWindowEffectivelyViewable(grab_window))
        return GrabNotViewable;
    if (confine_to != None && (!IS_TYPE(confine_to, WINDOW) ||
                               !isWindowEffectivelyViewable(confine_to)))
        return GrabNotViewable;
    /* AlreadyGrabbed applies when another client owns the active grab. This
     * SDL-backed implementation has a single in-process client, so a second
     * explicit grab is a same-client regrab and must update the active grab.
     * Motif menu bars use that to switch pointer modes while posting a menu.
     */
    if (pointerGrab.active && !pointerGrab.passive) {
        replacePointerGrab(display, grab_window, owner_events, event_mask,
                           pointer_mode, keyboard_mode, confine_to, cursor);
        return GrabSuccess;
    }
    /* GrabInvalidTime: the requested time must be CurrentTime or no later than
     * the last server time. Compare as CARD32 with a signed delta so a 32-bit
     * tick rollover (every ~49 days of uptime) doesn't spuriously fire. Time is
     * unsigned long (LP64: 64 bits) but server time is delivered as a 32-bit X
     * protocol value, so masking to 32 bits before the compare keeps both sides
     * in the same domain.
     */
    if (time != CurrentTime) {
        Uint32 now = SDL_GetTicks();
        Uint32 requested = (Uint32) time;
        if ((Sint32) (requested - now) > 0)
            return GrabInvalidTime;
    }
    if (pointerGrab.passive)
        clearPointerGrab(display);
    installPointerGrab(display, grab_window, owner_events, event_mask,
                       pointer_mode, keyboard_mode, confine_to, cursor, False);
    return GrabSuccess;
}

int XGrabButton(Display *display,
                unsigned int button,
                unsigned int modifiers,
                Window grab_window,
                Bool owner_events,
                unsigned int event_mask,
                int pointer_mode,
                int keyboard_mode,
                Window confine_to,
                Cursor cursor)
{
    SET_X_SERVER_REQUEST(display, X_GrabButton);
    TYPE_CHECK(grab_window, WINDOW, display, BadWindow);
    /* Xlib semantics: a passive grab on the same (button, modifiers,
     * grab_window) triple replaces the previous one. Appending would silently
     * leak the prior grab and route events through whichever matched first.
     */
    ButtonGrab *grab = NULL;
    for (ButtonGrab *existing = buttonGrabs; existing;
         existing = existing->next) {
        if (existing->button == button && existing->modifiers == modifiers &&
            existing->grab_window == grab_window) {
            grab = existing;
            break;
        }
    }
    if (!grab) {
        grab = calloc(1, sizeof(*grab));
        if (!grab)
            return BadAlloc;
        grab->next = buttonGrabs;
        buttonGrabs = grab;
    }
    grab->button = button;
    grab->modifiers = modifiers;
    grab->grab_window = grab_window;
    grab->owner_events = owner_events;
    grab->event_mask = event_mask;
    grab->pointer_mode = pointer_mode;
    grab->keyboard_mode = keyboard_mode;
    grab->confine_to = confine_to;
    grab->cursor = cursor;
    return Success;
}

int XUngrabButton(Display *display,
                  unsigned int button,
                  unsigned int modifiers,
                  Window grab_window)
{
    SET_X_SERVER_REQUEST(display, X_UngrabButton);
    ButtonGrab **p = &buttonGrabs;
    while (*p) {
        ButtonGrab *grab = *p;
        Bool buttonMatch = button == AnyButton || grab->button == button;
        Bool modMatch =
            modifiers == AnyModifier || grab->modifiers == modifiers;
        Bool winMatch = grab->grab_window == grab_window;
        if (buttonMatch && modMatch && winMatch) {
            *p = grab->next;
            free(grab);
        } else {
            p = &grab->next;
        }
    }
    return Success;
}

/* Drop any passive button grabs that reference "window" so a later XID reuse
 * cannot misroute events through a stale grab entry. Called from destroyWindow
 * when a window is torn down.
 */
void releaseButtonGrabsForWindow(Window window)
{
    ButtonGrab **p = &buttonGrabs;
    while (*p) {
        ButtonGrab *grab = *p;
        if (grab->grab_window == window || grab->confine_to == window) {
            *p = grab->next;
            free(grab);
        } else {
            p = &grab->next;
        }
    }
}

int XUngrabPointer(Display *display, Time time)
{
    // https://tronche.com/gui/x/xlib/input/XUngrabPointer.html
    SET_X_SERVER_REQUEST(display, X_UngrabPointer);
    clearPointerGrab(display);
    return 1;
}
