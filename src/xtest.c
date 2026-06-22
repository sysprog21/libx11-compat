/* XTest extension implementation
 *
 * Real X11 implements XTest in the server: a test client calls
 * XTestFakeXxxEvent and the server distributes the resulting events to every
 * connected client. libx11-compat is single-process, so there is no server and
 * no IPC; fake events here are injected directly into the calling process's SDL
 * event queue via SDL_PushEvent. The synthesized SDL_Event then flows through
 * the same onSdlEvent filter and convertEvent() path that real user input
 * takes, so resulting ButtonPress/KeyPress/MotionNotify is indistinguishable
 * from a real one downstream.
 *
 * This is what overcomes the macOS NSEvent-injection limitation: the synthetic
 * SDL_Event lives inside the libx11-compat process and never has to cross the
 * NSWindow/AppKit responder chain that drops external CGEvent button presses
 * against non-keyWindow processes.
 */

#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/extensions/XTest.h>
#include "sdl-compat.h"
#include "extension.h"
#include "window.h"
#include "events.h"
#include "replay-target.h"
#include "util.h"

#define XTEST_MAJOR 2
#define XTEST_MINOR 2

/* The XTest spec says delay is "in milliseconds before delivering the event";
 * servers honor it because they have an internal scheduler. honorDelay just
 * sleeps when the value is nonzero. Callers chain fakes with delay=0, delay=10,
 * delay=10... to pace a sequence.
 */
static void honorDelay(unsigned long delay_ms)
{
    if (delay_ms > 0)
        SDL_Delay((Uint32) delay_ms);
}

static int pushFakeEvent(Display *display, SDL_Event *event)
{
    if (SDL_PushEvent(event) != 1)
        return 0;
    wakeEventPipeForExternalEvent(display);
    return 1;
}

Bool XTestQueryExtension(Display *display,
                         int *event_basep,
                         int *error_basep,
                         int *majorp,
                         int *minorp)
{
    if (!display)
        return False;
    int opcode = 0;
    int eventBase = 0;
    int errorBase = 0;
    if (!XQueryExtension(display, XTestExtensionName, &opcode, &eventBase,
                         &errorBase))
        return False;
    if (event_basep)
        *event_basep = eventBase;
    if (error_basep)
        *error_basep = errorBase;
    if (majorp)
        *majorp = XTEST_MAJOR;
    if (minorp)
        *minorp = XTEST_MINOR;
    return True;
}

int XTestFakeMotionEvent(Display *display,
                         int screen_number,
                         int x,
                         int y,
                         unsigned long delay)
{
    (void) screen_number;
    honorDelay(delay);
    /* Coherent snapshot of (id, root) so a retarget between reading the id and
     * the root cannot send this event to one window with another window's local
     * coordinates.
     */
    Uint32 winId = 0;
    int localX = 0, localY = 0;
    if (!replayTargetTranslateRoot(x, y, &winId, &localX, &localY))
        return 0;
    replayTargetRememberPointer(localX, localY);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_MOUSEMOTION;
    ev.motion.timestamp = XC_NOW_EVENT_TS();
    ev.motion.windowID = winId;
    ev.motion.x = localX;
    ev.motion.y = localY;
    return pushFakeEvent(display, &ev);
}

int XTestFakeRelativeMotionEvent(Display *display,
                                 int x,
                                 int y,
                                 unsigned long delay)
{
    honorDelay(delay);
    Uint32 winId = replayTargetWindowId();
    if (winId == 0)
        return 0;
    int prevX = 0, prevY = 0;
    if (!replayTargetReadPointer(&prevX, &prevY))
        SDL_GetMouseState(&prevX, &prevY);
    int newX = prevX + x;
    int newY = prevY + y;
    replayTargetRememberPointer(newX, newY);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_MOUSEMOTION;
    ev.motion.timestamp = XC_NOW_EVENT_TS();
    ev.motion.windowID = winId;
    ev.motion.x = newX;
    ev.motion.y = newY;
    ev.motion.xrel = x;
    ev.motion.yrel = y;
    return pushFakeEvent(display, &ev);
}

int XTestFakeButtonEvent(Display *display,
                         unsigned int button,
                         Bool is_press,
                         unsigned long delay)
{
    honorDelay(delay);
    Uint32 winId = replayTargetWindowId();
    if (winId == 0)
        return 0;
    int sdlButton;
    switch (button) {
    case Button1:
        sdlButton = SDL_BUTTON_LEFT;
        break;
    case Button2:
        sdlButton = SDL_BUTTON_MIDDLE;
        break;
    case Button3:
        sdlButton = SDL_BUTTON_RIGHT;
        break;
    case 4:
    case 5: {
        /* X11 buttons 4/5 are scroll wheel up/down. SDL models scrolling as
         * SDL_MOUSEWHEEL, not button events. Synthesize one wheel tick with the
         * appropriate y direction on press; the X-protocol release that follows
         * real wheel events has no SDL analog, so swallow it.
         */
        if (!is_press)
            return 1;
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_MOUSEWHEEL;
        ev.wheel.timestamp = XC_NOW_EVENT_TS();
        ev.wheel.windowID = winId;
        ev.wheel.y = (button == 4) ? 1 : -1;
        ev.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        /* Tag the event as synthetic so convertEvent's SDL_MOUSEWHEEL handler
         * knows to use the injected pointer position instead of
         * SDL_GetMouseState. SDL_TOUCH_MOUSEID is the documented sentinel for
         * "not a real mouse instance" and never collides with a device-driven
         * which value. Without this tag, a real interactive wheel scroll AFTER
         * any XTest activity would also route through the injected coordinates
         * because convertEvent cannot tell synthetic and real events apart.
         */
        ev.wheel.which = SDL_TOUCH_MOUSEID;
        return pushFakeEvent(display, &ev);
    }
    default:
        return 0;
    }

    int curX = 0, curY = 0;
    if (!replayTargetReadPointer(&curX, &curY))
        SDL_GetMouseState(&curX, &curY);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = is_press ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
    ev.button.timestamp = XC_NOW_EVENT_TS();
    ev.button.windowID = winId;
    ev.button.button = (Uint8) sdlButton;
    XC_EVENT_SET_BUTTON_PRESSED(&ev, is_press);
    ev.button.clicks = 1;
    ev.button.x = curX;
    ev.button.y = curY;
    return pushFakeEvent(display, &ev);
}

int XTestFakeKeyEvent(Display *display,
                      unsigned int keycode,
                      Bool is_press,
                      unsigned long delay)
{
    honorDelay(delay);
    Uint32 winId = replayTargetWindowId();
    if (winId == 0)
        return 0;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = is_press ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.timestamp = XC_NOW_EVENT_TS();
    ev.key.windowID = winId;
    XC_EVENT_SET_KEY_PRESSED(&ev, is_press);
    /* X keycodes are server-defined; SDL scancodes are SDL's own enum and the
     * convertEvent path derives the X keycode back from keysym.sym (low byte).
     * Pass the requested code through as the SDL_Keycode so the round-trip
     * lands on the same X keycode the caller asked for, and let
     * SDL_GetScancodeFromKey fill in the scancode for callers that consume it.
     * Callers wanting a specific keysym should XStringToKeysym first.
     */
    XC_EVENT_SET_KEYSYM(&ev, (SDL_Keycode) keycode);
    XC_EVENT_SET_SCANCODE(&ev, SDL_GetScancodeFromKey((SDL_Keycode) keycode));
    return pushFakeEvent(display, &ev);
}

/* Stubs: device-extension variants need XInput plumbing libx11-compat doesn't
 * model.
 *
 * Return success; the caller's chain typically falls back to non-device XTest
 * paths anyway.
 */
int XTestFakeDeviceKeyEvent(Display *display,
                            XDevice *dev,
                            unsigned int keycode,
                            Bool is_press,
                            int *axes,
                            int n_axes,
                            unsigned long delay)
{
    (void) dev;
    (void) axes;
    (void) n_axes;
    return XTestFakeKeyEvent(display, keycode, is_press, delay);
}

int XTestFakeDeviceButtonEvent(Display *display,
                               XDevice *dev,
                               unsigned int button,
                               Bool is_press,
                               int *axes,
                               int n_axes,
                               unsigned long delay)
{
    (void) dev;
    (void) axes;
    (void) n_axes;
    return XTestFakeButtonEvent(display, button, is_press, delay);
}

int XTestFakeProximityEvent(Display *display,
                            XDevice *dev,
                            Bool in_prox,
                            int *axes,
                            int n_axes,
                            unsigned long delay)
{
    (void) display;
    (void) dev;
    (void) in_prox;
    (void) axes;
    (void) n_axes;
    honorDelay(delay);
    return 1;
}

int XTestFakeDeviceMotionEvent(Display *display,
                               XDevice *dev,
                               Bool is_relative,
                               int first_axis,
                               int *axes,
                               int n_axes,
                               unsigned long delay)
{
    (void) dev;
    (void) first_axis;
    if (n_axes < 2 || !axes)
        return 0;
    if (is_relative)
        return XTestFakeRelativeMotionEvent(display, axes[0], axes[1], delay);
    return XTestFakeMotionEvent(display, 0, axes[0], axes[1], delay);
}

Bool XTestCompareCursorWithWindow(Display *display,
                                  Window window,
                                  Cursor cursor)
{
    (void) display;
    (void) window;
    (void) cursor;
    /* No cursor introspection; reporting "same" prevents test programs from
     * looping forever waiting for a cursor change libx11-compat cannot observe.
     */
    return True;
}

Bool XTestCompareCurrentCursorWithWindow(Display *display, Window window)
{
    (void) display;
    (void) window;
    return True;
}

int XTestGrabControl(Display *display, Bool impervious)
{
    (void) display;
    (void) impervious;
    /* No grab semantics: events flow directly to widgets. */
    return 1;
}

void XTestSetGContextOfGC(GC gc, GContext gid)
{
    if (gc)
        gc->gid = gid;
}

void XTestSetVisualIDOfVisual(Visual *visual, VisualID visualid)
{
    if (visual)
        visual->visualid = visualid;
}

Status XTestDiscard(Display *display)
{
    (void) display;
    /* No buffered fake-event records to flush. */
    return 1;
}
