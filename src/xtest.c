/*
 * XTest extension implementation
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
#include "X11/XKBlib.h"
#include <X11/extensions/XTest.h>
#include "sdl-compat.h"
#include "extension.h"
#include "window.h"
#include "events.h"
#include "input-method.h"
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

/* Map a modifier key's keycode (low byte of the SDL keycode) to its KMOD mask.
 * Returns 0 for non-modifier keys.
 */
static Uint16 kmodForModifierKeycode(unsigned int keycode)
{
    switch (keycode & 0xFF) {
    case 0xE0: /* SDLK_LCTRL */
    case 0xE4: /* SDLK_RCTRL */
        return KMOD_CTRL;
    case 0xE1: /* SDLK_LSHIFT */
    case 0xE5: /* SDLK_RSHIFT */
        return KMOD_SHIFT;
    case 0xE2: /* SDLK_LALT */
    case 0xE6: /* SDLK_RALT */
        return KMOD_ALT;
    default:
        return 0;
    }
}

/* Modifier keys held by prior fake presses. Synthetic XTest key events carry no
 * modifier state on their own, so track held modifier keycodes here and stamp
 * the running mask onto every fake key event. This lets replays hold Shift and
 * type uppercase, matching how a real keyboard reports modifiers.
 *
 * Single static for the serial replay-input path, no locking.
 */
static Uint16 fakeHeldMods = 0;

/* A real keyboard emits SDL_TEXTINPUT alongside the keydown for a printable
 * key; the IM path turns that into the committed character, and when an IC owns
 * focus the bare keydown is suppressed in favor of it. XTest only fakes the
 * keydown, so without a companion text event the character would be lost under
 * a focused IC (and text widgets that read the commit see nothing). Emit it for
 * printable keys so synthetic typing reaches widgets the same way real typing
 * does.
 *
 * Only when an IC owns focus: that is the only case where the keydown is
 * suppressed, so it is the only case that needs the commit. With no focused IC
 * the raw keydown is delivered and looked up directly, so a text event would
 * just be dropped. Deliver to winId, the same target the keydown used.
 *
 * keycode is this layer's X keycode (the SDL keysym low byte), so it doubles as
 * the character for the printable ranges. SDL2 copies the text into the event
 * at push; SDL3 keeps the pointer, so the string must outlive the push. Hold
 * recent ones in a static ring sized well past the depth the serialized,
 * delay-paced replay ever leaves unprocessed.
 */
static void pushFakeTextForKey(Display *display,
                               Uint32 winId,
                               unsigned int keycode)
{
    if (!inputMethodHasActiveTextInput())
        return;
    if (fakeHeldMods & (KMOD_CTRL | KMOD_ALT | KMOD_GUI))
        return;
    KeySym cp = NoSymbol;
    XkbLookupKeySym(display, (KeyCode) keycode,
                    convertModifierState(fakeHeldMods), NULL, &cp);
    if (!((cp >= 0x20 && cp <= 0x7e) || (cp >= 0xa0 && cp <= 0xff)))
        return;
    enum { TEXT_RING_SLOTS = 64 };
    static char ring[TEXT_RING_SLOTS][4];
    static unsigned int ringIndex;
    char *buf = ring[ringIndex++ % TEXT_RING_SLOTS];
    int n = 0;
    if (cp < 0x80) {
        buf[n++] = (char) cp;
    } else {
        buf[n++] = (char) (0xc0 | (cp >> 6));
        buf[n++] = (char) (0x80 | (cp & 0x3f));
    }
    buf[n] = '\0';
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_TEXTINPUT;
    ev.text.timestamp = XC_NOW_EVENT_TS();
    ev.text.windowID = winId;
    XC_SET_TEXT_EVENT(ev, buf);
    pushFakeEvent(display, &ev);
}

int XTestFakeKeyEvent(Display *display,
                      unsigned int keycode,
                      Bool is_press,
                      unsigned long delay)
{
    honorDelay(delay);
    /* Compute the held-modifier set after this key without committing it yet.
     * The event itself carries the pre-press mask (a Shift-down event does not
     * report Shift; a Shift-up still does), matching how a real keyboard
     * reports modifier state.
     */
    Uint16 modBit = kmodForModifierKeycode(keycode);
    Uint16 nextMods = fakeHeldMods;
    if (modBit) {
        if (is_press)
            nextMods |= modBit;
        else
            nextMods &= (Uint16) ~modBit;
    }
    Uint32 winId = replayTargetWindowId();
    if (winId == 0) {
        /* No delivery target right now, but keep tracking modifier state so a
         * later targeted key still sees the correct held set.
         */
        fakeHeldMods = nextMods;
        return 0;
    }
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = is_press ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.timestamp = XC_NOW_EVENT_TS();
    ev.key.windowID = winId;
    XC_EVENT_SET_KEY_PRESSED(&ev, is_press);
    XC_EVENT_SET_KEYMOD(&ev, fakeHeldMods);
    /* X keycodes are server-defined; SDL scancodes are SDL's own enum and the
     * convertEvent path derives the X keycode back from keysym.sym (low byte).
     * Pass the requested code through as the SDL_Keycode so the round-trip
     * lands on the same X keycode the caller asked for, and let
     * SDL_GetScancodeFromKey fill in the scancode for callers that consume it.
     * Callers wanting a specific keysym should XStringToKeysym first.
     */
    XC_EVENT_SET_KEYSYM(&ev, (SDL_Keycode) keycode);
    XC_EVENT_SET_SCANCODE(&ev, SDL_GetScancodeFromKey((SDL_Keycode) keycode));
    int rc = pushFakeEvent(display, &ev);
    if (rc) {
        fakeHeldMods = nextMods;
        if (is_press)
            pushFakeTextForKey(display, winId, keycode);
    }
    return rc;
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
