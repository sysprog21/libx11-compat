#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xlibint.h>

#include "sdl-compat.h"
#include "events.h"
#include "selection.h"
#include "events-ewmh.h"
#include "events-expose.h"
#include "errors.h"
#include "input.h"
#include "input-method.h"
#include "display.h"
#include "atoms.h"
#include "replay-target.h"
#include "util.h"
#include "drawing.h"
#include "mac-live-resize.h"
#include "window-internal.h"
#include "replay-target.h"
#include "main-dispatch.h"
#include "snapshot.h"
#include "state-snapshot.h"
#include "timeline.h"

static int eventFds[2] = {-1, -1};
#define READ_EVENT_FD eventFds[0]
#define WRITE_EVENT_FD eventFds[1]

static unsigned long lastEventSerial = 1;

/* lastEventSerial is a process-global event serial counter bumped and read from
 * multiple threads (Xlib entry points plus the SDL event thread), so every
 * access goes through these relaxed atomics to stay data-race free under
 * XInitThreads. Relaxed ordering suffices: it is a standalone counter with no
 * happens-before dependency on other state.
 */
static unsigned long currentEventSerial(void)
{
    return __atomic_load_n(&lastEventSerial, __ATOMIC_RELAXED);
}

static void bumpEventSerial(void)
{
    __atomic_add_fetch(&lastEventSerial, 1, __ATOMIC_RELAXED);
}
static SDL_mutex *eventQueueLengthLock = NULL;

/* Concurrency: SDL invokes onSdlEvent on its own event thread while Xlib API
 * entries run on the application thread. Each shared piece of state has its own
 * mutex; helper wrappers tolerate a NULL lock so static initializers and
 * pre-init code do not deadlock. The locks are created in initEventPipe and
 * destroyed when the last display closes. These fine-grained locks are always
 * active (the SDL event thread races the app thread regardless of
 * XInitThreads); only the coarse LockDisplay is XInitThreads-gated.
 *
 * Lock order (outermost first); acquire only left-to-right, never the reverse:
 *   LockDisplay > eventPipeGlobalLock > trackedDisplaysLock
 *     > { putBackEventsLock, eventQueueLengthLock, activePointerWindowLock }
 * Those three are leaves and are never held at the same time (each is taken and
 * released within a single helper). onSdlEvent (the SDL-thread producer) takes
 * only eventQueueLengthLock and never LockDisplay, which is what keeps the
 * coarse lock off the SDL thread and the whole order acyclic.
 *
 * The T2 off-main-thread dispatch adds two more, both below LockDisplay and
 * never inverting it: LockDisplay is now the DisplayLock monitor in
 * src/display.c (its internal monitor mutex is a leaf, held only inside
 * compat{Lock,Unlock}Display and the handoff helpers, never across other
 * locks); and each runOnMainThread call has a per-request cmd mutex/cond on the
 * worker stack, taken only after the worker has shed the display lock, so it is
 * a strict leaf that never nests under LockDisplay. The mainEventThreadId
 * ownership id is a lock-free atomic and takes no part in the order.
 */
static SDL_mutex *putBackEventsLock = NULL;
static SDL_mutex *trackedDisplaysLock = NULL;
static SDL_mutex *activePointerWindowLock = NULL;

/* Tracks whether the pump-wake timer has an outstanding, not-yet-consumed byte
 * in the event pipe. See xtWakeTimerCallback and consumePumpWakeByte. Kept
 * separate from the SDL event queue so the periodic Cocoa/SDL pump wake never
 * depends on SDL_HasEvent/SDL_PeepEvents queue semantics (which sdl2-compat and
 * SDL3 do not honor for user events pushed from a timer thread).
 */
static SDL_atomic_t pumpWakePending = {0};
static SDL_TimerID xtWakeTimer = 0;
static Array trackedDisplays = {NULL, 0, 0};

#if SDL_VERSION_ATLEAST(2, 0, 18)
static __thread float wheelPreciseX = 0.0f, wheelPreciseY = 0.0f;
#endif

/* Serializes the first-open / last-close blocks in {init,closeEventPipe}. The
 * named SDL_mutex slots they manage are created and destroyed there, so a
 * parallel open + close on the same boundary (length 0->1 racing length 1->0)
 * would otherwise race on SDL_{Create,Destroy}Mutex and on the wake timer
 * setup.
 */
static SDL_SpinLock eventPipeGlobalLock;

static void lockPutBackEvents(void)
{
    if (putBackEventsLock)
        SDL_LockMutex(putBackEventsLock);
}

static void unlockPutBackEvents(void)
{
    if (putBackEventsLock)
        SDL_UnlockMutex(putBackEventsLock);
}
static void lockTrackedDisplays(void)
{
    if (trackedDisplaysLock)
        SDL_LockMutex(trackedDisplaysLock);
}

static void unlockTrackedDisplays(void)
{
    if (trackedDisplaysLock)
        SDL_UnlockMutex(trackedDisplaysLock);
}

static void lockActivePointerWindow(void)
{
    if (activePointerWindowLock)
        SDL_LockMutex(activePointerWindowLock);
}

static void unlockActivePointerWindow(void)
{
    if (activePointerWindowLock)
        SDL_UnlockMutex(activePointerWindowLock);
}

static Bool pointerHoverIsWithin(Window window);

/* clearWindowTreeWithoutExpose, postFullWindowExpose, and
 * postSyntheticWindowResize live in src/events-expose.c alongside the fan-out
 * expose helpers.
 */

/* SDL_PumpEvents is only safe on the thread that owns SDL windows. XOpenDisplay
 * captures that owner before client threads can issue Xlib requests. Written on
 * the main thread at display open/close, read from client worker threads
 * (onMainEventThread), so accessed atomically. One field, not an id plus a
 * captured flag: 0 means "not captured yet" (still single-threaded). A single
 * atomic id has no two-field ordering to get wrong -- the release store
 * publishes it, the acquire load consumes it. Matches the __atomic style
 * already used for request / lastEventSerial. 0 is never a real thread id, and
 * the pre-atomic code already used it as the unset sentinel.
 */
static SDL_threadID mainEventThreadId = 0;

void captureMainEventThreadIfUnset(void)
{
    SDL_threadID self = SDL_ThreadID();
    SDL_threadID expected = 0;

    /* Set only while unset; if two threads race their first XOpenDisplay, the
     * CAS deterministically anoints one as the owner.
     */
    __atomic_compare_exchange_n(&mainEventThreadId, &expected, self, False,
                                __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

void releaseMainEventThread(void)
{
    /* Allow a later XOpenDisplay to capture a new owner thread. Precondition
     * (the standard Xlib one): no client thread issues Xlib calls during
     * XCloseDisplay. Resetting the id to 0 means a straggler off-thread call
     * after this point would see onMainEventThread() True and run its SDL work
     * inline on the worker; that is a client teardown-ordering bug, not one
     * this layer defends against.
     */
    __atomic_store_n(&mainEventThreadId, 0, __ATOMIC_RELEASE);
}

static Bool onMainEventThread(void)
{
    SDL_threadID owner = __atomic_load_n(&mainEventThreadId, __ATOMIC_ACQUIRE);
    return owner == 0 || SDL_ThreadID() == owner;
}

Bool libx11CompatOnMainEventThread(void)
{
    return onMainEventThread();
}

/* The pump-wake timer (xtWakeTimerCallback) writes a single byte to the event
 * pipe to unblock a client blocked in poll(ConnectionNumber) so the main thread
 * can pump SDL/Cocoa events. That byte is tracked by pumpWakePending,
 * independently of the SDL event queue and the qlen accounting; consume it
 * here, on the main thread once the pump has run, so poll() blocks again
 * instead of spinning.
 *
 * Ordering is deliberate: the timer sets pumpWakePending (0 to 1) before it
 * writes the byte, so read the byte first and only clear the flag when the read
 * actually removed it. Clearing the flag before reading would leave an
 * untracked byte behind in this interleaving: the consumer clears the flag and
 * sees an empty non-blocking pipe (EAGAIN) in the window after the timer set
 * the flag but before it wrote, then the timer's write lands with the flag
 * already 0, so the byte is never drained and keeps ConnectionNumber
 * permanently readable (a busy-spin). A failed read leaves the flag set so a
 * later call drains the byte once the write lands. This is safe because the
 * timer is the only producer and the main thread the only consumer, and the
 * timer cannot re-arm (CAS 0 to 1) while the flag is still 1.
 */
static void consumePumpWakeByte(void)
{
    if (READ_EVENT_FD < 0 || !SDL_AtomicGet(&pumpWakePending))
        return;
    char buffer;
    if (read(READ_EVENT_FD, &buffer, sizeof(buffer)) ==
        (ssize_t) sizeof(buffer))
        SDL_AtomicSet(&pumpWakePending, 0);
}

void pumpEventsSafe(void)
{
    /* onMainEventThread() is True both pre-capture (single-threaded, safe to
     * pump) and on the captured owner thread; only a captured-but-wrong thread
     * is turned away.
     */
    if (!onMainEventThread()) {
        LOG("SDL_PumpEvents skipped: called from thread %lu, main is %lu\n",
            (unsigned long) SDL_ThreadID(),
            (unsigned long) __atomic_load_n(&mainEventThreadId,
                                            __ATOMIC_RELAXED));
        return;
    }

    /* Never pump Cocoa while a live-resize present frame is unwinding. The
     * frame runs the client reflow (xwpe), whose repaint reaches XQueryPointer
     * / XFlush / XEventsQueued - each of which normally calls SDL_PumpEvents.
     * When the frame is driven from the NSWindowDidEndLiveResize handler (which
     * fires as AppKit unwinds -[NSWindow _endLiveResize]), pumping here
     * re-enters Cocoa's event dispatch, which picks up the pending mouse event
     * and starts a NESTED window-resize loop -> unbounded re-entrancy / hang
     * (observed: did-end -> reflow -> XQueryPointer -> SDL_PumpEvents ->
     * _resizeWithEvent). The observer already presents the frame; a nested pump
     * is never needed inside it and always unsafe, so suppress it. Still drain
     * any wake byte queued during the reflow, otherwise the pipe stays readable
     * and keeps re-triggering the event loop after the drag ends.
     */
    if (libx11CompatInLiveResizePresent()) {
        consumePumpWakeByte();
        return;
    }

    SDL_PumpEvents();
    consumePumpWakeByte();
    mainDispatchRunDeferred();
}

static Uint32 xtWakeTimerCallback(XC_TIMER_CALLBACK_PARAMS)
{
    (void) param;

    /* macOS/Cocoa (and every SDL video backend) requires the event loop to be
     * serviced on the main thread via SDL_PumpEvents, which libx11-compat only
     * runs when the client re-enters an Xlib API. This timer periodically wakes
     * a client blocked in poll(ConnectionNumber) by writing one byte to the
     * event pipe, so the main thread returns from poll() and pumps.
     *
     * The wake byte is tracked with pumpWakePending rather than routed through
     * the SDL event queue. Pushing an SDL user event and gating on SDL_HasEvent
     * proved unreliable on sdl2-compat/SDL3: a pushed user event can be
     * reported by SDL_HasEvent and SDL_PeepEvents(PEEK) yet never removed by a
     * full-range SDL_PeepEvents(GETEVENT), which permanently throttled this
     * timer and starved the Cocoa run loop (the app showed as "Not Responding"
     * while otherwise rendering correctly). A dedicated pipe byte keeps the
     * wake independent of SDL's queue accounting; pumpEventsSafe() consumes it.
     */
    if (WRITE_EVENT_FD < 0)
        return interval;

    if (SDL_AtomicCAS(&pumpWakePending, 0, 1)) {
        char buffer = 'e';
        if (write(WRITE_EVENT_FD, &buffer, sizeof(buffer)) != sizeof(buffer))
            SDL_AtomicSet(&pumpWakePending, 0);
    }
    return interval;
}

static Bool shouldStartXtWakeTimer(void)
{
    const char *driver = SDL_GetCurrentVideoDriver();
    return !driver || strcmp(driver, "dummy") != 0;
}

typedef struct PutBackEvent {
    Display *display;
    XEvent event;

    /* True when convertEvent's terminal tap already ran before the event was
     * placed on this queue. Put-back delivery paths use this to avoid
     * double-tapping such events into the timeline. Synthesized events that
     * bypass convertEvent (e.g. crossings built by appendPointerCrossingEvent)
     * leave this False so the timeline still sees them when consumed.
     */
    Bool alreadyTapped;
    struct PutBackEvent *next;
} PutBackEvent;

static PutBackEvent *putBackEvents = NULL;

static void updateWindowRenderTargets(Display *display);
static XC_EVENTFILTER_RET onSdlEvent(void *userdata, SDL_Event *event);
static Bool getEventQueueLength(int *qlen);
static int countPutBackEvents(Display *display);
static Bool enqueuePutBackEvent(Display *display, const XEvent *event);
static Bool appendPutBackEvent(Display *display, const XEvent *event);
int convertEvent(Display *display,
                 SDL_Event *sdlEvent,
                 XEvent *xEvent,
                 Bool freeInternalEvents);

static __thread int sdlPeepEventsXlibDrainDepth;

int libx11CompatSdlPeepEventsIsXlibDrain(void)
{
    return sdlPeepEventsXlibDrainDepth > 0;
}

static int sdlPeepEventsForXlibDrain(SDL_Event *events,
                                     int numevents,
                                     SDL_eventaction action,
                                     Uint32 minType,
                                     Uint32 maxType)
{
    sdlPeepEventsXlibDrainDepth++;
    int result = SDL_PeepEvents(events, numevents, action, minType, maxType);
    sdlPeepEventsXlibDrainDepth--;
    return result;
}

/* Pipe write/read are best-effort wake-up signals; the authoritative "event
 * ready" tracker is GET_DISPLAY(display)->qlen plus the putBackEvents linked
 * list. Both file descriptors are non-blocking after initEventPipe so a full or
 * empty pipe never stalls the event loop on a slow client or a phantom qlen.
 */
static void lockEventQueueLength(void)
{
    if (eventQueueLengthLock)
        SDL_LockMutex(eventQueueLengthLock);
}

static void unlockEventQueueLength(void)
{
    if (eventQueueLengthLock)
        SDL_UnlockMutex(eventQueueLengthLock);
}

static int displayEventQueueLength(Display *display)
{
    lockEventQueueLength();
    int qlen = GET_DISPLAY(display)->qlen;
    unlockEventQueueLength();
    return qlen;
}

static void setDisplayEventQueueLength(Display *display, int qlen)
{
    lockEventQueueLength();
    GET_DISPLAY(display)->qlen = qlen < 0 ? 0 : qlen;
    unlockEventQueueLength();
}

static void discardPipeWakeups(void)
{
    char buffer[64];
    while (read(READ_EVENT_FD, buffer, sizeof(buffer)) > 0)
        ; /* drain */
}

/* Write count wake bytes into the event pipe in buffer-sized chunks, stopping
 * early if a short or failed write signals the pipe is full. Does no locking;
 * callers hold eventQueueLengthLock across the discard-then-rewrite.
 */
static void writePipeWakeBytes(int count)
{
    char buffer[64];
    memset(buffer, 'e', sizeof(buffer));
    while (count > 0) {
        size_t chunk = (size_t) count;
        if (chunk > sizeof(buffer))
            chunk = sizeof(buffer);
        ssize_t written = write(WRITE_EVENT_FD, buffer, chunk);
        if (written <= 0)
            break;
        count -= (int) written;
    }
}

static void resetEventWakeups(Display *display, int qlen)
{
    /* Hold eventQueueLengthLock across the whole discard-then-rewrite so it is
     * atomic against a concurrent ENQUEUE/READ producer (onSdlEvent), which
     * takes the same lock: the reconcile can no longer discard a wake byte the
     * producer just wrote or overwrite the matching qlen++ mid-flight. qlen is
     * set directly here rather than via setDisplayEventQueueLength, which would
     * re-lock the non-recursive mutex.
     */
    lockEventQueueLength();
    discardPipeWakeups();

    /* discardPipeWakeups() emptied the pipe, so any outstanding pump-wake byte
     * is gone too; clear its flag so the timer can re-arm and pumpEventsSafe()
     * does not later read a real-event byte in its place.
     */
    SDL_AtomicSet(&pumpWakePending, 0);
    writePipeWakeBytes(qlen);
    GET_DISPLAY(display)->qlen = qlen < 0 ? 0 : qlen;
    unlockEventQueueLength();
}

/* The wake-pipe byte and the qlen counter are mutated together under
 * eventQueueLengthLock so the pair moves atomically against resetEventWakeups'
 * discard-then-rewrite, which takes the same lock. Both fds are non-blocking,
 * so holding the lock across the write/read never stalls.
 */
#define ENQUEUE_EVENT_IN_PIPE(display)                       \
    do {                                                     \
        lockEventQueueLength();                              \
        char _b = 'e';                                       \
        ssize_t _w = write(WRITE_EVENT_FD, &_b, sizeof(_b)); \
        (void) _w;                                           \
        GET_DISPLAY(display)->qlen++;                        \
        unlockEventQueueLength();                            \
    } while (0)
#define READ_EVENT_IN_PIPE(display)                        \
    do {                                                   \
        lockEventQueueLength();                            \
        char _b;                                           \
        ssize_t _r = read(READ_EVENT_FD, &_b, sizeof(_b)); \
        (void) _r;                                         \
        if (GET_DISPLAY(display)->qlen > 0)                \
            GET_DISPLAY(display)->qlen--;                  \
        unlockEventQueueLength();                          \
    } while (0)

void libx11CompatSideQueueEventRemoved(SDL_EventFilter filter, void *userdata)
{
    if (filter != onSdlEvent)
        return;

    Display *display = (Display *) userdata;
    if (!display)
        return;

    lockTrackedDisplays();
    Bool tracked = findInArray(&trackedDisplays, display) >= 0;
    unlockTrackedDisplays();
    if (tracked)
        READ_EVENT_IN_PIPE(display);
}

void wakeEventPipeForExternalEvent(Display *display)
{
    (void) display;
    if (WRITE_EVENT_FD < 0)
        return;

    int sdlQueued = 0;
    Bool haveSdlQueued = getEventQueueLength(&sdlQueued);
    int maxDesiredQlen = 0;

    /* SDL has one process-wide event queue, but Xt/Motif may have multiple
     * Display handles alive and the one running XtAppNextEvent is not
     * necessarily the Display passed to XTest. Wake every tracked display so
     * whichever client loop polls first sees the synthetic input promptly.
     */
    lockTrackedDisplays();
    for (size_t i = 0; i < trackedDisplays.length; i++) {
        Display *target = trackedDisplays.array[i];
        if (!target)
            continue;

        if (haveSdlQueued) {
            int desiredQlen = countPutBackEvents(target) + sdlQueued;
            if (desiredQlen > maxDesiredQlen)
                maxDesiredQlen = desiredQlen;
            setDisplayEventQueueLength(target, desiredQlen);
        } else {
            ENQUEUE_EVENT_IN_PIPE(target);
        }
    }
    unlockTrackedDisplays();

    if (haveSdlQueued) {
        /* Same serialization as resetEventWakeups: hold eventQueueLengthLock
         * across the discard-then-rewrite so it is atomic against a concurrent
         * ENQUEUE/READ producer (onSdlEvent), which takes the same lock. The
         * per-display setDisplayEventQueueLength calls above are already
         * locked; only this shared-pipe rewrite needed it.
         */
        lockEventQueueLength();
        discardPipeWakeups();
        writePipeWakeBytes(maxDesiredQlen);
        unlockEventQueueLength();
    }
}

Bool postWmDeleteIfHandled(Display *display, Window window, Time time)
{
    if (!IS_TYPE(window, WINDOW))
        return False;

    /* Re-resolve per call: freeAtomStorage drops dynamic atoms on the last
     * XCloseDisplay without resetting lastUsedAtom, so a static cache would
     * dangle to a stale id across a close/open cycle.
     */
    Atom wmProtocolsAtom = internalInternAtom("WM_PROTOCOLS");
    Atom wmDeleteWindowAtom = internalInternAtom("WM_DELETE_WINDOW");
    WindowProperty *windowProperty = findProperty(
        &GET_WINDOW_STRUCT(window)->properties, wmProtocolsAtom, NULL);
    if (!windowProperty || windowProperty->type != XA_ATOM ||
        windowProperty->dataFormat != 32 || !windowProperty->data)
        return False;

    for (size_t i = 0; i < windowProperty->dataLength; i++) {
        if (((Atom *) windowProperty->data)[i] == wmDeleteWindowAtom) {
            XEvent event;
            memset(&event, 0, sizeof(event));
            event.xclient.type = ClientMessage;
            event.xclient.serial = currentEventSerial();
            event.xclient.send_event = True;
            event.xclient.display = display;
            event.xclient.window = window;
            event.xclient.message_type = wmProtocolsAtom;
            event.xclient.format = 32;
            event.xclient.data.l[0] = wmDeleteWindowAtom;
            event.xclient.data.l[1] = time;

            /* Append, not prepend: this is a freshly synthesized event, so it
             * must land behind whatever is already queued rather than jump to
             * the head the way a put-back re-read would.
             */
            return appendPutBackEvent(display, &event);
        }
    }
    return False;
}

/* postExposeEvent and postExposeEventsForMappedChildren live in
 * src/events-expose.c.
 */

static XC_EVENTFILTER_RET onSdlEvent(void *userdata, SDL_Event *event)
{
    if (SCREEN_WINDOW == None || !IS_TYPE(SCREEN_WINDOW, WINDOW))
        return 0;

    switch (event->type) {
        //        case SDL_QUIT:
    XC_CASE_WINDOWEVENT: {
        SDL_Window *screenSdlWindow =
            GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlWindow;
        if (screenSdlWindow &&
            event->window.windowID == SDL_GetWindowID(screenSdlWindow)) {
            return 0;
        }

        /* The screen (root) window is backed by an offscreen software renderer
         * and never owns an SDL_Window; every on-screen SDL window belongs to a
         * mapped top-level child (see realizeTopLevelWindow), so
         * screenSdlWindow is normally NULL. Historically that made this filter
         * drop every window subevent except CLOSE. That is correct for
         * pointer/focus/crossing and map/expose subevents, which are already
         * driven through other paths - routing them here would, for example,
         * let a stray ENTER for an override-redirect popup steal pointer hover
         * and misroute a grabbed click. The one gap was live-resize *size*:
         * RESIZED/SIZE_CHANGED must reach convertEvent to become
         * ConfigureNotify, or a host resize never reflows the client.
         * DISPLAY_CHANGED must also reach it on the SDL3 backend: moving a
         * window to a monitor with a different backing scale changes the
         * physical pixel geometry with no RESIZED, and the handler there
         * re-promotes the window and reflows the client. Let those through, and
         * only when they map to a known top-level; keep dropping everything
         * else. A bare MOVED is dropped too (it needs no client reflow and,
         * arriving asynchronously here, would inject spurious
         * ConfigureNotifies) with one exception handled just below: an anchored
         * popup's MOVED carries a compositor slide/flip that ws->x/y must
         * track.
         */
        Uint32 sub = XC_WINDOW_SUBEVENT(event);
        Bool resizeSubevent = sub == SDL_WINDOWEVENT_RESIZED ||
                              sub == SDL_WINDOWEVENT_SIZE_CHANGED;
        Bool displayChangedSubevent = False;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        displayChangedSubevent = sub == SDL_WINDOWEVENT_DISPLAY_CHANGED;
#endif

        /* A bare MOVED is normally dropped (see above), but an anchored
         * xdg_popup that the compositor slid or flipped to keep on screen
         * reports its new parent-relative position only through MOVED. That
         * must reach convertEvent so ws->x/y tracks where the popup actually
         * landed; otherwise pointer-to-root translation for a grabbed submenu
         * stays offset by the slide. Let a popup MOVED through; convertEvent
         * updates ws->x/y and emits no client event for it.
         */
        Bool popupMoveSubevent = False;
        if (sub == SDL_WINDOWEVENT_MOVED) {
            Window movedWindow = getWindowFromId(event->window.windowID);
            if (movedWindow != None && IS_TYPE(movedWindow, WINDOW)) {
                WindowStruct *mws = GET_WINDOW_STRUCT(movedWindow);
                if (mws && mws->popupParent != None)
                    popupMoveSubevent = True;
#ifndef LIBX11_COMPAT_SDL3
                /* SDL2/X11: a window-manager move (opaque drag) uncovers the
                 * top-levels below the window's old position, but Xvnc does not
                 * deliver their Expose, so their old pixels linger as a drag
                 * trail. Let the MOVED through so convertEvent can repaint
                 * them. A client XMoveWindow's own SDL echo is different:
                 * configureWindow recorded the host origin it wrote and posts
                 * the one ConfigureNotify itself (repainting the vacated region
                 * through its move snapshot), so drop the echo whose origin
                 * matches and clear the token, or the move would deliver two
                 * ConfigureNotifies. A genuine WM drag reports a different
                 * origin and is kept.
                 */
                else if (mws && IS_MAPPED_TOP_LEVEL_WINDOW(movedWindow)) {
                    if (SDL_AtomicGet(&mws->moveEchoValid) &&
                        event->window.data1 == mws->moveEchoHostX &&
                        event->window.data2 == mws->moveEchoHostY) {
                        SDL_AtomicSet(&mws->moveEchoValid, 0);
                        return 0;
                    }
                    popupMoveSubevent = True;
                }
#endif
            }
        }
        if (sub != SDL_WINDOWEVENT_CLOSE && !resizeSubevent &&
            !displayChangedSubevent && !popupMoveSubevent)
            return 0;
        if (displayChangedSubevent &&
            getWindowFromId(event->window.windowID) == None)
            return 0;
        if (resizeSubevent) {
            Window resizeWindow = getWindowFromId(event->window.windowID);
            if (resizeWindow == None)
                return 0;

            /* Drop the SDL resize echo of a client-initiated XResizeWindow:
             * configureWindow arms this flag around its SDL_SetWindowSize and
             * already posts the ConfigureNotify itself, so converting the
             * echoed SDL event too would double-post. It pumps synchronously
             * while armed, so both the RESIZED and SIZE_CHANGED echoes SDL
             * posts are filtered here before it disarms. A genuine host-driven
             * resize (border drag) arrives with the flag clear and is kept so
             * it can become the ConfigureNotify that reflows the client.
             */
            WindowStruct *rws = GET_WINDOW_STRUCT(resizeWindow);
            if (SDL_AtomicGet(&rws->suppressSdlResizeEcho))
                return 0;

            /* The drag-end increment snap cannot pump (it runs inside the
             * present frame), so instead of the flag it records the exact
             * logical size it wrote via SDL_SetWindowSize. Drop any echo
             * carrying that size - both the RESIZED and SIZE_CHANGED posts - so
             * the deferred echo does not escape as a redundant ConfigureNotify;
             * a genuine resize to a different size still passes through. The
             * key is cleared once the RESIZED echo matches so a later host
             * resize back to the same size is kept.
             */
            if (rws->snapEchoW > 0 && rws->snapEchoH > 0 &&
                event->window.data1 == rws->snapEchoW &&
                event->window.data2 == rws->snapEchoH) {
                if (sub == SDL_WINDOWEVENT_RESIZED) {
                    rws->snapEchoW = 0;
                    rws->snapEchoH = 0;
                }
                return 0;
            }
        }
        /* Fall through to enqueue routable top-level window events. */
        ENQUEUE_EVENT_IN_PIPE((Display *) userdata);
        break;
    }
    default:
        ENQUEUE_EVENT_IN_PIPE((Display *) userdata);
        break;
    }
    return 1;
}

/* Guards libx11CompatPresentDuringLiveResize against re-entering
 * drawWindowDataToScreen: the present path can itself pump SDL, and a nested
 * invocation would recurse.
 */
static SDL_atomic_t liveResizeInPresent = {0};

/* Depth of the live-resize present frame. >0 for the entire duration of
 * libx11CompatPresentDuringLiveResize, so any Xlib call re-entered from the
 * observer/reflow callback (e.g. XCloseDisplay) can detect it and defer
 * teardown that would be unsafe from inside the CFRunLoop observer. The CAS on
 * liveResizeInPresent already prevents true re-entry, but increment/decrement
 * (not set 1/0) keeps the counter defensively correct and cheap.
 * Main-thread-only.
 */
static int liveResizePresentDepth = 0;

int libx11CompatInLiveResizePresent(void)
{
    return liveResizePresentDepth > 0;
}

/* Deferred-reflow (coalesce) state for the live-resize present path.
 *
 * The client reflow (xwpe) is expensive - 300ms to 1.8s per call - so running
 * it on every drag tick makes the drag lurch. Instead we coalesce: while the
 * window keeps changing size we only re-present the last good frame (cheap,
 * ~5ms, so the drag stays fluid), and we run the costly reflow only once the
 * size has SETTLED (paused for a few ticks) or at drag end (forceReflow). The
 * tracking state (previous-tick pixel size + settle counter) lives per
 * top-level on the WindowStruct, so two windows resizing at once do not
 * overwrite each other's coalesce state and each settles on its own. Reset when
 * a fresh drag arms the observer. Main-thread-only.
 */
void libx11CompatResetLiveResizeCoalesce(void)
{
    if (SCREEN_WINDOW == None || !IS_TYPE(SCREEN_WINDOW, WINDOW))
        return;
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    size_t count = GET_WINDOW_STRUCT(SCREEN_WINDOW)->children.length;
    for (size_t i = 0; i < count; i++) {
        if (!IS_TYPE(children[i], WINDOW))
            continue;

        WindowStruct *ws = GET_WINDOW_STRUCT(children[i]);
        ws->liveResizeLastSeenW = 0;
        ws->liveResizeLastSeenH = 0;
        ws->liveResizeSettleTicks = 0;

        /* Clear any snap echo-suppression key a previous drag-end snap left
         * set: if that snap's echo was coalesced away the key never matched,
         * and a stale key could otherwise drop a genuine resize of this new
         * drag that happens to land on the same size. The snap sets it fresh
         * each time.
         */
        ws->snapEchoW = 0;
        ws->snapEchoH = 0;
    }
}

/* Client-registered live-resize reflow callback. Registered once at client
 * startup on the main thread and only ever read on the main thread from the
 * observer path (or a unit-test seam), so a plain pointer is sufficient - no
 * cross-thread publication concern. NULL means no client opted in, and the
 * present path keeps its guessed margin fill for that window.
 */
static LibX11CompatLiveResizeReflowFn liveResizeReflowFn = NULL;

void libx11CompatRegisterLiveResizeReflow(LibX11CompatLiveResizeReflowFn fn)
{
    liveResizeReflowFn = fn;
}

int libx11CompatHasLiveResizeReflow(void)
{
    return liveResizeReflowFn != NULL;
}

void libx11CompatInvokeLiveResizeReflow(int newPixelWidth, int newPixelHeight)
{
    LibX11CompatLiveResizeReflowFn fn = liveResizeReflowFn;
    if (!fn)
        return;

    /* Bracket the callback with the same in-present depth the observer path
     * sets around reflow(), so this test seam faithfully reproduces the
     * re-entrancy state a client callback sees
     * (libx11CompatInLiveResizePresent() true). The observer path additionally
     * drains any deferred close as its last action; callers of this seam drain
     * explicitly via libx11CompatRunDeferredDisplayClose.
     */
    liveResizePresentDepth++;
    fn(newPixelWidth, newPixelHeight);
    liveResizePresentDepth--;
}

/* Runtime-gated live-resize tracer. Off unless LIBX11_COMPAT_LIVE_RESIZE_TRACE
 * names a file (or "-"/"stderr" for stderr). Line-buffered so the last line
 * survives a hang, and every line is stamped with a monotonic millisecond clock
 * so ticks-per-second and per-phase costs can be read off directly. No-op and
 * zero cost when the env var is unset. Main-thread-only (matches the observer
 * path), so the lazy init needs no lock.
 */
static uint64_t liveResizeNowNs(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
}

static FILE *liveResizeTraceFile(void)
{
    static Bool initialized = False;
    static FILE *file = NULL;
    if (!initialized) {
        initialized = True;
        const char *path = getenv("LIBX11_COMPAT_LIVE_RESIZE_TRACE");
        if (path && *path) {
            if (strcmp(path, "-") == 0 || strcmp(path, "stderr") == 0)
                file = stderr;
            else
                file = fopen(path, "w");
            if (file) {
                setvbuf(file, NULL, _IOLBF, 0);
                fprintf(file, "# t_ms\tevent\tdetail\n");
            }
        }
    }
    return file;
}

#define LR_TRACE(...)                                                     \
    do {                                                                  \
        FILE *_lrf = liveResizeTraceFile();                               \
        if (_lrf) {                                                       \
            static uint64_t _lrBase = 0;                                  \
            uint64_t _lrNow = liveResizeNowNs();                          \
            if (_lrBase == 0)                                             \
                _lrBase = _lrNow;                                         \
            fprintf(_lrf, "%.3f\t", (double) (_lrNow - _lrBase) / 1.0e6); \
            fprintf(_lrf, __VA_ARGS__);                                   \
            fputc('\n', _lrf);                                            \
        }                                                                 \
    } while (0)

void libx11CompatLiveResizeTrace(const char *tag)
{
    LR_TRACE("observer\t%s", tag ? tag : "");
}

/* Keep every mapped top-level in step with its live SDL window size during the
 * macOS live-resize drag. Called from the run-loop observer
 * (src/mac-live-resize.c) on each tick of the Cocoa modal resize loop - the one
 * context where SDL delivers no events yet the compositor is upscaling the
 * stale framebuffer, blurring the glyphs.
 *
 * Two modes, chosen per window by whether a client registered a reflow hook:
 *
 * 1. No reflow hook (default, non-xwpe clients): do NOT resize the backing or
 *    touch windowStruct->w/h. The client's draw loop is blocked for the whole
 *    drag, so recreating the backing here would only clear it to the background
 *    and present an empty frame every tick. Re-present the existing last-good
 *    backing 1:1 into the live (larger) surface; the present path clamps the
 *    smaller backing, blits the overlap sharply, and fills the newly exposed
 *    margin with a guessed background. The real SDL RESIZED event at drag end
 *    recreates the backing and drives the client's full repaint.
 *
 * 2. Reflow hook registered (xwpe): the client's reflow+repaint is re-entrant
 *    and does not block, so drive it here with real content instead of a
 *    guessed margin. When the live pixel size (logical SDL size times the
 *    cached HiDPI scale, computed exactly as the RESIZED ConfigureNotify
 * handler does) differs from the tracked geometry, adopt it (windowStruct->w/h
 * + resizeWindowTexture exactly as the drag-end ConfigureNotify path does),
 * then invoke the client callback so it recomputes its own grid and paints the
 * whole new area into the backing. The callback issues Xlib draws back into the
 * shim, each of which may XFlush (-> drawWindowDataToScreen); arm the coalesce
 * gate first so those intermediate presents are held and only the finished
 * frame reaches the screen this tick, mirroring the drag-end burst handling.
 * The observer's liveResizeInPresent CAS still guards this whole body against
 * re-entry, and the coalesce gate suppresses any nested present, so the
 * callback's Xlib calls cannot recurse into a partial frame.
 *
 * Safe no-op off the main SDL-owning thread.
 */
int libx11CompatPresentDuringLiveResize(void)
{
    return libx11CompatPresentDuringLiveResizeEx(0);
}

/* Replay the resize-increment snap a real window manager performs. A client
 * that publishes WM_NORMAL_HINTS PResizeInc (e.g. a text UI whose grid is whole
 * font cells) expects the WM to quantize user resizes to those increments;
 * there is no WM here, so the host free-resizes and the final size can leave a
 * sub-cell remainder band the client never draws into. Called once at drag end,
 * this rounds the SDL window DOWN to the nearest whole increment (anchored at
 * base/min the ICCCM way) so the settled window is an exact number of cells and
 * no band survives. No-op unless the top-level published PResizeInc, so clients
 * that never set it are unaffected.
 *
 * Returns True if it changed the SDL size.
 *
 * The increments are cached in the same X11 geometry space as ws->w/h; SDL
 * sizing is in logical points. Promoted windows convert through the HiDPI
 * ratio, logical toolkit windows use a 1:1 conversion, using the same
 * echo-suppress + pump the client-initiated resize path (configureWindow) uses
 * so the snap does not emit a duplicate ConfigureNotify.
 */
int libx11CompatSnapAxisToIncrement(int current, int inc, int base, int min)
{
    if (inc <= 0)
        return current;
    int b = base > 0 ? base : 0;

    /* ICCCM size = base + i*inc for i >= 0. Round the step count down but never
     * below 0, so the base size itself (i == 0) is reachable instead of being
     * bumped up a whole increment.
     */
    int steps = (current - b) / inc;
    if (steps < 0)
        steps = 0;
    int snapped = b + steps * inc;

    /* Honor the client's declared minimum, then guard against a degenerate
     * non-positive window when the base is 0 and the size rounds to 0.
     */
    if (snapped < min)
        snapped = min;
    if (snapped <= 0)
        snapped = inc;
    return snapped;
}

static Bool snapTopLevelToResizeIncrements(Window window)
{
    WindowStruct *ws = GET_WINDOW_STRUCT(window);
    if (!ws->hasResizeInc || ws->widthInc <= 0 || ws->heightInc <= 0)
        return False;

    int logicalW = 0, logicalH = 0;
    SDL_GetWindowSize(ws->sdlWindow, &logicalW, &logicalH);
    double sx = effectiveHiDpiScaleX(ws);
    double sy = effectiveHiDpiScaleY(ws);
    int pixelW = (int) lround((double) logicalW * sx);
    int pixelH = (int) lround((double) logicalH * sy);

    int snappedW = libx11CompatSnapAxisToIncrement(pixelW, ws->widthInc,
                                                   ws->baseWidth, ws->minWidth);
    int snappedH = libx11CompatSnapAxisToIncrement(
        pixelH, ws->heightInc, ws->baseHeight, ws->minHeight);

    int snappedLogicalW = (int) lround((double) snappedW / sx);
    int snappedLogicalH = (int) lround((double) snappedH / sy);
    if (snappedLogicalW == logicalW && snappedLogicalH == logicalH)
        return False;

    /* Do not pump SDL here: this runs inside the live-resize present frame
     * (forceReflow), and SDL_PumpEvents would re-enter the event path the
     * present must not be nested under. The present cannot pump to flush the
     * SDL echo synchronously, so record the exact logical size written instead:
     * the filter drops every echo carrying that size (both the RESIZED and
     * SIZE_CHANGED posts) so the deferred echo does not escape as a redundant
     * ConfigureNotify, while a genuine resize to any other size still passes.
     * The size key is self-limiting - the filter clears it on the matching
     * RESIZED echo - and each snap overwrites it, so it cannot go stale.
     */
    ws->snapEchoW = snappedLogicalW;
    ws->snapEchoH = snappedLogicalH;
    SDL_SetWindowSize(ws->sdlWindow, snappedLogicalW, snappedLogicalH);
    return True;
}

int libx11CompatPresentDuringLiveResizeEx(int forceReflow)
{
    if (SCREEN_WINDOW == None || !IS_TYPE(SCREEN_WINDOW, WINDOW))
        return 0;
    if (!onMainEventThread())
        return 0;
    if (!SDL_AtomicCAS(&liveResizeInPresent, 0, 1)) {
        LR_TRACE("present\tre-entry-skipped");
        return 0;
    }
    liveResizePresentDepth++;
    LR_TRACE("present\tenter depth=%d", liveResizePresentDepth);

    LibX11CompatLiveResizeReflowFn reflow = liveResizeReflowFn;
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    size_t count = GET_WINDOW_STRUCT(SCREEN_WINDOW)->children.length;
    Bool anyPending = False;
    Bool geometryChanged = False;

    /* Per-window, per-tick gate (reset at the top of each window iteration):
     * only mark a window for present (and thus incur the costly full-window
     * readback in drawWindowDataToScreen) when this tick actually produced new
     * content - a reflow ran, the window is still moving (the compositor is
     * upscaling a stale backing, so the last-good frame must be re-presented to
     * stay sharp), or this is the forced drag-end settle. A quiet, already-
     * settled tick has nothing new to show, so re-presenting it only burns
     * ~40ms readback for an identical frame; skipping it keeps the run loop
     * from backing up on release, so the final snapped frame lands immediately
     * instead of behind a queue of redundant presents.
     */
    Bool presentThisTick = False;
    for (size_t i = 0; i < count; i++) {
        Window window = children[i];
        if (!IS_MAPPED_TOP_LEVEL_WINDOW(window))
            continue;

        presentThisTick = False;
        if (!reflow) {
            /* Mode 1 (no reflow hook): the client's draw loop is blocked for
             * the whole drag, so there is no new content to compute. Do NOT
             * snap here: snapTopLevelToResizeIncrements resizes the SDL window
             * and suppresses the RESIZED echo, but without a reflow hook
             * nothing updates ws->w/h or reflows the client to the snapped
             * size, so the client would keep drawing at the pre-snap size and
             * its content would be clipped by up to a cell. Leave the window on
             * the size the drag produced and re-present the existing last-good
             * backing every tick so the compositor's upscale of the live
             * (larger) surface stays as sharp as possible until the real
             * RESIZED event at drag end drives the full repaint.
             */
            presentThisTick = True;
        }

        if (reflow) {
            /* At drag end, snap this top-level to whole resize increments the
             * way a real WM would, so the client settles on a whole-cell size.
             * Only done when a reflow hook is registered: the reflow below
             * reads the snapped size and repaints the client to match, so the
             * snapped geometry and the client content stay consistent. Mid-drag
             * (forceReflow == 0) the window tracks the cursor freely; the
             * settle happens once on release.
             */
            if (forceReflow)
                snapTopLevelToResizeIncrements(window);

            /* The drag-end increment snap above ran for this top-level, so the
             * size read here already reflects the quantized geometry. Derive
             * the live drag size the SAME way the RESIZED ConfigureNotify
             * handler does: logical points from SDL_GetWindowSize, promoted to
             * physical pixels only for clients whose X11 geometry uses the
             * backing scale. Self-scaling toolkits such as Motif/Tk remain in
             * logical pixels. Re-querying SDL_GetWindowSurface here instead
             * returns a size in logical points on a Retina host, which would
             * make the client compute its cell grid against physical font
             * metrics in a logical-sized window and present a mismatched blit -
             * the glyphs come out distorted. Using the canonical formula keeps
             * every drag tick identical to the drag-end configure, so the grid
             * and backing stay in one coordinate space.
             */
            WindowStruct *ws = GET_WINDOW_STRUCT(window);
            int logicalW = 0, logicalH = 0;
            SDL_GetWindowSize(ws->sdlWindow, &logicalW, &logicalH);
            double sx = effectiveHiDpiScaleX(ws);
            double sy = effectiveHiDpiScaleY(ws);
            int pixelW = (int) lround((double) logicalW * sx);
            int pixelH = (int) lround((double) logicalH * sy);

            /* Skip reflow at degenerate drag sizes. When a border is dragged
             * past the opposite edge macOS briefly reports a 1-point (a few
             * pixels) dimension; xwpe's reflow divides the area into its cell
             * grid and hangs/misbehaves when a dimension collapses toward zero
             * (observed freeze at px=...x2). Below this floor we leave the
             * backing untouched and just re-present the last good frame, so the
             * process stays responsive until the drag returns to a sane size.
             */
            enum { LIVE_RESIZE_MIN_PX = 16 };
            if (pixelW >= LIVE_RESIZE_MIN_PX && pixelH >= LIVE_RESIZE_MIN_PX) {
                int oldWidth = 0, oldHeight = 0;
                GET_WINDOW_DIMS(window, oldWidth, oldHeight);

                /* Coalesce: track whether the window is still moving (its pixel
                 * size differs from the previous tick) and how long it has held
                 * one size. While it keeps moving we skip the costly reflow and
                 * only re-present, so the drag stays fluid; we reflow once the
                 * size settles (SETTLE_TICKS at rest) or at drag end
                 * (forceReflow). ~16ms/tick, so ~2 ticks is a ~30ms pause.
                 */
                enum { LIVE_RESIZE_SETTLE_TICKS = 2 };
                Bool moving = (pixelW != ws->liveResizeLastSeenW ||
                               pixelH != ws->liveResizeLastSeenH);
                if (moving) {
                    ws->liveResizeLastSeenW = pixelW;
                    ws->liveResizeLastSeenH = pixelH;
                    ws->liveResizeSettleTicks = 0;
                    geometryChanged = True; /* still dragging: keep armed */
                } else {
                    ws->liveResizeSettleTicks++;
                }
                Bool backingStale = (pixelW != oldWidth || pixelH != oldHeight);
                Bool doReflow = backingStale &&
                                (forceReflow || ws->liveResizeSettleTicks >=
                                                    LIVE_RESIZE_SETTLE_TICKS);

                /* Re-present only when something changed: a reflow will land
                 * new content, or the window is still moving so the upscaled
                 * stale backing must be refreshed, or this is the forced
                 * drag-end settle. A quiet, already-settled tick reuses the
                 * identical frame, so skip its readback.
                 */
                if (doReflow || moving || forceReflow)
                    presentThisTick = True;

                /* window=SDL logical points, backing=physical-pixel target the
                 * client draws into. A backing lagging the window is exactly
                 * the un-sourced band (black right/bottom edge).
                 */
                LR_TRACE(
                    "geom\twin_pt=%dx%d px=%dx%d backing=%dx%d "
                    "settle=%d%s%s",
                    logicalW, logicalH, pixelW, pixelH, oldWidth, oldHeight,
                    ws->liveResizeSettleTicks, moving ? " MOVING" : "",
                    doReflow ? " REFLOW" : "");
                if (doReflow) {
                    ws->w = (unsigned int) pixelW;
                    ws->h = (unsigned int) pixelH;
                    uint64_t rtStart = liveResizeNowNs();
                    resizeWindowTexture(window);

                    /* The top-level's own cached visibleRegion is its (0,0,w,h)
                     * frame; it goes stale the moment ws->w/h grow here. The
                     * client's reflow copies its backbuf into the window
                     * through XCopyArea, whose per-iteration clip intersects
                     * that stale (smaller) region, so the newly-grown
                     * columns/rows would be clipped away and stay black.
                     * Invalidate it exactly as the drag-end RESIZED
                     * ConfigureNotify path does.
                     */
                    invalidateVisibleRegionForTopLevel(window);

                    /* Hold the callback's intermediate XFlush presents so only
                     * its finished frame lands this tick.
                     */
                    beginCoalesceClientRepaint();
                    uint64_t reflowStart = liveResizeNowNs();
                    reflow(pixelW, pixelH);
                    uint64_t reflowEnd = liveResizeNowNs();
                    LR_TRACE("reflow\tretexture_ms=%.3f reflow_ms=%.3f",
                             (double) (reflowStart - rtStart) / 1.0e6,
                             (double) (reflowEnd - reflowStart) / 1.0e6);
                }
            } else {
                /* Degenerate drag size: skip the reflow but re-present the
                 * last-good frame so the window stays visible while the border
                 * is dragged past the opposite edge.
                 */
                presentThisTick = True;
                LR_TRACE("geom\tdegenerate px=%dx%d (skip reflow)", pixelW,
                         pixelH);
            }
        }

        /* Only incur the present readback when this tick produced new content.
         * A quiet, already-settled reflow tick reuses the identical frame, so
         * skipping its full-window readback keeps idle ticks near-free and lets
         * the run loop drain instantly on release.
         */
        if (presentThisTick) {
            markWindowNeedsPresent(window);
            anyPending = True;
        }
    }

    if (anyPending) {
        /* Release the coalesce gate and present the finished frame. When a
         * reflow ran this tick, this flushes the client's complete repaint;
         * otherwise it clears any stale gate left by a prior drag-end so the
         * re-present keeps the window sharp instead of being held back.
         */
        endCoalesceClientRepaint();
        uint64_t presentStart = liveResizeNowNs();
        drawWindowDataToScreen();
        LR_TRACE("present\tdraw_ms=%.3f",
                 (double) (liveResizeNowNs() - presentStart) / 1.0e6);
    }

    /* Clear the in-present flags before draining any deferred close so the real
     * XCloseDisplay runs with liveResizePresentDepth back at 0. The drain is
     * the LAST action so the whole body (including the reflow callback) is
     * bracketed by a nonzero depth.
     */
    liveResizePresentDepth--;
    SDL_AtomicSet(&liveResizeInPresent, 0);
    libx11CompatRunDeferredDisplayClose();
    LR_TRACE("present\texit pending=%d changed=%d", anyPending ? 1 : 0,
             geometryChanged ? 1 : 0);
    return geometryChanged ? 1 : 0;
}

static Bool getEventQueueLength(int *qlen)
{
    /* Motif expose/configure bursts and resize-driven repaints routinely push
     * the queue past 25 entries in a single SDL_PumpEvents tick; callers use
     * the returned length both to size a follow-up GETEVENT drain and to feed
     * XEventsQueued, so undercounting throttled Xt's main loop and dropped
     * events behind a quiet ceiling.
     *
     * Count with a NULL buffer: under SDL_PEEKEVENT, SDL_PeepEvents returns the
     * number of matching events without copying any and leaves the queue
     * intact, so the count is exact and unbounded. A real peek buffer would
     * need one SDL_Event per queued event on the stack; at any realistic cap
     * that array is hundreds of KB (4096 entries is ~512 KB) and overflows a
     * small secondary-thread stack (macOS threads default to 512 KB) the moment
     * an off-main caller drains events through here.
     *
     * SDL_PeepEvents has returned the full match count for a NULL buffer since
     * SDL 2.0.5 (earlier releases reported only a 0/1 presence result), so this
     * holds on the SDL2 2.0.10 differential host as well as on SDL2 2.30 and
     * SDL3.
     */
    *qlen =
        SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
    if (*qlen < 0) {
        LOG("Failed to get the length of the input queue: %s\n",
            SDL_GetError());
        return False;
    }
    return True;
}

static Bool enqueuePutBackEventWithTap(Display *display,
                                       const XEvent *event,
                                       Bool alreadyTapped)
{
    PutBackEvent *node = malloc(sizeof(PutBackEvent));
    if (!node) {
        handleOutOfMemory(0, display, 0, 0);
        return False;
    }

    node->display = display;
    memcpy(&node->event, event, sizeof(XEvent));
    node->alreadyTapped = alreadyTapped;
    lockPutBackEvents();
    node->next = putBackEvents;
    putBackEvents = node;
    unlockPutBackEvents();

    /* Match the SDL filter's pipe-byte accounting so that clients select()ing
     * on ConnectionNumber actually wake up for put-backs.
     */
    ENQUEUE_EVENT_IN_PIPE(display);
    return True;
}

static Bool enqueuePutBackEvent(Display *display, const XEvent *event)
{
    return enqueuePutBackEventWithTap(display, event, False);
}

static Bool appendPutBackEventWithTap(Display *display,
                                      const XEvent *event,
                                      Bool alreadyTapped)
{
    PutBackEvent *node = malloc(sizeof(PutBackEvent));
    if (!node) {
        handleOutOfMemory(0, display, 0, 0);
        return False;
    }
    node->display = display;
    memcpy(&node->event, event, sizeof(XEvent));
    node->alreadyTapped = alreadyTapped;
    node->next = NULL;

    lockPutBackEvents();
    PutBackEvent **link = &putBackEvents;
    while (*link)
        link = &(*link)->next;
    *link = node;
    unlockPutBackEvents();
    ENQUEUE_EVENT_IN_PIPE(display);
    return True;
}

static Bool appendPutBackEvent(Display *display, const XEvent *event)
{
    return appendPutBackEventWithTap(display, event, False);
}

static void deliverPutBackEvent(Display *display,
                                PutBackEvent *node,
                                XEvent *event)
{
    Bool alreadyTapped = node->alreadyTapped;
    memcpy(event, &node->event, sizeof(XEvent));
    free(node);

    /* Match the pipe byte enqueuePutBackEvent wrote so the wake-up accounting
     * stays consistent.
     */
    READ_EVENT_IN_PIPE(display);

    /* Put-back events that bypass convertEvent's terminal tap (synthesized
     * crossings, send-event payloads) need to be tapped here so timeline JSONL
     * captures the full event stream. Events that already went through
     * convertEvent's tap before being put back carry alreadyTapped=True so the
     * timeline does not see them twice.
     */
    if (!alreadyTapped)
        timelineTapXEvent(event);
}

static Bool popPutBackEvent(Display *display, XEvent *event)
{
    lockPutBackEvents();
    PutBackEvent **link = &putBackEvents;
    while (*link) {
        PutBackEvent *node = *link;
        if (node->display == display) {
            *link = node->next;
            unlockPutBackEvents();
            deliverPutBackEvent(display, node, event);
            return True;
        }
        link = &node->next;
    }
    unlockPutBackEvents();
    return False;
}

static int countPutBackEvents(Display *display)
{
    int count = 0;
    lockPutBackEvents();
    for (PutBackEvent *node = putBackEvents; node; node = node->next) {
        if (node->display == display)
            count++;
    }
    unlockPutBackEvents();
    return count;
}

static int drainSdlEventsToPutBack(Display *display)
{
    /* Serialize this compound drain-convert-refill-and-wake-accounting against
     * the same sequence in checkTypedEvent/checkIfEvent, which run under
     * LockDisplay. Without it, a thread in XPending/XEventsQueued (which reach
     * here) races a thread in XCheckTypedEvent: both SDL_PeepEvents(GETEVENT)
     * the same queue, so the count one reads goes stale under the other's drain
     * and resetEventWakeups writes the wrong number of pipe wake bytes. The
     * lock is recursive and only live under XInitThreads, so convertEvent
     * nesting back through here is safe and a single-threaded client pays
     * nothing.
     *
     * The convertEvent loop below can run a MAIN_DISPATCH thunk (a routed Xlib
     * body, e.g. mapWindowImpl) that pumps SDL / presents, so unlike the
     * original design this lock CAN be held across a re-entrant SDL call. That
     * is safe: the lock is recursive and this (main) thread is its owner, so
     * the thunk's nested Xlib entries just re-lock as the same owner, and the
     * main thread never blocks in runOnMainThread from here (it runs the thunk
     * inline). A future refinement could hoist such thunks out to the top-level
     * XNextEvent loop (which holds no lock) via the existing re-queue path.
     */
    LockDisplay(display);

    /* Run any dispatch commands parked while a handoff was pending; the handoff
     * may have cleared since the last drain even if no new event arrived.
     */
    mainDispatchRunDeferred();
    int qlen = 0;
    getEventQueueLength(&qlen);
    if (qlen <= 0) {
        int putBackCount = countPutBackEvents(display);
        if (displayEventQueueLength(display) > putBackCount)
            resetEventWakeups(display, putBackCount);
        UnlockDisplay(display);
        return putBackCount;
    }

    SDL_Event *events = calloc((size_t) qlen, sizeof(SDL_Event));
    if (!events) {
        UnlockDisplay(display);
        handleOutOfMemory(0, display, 0, 0);
        return countPutBackEvents(display);
    }

    qlen = sdlPeepEventsForXlibDrain(events, qlen, SDL_GETEVENT, SDL_FIRSTEVENT,
                                     SDL_LASTEVENT);
    if (qlen < 0) {
        LOG("Unable to read event queue: %s\n", SDL_GetError());
        free(events);
        UnlockDisplay(display);
        return countPutBackEvents(display);
    }

    for (int i = 0; i < qlen; i++)
        READ_EVENT_IN_PIPE(display);
    for (int i = 0; i < qlen; i++) {
        XEvent converted;
        if (convertEvent(display, &events[i], &converted, True) == 0)

            /* convertEvent already ran its terminal timelineTapXEvent for these
             * events; mark them so popPutBackEvent does not tap a second time
             * when they are eventually consumed.
             */
            appendPutBackEventWithTap(display, &converted, True);
    }
    int remainingSdlEvents = 0;
    if (getEventQueueLength(&remainingSdlEvents)) {
        int desiredQlen = countPutBackEvents(display) + remainingSdlEvents;
        if (displayEventQueueLength(display) != desiredQlen)
            resetEventWakeups(display, desiredQlen);
    }

    free(events);
    int result = countPutBackEvents(display);
    UnlockDisplay(display);
    return result;
}

static Bool enqueuePutBackExpose(Display *display, Window window)
{
    if (!HAS_EVENT_MASK(window, ExposureMask) || IS_INPUT_ONLY(window) ||
        GET_WINDOW_STRUCT(window)->mapState != Mapped) {
        return True;
    }
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xexpose.type = Expose;
    event.xexpose.serial = currentEventSerial();
    event.xexpose.send_event = False;
    event.xexpose.display = display;
    event.xexpose.window = window;
    event.xexpose.x = 0;
    event.xexpose.y = 0;
    GET_WINDOW_DIMS(window, event.xexpose.width, event.xexpose.height);
    event.xexpose.count = 0;
    return enqueuePutBackEvent(display, &event);
}

static void fillCrossingEvent(Display *display,
                              XCrossingEvent *event,
                              Window window,
                              int type,
                              int mode,
                              int detail,
                              unsigned int state)
{
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->serial = currentEventSerial();
    event->send_event = False;
    event->display = display;
    event->root = SCREEN_WINDOW;
    event->window = window;
    event->subwindow = None;
    event->time = CurrentTime;
    SDL_GetMouseState(&event->x_root, &event->y_root);
    event->x = event->x_root;
    event->y = event->y_root;
    if (window != SCREEN_WINDOW && IS_TYPE(window, WINDOW)) {
        translateWindowPoint(SCREEN_WINDOW, window, event->x_root,
                             event->y_root, &event->x, &event->y);
        event->subwindow =
            getDirectChildContainingPoint(window, event->x, event->y);
    }
    event->mode = mode;
    event->detail = detail;
    event->same_screen = True;
    event->focus = getKeyboardFocus() == window;
    event->state = state;
}

static void fillCrossingEventAt(Display *display,
                                XCrossingEvent *event,
                                Window window,
                                int type,
                                int mode,
                                int detail,
                                unsigned int state,
                                int rootX,
                                int rootY,
                                Time time)
{
    fillCrossingEvent(display, event, window, type, mode, detail, state);
    event->time = time;
    event->x_root = rootX;
    event->y_root = rootY;
    event->x = rootX;
    event->y = rootY;
    if (window != SCREEN_WINDOW && IS_TYPE(window, WINDOW)) {
        translateWindowPoint(SCREEN_WINDOW, window, rootX, rootY, &event->x,
                             &event->y);
        event->subwindow =
            getDirectChildContainingPoint(window, event->x, event->y);
    }
}

static void translateRootPointToWindow(Display *display,
                                       Window root,
                                       Window window,
                                       int rootX,
                                       int rootY,
                                       int *windowX,
                                       int *windowY)
{
    (void) display;
    *windowX = rootX;
    *windowY = rootY;
    if (window != None && window != root && IS_TYPE(window, WINDOW))
        translateWindowPoint(root, window, rootX, rootY, windowX, windowY);
}

static void translateSdlPointToRoot(Display *display,
                                    Window sdlWindow,
                                    int localX,
                                    int localY,
                                    int *rootX,
                                    int *rootY)
{
    (void) display;
    *rootX = localX;
    *rootY = localY;
    if (sdlWindow != None && sdlWindow != SCREEN_WINDOW &&
        IS_TYPE(sdlWindow, WINDOW)) {
        translateWindowPoint(sdlWindow, SCREEN_WINDOW, localX, localY, rootX,
                             rootY);
    }
}

/* Map a top-level's SDL-reported host origin to the absolute X11 logical origin
 * kept in ws->x/y. SDL reports an anchored xdg_popup's position relative to its
 * parent surface, so recover the absolute origin by adding the parent's own
 * absolute origin (both windows live in the same X11 logical space). Without
 * this a compositor slide/flip would store the small parent-relative offset as
 * the popup's absolute origin and every pointer event routed through it would
 * land off by the parent offset. Absolute top-levels take the ordinary
 * host-to-logical mapping.
 */
static void sdlTopLevelOriginToX11(Window window,
                                   int sdlX,
                                   int sdlY,
                                   int *outX,
                                   int *outY)
{
    WindowStruct *ws = GET_WINDOW_STRUCT(window);
    if (ws && ws->popupParent != None) {
        WindowStruct *parent = GET_WINDOW_STRUCT(ws->popupParent);
        if (parent) {
            *outX = parent->x + sdlX;
            *outY = parent->y + sdlY;
            return;
        }
    }
    topLevelWindowLogicalPosition(window, sdlX, sdlY, outX, outY);
}

/* Shared HiDPI point scaler; see the declaration in events.h for the rationale.
 * Kept here (the event delivery hot path) and reused by pointer.c so both agree
 * on rounding and scale selection.
 */
void scaleSdlPointToPixels(Window sdlWindow,
                           int inX,
                           int inY,
                           int *outX,
                           int *outY)
{
    double sx = 1.0, sy = 1.0;
    if (sdlWindow != None && sdlWindow != SCREEN_WINDOW &&
        IS_TYPE(sdlWindow, WINDOW)) {
        WindowStruct *ws = GET_WINDOW_STRUCT(sdlWindow);
        sx = effectiveHiDpiScaleX(ws);
        sy = effectiveHiDpiScaleY(ws);
    }
    *outX = (int) lround((double) inX * sx);
    *outY = (int) lround((double) inY * sy);
}

static Bool windowSelectsAny(Window window, long mask)
{
    return IS_TYPE(window, WINDOW) &&
           (GET_WINDOW_STRUCT(window)->eventMask & mask) != 0;
}

typedef struct {
    Window deepest;
    Window hoverDeepest;
} PointerHit;

/* Test-only instrumentation: counts pointer tree descents so check.c can assert
 * each event resolves its target once. The relaxed atomic add is negligible;
 * the reset/read helpers below carry external linkage solely so the separately
 * linked test binary can reach them.
 */
static unsigned long pointerResolveDescents = 0;

void resetPointerResolveDescentCount(void)
{
    __atomic_store_n(&pointerResolveDescents, 0, __ATOMIC_RELAXED);
}

unsigned long pointerResolveDescentCount(void)
{
    return __atomic_load_n(&pointerResolveDescents, __ATOMIC_RELAXED);
}

static Window countedContainingWindow(Window window, int x, int y)
{
    __atomic_add_fetch(&pointerResolveDescents, 1, __ATOMIC_RELAXED);
    return getContainingWindow(window, x, y);
}

/* Descend from base to the deepest window under a root-relative point. Root
 * coordinates are already base-local when base is the root itself; otherwise
 * subtract base's origin first.
 */
static Window descendFromBase(Window base, Window root, int rootX, int rootY)
{
    int localX = rootX, localY = rootY;
    if (base != root) {
        int bx = 0, by = 0;
        GET_WINDOW_POS(base, bx, by);
        localX = rootX - bx;
        localY = rootY - by;
    }
    return countedContainingWindow(base, localX, localY);
}

static Window directChildFromDeepest(Window window, Window deepest)
{
    Window child = deepest;
    while (child != None && child != window && IS_TYPE(child, WINDOW)) {
        Window parent = GET_PARENT(child);
        if (parent == window)
            return child;
        child = parent;
    }
    return None;
}

static PointerHit resolvePointerHit(Window root,
                                    Window clickTopLevel,
                                    int rootX,
                                    int rootY)
{
    PointerHit hit = {None, None};
    Window rootChild = getDirectChildContainingPoint(root, rootX, rootY);
    Window base = None;

    if (rootChild != None && rootChild != clickTopLevel &&
        IS_TYPE(rootChild, WINDOW) &&
        GET_WINDOW_STRUCT(rootChild)->overrideRedirect &&
        pointerHoverIsWithin(rootChild) && getGrabbedPointerWindow() != None) {
        base = rootChild;
    } else if (clickTopLevel != None && clickTopLevel != SCREEN_WINDOW &&
               IS_TYPE(clickTopLevel, WINDOW) &&
               GET_PARENT(clickTopLevel) == root) {
        int tlx = 0, tly = 0, tlw = 0, tlh = 0;
        GET_WINDOW_POS(clickTopLevel, tlx, tly);
        GET_WINDOW_DIMS(clickTopLevel, tlw, tlh);
        int localX = rootX - tlx, localY = rootY - tly;
        if (localX >= 0 && localX < tlw && localY >= 0 && localY < tlh)
            base = clickTopLevel;
    }

    if (base == None)
        base = rootChild != None ? rootChild : root;

    hit.deepest = descendFromBase(base, root, rootX, rootY);
    if (hit.deepest == None && base != rootChild) {
        base = rootChild != None ? rootChild : root;
        hit.deepest = descendFromBase(base, root, rootX, rootY);
    }
    if (base == root || base == rootChild || rootChild == None)
        hit.hoverDeepest = hit.deepest;
    else
        hit.hoverDeepest = descendFromBase(rootChild, root, rootX, rootY);
    return hit;
}

#if SDL_VERSION_ATLEAST(2, 0, 18)

/* Fold a sub-notch precise wheel delta into an integer notch. When the raw
 * integer axis already carries a notch, just clear the accumulator. Otherwise
 * accumulate the fraction and emit at most one notch per event, dropping any
 * surplus beyond a notch so a large burst delta cannot grow without bound.
 */
static int accumulateWheelNotch(int raw, float precise, float *accum)
{
    if (raw != 0) {
        *accum = 0.0f;
        return raw;
    }
    if (precise == 0.0f)
        return 0;
    int notch = 0;
    *accum += precise;
    if (*accum >= 1.0f) {
        notch = 1;
        *accum -= 1.0f;
    } else if (*accum <= -1.0f) {
        notch = -1;
        *accum += 1.0f;
    }
    if (*accum >= 1.0f || *accum <= -1.0f)
        *accum = 0.0f;
    return notch;
}
#endif

static Window selectPointerEventWindow(Display *display,
                                       Window root,
                                       int rootX,
                                       int rootY,
                                       Window deepest,
                                       long mask,
                                       Window *subwindowReturn,
                                       int *eventXReturn,
                                       int *eventYReturn)
{
    Window eventWindow = deepest;
    while (eventWindow != None && eventWindow != SCREEN_WINDOW &&
           !windowSelectsAny(eventWindow, mask)) {
        eventWindow = GET_PARENT(eventWindow);
    }
    if (eventWindow == None || !windowSelectsAny(eventWindow, mask))
        return None;

    translateRootPointToWindow(display, root, eventWindow, rootX, rootY,
                               eventXReturn, eventYReturn);
    if (subwindowReturn)
        *subwindowReturn = directChildFromDeepest(eventWindow, deepest);
    return eventWindow;
}

static Bool routePointerGrabEvent(Display *display,
                                  Window root,
                                  int rootX,
                                  int rootY,
                                  Window deepest,
                                  long mask,
                                  Window *eventWindow,
                                  Window *subwindowReturn,
                                  int *eventXReturn,
                                  int *eventYReturn)
{
    Window grabWindow = getGrabbedPointerWindow();
    if (grabWindow == None || !IS_TYPE(grabWindow, WINDOW))
        return False;

    if (getPointerGrabOwnerEvents()) {
        Window ownerWindow = selectPointerEventWindow(
            display, root, rootX, rootY, deepest, mask, subwindowReturn,
            eventXReturn, eventYReturn);
        if (ownerWindow != None) {
            *eventWindow = ownerWindow;
            return True;
        }
    }

    if ((getPointerGrabEventMask() & mask) == 0)
        return True;

    *eventWindow = grabWindow;
    translateRootPointToWindow(display, root, grabWindow, rootX, rootY,
                               eventXReturn, eventYReturn);
    if (subwindowReturn) {
        *subwindowReturn = directChildFromDeepest(grabWindow, deepest);
        if (*subwindowReturn == None)
            *subwindowReturn = getDirectChildContainingPoint(
                grabWindow, *eventXReturn, *eventYReturn);
    }
    return True;
}

Bool postCrossingEvent(Display *display,
                       Window window,
                       int type,
                       int mode,
                       int detail,
                       unsigned int state)
{
    long mask = type == EnterNotify ? EnterWindowMask : LeaveWindowMask;
    if (!HAS_EVENT_MASK(window, mask))
        return True;
    XCrossingEvent *event = malloc(sizeof(XCrossingEvent));
    if (!event) {
        handleOutOfMemory(0, display, 0, 0);
        return False;
    }
    fillCrossingEvent(display, event, window, type, mode, detail, state);
    if (!enqueueEvent(display, window, event)) {
        free(event);
        return False;
    }
    return True;
}

Bool postFocusEvent(Display *display, Window window, int type, int detail)
{
    if (window == None || !IS_TYPE(window, WINDOW) ||
        !HAS_EVENT_MASK(window, FocusChangeMask))
        return True;

    XFocusChangeEvent *event = malloc(sizeof(XFocusChangeEvent));
    if (!event) {
        handleOutOfMemory(0, display, 0, 0);
        return False;
    }
    event->type = type;
    event->serial = currentEventSerial();
    event->send_event = False;
    event->display = display;
    event->window = window;
    event->mode = NotifyNormal;
    event->detail = detail;
    if (!enqueueEvent(display, window, event)) {
        free(event);
        return False;
    }
    return True;
}

/* Defined later beside postPointerCrossingEvents, which reuses these. */
static int buildWindowPathToRoot(Window window, Window *path, int capacity);
static int findPathIndex(Window *path, int count, Window window);

static int rootDetailFor(FocusKind kind)
{
    return kind == FocusKindPointerRoot ? NotifyPointerRoot : NotifyDetailNone;
}

/* Full ancestor walk per Xlib spec 10.5/10.7. Motif's shell modal bookkeeping
 * needs the NotifyVirtual/NotifyNonlinearVirtual intermediates between each
 * leaf and the LCA, not just the leaf events. Transitions involving None or
 * PointerRoot also fire on the root with detail NotifyDetailNone or
 * NotifyPointerRoot so listeners can tell the two non-window targets apart.
 */
void postFocusChange(Display *display,
                     FocusKind oldKind,
                     Window oldFocus,
                     FocusKind newKind,
                     Window newFocus)
{
    if (oldKind == newKind && oldFocus == newFocus)
        return;

    if (oldKind == FocusKindWindow && newKind == FocusKindWindow) {
        Window oldPath[256];
        Window newPath[256];
        int oldCount = IS_TYPE(oldFocus, WINDOW)
                           ? buildWindowPathToRoot(oldFocus, oldPath, 256)
                           : 0;
        int newCount = IS_TYPE(newFocus, WINDOW)
                           ? buildWindowPathToRoot(newFocus, newPath, 256)
                           : 0;

        int oldLca = -1, newLca = -1;
        for (int i = 0; i < oldCount; i++) {
            int j = findPathIndex(newPath, newCount, oldPath[i]);
            if (j >= 0) {
                oldLca = i;
                newLca = j;
                break;
            }
        }

        /* oldLca >= 0 implies both paths are non-empty: the LCA search only
         * runs when oldCount > 0 and j >= 0 requires newCount > 0.
         */
        if (oldLca == 0) {
            postFocusEvent(display, oldPath[0], FocusOut, NotifyInferior);
            for (int i = newLca - 1; i > 0; i--)
                postFocusEvent(display, newPath[i], FocusIn, NotifyVirtual);
            postFocusEvent(display, newPath[0], FocusIn, NotifyAncestor);
            return;
        }
        if (newLca == 0) {
            postFocusEvent(display, oldPath[0], FocusOut, NotifyAncestor);
            for (int i = 1; i < oldLca; i++)
                postFocusEvent(display, oldPath[i], FocusOut, NotifyVirtual);
            postFocusEvent(display, newPath[0], FocusIn, NotifyInferior);
            return;
        }

        /* Nonlinear: different subtrees, or one side has no LCA. */
        if (oldCount > 0) {
            int limit = oldLca >= 0 ? oldLca : oldCount;
            postFocusEvent(display, oldPath[0], FocusOut, NotifyNonlinear);
            for (int i = 1; i < limit; i++)
                postFocusEvent(display, oldPath[i], FocusOut,
                               NotifyNonlinearVirtual);
        }
        if (newCount > 0) {
            int limit = newLca >= 0 ? newLca : newCount;
            for (int i = limit - 1; i > 0; i--)
                postFocusEvent(display, newPath[i], FocusIn,
                               NotifyNonlinearVirtual);
            postFocusEvent(display, newPath[0], FocusIn, NotifyNonlinear);
        }
        return;
    }

    /* One side is None or PointerRoot (Xlib 10.7.1: nonlinear). The window
     * chain uses NotifyNonlinear[Virtual] all the way through the root, then
     * the root receives a separate companion event with the non-window detail
     * (rootDetailFor) naming the arrived state.
     */
    if (oldKind == FocusKindWindow) {
        Window oldPath[256];
        int oldCount = IS_TYPE(oldFocus, WINDOW)
                           ? buildWindowPathToRoot(oldFocus, oldPath, 256)
                           : 0;
        if (oldCount == 0) {
            postFocusEvent(display, SCREEN_WINDOW, FocusIn,
                           rootDetailFor(newKind));
            return;
        }
        postFocusEvent(display, oldPath[0], FocusOut, NotifyNonlinear);
        for (int i = 1; i < oldCount; i++)
            postFocusEvent(display, oldPath[i], FocusOut,
                           NotifyNonlinearVirtual);
        postFocusEvent(display, oldPath[oldCount - 1], FocusIn,
                       rootDetailFor(newKind));
    } else if (newKind == FocusKindWindow) {
        Window newPath[256];
        int newCount = IS_TYPE(newFocus, WINDOW)
                           ? buildWindowPathToRoot(newFocus, newPath, 256)
                           : 0;
        if (newCount == 0) {
            postFocusEvent(display, SCREEN_WINDOW, FocusOut,
                           rootDetailFor(oldKind));
            return;
        }
        postFocusEvent(display, newPath[newCount - 1], FocusOut,
                       rootDetailFor(oldKind));
        for (int i = newCount - 1; i > 0; i--)
            postFocusEvent(display, newPath[i], FocusIn,
                           NotifyNonlinearVirtual);
        postFocusEvent(display, newPath[0], FocusIn, NotifyNonlinear);
    } else {
        /* None <-> PointerRoot: only the root sees the transition. */
        postFocusEvent(display, SCREEN_WINDOW, FocusOut,
                       rootDetailFor(oldKind));
        postFocusEvent(display, SCREEN_WINDOW, FocusIn, rootDetailFor(newKind));
    }
}

static Bool enqueueResetExposures(Display *display)
{
    WindowStruct *screen = GET_WINDOW_STRUCT(SCREEN_WINDOW);
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    for (size_t i = screen->children.length; i > 0; i--) {
        Window child = children[i - 1];
        if (GET_WINDOW_STRUCT(child)->sdlWindow &&
            !enqueuePutBackExpose(display, child)) {
            return False;
        }
    }
    return True;
}

static Bool removeMatchingPutBackEvent(
    Display *display,
    Window w,
    int type,
    long mask,
    XEvent *event,
    Bool(predicate)(Window, int, long, XEvent *))
{
    lockPutBackEvents();
    PutBackEvent **link = &putBackEvents;
    while (*link) {
        PutBackEvent *node = *link;
        if (node->display == display &&
            predicate(w, type, mask, &node->event)) {
            *link = node->next;
            unlockPutBackEvents();
            deliverPutBackEvent(display, node, event);
            return True;
        }
        link = &node->next;
    }
    unlockPutBackEvents();
    return False;
}

static Bool removeMatchingPutBackIfEvent(Display *display,
                                         XEvent *event,
                                         Bool (*predicate)(Display *,
                                                           XEvent *,
                                                           char *),
                                         char *arg)
{
    lockPutBackEvents();
    PutBackEvent **link = &putBackEvents;
    while (*link) {
        PutBackEvent *node = *link;
        if (node->display == display && predicate(display, &node->event, arg)) {
            *link = node->next;
            unlockPutBackEvents();
            deliverPutBackEvent(display, node, event);
            return True;
        }
        link = &node->next;
    }
    unlockPutBackEvents();
    return False;
}

/* An IM commit's text lives in the global commit table keyed by the id stored
 * in xkey.subwindow. When its synthetic KeyPress is discarded instead of
 * delivered, release that entry so it does not linger until eviction. Real keys
 * leave subwindow None, so the keycode==0 guard keeps this from touching a
 * normal KeyPress whose subwindow is a child window.
 */
static void releaseDiscardedImCommit(const XEvent *event)
{
    if (event->type == KeyPress && event->xkey.keycode == 0)
        inputMethodConsumePendingText((unsigned long) event->xkey.subwindow);
}

void discardQueuedEventsForWindow(Display *display, Window window)
{
    /* Hold the display lock across BOTH the put-back sweep and the SDL drain.
     * If it were taken only around the drain, a concurrent XCheck* on another
     * thread (holding LockDisplay) could drain an SDL event for this window and
     * append it to the put-back list in the gap after the sweep, so the event
     * would survive teardown. Holding it throughout makes the discard atomic
     * against every consumer. It also serializes the drain's
     * SDL_PeepEvents(GETEVENT) and wake-byte accounting against
     * drainSdlEventsToPutBack/checkTypedEvent, which run under the same lock.
     * Recursive and XInitThreads-gated, so a single-threaded caller pays
     * nothing, and putBackEventsLock nests inside (display-then-fine-grained).
     */
    LockDisplay(display);
    int removedPutBackEvents = 0;
    lockPutBackEvents();
    PutBackEvent **link = &putBackEvents;
    while (*link) {
        PutBackEvent *node = *link;
        if (node->display == display && node->event.xany.window == window) {
            *link = node->next;
            releaseDiscardedImCommit(&node->event);
            free(node);
            removedPutBackEvents++;
            continue;
        }
        link = &node->next;
    }
    unlockPutBackEvents();

    while (removedPutBackEvents-- > 0)
        READ_EVENT_IN_PIPE(display);

    int qlen = 0;
    getEventQueueLength(&qlen);
    if (qlen <= 0) {
        UnlockDisplay(display);
        return;
    }

    SDL_Event *events = calloc((size_t) qlen, sizeof(SDL_Event));
    if (!events) {
        UnlockDisplay(display);
        handleOutOfMemory(0, display, 0, 0);
        return;
    }
    qlen = sdlPeepEventsForXlibDrain(events, qlen, SDL_GETEVENT, SDL_FIRSTEVENT,
                                     SDL_LASTEVENT);
    if (qlen < 0) {
        LOG("Unable to read event queue: %s\n", SDL_GetError());
        free(events);
        UnlockDisplay(display);
        return;
    }

    for (int i = 0; i < qlen; i++)
        READ_EVENT_IN_PIPE(display);
    for (int i = 0; i < qlen; i++) {
        XEvent converted;
        if (convertEvent(display, &events[i], &converted, True) != 0)
            continue;
        if (converted.xany.window == window) {
            /* Discarded with the window: release any IM commit it carried so
             * its entry does not linger until eviction.
             */
            releaseDiscardedImCommit(&converted);
            continue;
        }
        appendPutBackEventWithTap(display, &converted, True);
    }
    int remainingSdlEvents = 0;
    if (getEventQueueLength(&remainingSdlEvents)) {
        int desiredQlen = countPutBackEvents(display) + remainingSdlEvents;
        if (displayEventQueueLength(display) != desiredQlen)
            resetEventWakeups(display, desiredQlen);
    }
    free(events);
    UnlockDisplay(display);
}

/* SDL_SetEventFilter only stores one (callback, userdata) pair, so the event
 * layer keeps a registry of every Display that has installed onSdlEvent. The
 * active slot always points at the most recent open; closing the active slot
 * hands it off to whichever display is still open (most-recently-opened wins)
 * instead of nulling the filter and stranding the remaining display(s). The
 * registry uses the project's dynamic Array helper so multi-display flows are
 * not silently capped at an arbitrary number of opens.
 */
static void trackDisplay(Display *display)
{
    lockTrackedDisplays();
    if (findInArray(&trackedDisplays, display) < 0) {
        if (!insertArray(&trackedDisplays, display)) {
            LOG("Failed to track display %p for SDL event-filter handoff\n",
                (void *) display);
        }
    }
    unlockTrackedDisplays();
}

static void untrackDisplay(Display *display)
{
    lockTrackedDisplays();
    ssize_t index = findInArray(&trackedDisplays, display);
    if (index >= 0)
        removeArray(&trackedDisplays, (size_t) index, True);
    if (trackedDisplays.length == 0)
        freeArray(&trackedDisplays);
    unlockTrackedDisplays();
}

/* Lazily create a named mutex slot, logging the SDL error on failure. The name
 * is interpolated into the LOG message so each lock keeps its own diagnostic
 * identity without four near-identical init blocks.
 */
static Bool ensureNamedMutex(SDL_mutex **slot, const char *name)
{
    if (*slot)
        return True;
    *slot = SDL_CreateMutex();
    if (!*slot) {
        LOG("Could not create the %s lock: %s", name, SDL_GetError());
        return False;
    }
    return True;
}

static void destroyMutexSlot(SDL_mutex **slot)
{
    if (*slot) {
        SDL_DestroyMutex(*slot);
        *slot = NULL;
    }
}

int initEventPipe(Display *display)
{
    captureMainEventThreadIfUnset();
    __atomic_store_n(&lastEventSerial, 1, __ATOMIC_RELAXED);
    SDL_AtomicLock(&eventPipeGlobalLock);
    if (!ensureNamedMutex(&eventQueueLengthLock, "event queue") ||
        !ensureNamedMutex(&putBackEventsLock, "put-back events") ||
        !ensureNamedMutex(&trackedDisplaysLock, "tracked-displays") ||
        !ensureNamedMutex(&activePointerWindowLock, "active-pointer-window")) {
        SDL_AtomicUnlock(&eventPipeGlobalLock);
        return -1;
    }

    /* The eventFds globals are shared across displays. Only the first open
     * creates the pipe; subsequent opens reuse it. Recreating it per call would
     * leak the previous fds since closeEventPipe runs only on the final display
     * close.
     */
    lockTrackedDisplays();
    Bool isFirstDisplay = trackedDisplays.length == 0;
    unlockTrackedDisplays();
    if (isFirstDisplay) {
        if (pipe(eventFds) == -1) {
            LOG("Could not create the event pipe: %s", strerror(errno));
            SDL_AtomicUnlock(&eventPipeGlobalLock);
            return -1;
        }

        /* Real X11 clients select() / poll() on the connection FD then call
         * XNextEvent in a non-blocking mode. The pipe is just a wake-up signal;
         * reads happen inside XNextEvent itself, so make the FD non-blocking so
         * a desynced qlen does not stall the whole event loop on a missing
         * byte. The original spelling used F_SETFD (FD_CLOEXEC) instead of
         * F_SETFL (O_NONBLOCK), silently leaving the FD in blocking mode.
         */
        int flags = fcntl(READ_EVENT_FD, F_GETFL);
        fcntl(READ_EVENT_FD, F_SETFL, flags | O_NONBLOCK);
        flags = fcntl(WRITE_EVENT_FD, F_GETFL);
        fcntl(WRITE_EVENT_FD, F_SETFL, flags | O_NONBLOCK);
    }
    int qlen;
    getEventQueueLength(&qlen);
    LOG("Events in queue = %d at initialisation\n", qlen);
    for (; qlen > 0; qlen--)
        ENQUEUE_EVENT_IN_PIPE(display);
    SDL_SetEventFilter(onSdlEvent, display);
    trackDisplay(display);
    if (isFirstDisplay && xtWakeTimer == 0 && shouldStartXtWakeTimer()) {
        SDL_AtomicSet(&pumpWakePending, 0);
        xtWakeTimer = SDL_AddTimer(33, xtWakeTimerCallback, NULL);

        /* Keep the screen sharp during the macOS live-resize modal loop, where
         * SDL delivers no events (see libx11CompatPresentDuringLiveResize). A
         * run-loop observer drives the present on each drag tick. No-op on
         * non-macOS and under the dummy driver.
         */
        libx11CompatStartLiveResizeObserver();
    }
    SDL_AtomicUnlock(&eventPipeGlobalLock);
    return READ_EVENT_FD;
}

/* Detach the SDL event filter before XCloseDisplay frees the Display.
 * SDL_SetEventFilter only tracks the latest registration, so closing a display
 * whose pointer is still installed as filter userdata leaves a dangling
 * reference; the next SDL_PumpEvents would deliver an event through onSdlEvent
 * and dereference freed memory (AddressSanitizer caught this as a
 * heap-use-after-free during test_extensions when a nested
 * XOpenDisplay/XCloseDisplay pair handed the slot off and the outer display
 * then pushed an event). When other displays remain open closeEventPipe hands
 * the slot to one of them so their event pipe keeps draining; only the final
 * close nulls the filter.
 */
void closeEventPipe(Display *display)
{
    SDL_AtomicLock(&eventPipeGlobalLock);
    untrackDisplay(display);
    SDL_EventFilter currentFilter = NULL;
    void *currentUserdata = NULL;
    SDL_GetEventFilter(&currentFilter, &currentUserdata);
    lockTrackedDisplays();
    size_t remainingDisplays = trackedDisplays.length;
    Display *handoffTarget = remainingDisplays > 0
                                 ? trackedDisplays.array[remainingDisplays - 1]
                                 : NULL;
    unlockTrackedDisplays();

    /* The SDL filter handoff is conditional: only the display that currently
     * owns the slot (currentUserdata == display) hands it over. Last-display
     * cleanup is unconditional: when no displays remain, mutex slots and pipe
     * fds always tear down. Otherwise a close after a third-party
     * SDL_SetEventFilter would leak the pipe fds and the three lock slots.
     */
    if (currentFilter == onSdlEvent && currentUserdata == display) {
        if (remainingDisplays > 0)
            SDL_SetEventFilter(onSdlEvent, handoffTarget);
        else
            SDL_SetEventFilter(NULL, NULL);
    }
    if (remainingDisplays == 0) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
        wheelPreciseX = 0.0f;
        wheelPreciseY = 0.0f;
#endif
        if (xtWakeTimer != 0) {
            SDL_RemoveTimer(xtWakeTimer);
            xtWakeTimer = 0;
        }

        /* Stop the observer unconditionally: it may have been armed even when
         * SDL_AddTimer failed (returned 0), so gating this on xtWakeTimer would
         * leak a still-registered run-loop observer past the last display.
         */
        libx11CompatStopLiveResizeObserver();
        SDL_AtomicSet(&pumpWakePending, 0);
        destroyMutexSlot(&eventQueueLengthLock);
        destroyMutexSlot(&putBackEventsLock);
        destroyMutexSlot(&trackedDisplaysLock);
        destroyMutexSlot(&activePointerWindowLock);

        /* Release the shared pipe fds so a later XOpenDisplay creates fresh
         * ones instead of leaking them.
         */
        if (READ_EVENT_FD >= 0) {
            close(READ_EVENT_FD);
            READ_EVENT_FD = -1;
        }
        if (WRITE_EVENT_FD >= 0) {
            close(WRITE_EVENT_FD);
            WRITE_EVENT_FD = -1;
        }
    }
    SDL_AtomicUnlock(&eventPipeGlobalLock);
}

unsigned int convertModifierState(Uint16 mod)
{
    unsigned int state = 0;
    if (HAS_VALUE(mod, KMOD_SHIFT))
        state |= ShiftMask;
    if (HAS_VALUE(mod, KMOD_CTRL))
        state |= ControlMask;
    if (HAS_VALUE(mod, KMOD_CAPS))
        state |= LockMask;

    /* X11 convention: Mod1Mask is Alt, Mod2Mask is NumLock. Motif accelerators
     * (Alt-F for File, Alt-E for Edit, etc.) test for Mod1; without this,
     * accelerators silently never fire. The previous mapping put NumLock on
     * Mod1, leaving Alt unreachable.
     */
    if (HAS_VALUE(mod, KMOD_ALT))
        state |= Mod1Mask;
    if (HAS_VALUE(mod, KMOD_NUM))
        state |= Mod2Mask;
    return state;
}

static unsigned int pointerButtonState = 0;

/* pointerButtonState shares activePointerWindowLock because the two form one
 * logical "pointer state" tuple. Motion and wheel handlers only need the
 * current value, so snapshot it through the lock to avoid a torn read against
 * the button-press/release writer.
 */
static unsigned int pointerButtonStateSnapshot(void)
{
    lockActivePointerWindow();
    unsigned int s = pointerButtonState;
    unlockActivePointerWindow();
    return s;
}
static Window activePointerWindow = None;
static Window pointerHoverWindow = None;
static int pointerHoverRootX = 0, pointerHoverRootY = 0;

static Bool pointerHoverIsWithin(Window window)
{
    Bool inside = False;
    lockActivePointerWindow();
    inside =
        pointerHoverWindow == window || (IS_TYPE(pointerHoverWindow, WINDOW) &&
                                         isParent(window, pointerHoverWindow));
    unlockActivePointerWindow();
    return inside;
}

static int buildWindowPathToRoot(Window window, Window *path, int capacity)
{
    int count = 0;
    while (window != None && IS_TYPE(window, WINDOW) && count < capacity) {
        path[count++] = window;
        if (window == SCREEN_WINDOW)
            break;
        window = GET_PARENT(window);
    }
    return count;
}

static int findPathIndex(Window *path, int count, Window window)
{
    for (int i = 0; i < count; i++) {
        if (path[i] == window)
            return i;
    }
    return -1;
}

static Bool appendPointerCrossingEvent(Display *display,
                                       Window window,
                                       int type,
                                       int detail,
                                       unsigned int state,
                                       int rootX,
                                       int rootY,
                                       Time time)
{
    long mask = type == EnterNotify ? EnterWindowMask : LeaveWindowMask;
    if (window == None || !IS_TYPE(window, WINDOW) ||
        !HAS_EVENT_MASK(window, mask))
        return False;

    XEvent event;
    fillCrossingEventAt(display, &event.xcrossing, window, type, NotifyNormal,
                        detail, state, rootX, rootY, time);
    return appendPutBackEvent(display, &event);
}

void clearActivePointerWindow(void)
{
    lockActivePointerWindow();
    activePointerWindow = None;
    pointerHoverWindow = None;
    unlockActivePointerWindow();
}

/* Clear cached pointer-target XIDs when their window is being destroyed.
 * Without this, the next SDL motion event drives postPointerCrossingEvents
 * which walks GET_PARENT(pointerHoverWindow) into a freed WindowStruct
 * (ASan-flagged heap-use-after-free in CI). Match the pattern of
 * releaseButtonGrabsForWindow: invoked from destroyWindow before the
 * WindowStruct is freed.
 */
void clearPointerStateForWindow(Window window)
{
    if (window == None)
        return;
    lockActivePointerWindow();
    if (activePointerWindow == window ||
        (IS_TYPE(activePointerWindow, WINDOW) &&
         isParent(window, activePointerWindow))) {
        activePointerWindow = None;
        pointerButtonState = 0;
    }
    if (pointerHoverWindow == window || (IS_TYPE(pointerHoverWindow, WINDOW) &&
                                         isParent(window, pointerHoverWindow)))
        pointerHoverWindow = None;
    unlockActivePointerWindow();
}

static Bool postPointerCrossingEvents(Display *display,
                                      int rootX,
                                      int rootY,
                                      Window newHoverWindow,
                                      unsigned int state,
                                      Time time)
{
    /* Hold activePointerWindowLock through the path walks AND the
     * appendPointerCrossingEvent calls, not just the initial snapshot. Both
     * reviewers (PR #7 round 4) flagged a UAF window in the earlier shape: this
     * function dropped the lock right after sampling oldHoverWindow, then
     * walked its WindowStruct via buildWindowPathToRoot. A concurrent
     * XDestroyWindow on oldHoverWindow could pass destroyWindow's
     * clearPointerStateForWindow (the lock briefly cleared and re-released) and
     * then free the WindowStruct before this thread's walk dereferenced it.
     *
     * Lock-order check: this function calls appendPointerCrossingEvent ->
     * appendPutBackEvent which takes putBackEventsLock. destroyWindow takes
     * putBackEventsLock first (via discardQueuedEventsForWindow) and releases
     * before reaching clearPointerStateForWindow, so the two threads never try
     * to hold both locks simultaneously and there is no circular wait.
     */
    lockActivePointerWindow();
    Window oldHoverWindow = pointerHoverWindow;
    if (oldHoverWindow == newHoverWindow) {
        pointerHoverRootX = rootX;
        pointerHoverRootY = rootY;
        unlockActivePointerWindow();
        return False;
    }
    pointerHoverWindow = newHoverWindow;
    pointerHoverRootX = rootX;
    pointerHoverRootY = rootY;

    Window oldPath[256];
    Window newPath[256];
    int oldCount = buildWindowPathToRoot(oldHoverWindow, oldPath, 256);
    int newCount = buildWindowPathToRoot(newHoverWindow, newPath, 256);
    int oldLca = -1;
    int newLca = -1;
    for (int i = 0; i < oldCount; i++) {
        int j = findPathIndex(newPath, newCount, oldPath[i]);
        if (j >= 0) {
            oldLca = i;
            newLca = j;
            break;
        }
    }

    Bool queued = False;
    if (oldLca == 0 && newLca > 0) {
        queued |= appendPointerCrossingEvent(display, oldPath[0], LeaveNotify,
                                             NotifyInferior, state, rootX,
                                             rootY, time);
        for (int i = newLca - 1; i > 0; i--) {
            queued |= appendPointerCrossingEvent(display, newPath[i],
                                                 EnterNotify, NotifyVirtual,
                                                 state, rootX, rootY, time);
        }
        queued |= appendPointerCrossingEvent(display, newPath[0], EnterNotify,
                                             NotifyAncestor, state, rootX,
                                             rootY, time);
        unlockActivePointerWindow();
        return queued;
    }

    if (newLca == 0 && oldLca > 0) {
        queued |= appendPointerCrossingEvent(display, oldPath[0], LeaveNotify,
                                             NotifyAncestor, state, rootX,
                                             rootY, time);
        for (int i = 1; i < oldLca; i++) {
            queued |= appendPointerCrossingEvent(display, oldPath[i],
                                                 LeaveNotify, NotifyVirtual,
                                                 state, rootX, rootY, time);
        }
        queued |= appendPointerCrossingEvent(display, newPath[0], EnterNotify,
                                             NotifyInferior, state, rootX,
                                             rootY, time);
        unlockActivePointerWindow();
        return queued;
    }

    int oldLimit = oldLca >= 0 ? oldLca : oldCount;
    for (int i = 0; i < oldLimit; i++) {
        int detail = i == 0 ? NotifyNonlinear : NotifyNonlinearVirtual;
        queued |= appendPointerCrossingEvent(display, oldPath[i], LeaveNotify,
                                             detail, state, rootX, rootY, time);
    }

    int newLimit = newLca >= 0 ? newLca : newCount;
    for (int i = newLimit - 1; i >= 0; i--) {
        int detail = i == 0 ? NotifyNonlinear : NotifyNonlinearVirtual;
        queued |= appendPointerCrossingEvent(display, newPath[i], EnterNotify,
                                             detail, state, rootX, rootY, time);
    }
    unlockActivePointerWindow();
    return queued;
}

static Bool queueNestedPointerLeaves(Display *display,
                                     Window topLevel,
                                     unsigned int state,
                                     Time time)
{
    /* Same lifetime guard as postPointerCrossingEvents: hold the lock through
     * buildWindowPathToRoot and the appendPointerCrossingEvent calls so a
     * concurrent destroyWindow on a hover descendant cannot free the
     * WindowStruct mid-walk.
     */
    lockActivePointerWindow();
    Window oldHoverWindow = pointerHoverWindow;
    int rootX = pointerHoverRootX, rootY = pointerHoverRootY;
    pointerHoverWindow = None;

    if (oldHoverWindow == None || oldHoverWindow == topLevel) {
        unlockActivePointerWindow();
        return False;
    }

    Window oldPath[256];
    int oldCount = buildWindowPathToRoot(oldHoverWindow, oldPath, 256);
    Bool queued = False;
    for (int i = 0; i < oldCount && oldPath[i] != topLevel; i++) {
        int detail = i == 0 ? NotifyAncestor : NotifyVirtual;
        queued |= appendPointerCrossingEvent(display, oldPath[i], LeaveNotify,
                                             detail, state, rootX, rootY, time);
    }
    unlockActivePointerWindow();
    return queued;
}

static unsigned int convertSdlMouseButton(Uint8 button)
{
    if (button == SDL_BUTTON_LEFT)
        return Button1;
    if (button == SDL_BUTTON_MIDDLE)
        return Button2;
    if (button == SDL_BUTTON_RIGHT)
        return Button3;
    if (button == SDL_BUTTON_X1)
        return Button4;
    return Button5;
}

static unsigned int buttonMaskForXButton(unsigned int button)
{
    switch (button) {
    case Button1:
        return Button1Mask;
    case Button2:
        return Button2Mask;
    case Button3:
        return Button3Mask;
    case Button4:
        return Button4Mask;
    case Button5:
        return Button5Mask;
    default:
        return 0;
    }
}

static long motionMaskForButtonState(unsigned int buttonState)
{
    long mask = PointerMotionMask | PointerMotionHintMask;
    if (buttonState & Button1Mask)
        mask |= ButtonMotionMask | Button1MotionMask;
    if (buttonState & Button2Mask)
        mask |= ButtonMotionMask | Button2MotionMask;
    if (buttonState & Button3Mask)
        mask |= ButtonMotionMask | Button3MotionMask;
    if (buttonState & Button4Mask)
        mask |= ButtonMotionMask | Button4MotionMask;
    if (buttonState & Button5Mask)
        mask |= ButtonMotionMask | Button5MotionMask;
    return mask;
}

static char pendingAsciiRing[4];
static unsigned pendingAsciiHead, pendingAsciiTail;

static void clearPendingAsciiRing(void)
{
    pendingAsciiHead = pendingAsciiTail = 0;
}

static void pushPendingAsciiChar(char c)
{
    unsigned next = (pendingAsciiTail + 1) % 4;
    if (next != pendingAsciiHead) {
        pendingAsciiRing[pendingAsciiTail] = c;
        pendingAsciiTail = next;
    }
}

static Bool popMatchingPendingAsciiChar(char c)
{
    if (pendingAsciiHead == pendingAsciiTail || !c)
        return False;
    if (pendingAsciiRing[pendingAsciiHead] != c)
        return False;
    pendingAsciiHead = (pendingAsciiHead + 1) % 4;
    return True;
}

static char asciiTextForKeydown(Display *display, const SDL_Event *event)
{
    SDL_Keycode keycode = XC_EVENT_KEYSYM(event);
    unsigned int state = convertModifierState(XC_EVENT_KEYMOD(event));
    if (keycode >= SDLK_a && keycode <= SDLK_z) {
        Bool upper = !!(state & ShiftMask) != !!(state & LockMask);
        return upper ? (char) ('A' + keycode - SDLK_a) : (char) keycode;
    }
    KeySym keysym = NoSymbol;
    if (XkbLookupKeySym(display, (KeyCode) keycode, state, NULL, &keysym) &&
        keysym >= XK_space && keysym <= XK_asciitilde)
        return (char) keysym;
    return '\0';
}

static Bool shouldSuppressIMTextKeydown(Display *display,
                                        const SDL_Event *event)
{
    SDL_Keycode keycode = XC_EVENT_KEYSYM(event);
    SDL_Keymod modifiers = XC_EVENT_KEYMOD(event);
    if (inputMethodHasActivePreedit() &&
        (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER))
        return True;

    /* Any printable character key also arrives as SDL_TEXTINPUT, so suppressing
     * only ASCII would let a non-ASCII layout key (Latin-1 or any Unicode
     * codepoint) emit both a raw KeyPress and the IM commit. SDL keycodes for
     * printable keys are the Unicode value itself, so cover the whole codepoint
     * range; controls (< space, 0x7f..0x9f) and special keys (>= 0x40000000)
     * fall outside it and stay unsuppressed.
     */
    Bool textKey = (keycode >= XK_space && keycode <= XK_asciitilde) ||
                   (keycode >= XK_nobreakspace && keycode <= 0x10ffff);
    switch (keycode) {
    case SDLK_KP_0:
    case SDLK_KP_1:
    case SDLK_KP_2:
    case SDLK_KP_3:
    case SDLK_KP_4:
    case SDLK_KP_5:
    case SDLK_KP_6:
    case SDLK_KP_7:
    case SDLK_KP_8:
    case SDLK_KP_9:
    case SDLK_KP_DIVIDE:
    case SDLK_KP_MULTIPLY:
    case SDLK_KP_MINUS:
    case SDLK_KP_PLUS:
    case SDLK_KP_PERIOD:
    case SDLK_KP_EQUALS:
    case SDLK_KP_COMMA:
    case SDLK_KP_EQUALSAS400:
    case SDLK_KP_LEFTPAREN:
    case SDLK_KP_RIGHTPAREN:
    case SDLK_KP_LEFTBRACE:
    case SDLK_KP_RIGHTBRACE:
    case SDLK_KP_A:
    case SDLK_KP_B:
    case SDLK_KP_C:
    case SDLK_KP_D:
    case SDLK_KP_E:
    case SDLK_KP_F:
    case SDLK_KP_PERCENT:
    case SDLK_KP_LESS:
    case SDLK_KP_GREATER:
    case SDLK_KP_AMPERSAND:
    case SDLK_KP_VERTICALBAR:
    case SDLK_KP_COLON:
    case SDLK_KP_HASH:
    case SDLK_KP_SPACE:
    case SDLK_KP_AT:
    case SDLK_KP_PLUSMINUS:
    case SDLK_KP_DECIMAL:
        textKey = True;
        break;
    }
    if (!inputMethodHasActiveTextInput() ||
        (modifiers & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) || !textKey)
        return False;
    if (keycode < XK_space || keycode > XK_asciitilde)
        return True;
    char ascii = asciiTextForKeydown(display, event);
    if (ascii)
        pushPendingAsciiChar(ascii);
    return ascii == '\0';
}


int convertEvent(Display *display,
                 SDL_Event *sdlEvent,
                 XEvent *xEvent,
                 Bool freeInternalEvents)
{
    Bool sendEvent = False;
    Window eventWindow = None;
    int type = -1;
    unsigned long serial = currentEventSerial();
#define FILL_STANDARD_VALUES(eventStruct)       \
    xEvent->eventStruct.type = type;            \
    xEvent->eventStruct.serial = serial;        \
    xEvent->eventStruct.send_event = sendEvent; \
    xEvent->eventStruct.display = display
    switch (sdlEvent->type) {
    case SDL_KEYDOWN:
        if (shouldSuppressIMTextKeydown(display, sdlEvent))
            return -1;
        type = KeyPress;
        LOG("SDL_KEYDOWN for winId %d\n", sdlEvent->key.windowID);
    case SDL_KEYUP:
        if (sdlEvent->type == SDL_KEYUP) {
            LOG("SDL_KEYUP for winId %d\n", sdlEvent->key.windowID);
            type = KeyRelease;
        }
        FILL_STANDARD_VALUES(xkey);
        Window sdlKeyWindow = getWindowFromId(sdlEvent->key.windowID);
        xEvent->xkey.root = SCREEN_WINDOW;
        xEvent->xkey.state = convertModifierState(XC_EVENT_KEYMOD(sdlEvent));

        /* X11 KeyCode is 8-bit, but SDL keycodes span ASCII (< 0x80) plus a
         * separate 0x4000xxxx scancode range, so no collision-free mapping into
         * 0..255 exists. Truncating to the low byte keeps every ASCII key on
         * its natural keycode; the cost is that a scancode key whose low byte
         * lands on a printable keycode aliases onto it (SDLK_EXECUTE 0x40000074
         * -> 116 == 't', SDLK_KP_0 0x40000062 -> 98 == 'b'). XkbKeycodeToKeysym
         * uses the same truncation so decoding round-trips for the common
         * (ASCII) keys. XKeysymToKeycode guards the reverse direction (it
         * refuses a keycode that does not round-trip) so Motif cannot bind a
         * special key onto a letter; pressing one of the rare aliasing scancode
         * keys still types the colliding character, which is accepted given the
         * 8-bit limit.
         */
        xEvent->xkey.keycode = (unsigned int) XC_EVENT_KEYSYM(sdlEvent) & 0xFF;

        /* Route priority for key events:
         * 1. Active XGrabKeyboard (modal dialogs like Motif's Help popup)
         * 2. Passive XGrabKey match (Motif accelerators)
         * 3. The keyboard-focus window
         * 4. The event's root window as a last resort. Items 1 and 2 are
         * independent of focus; item 3 is the normal path.
         */
        Window keyboardGrabWindow = getGrabbedKeyboardWindow();
        Window passiveGrabWindow =
            findKeyGrabWindow((int) xEvent->xkey.keycode, xEvent->xkey.state);
        if (keyboardGrabWindow != None) {
            eventWindow = keyboardGrabWindow;
        } else if (passiveGrabWindow != None) {
            eventWindow = passiveGrabWindow;
        } else {
            eventWindow = getKeyboardFocus();
            if (eventWindow == None && !isKeyboardFocusFromClient() &&
                getKeyboardFocusKind() == FocusKindNone) {
                /* SDL emits no FOCUS_GAINED transition for a window that is
                 * born with keyboard focus, so syncKeyboardFocusFromHost may
                 * never have run and keyboardFocus is still None. A
                 * single-window client like xwpe never calls XSetInputFocus
                 * itself, so every key would route to the root window and the
                 * app would look deaf to the keyboard. Adopt the window SDL
                 * currently reports as keyboard-focused. Guarded on client
                 * ownership so explicit None/PointerRoot focus is left
                 * untouched.
                 */
                SDL_Window *sdlFocus = SDL_GetKeyboardFocus();
                if (sdlFocus) {
                    Window hostFocus =
                        getWindowFromId(SDL_GetWindowID(sdlFocus));
                    if (hostFocus != None) {
                        FocusKind oldKind = getKeyboardFocusKind();
                        Window oldFocus = getKeyboardFocus();
                        syncKeyboardFocusFromHost(hostFocus);
                        eventWindow = getKeyboardFocus();

                        /* Deliver the FocusIn the absent SDL FOCUS_GAINED never
                         * produced, so a client selecting FocusChangeMask still
                         * learns it holds focus. It trails this first key by
                         * one event instead of preceding it, the cost of a
                         * born-focused window SDL never announced.
                         */
                        postFocusChange(display, oldKind, oldFocus,
                                        getKeyboardFocusKind(), eventWindow);
                    }
                }
            }
            eventWindow = eventWindow == None ? xEvent->xkey.root : eventWindow;
        }

        xEvent->xkey.window = eventWindow;
        xEvent->xkey.subwindow = None;
        xEvent->xkey.time = XC_EVENT_TIME_MS(sdlEvent->key.timestamp);
        int pointerX = 0, pointerY = 0;

        /* Injected/replay coordinates are already X11 physical pixels; only
         * real SDL_GetMouseState points need scaling. Mirrors the
         * button/motion/wheel synthetic handling; scaling twice would double
         * the reported x/y on a promoted Retina window.
         */
        if (!replayTargetReadPointer(&pointerX, &pointerY)) {
            SDL_GetMouseState(&pointerX, &pointerY);
            scaleSdlPointToPixels(sdlKeyWindow, pointerX, pointerY, &pointerX,
                                  &pointerY);
        }
        translateSdlPointToRoot(display, sdlKeyWindow, pointerX, pointerY,
                                &xEvent->xkey.x_root, &xEvent->xkey.y_root);
        translateRootPointToWindow(display, SCREEN_WINDOW, eventWindow,
                                   xEvent->xkey.x_root, xEvent->xkey.y_root,
                                   &xEvent->xkey.x, &xEvent->xkey.y);
        // xEvent->xkey.keycode = (unsigned int) sdlEvent->key.keysym.scancode;
        xEvent->xkey.same_screen = True;
        break;
    case SDL_MOUSEBUTTONDOWN:
        LOG("SDL_MOUSEBUTTONDOWN\n");
        type = ButtonPress;

        /* Previously this case synthesized an EnterNotify from the first
         * mouse-down to compensate for environments that never delivered
         * SDL_WINDOWEVENT_ENTER. The hack double-fired Motif arm/focus
         * callbacks (one for the fake EnterNotify, one for the actual
         * ButtonPress) and reorganized the event stream out of X11 spec.
         * SDL_WINDOWEVENT_ENTER already drives EnterNotify cleanly in
         * convertEvent below; let SDL handle it.
         */
    case SDL_MOUSEBUTTONUP:
        if (sdlEvent->type == SDL_MOUSEBUTTONUP) {
            LOG("SDL_MOUSEBUTTONUP\n");
            type = ButtonRelease;
        }
        FILL_STANDARD_VALUES(xbutton);
        Window sdlButtonWindow = getWindowFromId(sdlEvent->button.windowID);
        xEvent->xbutton.root = SCREEN_WINDOW;
        xEvent->xbutton.time = XC_EVENT_TIME_MS(sdlEvent->button.timestamp);
        int sdlButtonX = sdlEvent->button.x, sdlButtonY = sdlEvent->button.y;

        /* XTest tags synthetic buttons with which == SDL_TOUCH_MOUSEID and
         * their coordinates are already X11 physical pixels; scaling again
         * would double them on Retina. Real hardware events carry SDL points
         * and still need the scale (mirrors the motion/wheel handling).
         */
        if (sdlEvent->button.which != SDL_TOUCH_MOUSEID)
            scaleSdlPointToPixels(sdlButtonWindow, sdlButtonX, sdlButtonY,
                                  &sdlButtonX, &sdlButtonY);
        translateSdlPointToRoot(display, sdlButtonWindow, sdlButtonX,
                                sdlButtonY, &xEvent->xbutton.x_root,
                                &xEvent->xbutton.y_root);
        xEvent->xbutton.button = convertSdlMouseButton(sdlEvent->button.button);
        unsigned int buttonState = buttonMaskForXButton(xEvent->xbutton.button);

        /* Snapshot the pointer state under a single critical section.
         * pointerButtonState and activePointerWindow describe the same logical
         * "pointer is held in window X with buttons B" tuple; updating them in
         * separate unlocked phases could lose or duplicate the
         * activePointerWindow transition when SDL and application threads race
         * on rapid press/release sequences.
         */
        lockActivePointerWindow();
        unsigned int previousButtonState = pointerButtonState;
        Window activePointerSnapshot = activePointerWindow;
        unlockActivePointerWindow();
        xEvent->xbutton.state =
            convertModifierState(SDL_GetModState()) | previousButtonState;
        long buttonMask =
            type == ButtonPress ? ButtonPressMask : ButtonReleaseMask;
        if (type == ButtonPress) {
            activatePassiveButtonGrab(
                display, xEvent->xbutton.root, xEvent->xbutton.x_root,
                xEvent->xbutton.y_root, xEvent->xbutton.button,
                xEvent->xbutton.state);
        }
        PointerHit buttonHit =
            resolvePointerHit(xEvent->xbutton.root, sdlButtonWindow,
                              xEvent->xbutton.x_root, xEvent->xbutton.y_root);

        /* Explicit XGrabPointer routing owns the event when
         * routePointerGrabEvent returns True; otherwise fall back to the normal
         * pointer-window selection (with a sticky ButtonRelease delivery to the
         * last button-press recipient).
         */
        if (!routePointerGrabEvent(
                display, xEvent->xbutton.root, xEvent->xbutton.x_root,
                xEvent->xbutton.y_root, buttonHit.deepest, buttonMask,
                &eventWindow, &xEvent->xbutton.subwindow, &xEvent->xbutton.x,
                &xEvent->xbutton.y)) {
            if (type == ButtonRelease && activePointerSnapshot != None &&
                windowSelectsAny(activePointerSnapshot, buttonMask)) {
                eventWindow = activePointerSnapshot;
                translateRootPointToWindow(
                    display, xEvent->xbutton.root, eventWindow,
                    xEvent->xbutton.x_root, xEvent->xbutton.y_root,
                    &xEvent->xbutton.x, &xEvent->xbutton.y);
                xEvent->xbutton.subwindow =
                    directChildFromDeepest(eventWindow, buttonHit.deepest);
                if (xEvent->xbutton.subwindow == None)
                    xEvent->xbutton.subwindow = getDirectChildContainingPoint(
                        eventWindow, xEvent->xbutton.x, xEvent->xbutton.y);
            } else {
                eventWindow = selectPointerEventWindow(
                    display, xEvent->xbutton.root, xEvent->xbutton.x_root,
                    xEvent->xbutton.y_root, buttonHit.deepest, buttonMask,
                    &xEvent->xbutton.subwindow, &xEvent->xbutton.x,
                    &xEvent->xbutton.y);
            }
        }
        Bool drainedActivePointer = False;
        if (freeInternalEvents) {
            /* Single critical section publishes both updates so a concurrent
             * reader sees a consistent (button-state, active-window) pair.
             */
            lockActivePointerWindow();
            if (type == ButtonPress)
                pointerButtonState |= buttonState;
            else
                pointerButtonState &= ~buttonState;
            if (type == ButtonPress && previousButtonState == 0 &&
                eventWindow != None) {
                activePointerWindow = eventWindow;
            }
            if (pointerButtonState == 0) {
                activePointerWindow = None;
                drainedActivePointer = True;
            }
            unlockActivePointerWindow();
        }
        if (eventWindow == None)
            return -1;
        xEvent->xbutton.window = eventWindow;
        xEvent->xbutton.same_screen = True;
        if (freeInternalEvents && type == ButtonRelease &&
            drainedActivePointer && pointerGrabIsPassive()) {
            releasePassivePointerGrab(display);
        }
        break;
    case SDL_MOUSEMOTION:
        LOG("SDL_MOUSEMOTION\n");
        type = MotionNotify;
        FILL_STANDARD_VALUES(xmotion);
        Window sdlMotionWindow = getWindowFromId(sdlEvent->motion.windowID);
        xEvent->xmotion.root = SCREEN_WINDOW;
        xEvent->xmotion.time = XC_EVENT_TIME_MS(sdlEvent->motion.timestamp);
        int sdlMotionX = sdlEvent->motion.x, sdlMotionY = sdlEvent->motion.y;

        /* XTest tags synthetic motion with which == SDL_TOUCH_MOUSEID and its
         * coordinates are already X11 physical pixels; scaling again would
         * double them on Retina. Real hardware motion carries SDL points and
         * still needs the scale.
         */
        if (sdlEvent->motion.which != SDL_TOUCH_MOUSEID)
            scaleSdlPointToPixels(sdlMotionWindow, sdlMotionX, sdlMotionY,
                                  &sdlMotionX, &sdlMotionY);
        translateSdlPointToRoot(display, sdlMotionWindow, sdlMotionX,
                                sdlMotionY, &xEvent->xmotion.x_root,
                                &xEvent->xmotion.y_root);
        PointerHit motionHit =
            resolvePointerHit(xEvent->xmotion.root, sdlMotionWindow,
                              xEvent->xmotion.x_root, xEvent->xmotion.y_root);
        unsigned int motionButtonState = pointerButtonStateSnapshot();
        unsigned int motionState =
            convertModifierState(SDL_GetModState()) | motionButtonState;
        Bool crossingQueued = postPointerCrossingEvents(
            display, xEvent->xmotion.x_root, xEvent->xmotion.y_root,
            motionHit.hoverDeepest, motionState,
            XC_EVENT_TIME_MS(sdlEvent->motion.timestamp));
        long motionMask = motionMaskForButtonState(motionButtonState);

        /* Explicit XGrabPointer routing owns the event when
         * routePointerGrabEvent returns True; otherwise the implicit
         * button-press grab (snapshot != None) wins, falling back to regular
         * pointer-window selection.
         */
        if (!routePointerGrabEvent(
                display, xEvent->xmotion.root, xEvent->xmotion.x_root,
                xEvent->xmotion.y_root, motionHit.deepest, motionMask,
                &eventWindow, &xEvent->xmotion.subwindow, &xEvent->xmotion.x,
                &xEvent->xmotion.y)) {
            lockActivePointerWindow();
            Window snapshot = activePointerWindow;
            unlockActivePointerWindow();
            if (snapshot != None && windowSelectsAny(snapshot, motionMask)) {
                eventWindow = snapshot;
                translateRootPointToWindow(
                    display, xEvent->xmotion.root, eventWindow,
                    xEvent->xmotion.x_root, xEvent->xmotion.y_root,
                    &xEvent->xmotion.x, &xEvent->xmotion.y);
                xEvent->xmotion.subwindow =
                    directChildFromDeepest(eventWindow, motionHit.deepest);
                if (xEvent->xmotion.subwindow == None)
                    xEvent->xmotion.subwindow = getDirectChildContainingPoint(
                        eventWindow, xEvent->xmotion.x, xEvent->xmotion.y);
            } else {
                eventWindow = selectPointerEventWindow(
                    display, xEvent->xmotion.root, xEvent->xmotion.x_root,
                    xEvent->xmotion.y_root, motionHit.deepest, motionMask,
                    &xEvent->xmotion.subwindow, &xEvent->xmotion.x,
                    &xEvent->xmotion.y);
            }
        }
        if (eventWindow == None)
            return -1;
        xEvent->xmotion.window = eventWindow;
        xEvent->xmotion.state = motionState;
        xEvent->xmotion.is_hint =
            HAS_EVENT_MASK(eventWindow, PointerMotionHintMask) ? NotifyHint
                                                               : NotifyNormal;
        xEvent->xmotion.same_screen = True;
        if (crossingQueued) {
            appendPutBackEvent(display, xEvent);
            return -1;
        }
        break;
    XC_CASE_WINDOWEVENT:
        eventWindow = getWindowFromId(sdlEvent->window.windowID);
        switch (XC_WINDOW_SUBEVENT(sdlEvent)) {
        case SDL_WINDOWEVENT_SHOWN:
            LOG("Window %d shown\n", sdlEvent->window.windowID);
            if (eventWindow != None) {
                WindowStruct *shownStruct = GET_WINDOW_STRUCT(eventWindow);
                shownStruct->mapState = Mapped;
                markWindowNeedsPresent(eventWindow);

                /* Drop the echo when showTopLevelWindow already posted an
                 * explicit MapNotify for this show; delivering both runs Xt/Tk
                 * map handlers twice. One-shot: clear as it is consumed.
                 */
                if (SDL_AtomicGet(&shownStruct->suppressSdlShowMap)) {
                    SDL_AtomicSet(&shownStruct->suppressSdlShowMap, 0);
                    return 0;
                }
            }
            type = MapNotify;
            FILL_STANDARD_VALUES(xmap);
            xEvent->xmap.window = eventWindow;
            xEvent->xmap.event = xEvent->xmap.window;
            xEvent->xmap.override_redirect =
                eventWindow != None
                    ? GET_WINDOW_STRUCT(eventWindow)->overrideRedirect
                    : False;
            break;
        case SDL_WINDOWEVENT_HIDDEN:
            LOG("Window %d hidden\n", sdlEvent->window.windowID);
            if (eventWindow != None)
                GET_WINDOW_STRUCT(eventWindow)->mapState = UnMapped;
            type = UnmapNotify;
            FILL_STANDARD_VALUES(xunmap);
            xEvent->xunmap.window = eventWindow;
            xEvent->xunmap.event = xEvent->xunmap.window;
            xEvent->xunmap.from_configure = False;
            break;
        case SDL_WINDOWEVENT_EXPOSED:
            LOG("Window %d exposed\n", sdlEvent->window.windowID);
            if (eventWindow != None)
                markWindowNeedsPresent(eventWindow);
            return -1;
            break;
        case SDL_WINDOWEVENT_MOVED:
            LOG("Window %d moved to %d,%d\n", sdlEvent->window.windowID,
                sdlEvent->window.data1, sdlEvent->window.data2);
            if (eventWindow != None) {
                WindowStruct *movedStruct = GET_WINDOW_STRUCT(eventWindow);
#ifndef LIBX11_COMPAT_SDL3
                /* Old on-screen rect (before ws->x/y is updated below) is the
                 * region a window-manager move vacates on the top-levels under
                 * it. Captured for the SDL2 repaint further down.
                 */
                SDL_Rect vacated = {movedStruct->x, movedStruct->y,
                                    clampToIntRange((int64_t) movedStruct->w),
                                    clampToIntRange((int64_t) movedStruct->h)};
#endif
                int logicalX, logicalY;
                sdlTopLevelOriginToX11(eventWindow, sdlEvent->window.data1,
                                       sdlEvent->window.data2, &logicalX,
                                       &logicalY);
                movedStruct->x = logicalX;
                movedStruct->y = logicalY;

                /* An anchored popup's MOVED arrives because the compositor slid
                 * or flipped it, not because the client asked. Update ws->x/y
                 * so pointer translation tracks the real position, but emit no
                 * ConfigureNotify: the toolkit still believes it placed the
                 * menu where it requested, and a configure it never asked for
                 * would fight its own menu tracking.
                 */
                if (movedStruct->popupParent != None)
                    return -1;
#ifndef LIBX11_COMPAT_SDL3
                /* SDL2/X11 window-manager move: repaint the top-levels this
                 * window uncovered at its old position, since Xvnc delivers
                 * them no Expose, so the drag trail clears. Then fall through
                 * to the ConfigureNotify below: a move-only configure carries
                 * no size change, so the client just learns its new position
                 * with no reflow.
                 */
                repaintTopLevelsOverlappingRect(eventWindow, vacated);
#endif
            }

            /* fall through: MOVED, RESIZED and SIZE_CHANGED share a single
             * ConfigureNotify dispatch below; the unified handler keys off
             * XC_WINDOW_SUBEVENT(sdlEvent) to decide which fields to query.
             */
        case SDL_WINDOWEVENT_RESIZED:
            if (XC_WINDOW_SUBEVENT(sdlEvent) == SDL_WINDOWEVENT_RESIZED) {
                LOG("Window %d resized to %dx%d\n", sdlEvent->window.windowID,
                    sdlEvent->window.data1, sdlEvent->window.data2);
            }
            /* fall through */
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            if (XC_WINDOW_SUBEVENT(sdlEvent) == SDL_WINDOWEVENT_SIZE_CHANGED) {
                LOG("Window %d size changed to %dx%d\n",
                    sdlEvent->window.windowID, sdlEvent->window.data1,
                    sdlEvent->window.data2);
            }
            type = ConfigureNotify;
            FILL_STANDARD_VALUES(xconfigure);
            xEvent->xconfigure.event = eventWindow;
            xEvent->xconfigure.window = xEvent->xconfigure.event;
            if (XC_WINDOW_SUBEVENT(sdlEvent) == SDL_WINDOWEVENT_MOVED) {
                xEvent->xconfigure.x = sdlEvent->window.data1;
                xEvent->xconfigure.y = sdlEvent->window.data2;
                if (eventWindow != None) {
                    sdlTopLevelOriginToX11(
                        eventWindow, xEvent->xconfigure.x, xEvent->xconfigure.y,
                        &xEvent->xconfigure.x, &xEvent->xconfigure.y);
                }
            } else {
                SDL_GetWindowPosition(
                    SDL_GetWindowFromID(sdlEvent->window.windowID),
                    &xEvent->xconfigure.x, &xEvent->xconfigure.y);
                if (eventWindow != None) {
                    sdlTopLevelOriginToX11(
                        eventWindow, xEvent->xconfigure.x, xEvent->xconfigure.y,
                        &xEvent->xconfigure.x, &xEvent->xconfigure.y);

                    /* A resize from the top/left edge moves the window origin.
                     * The size-only branch below never touches position, so
                     * persist the queried logical origin here (mirroring the
                     * MOVED branch) to keep ws->x/y from going stale.
                     */
                    GET_WINDOW_STRUCT(eventWindow)->x = xEvent->xconfigure.x;
                    GET_WINDOW_STRUCT(eventWindow)->y = xEvent->xconfigure.y;
                }
            }

            /* Option 1b: the X11 window and its backing track physical pixels,
             * so the present path is a 1:1 blit (no upscaling). A RESIZED event
             * carries the new logical size in data1/data2 on both SDL2 and
             * SDL3. Promote it to physical pixels only for windows whose X11
             * geometry opted into that space (effectiveHiDpiScale); Motif/Tk
             * keep logical geometry so their widget/font layout does not shrink
             * after a host resize. We deliberately do NOT re-query the live SDL
             * surface/renderer here: resize events can be faked in tests where
             * the SDL window is never actually resized (resizeWindowTexture
             * documents the same constraint), and the cached scale is constant
             * for a given display. On non-HiDPI hosts and under the CI dummy
             * driver the scale is 1.0, so pixels equal points. SDL3
             * additionally fires SDL_WINDOWEVENT_SIZE_CHANGED as
             * PIXEL_SIZE_CHANGED; it is still ignored for geometry, RESIZED
             * remains the trigger.
             */
            Bool logicalResize =
                XC_WINDOW_SUBEVENT(sdlEvent) == SDL_WINDOWEVENT_RESIZED;
#ifndef LIBX11_COMPAT_SDL3
            /* SDL2 SIZE_CHANGED is also in logical units. */
            logicalResize = logicalResize || XC_WINDOW_SUBEVENT(sdlEvent) ==
                                                 SDL_WINDOWEVENT_SIZE_CHANGED;
#endif
            if (logicalResize) {
                int logicalW = sdlEvent->window.data1;
                int logicalH = sdlEvent->window.data2;
                double scaleX = 1.0, scaleY = 1.0;
                if (eventWindow != None) {
                    WindowStruct *ws = GET_WINDOW_STRUCT(eventWindow);
                    scaleX = effectiveHiDpiScaleX(ws);
                    scaleY = effectiveHiDpiScaleY(ws);
                }
                int pixelW = (int) lround((double) logicalW * scaleX);
                int pixelH = (int) lround((double) logicalH * scaleY);
                xEvent->xconfigure.width = pixelW;
                xEvent->xconfigure.height = pixelH;
                if (eventWindow != None) {
                    int oldWidth = 0, oldHeight = 0;
                    GET_WINDOW_DIMS(eventWindow, oldWidth, oldHeight);

                    /* On macOS the live-resize observer already adopts the
                     * final drag size (windowStruct->w/h +
                     * resizeWindowTexture), runs the client reflow and presents
                     * a correct frame before this SDL RESIZED event is
                     * delivered at drag end. When the event carries that same
                     * size, re-running the destructive path below (recreate the
                     * backing, clear the growth bands, re-expose the whole
                     * subtree, arm a fresh coalesce gate) would discard the
                     * freshly reflowed frame and hold its recovery behind a new
                     * gate until the client next idles - leaving the window
                     * showing the pre-reflow (stale) content. The
                     * ConfigureNotify is still emitted above so the client sees
                     * the size; only the redundant backing churn is skipped. A
                     * genuine size change (client XResizeWindow, or a host
                     * resize the observer never saw) still takes the full path.
                     */
                    Bool sizeChanged =
                        (pixelW != oldWidth || pixelH != oldHeight);
                    GET_WINDOW_STRUCT(eventWindow)->w = (unsigned int) pixelW;
                    GET_WINDOW_STRUCT(eventWindow)->h = (unsigned int) pixelH;
                    if (sizeChanged) {
                        resizeWindowTexture(eventWindow);

                        /* The top-level's own cached visibleRegion is its
                         * (0,0,w,h) frame; it goes stale on every resize driven
                         * by SDL outside of configureWindow.
                         */
                        invalidateVisibleRegionForTopLevel(eventWindow);

                        /* How much of the backing to wipe depends on the
                         * top-level's bit gravity, and must match what
                         * resizeWindowTexture just carried over. Only a
                         * top-left-anchored retaining gravity
                         * (NorthWestGravity/StaticGravity, which xwpe sets on
                         * its cells) keeps its old pixels pinned at (0,0): for
                         * those, clear only the bands that newly appeared
                         * because a dimension grew and leave the retained
                         * region
                         * - already copied into the new backing by
                         * resizeWindowTexture - untouched. Wiping that region
                         * would blank content the client has not yet repainted;
                         * during the macOS modal resize loop the repaint lags a
                         * few frames, so the blank shows as a transient black
                         * flash. ForgetGravity and every unmodeled gravity let
                         * the client discard contents on resize, so clear the
                         * whole subtree here; any top-left copy
                         * resizeWindowTexture kept for ForgetGravity is only a
                         * transient flash-avoidance during the modal drag,
                         * superseded once this runs. The full Expose below
                         * drives the repaint either way.
                         */
                        if (!bitGravityIsTopLeftAnchored(
                                GET_WINDOW_STRUCT(eventWindow)->bitGravity)) {
                            clearWindowTreeWithoutExpose(display, eventWindow);
                        } else {
                            if (pixelW > oldWidth) {
                                XClearArea(display, eventWindow, oldWidth, 0,
                                           (unsigned int) (pixelW - oldWidth),
                                           (unsigned int) pixelH, False);
                            }
                            if (pixelH > oldHeight) {
                                int bandW =
                                    pixelW < oldWidth ? pixelW : oldWidth;
                                XClearArea(display, eventWindow, 0, oldHeight,
                                           (unsigned int) bandW,
                                           (unsigned int) (pixelH - oldHeight),
                                           False);
                            }
                        }
                        postResizeConfigureForMappedChildren(
                            display, eventWindow, oldWidth, oldHeight, pixelW,
                            pixelH);

                        /* Re-expose the whole subtree so every mapped
                         * descendant repaints at the new size (whether or not
                         * it was cleared).
                         */
                        postFullWindowExpose(display, eventWindow);

                        /* The client is about to run its full repaint (clear
                         * the window, then redraw every cell) in response to
                         * this ConfigureNotify, issuing intermediate
                         * XFlush/XSync calls. Hold those partial presents back
                         * so only the final frame reaches the screen, avoiding
                         * a black flash during the redraw. Released when the
                         * client next idles in XNextEvent.
                         */
                        beginCoalesceClientRepaint();
                    }
                }
            } else if (eventWindow != None) {
                /* Non-resize ConfigureNotify (e.g. MOVED): the size is
                 * unchanged, so report the window's current cached X11 geometry
                 * that the client already tracks (promoted physical pixels, or
                 * logical pixels for Motif/Tk). SDL_GetWindowSize returns
                 * logical points here, which on a HiDPI host is half a promoted
                 * window's physical size and would make the client shrink its
                 * cell grid on a mere move. Use the cached dimensions instead.
                 */
                GET_WINDOW_DIMS(eventWindow, xEvent->xconfigure.width,
                                xEvent->xconfigure.height);
            } else {
                SDL_GetWindowSize(
                    SDL_GetWindowFromID(sdlEvent->window.windowID),
                    &xEvent->xconfigure.width, &xEvent->xconfigure.height);
            }

            xEvent->xconfigure.above = None;

            /* eventWindow came from getWindowFromId and can be None for an
             * untracked SDL window. The caller's XEvent is not zeroed and
             * FILL_STANDARD_VALUES does not touch these two fields, so set
             * explicit defaults in that case rather than leaving stack garbage.
             * One lookup otherwise feeds both the configure fields and the
             * replay-target offer.
             */
            if (eventWindow != None) {
                WindowStruct *ws = GET_WINDOW_STRUCT(eventWindow);
                xEvent->xconfigure.border_width = ws->borderWidth;
                xEvent->xconfigure.override_redirect = ws->overrideRedirect;
                replayTargetOfferWindow(sdlEvent->window.windowID, ws->x, ws->y,
                                        (int) ws->w, (int) ws->h);
            } else {
                xEvent->xconfigure.border_width = 0;
                xEvent->xconfigure.override_redirect = False;
            }
            break;
#if SDL_VERSION_ATLEAST(2, 0, 18)
        case SDL_WINDOWEVENT_DISPLAY_CHANGED: {
            /* The window moved to a different display, which may have a
             * different HiDPI backing scale than the one cached at realize
             * time. Re-derive the per-window scale from the live pixel/point
             * ratio so the next resize's logical<->physical conversion and the
             * present blit use the new monitor's scale instead of a stale one.
             * This is event-driven and never runs on the resize fast path, so
             * faked test resizes are unaffected.
             */
            if (eventWindow != None && compatSdlHasWindowSizeInPixels()) {
                SDL_Window *movedWin =
                    SDL_GetWindowFromID(sdlEvent->window.windowID);
                if (movedWin) {
                    int lw = 0, lh = 0, pw = 0, ph = 0;
                    SDL_GetWindowSize(movedWin, &lw, &lh);
#if SDL_VERSION_ATLEAST(2, 26, 0)
                    SDL_GetWindowSizeInPixels(movedWin, &pw, &ph);
#endif
                    WindowStruct *movedWs = GET_WINDOW_STRUCT(eventWindow);
                    double oldScaleX = movedWs->hiDpiScaleX;
                    double oldScaleY = movedWs->hiDpiScaleY;
                    if (lw > 0 && pw > 0)
                        movedWs->hiDpiScaleX = (double) pw / (double) lw;
                    if (lh > 0 && ph > 0)
                        movedWs->hiDpiScaleY = (double) ph / (double) lh;

                    /* The cached PResizeInc increments live in physical-pixel
                     * space (see WindowStruct), so a monitor move that changes
                     * the backing scale makes them stale: they still describe
                     * the previous monitor's cell size. A client that
                     * re-derives its font from the scale (xwpe re-publishes
                     * WM_NORMAL_HINTS after refitting) will overwrite these;
                     * but a client that does not leaves the drag-end snap
                     * quantising to the old cell, leaving a sub-cell remainder
                     * band. Rescale them by newScale/oldScale so the snap grid
                     * matches the destination monitor even without a
                     * re-publish.
                     */
                    if (movedWs->hiDpiPromoted && movedWs->hasResizeInc &&
                        oldScaleX > 0.0 && oldScaleY > 0.0) {
                        double rx = movedWs->hiDpiScaleX / oldScaleX;
                        double ry = movedWs->hiDpiScaleY / oldScaleY;
                        if (rx > 0.0 && rx != 1.0) {
                            movedWs->widthInc =
                                (int) (movedWs->widthInc * rx + 0.5);
                            movedWs->baseWidth =
                                (int) (movedWs->baseWidth * rx + 0.5);
                            movedWs->minWidth =
                                (int) (movedWs->minWidth * rx + 0.5);
                        }
                        if (ry > 0.0 && ry != 1.0) {
                            movedWs->heightInc =
                                (int) (movedWs->heightInc * ry + 0.5);
                            movedWs->baseHeight =
                                (int) (movedWs->baseHeight * ry + 0.5);
                            movedWs->minHeight =
                                (int) (movedWs->minHeight * ry + 0.5);
                        }
                        if (movedWs->widthInc <= 0 || movedWs->heightInc <= 0)
                            movedWs->hasResizeInc = False;
                    }
                    compatTrace(
                        "SDL_WINDOWEVENT_DISPLAY_CHANGED: window=%lu "
                        "logical=(%dx%d) phys=(%dx%d) newScale=(%.3f,%.3f) "
                        "cachedPhys=(%ux%u)\n",
                        eventWindow, lw, lh, pw, ph, movedWs->hiDpiScaleX,
                        movedWs->hiDpiScaleY, movedWs->w, movedWs->h);

                    /* The global font scale tracks the same host backing as
                     * this window, so refresh it and republish the root
                     * property on both backends; a client re-deriving its font
                     * from _LIBX11_COMPAT_HIDPI_SCALE (xwpe) would otherwise
                     * read the source monitor's stale scale after the move. On
                     * a single-scale setup the value is unchanged.
                     */
                    if (movedWs->hiDpiPromoted && pw > 0 && ph > 0) {
                        if (lw > 0)
                            compatSetGlobalHiDpiScale((double) pw /
                                                      (double) lw);
                        else if (lh > 0)
                            compatSetGlobalHiDpiScale((double) ph /
                                                      (double) lh);
                        compatPublishHiDpiScaleProperty(display);
#if defined(LIBX11_COMPAT_SDL3)

                        /* SDL2 follows a DISPLAY_CHANGED with a SIZE_CHANGED
                         * that reflows geometry using the scale refreshed
                         * above. The SDL3 backend delivers no such geometry
                         * event for a bare monitor move, so the window keeps
                         * the previous monitor's physical pixel dimensions
                         * (over- or under-promoted) and the present mis-scales.
                         * Re-promote to the new monitor's physical size and
                         * reflow the client here.
                         */
                        if (pw != (int) movedWs->w || ph != (int) movedWs->h) {
                            compatTrace(
                                "SDL_WINDOWEVENT_DISPLAY_CHANGED: window=%lu "
                                "RE-PROMOTE (%ux%u)->(%dx%d) "
                                "globalScale=%.3f\n",
                                eventWindow, movedWs->w, movedWs->h, pw, ph,
                                compatGlobalHiDpiScale());
                            postSyntheticWindowResize(display, eventWindow, pw,
                                                      ph);

                            /* The cached visible region is the pre-move
                             * (0,0,w,h) rect; the re-promote just grew the
                             * top-level, so invalidate it as the RESIZED
                             * ConfigureNotify path does or the added physical
                             * columns/rows stay clipped and black.
                             */
                            invalidateVisibleRegionForTopLevel(eventWindow);
                        }
#endif
                    }
                }
            }
            return -1;
        }
#endif
        case SDL_WINDOWEVENT_MINIMIZED:
            LOG("Window %d minimized\n", sdlEvent->window.windowID);
            if (eventWindow != None)
                GET_WINDOW_STRUCT(eventWindow)->mapState = UnMapped;
            return -1;
            break;
        case SDL_WINDOWEVENT_MAXIMIZED:
            LOG("Window %d maximized\n", sdlEvent->window.windowID);
            return -1;
            break;
        case SDL_WINDOWEVENT_RESTORED:
            LOG("Window %d restored\n", sdlEvent->window.windowID);
            if (eventWindow != None) {
                GET_WINDOW_STRUCT(eventWindow)->mapState = Mapped;
                markWindowNeedsPresent(eventWindow);
                SDL_Rect windowArea = {0, 0, 0, 0};
                GET_WINDOW_DIMS(eventWindow, windowArea.w, windowArea.h);
                postExposeEvent(display, eventWindow, &windowArea, 1);
            }
            return -1;
            break;
        case SDL_WINDOWEVENT_ENTER:
            LOG("Mouse entered window %d\n", sdlEvent->window.windowID);
            type = EnterNotify;
        case SDL_WINDOWEVENT_LEAVE:
            if (XC_WINDOW_SUBEVENT(sdlEvent) == SDL_WINDOWEVENT_LEAVE) {
                LOG("Mouse left window %d\n", sdlEvent->window.windowID);
                type = LeaveNotify;
            }
            fillCrossingEvent(display, &xEvent->xcrossing, eventWindow, type,
                              NotifyNormal, NotifyAncestor,
                              convertModifierState(SDL_GetModState()));
            xEvent->xcrossing.time =
                XC_EVENT_TIME_MS(sdlEvent->window.timestamp);
            Bool queuedNestedLeaves = False;
            if (type == LeaveNotify) {
                queuedNestedLeaves = queueNestedPointerLeaves(
                    display, eventWindow, xEvent->xcrossing.state,
                    XC_EVENT_TIME_MS(sdlEvent->window.timestamp));
            }

            /* Keep pointerHoverWindow in sync with the SDL-level crossing that
             * was just emitted; otherwise the next motion event's
             * postPointerCrossingEvents would either fire a duplicate
             * EnterNotify (state was None) or suppress a legitimate Enter after
             * a leave/re-enter (state still pointed at the previous hover
             * child).
             */
            lockActivePointerWindow();
            pointerHoverWindow = type == EnterNotify ? eventWindow : None;
            unlockActivePointerWindow();
            if (queuedNestedLeaves) {
                appendPutBackEvent(display, xEvent);
                return -1;
            }
            break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            LOG("Window %d gained keyboard focus\n", sdlEvent->window.windowID);
            type = FocusIn;
            if (eventWindow != None) {
                /* Do not clobber a client XSetInputFocus target. Adopt the host
                 * top-level only while focus is still host/default-owned, and
                 * never when it already points at this window or a descendant
                 * of it.
                 */
                Window currentFocus = getKeyboardFocus();
                FocusKind currentFocusKind = getKeyboardFocusKind();
                if (currentFocusKind != FocusKindPointerRoot &&
                    !isKeyboardFocusFromClient() &&
                    currentFocus != eventWindow &&
                    (currentFocus == None || !IS_TYPE(currentFocus, WINDOW) ||
                     !isParent(eventWindow, currentFocus)))
                    syncKeyboardFocusFromHost(eventWindow);
            }
            /* fallthrough */
        case SDL_WINDOWEVENT_FOCUS_LOST:
            if (XC_WINDOW_SUBEVENT(sdlEvent) == SDL_WINDOWEVENT_FOCUS_LOST) {
                LOG("Window %d lost keyboard focus\n",
                    sdlEvent->window.windowID);
                type = FocusOut;
                if (eventWindow != None && getKeyboardFocus() == eventWindow &&
                    isKeyboardFocusFromHost())
                    syncKeyboardFocusFromHost(None);
            }
            FILL_STANDARD_VALUES(xfocus);
            xEvent->xfocus.window = eventWindow;
            xEvent->xfocus.mode = NotifyNormal;
            xEvent->xfocus.detail = NotifyAncestor;
            break;
        case SDL_WINDOWEVENT_CLOSE:
            LOG("Window %d closed\n", sdlEvent->window.windowID);
            Bool handledClose =
                eventWindow != None
                    ? postWmDeleteIfHandled(
                          display, eventWindow,
                          (Time) XC_EVENT_TIME_MS(sdlEvent->window.timestamp))
                    : False;

            /* Real X11 kills the client connection here. Route through the IO
             * error hook so a client that installed one can intercept;
             * otherwise it terminates.
             */
            if (!handledClose)
                triggerIOError(display);
            return -1;
            break;
        default:
            LOG("Window %d got unknown event %d\n", sdlEvent->window.windowID,
                XC_WINDOW_SUBEVENT(sdlEvent));
            return -1;
        }
        break;
    case SDL_QUIT: /**< User-requested quit */
        LOG("SDL_QUIT\n");
        /* Cmd+Q, SIGTERM, or dock-quit. Prefer a graceful WM_DELETE to every
         * top-level that advertises it, the way a window manager closes an app
         * on logout, so the client shuts down through its own handler. Only
         * when nothing handles the delete do we emulate a fatal IO error.
         * Routing straight to triggerIOError ran the client's XIOErrorHandler;
         * GDK's gdk_x_io_error crashes inside glib charset conversion on macOS,
         * turning every quit into a segfault.
         */
        {
            Bool quitHandled = False;
            if (SCREEN_WINDOW != None && IS_TYPE(SCREEN_WINDOW, WINDOW)) {
                Window *topLevels = GET_CHILDREN(SCREEN_WINDOW);
                size_t topCount =
                    GET_WINDOW_STRUCT(SCREEN_WINDOW)->children.length;
                Time quitTime =
                    (Time) XC_EVENT_TIME_MS(sdlEvent->quit.timestamp);
                for (size_t i = 0; i < topCount; i++) {
                    if (postWmDeleteIfHandled(display, topLevels[i], quitTime))
                        quitHandled = True;
                }
            }
            if (!quitHandled)
                triggerIOError(display);
        }
        return -1;
    case SDL_APP_TERMINATING: /**< The application is being terminated by
                                 the OS Called on iOS in
                                 applicationWillTerminate() Called on
                                 Android in onDestroy()
                            */
        LOG("SDL_APP_TERMINATING\n");
        return -1;
    case SDL_APP_LOWMEMORY: /**< The application is low on memory, free
                               memory if possible. Called on iOS in
                               applicationDidReceiveMemoryWarning() Called
                               on Android in onLowMemory()
                            */
        LOG("SDL_APP_LOWMEMORY\n");
        return -1;
    case SDL_APP_WILLENTERBACKGROUND: /**< The application is about to enter
                                 the background Called on iOS in
                                 applicationWillResignActive() Called on
                                 Android in onPause()
                            */
        LOG("SDL_APP_WILLENTERBACKGROUND\n");
        return -1;
    case SDL_APP_DIDENTERBACKGROUND: /**< The application did enter the
                                 background and may not get CPU for some
                                 time Called on iOS in
                                 applicationDidEnterBackground() Called on
                                 Android in onPause()
                            */
        LOG("SDL_APP_DIDENTERBACKGROUND\n");
        return -1;
    case SDL_APP_WILLENTERFOREGROUND: /**< The application is about to enter
                                 the foreground Called on iOS in
                                 applicationWillEnterForeground() Called on
                                 Android in onResume()
                            */
        LOG("SDL_APP_WILLENTERFOREGROUND\n");
        return -1;
    case SDL_APP_DIDENTERFOREGROUND: /**< The application is now interactive
                                 Called on iOS in
                                 applicationDidBecomeActive() Called on
                                 Android in onResume()
                            */
        LOG("SDL_APP_DIDENTERFOREGROUND\n");
        return -1;
#ifndef LIBX11_COMPAT_SDL3
    case SDL_SYSWMEVENT: /**< System specific event */
        LOG("SDL_SYSWMEVENT\n");
        return -1;
#endif
    case SDL_TEXTEDITING: /**< Keyboard text editing (composition) */
        LOG("SDL_TEXTEDITING\n");
        clearPendingAsciiRing();
        inputMethodHandlePreedit(XC_EDITING_EVENT_TEXT(sdlEvent),
                                 sdlEvent->edit.start);
        return -1;
#if !defined(LIBX11_COMPAT_SDL3) && SDL_VERSION_ATLEAST(2, 0, 22)
    case SDL_TEXTEDITING_EXT: /**< Long composition that overflows the
                                 inline SDL_TEXTEDITING buffer; text is a
                                 heap char* SDL hands us to free.
                                 */
        LOG("SDL_TEXTEDITING_EXT\n");
        clearPendingAsciiRing();
        inputMethodHandlePreedit(
            sdlEvent->editExt.text ? sdlEvent->editExt.text : "",
            sdlEvent->editExt.start);
        SDL_free(sdlEvent->editExt.text);
        return -1;
#endif
    case SDL_TEXTINPUT: /**< Keyboard text input */
        LOG("SDL_TEXTINPUT\n");
        {
            if (!inputMethodHasActiveTextInput())
                return -1;
            const char *incomingText = XC_TEXT_EVENT_TEXT(sdlEvent);
            if (!incomingText[1] &&
                popMatchingPendingAsciiChar(incomingText[0]))
                return -1;
            clearPendingAsciiRing();
            type = KeyPress;
            FILL_STANDARD_VALUES(xkey);

            /* Route the commit like a real key: an active keyboard grab wins
             * over the focus window so IM text composed during a modal grab
             * still reaches the grab holder.
             */
            Window imGrabWindow = getGrabbedKeyboardWindow();
            eventWindow =
                imGrabWindow != None ? imGrabWindow : getKeyboardFocus();
            if (eventWindow == None)
                eventWindow = SCREEN_WINDOW;
            char *text = strdup(incomingText);
            if (!text)
                return -1;
            inputMethodHandlePreedit("", 0);
            unsigned long commitId = inputMethodSetCurrentText(text);

            /* No id means the commit was not queued (OOM, or a preedit-done
             * callback above cleared focus), so do not deliver a phantom IM
             * KeyPress that would look up as XLookupNone.
             */
            if (commitId == 0)
                return -1;

            xEvent->xkey.root = SCREEN_WINDOW;
            xEvent->xkey.window = eventWindow;

            /* Carry the commit id so the lookup retrieves this exact commit
             * regardless of delivery order; keycode stays 0 as the IM marker.
             */
            xEvent->xkey.subwindow = (Window) commitId;
            xEvent->xkey.time = XC_EVENT_TIME_MS(sdlEvent->text.timestamp);
            xEvent->xkey.x = xEvent->xkey.y = 0;
            xEvent->xkey.x_root = xEvent->xkey.y_root = 0;
            xEvent->xkey.state = 0;
            xEvent->xkey.keycode = 0;
            xEvent->xkey.same_screen = True;
        }
        break;
    case SDL_MOUSEWHEEL: /**< Mouse wheel motion */
        LOG("SDL_MOUSEWHEEL\n");
        {
#ifdef LIBX11_COMPAT_SDL3

            /* SDL3 wheel deltas are floats carrying any sub-notch fraction
             * directly in x/y, so accumulate them through the same notch filter
             * the SDL2 precise path used.
             */
            int wy = accumulateWheelNotch(0, sdlEvent->wheel.y, &wheelPreciseY);
            int wx = accumulateWheelNotch(0, sdlEvent->wheel.x, &wheelPreciseX);
#else
            int wy = sdlEvent->wheel.y, wx = sdlEvent->wheel.x;
#if SDL_VERSION_ATLEAST(2, 0, 18)

            /* sdl2-compat can report sub-notch wheel deltas in preciseX/Y while
             * leaving x/y at 0. Accumulate those fractions so smooth wheels do
             * not turn each partial delta into a full X11 wheel click.
             */
            wy = accumulateWheelNotch(wy, sdlEvent->wheel.preciseY,
                                      &wheelPreciseY);
            wx = accumulateWheelNotch(wx, sdlEvent->wheel.preciseX,
                                      &wheelPreciseX);
#endif
#endif
            if (sdlEvent->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                wy = -wy;
                wx = -wx;
            }
            unsigned int wheelButton = 0;
            if (wy > 0)
                wheelButton = Button4;
            else if (wy < 0)
                wheelButton = Button5;
            else if (wx > 0)
                wheelButton = 7; /* horizontal right */
            else if (wx < 0)
                wheelButton = 6; /* horizontal left */
            if (wheelButton == 0)
                return -1;
            type = ButtonPress;
            FILL_STANDARD_VALUES(xbutton);
            Window sdlWheelWindow = getWindowFromId(sdlEvent->wheel.windowID);
            xEvent->xbutton.root = SCREEN_WINDOW;
            int mx = 0, my = 0;

            /* SDL wheel events carry no coordinates. Use the injected position
             * only for XTest-synthesized events (xtest.c tags them with
             * wheel.which = SDL_TOUCH_MOUSEID); real hardware wheels keep the
             * SDL_GetMouseState path so the user's physical cursor still wins
             * after any XTest activity. The sentinel-based dispatch was a
             * gemini-flagged fix to the earlier xtestHasInjectedPos-only check,
             * which never reset and would have routed every later real wheel to
             * the stale injected coords.
             */
            Bool wheelInjected = False;
            if (sdlEvent->wheel.which == SDL_TOUCH_MOUSEID &&
                replayTargetReadPointer(&mx, &my)) {
                /* injected pos wins */
                wheelInjected = True;
            } else {
                SDL_GetMouseState(&mx, &my);
            }
            xEvent->xbutton.time = XC_EVENT_TIME_MS(sdlEvent->wheel.timestamp);

            /* Injected pointer coordinates are already X11 physical pixels
             * (from replayTargetReadPointer); only real SDL_GetMouseState
             * points need scaling. Matches the motion/button synthetic
             * handling.
             */
            if (!wheelInjected)
                scaleSdlPointToPixels(sdlWheelWindow, mx, my, &mx, &my);
            translateSdlPointToRoot(display, sdlWheelWindow, mx, my,
                                    &xEvent->xbutton.x_root,
                                    &xEvent->xbutton.y_root);
            PointerHit wheelHit = resolvePointerHit(
                xEvent->xbutton.root, sdlWheelWindow, xEvent->xbutton.x_root,
                xEvent->xbutton.y_root);
            if (!routePointerGrabEvent(
                    display, xEvent->xbutton.root, xEvent->xbutton.x_root,
                    xEvent->xbutton.y_root, wheelHit.deepest, ButtonPressMask,
                    &eventWindow, &xEvent->xbutton.subwindow,
                    &xEvent->xbutton.x, &xEvent->xbutton.y)) {
                eventWindow = selectPointerEventWindow(
                    display, xEvent->xbutton.root, xEvent->xbutton.x_root,
                    xEvent->xbutton.y_root, wheelHit.deepest, ButtonPressMask,
                    &xEvent->xbutton.subwindow, &xEvent->xbutton.x,
                    &xEvent->xbutton.y);
            }
            if (eventWindow == None)
                return -1;
            xEvent->xbutton.window = eventWindow;
            xEvent->xbutton.state = convertModifierState(SDL_GetModState()) |
                                    pointerButtonStateSnapshot();
            xEvent->xbutton.button = wheelButton;
            xEvent->xbutton.same_screen = True;

            /* Real X11 always pairs a wheel ButtonPress with an immediate
             * matching ButtonRelease, and Motif translations (XmScrollBar,
             * XmText) only fire their scroll actions once they see the full
             * press/release sequence. Synthesizing only the press leaves
             * Btn4Down/Btn5Down translations latched mid-sequence and the
             * scroll never lands, which is why wheel input through libx11-
             * compat appears as a no-op while the same replay driven via
             * xdotool against system X11 scrolls correctly.
             *
             * Queue both ends at the put-back queue head (last enqueue ends up
             * on top, so push Release first and Press second) and bail out with
             * -1 so all consumer paths -- XNextEvent's main pump and the
             * drainSdlEventsToPutBack drains -- pull them in the right order
             * without the caller redundantly appending the Press behind the
             * Release we already queued.
             */
            XEvent pressEvent = *xEvent;
            pressEvent.xbutton.type = ButtonPress;
            pressEvent.xbutton.serial = serial;
            pressEvent.xbutton.send_event = sendEvent;
            pressEvent.xbutton.display = display;
            pressEvent.xbutton.window = eventWindow;
            timelineTapXEvent(&pressEvent);
            XEvent releaseEvent = pressEvent;
            releaseEvent.xbutton.type = ButtonRelease;
            timelineTapXEvent(&releaseEvent);
            enqueuePutBackEventWithTap(display, &releaseEvent, True);
            enqueuePutBackEventWithTap(display, &pressEvent, True);
            return -1;
        }
    case SDL_JOYAXISMOTION: /**< Joystick axis motion */
        LOG("SDL_JOYAXISMOTION\n");
        return -1;
    case SDL_JOYBALLMOTION: /**< Joystick trackball motion */
        LOG("SDL_JOYBALLMOTION\n");
        return -1;
    case SDL_JOYHATMOTION: /**< Joystick hat position change */
        LOG("SDL_JOYHATMOTION\n");
        return -1;
    case SDL_JOYBUTTONDOWN: /**< Joystick button pressed */
        LOG("SDL_JOYBUTTONDOWN\n");
        return -1;
    case SDL_JOYBUTTONUP: /**< Joystick button released */
        LOG("SDL_JOYBUTTONUP\n");
        return -1;
    case SDL_JOYDEVICEADDED: /**< A new joystick has been inserted into the
                                system
                                */
        LOG("SDL_JOYDEVICEADDED\n");
        return -1;
    case SDL_JOYDEVICEREMOVED: /**< An opened joystick has been removed */
        LOG("SDL_JOYDEVICEREMOVED\n");
        return -1;
    case SDL_CONTROLLERAXISMOTION: /**< Game controller axis motion */
        LOG("SDL_CONTROLLERAXISMOTION\n");
        return -1;
    case SDL_CONTROLLERBUTTONDOWN: /**< Game controller button pressed */
        LOG("SDL_CONTROLLERBUTTONDOWN\n");
        return -1;
    case SDL_CONTROLLERBUTTONUP: /**< Game controller button released */
        LOG("SDL_CONTROLLERBUTTONUP\n");
        return -1;
    case SDL_CONTROLLERDEVICEADDED: /**< A new Game controller has been
                                       inserted into the system
                                       */
        LOG("SDL_CONTROLLERDEVICEADDED\n");
        return -1;
    case SDL_CONTROLLERDEVICEREMOVED: /**< An opened Game controller has
                                         been removed
                                         */
        LOG("SDL_CONTROLLERDEVICEREMOVED\n");
        return -1;
    case SDL_CONTROLLERDEVICEREMAPPED: /**< The controller mapping was
                                        * updated
                                        */
        LOG("SDL_CONTROLLERDEVICEREMAPPED\n");
        return -1;
    case SDL_FINGERDOWN:  // Should not happen
        LOG("FINGER BUTTONDOWN\n");
        return -1;
        type = ButtonPress;
    case SDL_FINGERUP:  // Should not happen
        LOG("SDL_FINGERUP\n");
        return -1;
        if (sdlEvent->type == SDL_FINGERUP)
            type = ButtonRelease;
        FILL_STANDARD_VALUES(xbutton);
        eventWindow = SCREEN_WINDOW;
        xEvent->xbutton.root = eventWindow;
        xEvent->xbutton.window =
            xEvent->xbutton.root;  // The event window is always the SDL Window.
        xEvent->xbutton.subwindow = None;
        xEvent->xbutton.time = XC_EVENT_TIME_MS(sdlEvent->tfinger.timestamp);
        xEvent->xbutton.x = sdlEvent->tfinger.x;
        xEvent->xbutton.y = sdlEvent->tfinger.y;
        xEvent->xbutton.x_root =
            xEvent->xbutton.x;  // Because root and window are the same.
        xEvent->xbutton.y_root = xEvent->xbutton.y;
        xEvent->xbutton.state = convertModifierState(SDL_GetModState());
        xEvent->xbutton.button = Button1;
        xEvent->xbutton.same_screen = True;
        break;
    case SDL_FINGERMOTION:  // Should not happen
        LOG("SDL_FINGERMOTION\n");
        return -1;
#ifndef LIBX11_COMPAT_SDL3
    case SDL_DOLLARGESTURE:
        LOG("SDL_DOLLARGESTURE\n");
        return -1;
    case SDL_DOLLARRECORD:
        LOG("SDL_DOLLARRECORD\n");
        return -1;
    case SDL_MULTIGESTURE:
        LOG("SDL_MULTIGESTURE\n");
        return -1;
#endif
    case SDL_CLIPBOARDUPDATE: /**< The clipboard changed */
        LOG("SDL_CLIPBOARDUPDATE\n");
        clipboardHandleExternalUpdate(display);
        return -1;
    case SDL_DROPFILE: /**< The system requests a file open */
        LOG("SDL_DROPFILE\n");
        return -1;
    case SDL_RENDER_TARGETS_RESET:
        LOG("SDL_RENDER_TARGETS_RESET\n");
        updateWindowRenderTargets(display);
        enqueueResetExposures(display);
        return -1;
    case SDL_RENDER_DEVICE_RESET:
        LOG("SDL_RENDER_DEVICE_RESET\n");
        updateWindowRenderTargets(display);
        enqueueResetExposures(display);
        return -1;
    default:
        if (sdlEvent->type >= SDL_USEREVENT &&
            sdlEvent->type <= SDL_LASTEVENT) {
            LOG("convertEvent USEREVENT code=%d\n", sdlEvent->user.code);
            if (snapshotOwnsEventType(sdlEvent->type) &&
                sdlEvent->user.code == SNAPSHOT_EVENT_CODE) {
                /* Smoke snapshot pump: this branch runs on the main (X-client)
                 * thread, the only place where SDL_GetWindowSurface is allowed.
                 * snapshotHandleEvent frees the path buffer and signals the
                 * replay thread so its synchronous wait can return.
                 */
                snapshotHandleEvent(sdlEvent);
                return -1;
            }
            if (snapshotOwnsEventType(sdlEvent->type) &&
                sdlEvent->user.code == RESIZE_EVENT_CODE) {
                /* Replay-driven resize. SDL_SetWindowSize must run on the
                 * thread that created the window; on macOS that's the X-client
                 * main thread (the same one running here). Synchronous
                 * round-trip via the same condvar the snapshot path uses.
                 */
                snapshotHandleResizeEvent(display, sdlEvent);
                return -1;
            }
            if (snapshotOwnsEventType(sdlEvent->type) &&
                sdlEvent->user.code == FOCUS_AT_EVENT_CODE) {
                /* Replay-driven focus shift. getContainingWindow walks the
                 * window tree and XSetInputFocus mutates global focus state;
                 * both must run on the main thread to stay safe against
                 * concurrent window-tree mutators.
                 */
                snapshotHandleFocusAtEvent(display, sdlEvent);
                return -1;
            }
            if (mainDispatchOwnsEventType(sdlEvent->type) &&
                sdlEvent->user.code == MAIN_DISPATCH_EVENT_CODE) {
                /* An off-main-thread Xlib entry point routed its whole body
                 * here so window-tree mutation and SDL calls both run on the
                 * SDL-owning thread. Synchronous: the worker blocks until this
                 * returns. A queued thunk can also be drained here during
                 * window teardown (discardQueuedEventsForWindow runs the same
                 * convertEvent path), so a routed op may run while its target
                 * window is being destroyed; the routed bodies tolerate that
                 * via TYPE_CHECK, which fails cleanly on a stale window.
                 *
                 * convertEvent runs on whatever thread drained the SDL queue,
                 * and XPending / XEventsQueued / XCheckTypedEvent let a SECOND
                 * client thread drain it. The display-lock handoff gate only
                 * excludes other threads while the routing worker held an
                 * XLockDisplay; a worker that routed without holding the lock
                 * (the common case) leaves this unguarded. So the thunk touches
                 * SDL and MUST run on the main thread only: if a non-main
                 * thread drained it, put the event back and nudge the main
                 * thread rather than run it here (which would abort via
                 * requireMainEventThread, or crash off-main without the guard).
                 */
                if (!libx11CompatOnMainEventThread()) {
                    /* Put it back for the main thread. The drain already
                     * removed it from the SDL queue, so on re-queue failure it
                     * is gone: drop the command instead, or the worker blocking
                     * on it would hang forever (there is no timeout).
                     */
                    if (SDL_PushEvent(sdlEvent) <= 0) {
                        LOG("main-dispatch: re-queue from non-main drain "
                            "failed "
                            "(%s), dropping\n",
                            SDL_GetError());
                        mainDispatchDropCmd(sdlEvent);
                        return -1;
                    }
                    wakeEventPipeForExternalEvent(NULL);
                    return -1;
                }
                mainDispatchHandleEvent(sdlEvent);
                return -1;
            }
            if (stateSnapshotOwnsEventType(sdlEvent->type)) {
                stateSnapshotHandleEvent(display, sdlEvent);
                return -1;
            }
            if (presentWakeOwnsEventType(sdlEvent->type) &&
                sdlEvent->user.code == PRESENT_EVENT_CODE) {
                drawWindowDataToScreen();
                return -1;
            }
            if (sdlEvent->user.code == INTERNAL_EVENT_CODE) {
                XAnyEvent *allocEvent = sdlEvent->user.data1;
                eventWindow = (Window) sdlEvent->user.data2;
                type = allocEvent->type;
                sendEvent = allocEvent->send_event;
                serial = atomicLoadRequest(display);
                allocEvent->serial = serial;
                if (clipboardConsumeInternalEvent(display, eventWindow, type,
                                                  allocEvent)) {
                    if (freeInternalEvents)
                        free(allocEvent);
                    return -1;
                }
                switch (type) {
                case KeyRelease:
                case KeyPress:
                    memcpy(&xEvent->xkey, allocEvent, sizeof(XKeyEvent));
                    break;
                case ButtonPress:
                case ButtonRelease:
                    memcpy(&xEvent->xbutton, allocEvent, sizeof(XButtonEvent));
                    break;
                case MotionNotify:
                    memcpy(&xEvent->xmotion, allocEvent, sizeof(XMotionEvent));
                    break;
                case EnterNotify:
                case LeaveNotify:
                    memcpy(&xEvent->xcrossing, allocEvent,
                           sizeof(XCrossingEvent));
                    break;
                case FocusIn:
                case FocusOut:
                    memcpy(&xEvent->xfocus, allocEvent,
                           sizeof(XFocusChangeEvent));
                    break;
                case Expose:
                    memcpy(&xEvent->xexpose, allocEvent, sizeof(XExposeEvent));
                    break;
                case KeymapNotify:
                    memcpy(&xEvent->xkeymap, allocEvent, sizeof(XKeymapEvent));
                    break;
                case GraphicsExpose:
                    memcpy(&xEvent->xgraphicsexpose, allocEvent,
                           sizeof(XGraphicsExposeEvent));
                    break;
                case NoExpose:
                    memcpy(&xEvent->xnoexpose, allocEvent,
                           sizeof(XNoExposeEvent));
                    break;
                case VisibilityNotify:
                    memcpy(&xEvent->xvisibility, allocEvent,
                           sizeof(XVisibilityEvent));
                    break;
                case CreateNotify:
                    memcpy(&xEvent->xcreatewindow, allocEvent,
                           sizeof(XCreateWindowEvent));
                    break;
                case DestroyNotify:
                    memcpy(&xEvent->xdestroywindow, allocEvent,
                           sizeof(XDestroyWindowEvent));
                    break;
                case UnmapNotify:
                    memcpy(&xEvent->xunmap, allocEvent, sizeof(XUnmapEvent));
                    break;
                case MapNotify:
                    memcpy(&xEvent->xmap, allocEvent, sizeof(XMapEvent));
                    break;
                case MapRequest:
                    memcpy(&xEvent->xmaprequest, allocEvent,
                           sizeof(XMapRequestEvent));
                    break;
                case ReparentNotify:
                    memcpy(&xEvent->xreparent, allocEvent,
                           sizeof(XReparentEvent));
                    break;
                case ConfigureNotify:
                    memcpy(&xEvent->xconfigure, allocEvent,
                           sizeof(XConfigureEvent));
                    break;
                case ConfigureRequest:
                    memcpy(&xEvent->xconfigurerequest, allocEvent,
                           sizeof(XConfigureRequestEvent));
                    break;
                case GravityNotify:
                    memcpy(&xEvent->xgravity, allocEvent,
                           sizeof(XGravityEvent));
                    break;
                case ResizeRequest:
                    memcpy(&xEvent->xresizerequest, allocEvent,
                           sizeof(XResizeRequestEvent));
                    break;
                case CirculateNotify:
                    memcpy(&xEvent->xcirculate, allocEvent,
                           sizeof(XCirculateEvent));
                    break;
                case CirculateRequest:
                    memcpy(&xEvent->xcirculaterequest, allocEvent,
                           sizeof(XCirculateRequestEvent));
                    break;
                case PropertyNotify:
                    memcpy(&xEvent->xproperty, allocEvent,
                           sizeof(XPropertyEvent));
                    break;
                case SelectionClear:
                    memcpy(&xEvent->xselectionclear, allocEvent,
                           sizeof(XSelectionClearEvent));
                    break;
                case SelectionRequest:
                    memcpy(&xEvent->xselectionrequest, allocEvent,
                           sizeof(XSelectionRequestEvent));
                    break;
                case SelectionNotify:
                    memcpy(&xEvent->xselection, allocEvent,
                           sizeof(XSelectionEvent));
                    break;
                case ColormapNotify:
                    memcpy(&xEvent->xcolormap, allocEvent,
                           sizeof(XColormapEvent));
                    break;
                case ClientMessage:
                    memcpy(&xEvent->xclient, allocEvent,
                           sizeof(XClientMessageEvent));
                    break;
                case MappingNotify:
                    memcpy(&xEvent->xmapping, allocEvent,
                           sizeof(XMappingEvent));
                    break;
                default:
                    /* Extension events (e.g. ShmCompletion) come through here.
                     * Contract: callers that enqueue an extension event MUST
                     * allocate the full XEvent union, not just the per-type
                     * struct -- otherwise this memcpy reads past the end. See
                     * src/xshm.c XShmPutImage for the only current caller.
                     */
                    if (*((int *) allocEvent) >= LASTEvent) {
                        memcpy(xEvent, allocEvent, sizeof(XEvent));
                    } else {
                        LOG("convertEvent: dropping unknown INTERNAL event "
                            "type "
                            "%d\n",
                            *((int *) allocEvent));
                    }
                    break;
                }
                if (freeInternalEvents)
                    free(allocEvent);
                break;
            } else if (sdlEvent->user.code == SEND_EVENT_CODE) {
                XEvent *sent = sdlEvent->user.data1;
                Window sentWindow = (Window) sdlEvent->user.data2;
                if (clipboardConsumeInternalEvent(display, sentWindow,
                                                  sent->type, sent)) {
                    if (freeInternalEvents)
                        free(sent);
                    return -1;
                }
                memcpy(xEvent, sdlEvent->user.data1, sizeof(XEvent));
                if (freeInternalEvents)
                    free(sdlEvent->user.data1);
                timelineTapXEvent(xEvent);
                return 0;
            }
        }
        return -1;
    }

    FILL_STANDARD_VALUES(xany);
    xEvent->xany.window = eventWindow;
    xEvent->type = type;
    timelineTapXEvent(xEvent);
    return 0;
#undef FILL_STANDARD_VALUES
}

static void updateWindowRenderTargets(Display *display)
{
    WARN_UNIMPLEMENTED;
}

static void printEventInfo(XEvent *event)
{
    char *eventType;
    char msg[100];
#define CASE_TYPE(type, format, ...)               \
    case type:                                     \
        eventType = #type;                         \
        snprintf(msg, 100, format, ##__VA_ARGS__); \
        break
    switch (event->type) {
        CASE_TYPE(KeyPress, "");
        CASE_TYPE(KeyRelease, "");
        CASE_TYPE(ButtonPress, "");
        CASE_TYPE(ButtonRelease, "");
        CASE_TYPE(MotionNotify, "");
        CASE_TYPE(EnterNotify, "");
        CASE_TYPE(LeaveNotify, "");
        CASE_TYPE(FocusIn, "");
        CASE_TYPE(FocusOut, "");
        CASE_TYPE(KeymapNotify, "");
        CASE_TYPE(
            Expose, "window %lu, rect (x = %d, y = %d, %dx%d), count = %d",
            event->xexpose.window, event->xexpose.x, event->xexpose.y,
            event->xexpose.width, event->xexpose.height, event->xexpose.count);
        CASE_TYPE(GraphicsExpose, "");
        CASE_TYPE(NoExpose, "");
        CASE_TYPE(VisibilityNotify, "");
        CASE_TYPE(CreateNotify, "");
        CASE_TYPE(DestroyNotify, "");
        CASE_TYPE(UnmapNotify, "");
        CASE_TYPE(MapNotify, "");
        CASE_TYPE(MapRequest, "");
        CASE_TYPE(ReparentNotify, "");
        CASE_TYPE(ConfigureNotify,
                  "window %lu (x = %d, y = %d, w = %d, h = %d), borderW = %d, "
                  "above %lu, overrideRedirect = %d",
                  event->xconfigure.window, event->xconfigure.x,
                  event->xconfigure.y, event->xconfigure.width,
                  event->xconfigure.height, event->xconfigure.border_width,
                  event->xconfigure.above, event->xconfigure.override_redirect);
        CASE_TYPE(ConfigureRequest, "");
        CASE_TYPE(GravityNotify, "");
        CASE_TYPE(ResizeRequest, "");
        CASE_TYPE(CirculateNotify, "");
        CASE_TYPE(CirculateRequest, "");
        CASE_TYPE(PropertyNotify, "");
        CASE_TYPE(SelectionClear, "");
        CASE_TYPE(SelectionRequest, "");
        CASE_TYPE(SelectionNotify, "");
        CASE_TYPE(ColormapNotify, "");
        CASE_TYPE(ClientMessage, "");
        CASE_TYPE(MappingNotify, "");
    default:
        eventType = "Unknown";
        msg[0] = '\0';
        break;
    }
#undef CASE_TYPE
    LOG("Got %s event\n", eventType);
    LOG("%s\n", msg);
}

static Bool isPresentWakeEvent(const SDL_Event *event)
{
    return event->type >= SDL_USEREVENT && event->type <= SDL_LASTEVENT &&
           presentWakeOwnsEventType(event->type) &&
           event->user.code == PRESENT_EVENT_CODE;
}

static Bool isInteractiveSdlEvent(const SDL_Event *event)
{
    switch (event->type) {
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    XC_CASE_WINDOWEVENT:
        return True;
    default:
        return False;
    }
}

int XNextEvent(Display *display, XEvent *event_return)
{
    // https://tronche.com/gui/x/xlib/event-handling/manipulating-event-queue/XNextEvent.html
    SDL_Event event;
    while (1) {
        if (popPutBackEvent(display, event_return)) {
            printEventInfo(event_return);
            return 0;
        }
        int qlen;
        getEventQueueLength(&qlen);
        LOG("Events in queue = %d, qlen = %d\n", qlen,
            displayEventQueueLength(display));
        if (sdlPeepEventsForXlibDrain(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT,
                                      SDL_LASTEVENT) != 1) {
            pumpEventsSafe();
            if (sdlPeepEventsForXlibDrain(&event, 1, SDL_GETEVENT,
                                          SDL_FIRSTEVENT, SDL_LASTEVENT) != 1) {
                /* Real X11 implicitly flushes the request queue when the client
                 * blocks on input. If input is already queued, handle it first
                 * so heavy readback/present work does not sit in front of mouse
                 * and keyboard events. Reaching this idle wait also marks the
                 * end of any coalesced repaint burst, so release it here and
                 * present the final frame once (a no-op when not coalescing).
                 */
                endCoalesceClientRepaint();
                drawWindowDataToScreen();
                int putBackCount = countPutBackEvents(display);
                if (displayEventQueueLength(display) > putBackCount)
                    resetEventWakeups(display, putBackCount);
                SDL_Delay(1);
                continue;
            }
        }
        if (isPresentWakeEvent(&event)) {
            SDL_Event next;
            if (SDL_PeepEvents(&next, 1, SDL_PEEKEVENT, SDL_FIRSTEVENT,
                               SDL_LASTEVENT) == 1 &&
                isInteractiveSdlEvent(&next)) {
                if (sdlPeepEventsForXlibDrain(&next, 1, SDL_GETEVENT,
                                              SDL_FIRSTEVENT,
                                              SDL_LASTEVENT) != 1)
                    continue;

                /* The present wake was already removed from SDL's queue above.
                 * Drain its old pipe byte before pushing it back to the tail,
                 * then let the normal path below drain the interactive event's
                 * byte. The sequence keeps select(ConnectionNumber) aligned
                 * with SDL's queue while still keeping input ahead of deferred
                 * readback.
                 */
                READ_EVENT_IN_PIPE(display);
                SDL_PushEvent(&event);
                event = next;
            }
        }

        /* Drain the wake-up byte the SDL filter wrote for this event so a
         * follow-up select(ConnectionNumber) reflects the real queue depth.
         * With the non-blocking pipe, a stale qlen no longer forces XNextEvent
         * to fabricate an Expose.
         */
        READ_EVENT_IN_PIPE(display);
        int convertResult = convertEvent(display, &event, event_return, True);
        if (convertResult == 0) {
            GET_DISPLAY(display)->last_request_read = event_return->xany.serial;
            printEventInfo(event_return);
            bumpEventSerial();
            LOG("Leaving XNextEvent\n");
            return 0;
        }
        if (convertResult < 0) {
            /* Silently swallowed SDL event (e.g. exposed). */
            bumpEventSerial();
            continue;
        }
        LOG("Got unknown SDL event %d, dropping.\n", event.type);
        bumpEventSerial();

        /* Do NOT fabricate a fake Expose: a wrong event type confuses Motif's
         * translation tables and Xt's event dispatcher far worse than a brief
         * spin in this loop. Wait for the next real event instead.
         */
    }
}

/* Shared SDL user-event type for the internal pump (enqueueEvent's
 * INTERNAL_EVENT_CODE) and XSendEvent's SEND_EVENT_CODE. The specific id is
 * irrelevant: convertEvent dispatches both purely on user.code, so one
 * registered type serves both paths. Registered lazily and guarded so two
 * threads issuing their first enqueueEvent/XSendEvent under XInitThreads cannot
 * each grab a separate id or race the plain read the two call sites used to do
 * on their own function-local statics. SDL-atomic-backed for a lock-free read
 * once registered, mirroring snapshot.c's snapshotEventType.
 */
static SDL_atomic_t internalEventType = {(int) (Uint32) -1};

static Uint32 getInternalEventType(void)
{
    Uint32 registered = (Uint32) SDL_AtomicGet(&internalEventType);
    if (registered != (Uint32) -1)
        return registered;

    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lock);
    registered = (Uint32) SDL_AtomicGet(&internalEventType);
    if (registered == (Uint32) -1) {
        registered = SDL_RegisterEvents(1);
        if (registered != (Uint32) -1)
            SDL_AtomicSet(&internalEventType, (int) registered);
    }
    pthread_mutex_unlock(&lock);
    return registered;
}

Bool enqueueEvent(Display *display, Window eventWindow, void *event)
{
    Uint32 sendEventType = getInternalEventType();
    if (sendEventType != ((Uint32) -1)) {
        int qlenBefore = displayEventQueueLength(display);
        SDL_Event sdlEvent;
        SDL_zero(sdlEvent);
        sdlEvent.type = sendEventType;
        sdlEvent.user.code = INTERNAL_EVENT_CODE;
        sdlEvent.user.data1 = event;
        sdlEvent.user.data2 = (void *) eventWindow;
        LOG("Enqueuing event\n");
        int pushed = SDL_PushEvent(&sdlEvent);
        if (pushed != 1) {
            LOG("SDL_PushEvent failed/filtered: result=%d error=%s type=%d "
                "window=%lu\n",
                pushed, SDL_GetError(), ((XAnyEvent *) event)->type,
                eventWindow);
            return False;
        }
        if (displayEventQueueLength(display) <= qlenBefore)
            ENQUEUE_EVENT_IN_PIPE(display);
        return True;
    }
    LOG("Failed to send event: SDL_RegisterEvents failed!");
    return False;
}

int XPutBackEvent(Display *display, XEvent *event)
{
    LockDisplay(display);
    Bool ok = enqueuePutBackEvent(display, event);
    UnlockDisplay(display);
    return ok ? 0 : 1;
}

static Bool acceptsEventMask(Window window, long event_mask)
{
    if (event_mask == NoEventMask)
        return True;
    return IS_TYPE(window, WINDOW) &&
           (GET_WINDOW_STRUCT(window)->eventMask & event_mask) != 0;
}

/* EWMH ClientMessage dispatch lives in src/events-ewmh.c. */

Status XSendEvent(Display *display,
                  Window window,
                  Bool propagate,
                  long event_mask,
                  XEvent *event_send)
{
    // https://tronche.com/gui/x/xlib/event-handling/XSendEvent.html
    SET_X_SERVER_REQUEST(display, X_SendEvent);
    TYPE_CHECK(window, WINDOW, display, 0);

    /* EWMH wire-protocol: ClientMessage to the root window with a recognized
     * message_type drives an in-process WM action and is consumed. This must
     * come BEFORE the acceptsEventMask walk because nothing in libx11-compat
     * selects SubstructureNotifyMask | SubstructureRedirectMask on the root
     * (the shim plays the WM role itself), so the walk would otherwise drop
     * every EWMH request. Unrecognized message_types fall through to the
     * regular walk.
     */
    if (window == SCREEN_WINDOW && event_send &&
        event_send->type == ClientMessage &&
        dispatchRootEwmhClientMessage(display, event_send))
        return 1;
    Window eventWindow = window;
    while (!acceptsEventMask(eventWindow, event_mask)) {
        if (!propagate || eventWindow == SCREEN_WINDOW ||
            GET_PARENT(eventWindow) == None) {
            return 1;
        }
        eventWindow = GET_PARENT(eventWindow);
    }
    event_send->xany.send_event = True;
    event_send->xany.serial = currentEventSerial();
    event_send->xany.display = display;
    bumpEventSerial();
    Uint32 sendEventType = getInternalEventType();
    if (sendEventType != ((Uint32) -1)) {
        XEvent *copy = malloc(sizeof(XEvent));
        if (!copy) {
            handleOutOfMemory(0, display, 0, 0);
            return 0;
        }
        memcpy(copy, event_send, sizeof(XEvent));
        SDL_Event sdlEvent;
        SDL_zero(sdlEvent);
        sdlEvent.type = sendEventType;
        sdlEvent.user.code = SEND_EVENT_CODE;
        sdlEvent.user.data1 = copy;
        sdlEvent.user.data2 = (void *) eventWindow;
        LOG("SEND event\n");
        if (SDL_PushEvent(&sdlEvent) != 1) {
            free(copy);
            return 0;
        }
        return 1;
    }
    return 0;
}

Bool XFilterEvent(XEvent *event, Window w)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XFilterEvent.3.xhtml
    (void) event;
    (void) w;

    /* The host input method consumes composition at the SDL layer before
     * convertEvent ever runs: preedit keystrokes arrive as SDL_TEXTEDITING and
     * are dropped with return -1, while committed text arrives as SDL_TEXTINPUT
     * and is delivered as a synthetic KeyPress the client decodes with
     * XmbLookupString. So no X event that reaches a client still needs input
     * method filtering, and this must never swallow one.
     *
     * An earlier version returned True for every KeyPress/KeyRelease whenever
     * getKeyboardFocus() was None. That was wrong: host text-input focus can be
     * live while the in-process WM has not assigned an X focus window (a
     * single-window client like xwpe never calls XSetInputFocus), so clients
     * that gate on XFilterEvent saw Enter, every other real key, and IM commits
     * silently vanish. Key routing is decided in convertEvent; filtering is not
     * this function's job here.
     */
    return False;
}

int XEventsQueued(Display *display, int mode)
{
    // https://tronche.com/gui/x/xlib/event-handling/XEventsQueued.html
    //    SET_X_SERVER_REQUEST(display, XCB_);
    if (mode == QueuedAlready)
        return displayEventQueueLength(display);
    if (mode == QueuedAfterFlush) {
        pumpEventsSafe();
        int sdlQueued = 0;
        if (getEventQueueLength(&sdlQueued) && sdlQueued > 0)
            return drainSdlEventsToPutBack(display);
        int putBackCount = countPutBackEvents(display);
        if (putBackCount > 0)
            return putBackCount;
        XFlush(display);
    } else {
        pumpEventsSafe();
    }
    return drainSdlEventsToPutBack(display);
}

int XFlush(Display *display)
{
    // https://tronche.com/gui/x/xlib/event-handling/XFlush.html
    pumpEventsSafe();
    drainSdlEventsToPutBack(display);

    /* Real X11 batches drawing on the server and flushes here. libx11-compat
     * accumulates primitives in the renderer's back buffer and only presents on
     * flush so partial frames don't reach the screen.
     */
    drawWindowDataToScreen();
    return 1;
}

static Window getSiblingBelow(Window window)
{
    Window parent = GET_PARENT(window);
    Window *children = GET_CHILDREN(parent);
    size_t i;
    for (i = 0; i < GET_WINDOW_STRUCT(parent)->children.length; i++) {
        if (children[i] == window &&
            i + 1 < GET_WINDOW_STRUCT(parent)->children.length) {
            return children[i + 1];
        }
    }
    return None;
}

static int getVisibilityState(Window window)
{
    if (GET_WINDOW_STRUCT(window)->mapState != Mapped)
        return VisibilityFullyObscured;
    Window parent = GET_PARENT(window);
    if (parent == None)
        return VisibilityUnobscured;
    Window *children = GET_CHILDREN(parent);
    Bool foundSelf = False;
    for (size_t i = 0; i < GET_WINDOW_STRUCT(parent)->children.length; i++) {
        if (children[i] == window) {
            foundSelf = True;
            continue;
        }
        if (foundSelf && GET_WINDOW_STRUCT(children[i])->mapState == Mapped &&
            windowsOverlap(window, children[i])) {
            return VisibilityPartiallyObscured;
        }
    }
    return VisibilityUnobscured;
}

/* convertEvent() frees the delivered pointer after dispatch, so multi-cast
 * notify events (parent via SubstructureNotifyMask, self via
 * StructureNotifyMask) cannot share one allocation -- the second dispatch would
 * double-free. enqueueAlias clones src, patches the per-event "event" field to
 * identify the recipient, and enqueues the copy.
 */
static Bool enqueueAlias(Display *display,
                         const void *src,
                         size_t size,
                         size_t eventFieldOffset,
                         Window recipient)
{
    void *copy = malloc(size);
    if (!copy) {
        handleOutOfMemory(0, display, 0, 0);
        return False;
    }
    memcpy(copy, src, size);
    *((Window *) ((char *) copy + eventFieldOffset)) = recipient;
    if (!enqueueEvent(display, recipient, copy)) {
        free(copy);
        return False;
    }
    return True;
}

/* Fan-out helper for DestroyNotify, ConfigureNotify, MapNotify, UnmapNotify and
 * GravityNotify: each can be reported to the parent (SubstructureNotify) and to
 * the window itself (StructureNotify).
 *
 *   FANOUT_FAIL     -- enqueue failed; event already freed, caller breaks.
 *   FANOUT_CONSUMED -- delivered to parent only; caller must NOT set
 * eventData (ownership transferred to the queue). FANOUT_KEEP -- event still
 * owned by caller; it must store it in eventData so the self delivery happens.
 * The "event" field has been patched to self.
 */
typedef enum { FANOUT_FAIL, FANOUT_CONSUMED, FANOUT_KEEP } FanoutResult;

static FanoutResult fanOutNotify(Display *display,
                                 void *event,
                                 size_t size,
                                 size_t eventFieldOffset,
                                 Window self,
                                 Bool wantParent,
                                 Bool wantSelf)
{
    Window parent = GET_PARENT(self);
    if (wantParent && wantSelf) {
        if (!enqueueAlias(display, event, size, eventFieldOffset, parent)) {
            free(event);
            return FANOUT_FAIL;
        }
    } else if (wantParent) {
        *((Window *) ((char *) event + eventFieldOffset)) = parent;
        if (!enqueueEvent(display, parent, event)) {
            free(event);
            return FANOUT_FAIL;
        }
        return FANOUT_CONSUMED;
    }
    *((Window *) ((char *) event + eventFieldOffset)) = self;
    return FANOUT_KEEP;
}

Bool postReparentUnmapNotify(Display *display,
                             Window eventWindow,
                             Window oldParent)
{
    Bool wantParent = HAS_EVENT_MASK(oldParent, SubstructureNotifyMask);
    Bool wantSelf = HAS_EVENT_MASK(eventWindow, StructureNotifyMask);
    if (!wantParent && !wantSelf)
        return True;

    XUnmapEvent *event = malloc(sizeof(XUnmapEvent));
    if (!event)
        return False;
    event->type = UnmapNotify;
    event->send_event = False;
    event->display = display;
    event->window = eventWindow;
    event->from_configure = False;

    if (wantParent && wantSelf) {
        if (!enqueueAlias(display, event, sizeof(*event),
                          offsetof(XUnmapEvent, event), oldParent)) {
            free(event);
            return False;
        }
    } else if (wantParent) {
        event->event = oldParent;
        if (!enqueueEvent(display, oldParent, event)) {
            free(event);
            return False;
        }
        return True;
    }

    event->event = eventWindow;
    if (!enqueueEvent(display, eventWindow, event)) {
        free(event);
        return False;
    }
    return True;
}

Bool postEvent(Display *display, Window eventWindow, unsigned int eventId, ...)
{
#define SKIP                 \
    {                        \
        eventNeeded = False; \
        break;               \
    }
    void *eventData = NULL;
    Bool eventNeeded = True;
    va_list args;
    va_start(args, eventId);
    switch (eventId) {
    case CreateNotify: {
        if (!HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask))
            SKIP XCreateWindowEvent *event = malloc(sizeof(XCreateWindowEvent));
        if (!event)
            break;
        event->type = eventId;
        event->send_event = False;
        event->display = display;
        WindowStruct *windowStruct = GET_WINDOW_STRUCT(eventWindow);
        event->parent = windowStruct->parent;
        event->window = eventWindow;
        event->x = windowStruct->x;
        event->y = windowStruct->y;
        event->width = windowStruct->w;
        event->height = windowStruct->h;
        event->border_width = windowStruct->borderWidth;
        event->override_redirect = windowStruct->overrideRedirect;
        eventData = event;
        eventWindow = event->parent;
        break;
    }
    case DestroyNotify: {
        Bool wantParent =
            HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask);
        Bool wantSelf = HAS_EVENT_MASK(eventWindow, StructureNotifyMask);
        if (!wantParent && !wantSelf)
            SKIP XDestroyWindowEvent *event =
                malloc(sizeof(XDestroyWindowEvent));
        if (!event)
            break;
        event->type = eventId;
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        FanoutResult r = fanOutNotify(display, event, sizeof(*event),
                                      offsetof(XDestroyWindowEvent, event),
                                      eventWindow, wantParent, wantSelf);
        if (r == FANOUT_KEEP)
            eventData = event;
        else if (r == FANOUT_CONSUMED)
            eventNeeded = False;
        break;
    }
    case Expose: {
        SDL_Rect *exposeRect = va_arg(args, SDL_Rect *);
        size_t exposeCount = va_arg(args, size_t);
        if (!HAS_EVENT_MASK(eventWindow, ExposureMask) ||
            IS_INPUT_ONLY(eventWindow) ||
            GET_WINDOW_STRUCT(eventWindow)->mapState != Mapped) {
            LOG("Skipping Expose for window %lu mask=0x%lx inputOnly=%d "
                "mapState=%d\n",
                eventWindow, GET_WINDOW_STRUCT(eventWindow)->eventMask,
                IS_INPUT_ONLY(eventWindow),
                GET_WINDOW_STRUCT(eventWindow)->mapState);
            SKIP
        }
        XEvent event;
        memset(&event, 0, sizeof(event));
        event.xexpose.type = eventId;
        event.xexpose.serial = atomicLoadRequest(display);
        event.xexpose.send_event = False;
        event.xexpose.display = display;
        event.xexpose.window = eventWindow;
        event.xexpose.x = exposeRect->x;
        event.xexpose.y = exposeRect->y;
        event.xexpose.width = exposeRect->w;
        event.xexpose.height = exposeRect->h;
        event.xexpose.count = exposeCount;

        /* Expose bursts are generated internally; queue them directly so Xt
         * exposure compression can see the complete burst. Drain already queued
         * SDL events first so this Expose does not jump ahead of older
         * input/client/window events.
         */
        drainSdlEventsToPutBack(display);
        if (!appendPutBackEvent(display, &event))
            break;
        eventNeeded = False;
        break;
    }
    case ConfigureRequest: {
        if (GET_WINDOW_STRUCT(eventWindow)->overrideRedirect ||
            !HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureRedirectMask))
            SKIP XConfigureRequestEvent *event =
                malloc(sizeof(XConfigureRequestEvent));
        if (!event)
            break;
        event->type = eventId;
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        event->parent = GET_PARENT(eventWindow);
        event->value_mask = va_arg(args, unsigned long);
        XWindowChanges *windowChanges = va_arg(args, XWindowChanges *);
        GET_WINDOW_POS(eventWindow, event->x, event->y);
        if (HAS_VALUE(event->value_mask, CWX))
            event->x = windowChanges->x;
        if (HAS_VALUE(event->value_mask, CWY))
            event->y = windowChanges->y;
        GET_WINDOW_DIMS(eventWindow, event->width, event->height);
        if (HAS_VALUE(event->value_mask, CWWidth))
            event->width = windowChanges->width;
        if (HAS_VALUE(event->value_mask, CWHeight))
            event->height = windowChanges->height;
        if (HAS_VALUE(event->value_mask, CWBorderWidth)) {
            event->border_width = windowChanges->border_width;
        } else {
            event->border_width = GET_WINDOW_STRUCT(eventWindow)->borderWidth;
        }
        event->above = HAS_VALUE(event->value_mask, CWSibling)
                           ? windowChanges->sibling
                           : None;
        event->detail = HAS_VALUE(event->value_mask, CWStackMode)
                            ? windowChanges->stack_mode
                            : Above;
        eventData = event;
        break;
    }
    case ConfigureNotify: {
        Bool wantParent =
            HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask);
        Bool wantSelf = HAS_EVENT_MASK(eventWindow, StructureNotifyMask);
        if (!wantParent && !wantSelf)
            SKIP XConfigureEvent *event = malloc(sizeof(XConfigureEvent));
        if (!event)
            break;
        event->type = eventId;
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        GET_WINDOW_POS(eventWindow, event->x, event->y);
        GET_WINDOW_DIMS(eventWindow, event->width, event->height);
        compatTrace(
            "deliver ConfigureNotify: window=%lu size=(%dx%d) "
            "wantSelf=%d wantParent=%d\n",
            eventWindow, event->width, event->height, wantSelf, wantParent);
        event->border_width = GET_WINDOW_STRUCT(eventWindow)->borderWidth;
        event->above = getSiblingBelow(eventWindow);
        event->override_redirect =
            GET_WINDOW_STRUCT(eventWindow)->overrideRedirect;
        FanoutResult r = fanOutNotify(display, event, sizeof(*event),
                                      offsetof(XConfigureEvent, event),
                                      eventWindow, wantParent, wantSelf);
        if (r == FANOUT_KEEP)
            eventData = event;
        else if (r == FANOUT_CONSUMED)
            eventNeeded = False;
        break;
    }
    case ReparentNotify: {
        Window oldParent = va_arg(args, Window);
        Window newParent = GET_PARENT(eventWindow);
        Window recipients[3];
        size_t numRecipients = 0;
        if (HAS_EVENT_MASK(oldParent, SubstructureNotifyMask))
            recipients[numRecipients++] = oldParent;
        if (HAS_EVENT_MASK(newParent, SubstructureNotifyMask))
            recipients[numRecipients++] = newParent;
        if (HAS_EVENT_MASK(eventWindow, StructureNotifyMask))
            recipients[numRecipients++] = eventWindow;
        if (numRecipients == 0)
            SKIP XReparentEvent *event = malloc(sizeof(XReparentEvent));
        if (!event)
            break;

        event->type = eventId;
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        event->parent = newParent;
        GET_WINDOW_POS(eventWindow, event->x, event->y);
        event->override_redirect =
            GET_WINDOW_STRUCT(eventWindow)->overrideRedirect;

        /* Deliver clones to every recipient except the last; the last keeps the
         * original allocation via eventData.
         */
        size_t i;
        for (i = 0; i + 1 < numRecipients; i++) {
            if (!enqueueAlias(display, event, sizeof(*event),
                              offsetof(XReparentEvent, event), recipients[i]))
                break;
        }
        if (i + 1 < numRecipients) {
            free(event);
            break;
        }
        event->event = recipients[numRecipients - 1];
        eventData = event;
        break;
    }
    case MapRequest: {
        if (!HAS_EVENT_MASK(GET_PARENT(eventWindow),
                            SubstructureRedirectMask) ||
            GET_WINDOW_STRUCT(eventWindow)->overrideRedirect ||
            GET_WINDOW_STRUCT(eventWindow)->mapState != UnMapped)
            SKIP XMapRequestEvent *event = malloc(sizeof(XMapRequestEvent));
        if (!event)
            break;
        event->type = eventId;
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        event->parent = GET_PARENT(eventWindow);
        eventData = event;
        break;
    }
    case MapNotify: {
        Bool wantParent =
            HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask);
        Bool wantSelf = HAS_EVENT_MASK(eventWindow, StructureNotifyMask);
        if (!wantParent && !wantSelf)
            SKIP XMapEvent *event = malloc(sizeof(XMapEvent));
        if (!event)
            break;

        event->type = eventId;
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        event->override_redirect =
            GET_WINDOW_STRUCT(eventWindow)->overrideRedirect;
        FanoutResult r = fanOutNotify(display, event, sizeof(*event),
                                      offsetof(XMapEvent, event), eventWindow,
                                      wantParent, wantSelf);
        if (r == FANOUT_KEEP)
            eventData = event;
        else if (r == FANOUT_CONSUMED)
            eventNeeded = False;
        break;
    }
    case UnmapNotify: {
        Bool wantParent =
            HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask);
        Bool wantSelf = HAS_EVENT_MASK(eventWindow, StructureNotifyMask);
        Bool fromConfigure = va_arg(args, Bool);
        if (!wantParent && !wantSelf)
            SKIP XUnmapEvent *event = malloc(sizeof(XUnmapEvent));
        if (!event)
            break;

        event->type = eventId;
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        event->from_configure = fromConfigure;
        FanoutResult r = fanOutNotify(display, event, sizeof(*event),
                                      offsetof(XUnmapEvent, event), eventWindow,
                                      wantParent, wantSelf);
        if (r == FANOUT_KEEP)
            eventData = event;
        else if (r == FANOUT_CONSUMED)
            eventNeeded = False;
        break;
    }
    case ClientMessage: {
        /* calloc instead of malloc: the data union has five long slots and the
         * spec lets clients consult any of them. malloc left data.l[1..4] full
         * of heap bytes, which fed Xt's WM_DELETE_WINDOW handler garbage in the
         * timestamp slot. send_event = True matches what real X servers stamp
         * on synthesized ICCCM messages, so Motif's compare against
         * e.send_event in WM_PROTOCOLS callbacks now sees the expected value.
         */
        XClientMessageEvent *event = calloc(1, sizeof(XClientMessageEvent));
        if (!event)
            break;

        event->type = eventId;
        event->send_event = True;
        event->display = display;
        event->window = eventWindow;
        event->format = va_arg(args, int);
        event->message_type = va_arg(args, Atom);
        event->data.l[0] = va_arg(args, Atom);
        event->data.l[1] = va_arg(args, Time);
        eventData = event;
        break;
    }
    case VisibilityNotify: {
        if (!HAS_EVENT_MASK(eventWindow, VisibilityChangeMask))
            SKIP XVisibilityEvent *event = malloc(sizeof(XVisibilityEvent));
        if (!event)
            break;

        event->type = eventId;
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        event->state = getVisibilityState(eventWindow);
        eventData = event;
        break;
    }
    case NoExpose: {
        XNoExposeEvent *event = malloc(sizeof(XNoExposeEvent));
        if (!event)
            break;
        event->type = eventId;
        event->serial = currentEventSerial();
        event->send_event = False;
        event->display = display;
        event->drawable = eventWindow;
        event->major_code = va_arg(args, int);
        event->minor_code = va_arg(args, int);
        eventData = event;
        break;
    }
    case GraphicsExpose: {
        SDL_Rect *exposeRect = va_arg(args, SDL_Rect *);
        size_t exposeCount = va_arg(args, size_t);
        XGraphicsExposeEvent *event = malloc(sizeof(XGraphicsExposeEvent));
        if (!event)
            break;
        event->type = eventId;
        event->serial = currentEventSerial();
        event->send_event = False;
        event->display = display;
        event->drawable = eventWindow;
        event->x = exposeRect ? exposeRect->x : 0;
        event->y = exposeRect ? exposeRect->y : 0;
        event->width = exposeRect ? exposeRect->w : 0;
        event->height = exposeRect ? exposeRect->h : 0;
        event->count = exposeCount > 0 ? (int) exposeCount - 1 : 0;
        event->major_code = va_arg(args, int);
        event->minor_code = va_arg(args, int);
        eventData = event;
        break;
    }
    case ColormapNotify: {
        if (!HAS_EVENT_MASK(eventWindow, ColormapChangeMask))
            SKIP XColormapEvent *event = malloc(sizeof(XColormapEvent));
        if (!event)
            break;
        event->type = eventId;
        event->serial = currentEventSerial();
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        event->colormap = va_arg(args, Colormap);
        event->new = va_arg(args, Bool);
        event->state = va_arg(args, int);
        eventData = event;
        break;
    }
    case GravityNotify: {
        Bool wantParent =
            HAS_EVENT_MASK(GET_PARENT(eventWindow), SubstructureNotifyMask);
        Bool wantSelf = HAS_EVENT_MASK(eventWindow, StructureNotifyMask);
        if (!wantParent && !wantSelf)
            SKIP XGravityEvent *event = malloc(sizeof(XGravityEvent));
        if (!event)
            break;
        event->type = eventId;
        event->serial = currentEventSerial();
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        GET_WINDOW_POS(eventWindow, event->x, event->y);
        FanoutResult r = fanOutNotify(display, event, sizeof(*event),
                                      offsetof(XGravityEvent, event),
                                      eventWindow, wantParent, wantSelf);
        if (r == FANOUT_KEEP)
            eventData = event;
        else if (r == FANOUT_CONSUMED)
            eventNeeded = False;
        break;
    }
    case PropertyNotify: {
        if (!HAS_EVENT_MASK(eventWindow, PropertyChangeMask))
            SKIP XPropertyEvent *event = malloc(sizeof(XPropertyEvent));
        if (!event)
            break;
        event->type = eventId;
        event->serial = currentEventSerial();
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        event->atom = va_arg(args, Atom);
        event->time = CurrentTime;
        event->state = va_arg(args, int);
        eventData = event;
        break;
    }
    case KeyRelease:
    case KeyPress:
        //            memcpy(&xEvent->xkey, allocEvent, sizeof(XKeyEvent));
        //            break;
    case ButtonPress:
    case ButtonRelease:
        //            memcpy(&xEvent->xbutton, allocEvent,
        //            sizeof(XButtonEvent)); break;
    case MotionNotify:
        //            memcpy(&xEvent->xmotion, allocEvent,
        //            sizeof(XMotionEvent)); break;
    case EnterNotify:
    case LeaveNotify:
        //            memcpy(&xEvent->xcrossing, allocEvent,
        //            sizeof(XCrossingEvent)); break;
    case FocusIn:
    case FocusOut:
        //            memcpy(&xEvent->xfocus, allocEvent,
        //            sizeof(XFocusChangeEvent)); break;
    case KeymapNotify:
        //            memcpy(&xEvent->xexpose, allocEvent,
        //            sizeof(XExposeEvent)); break;
    case ResizeRequest:
        //            memcpy(&xEvent->xresizerequest, allocEvent,
        //            sizeof(XResizeRequestEvent)); break;
    case CirculateNotify:
        //            memcpy(&xEvent->xcirculate, allocEvent,
        //            sizeof(XCirculateEvent)); break;
    case CirculateRequest:
        //            memcpy(&xEvent->xconfigurerequest, allocEvent,
        //            sizeof(XConfigureRequestEvent)); break;
    case SelectionClear:
        //            memcpy(&xEvent->xselectionclear, allocEvent,
        //            sizeof(XSelectionClearEvent)); break;
    case SelectionRequest:
        //            memcpy(&xEvent->xselectionrequest, allocEvent,
        //            sizeof(XSelectionRequestEvent)); break;
    case SelectionNotify:
        //            memcpy(&xEvent->xselection, allocEvent,
        //            sizeof(XSelectionEvent)); break;
    case MappingNotify:
        //            memcpy(&xEvent->xmapping, allocEvent,
        //            sizeof(XMappingEvent)); break;
    default:
        break;
    }

    va_end(args);

    if (!eventData)
        return !eventNeeded;

    /* enqueueEvent only owns the heap event once it actually pushes onto the
     * SDL queue. If SDL_RegisterEvents has never succeeded the call fails
     * without taking ownership, so free here to avoid a leak.
     */
    Bool ok = enqueueEvent(display, eventWindow, eventData);
    if (!ok)
        free(eventData);
    return ok;
#undef SKIP
}

static long eventMaskForType(int type)
{
    switch (type) {
    case KeyPress:
        return KeyPressMask;
    case KeyRelease:
        return KeyReleaseMask;
    case ButtonPress:
        return ButtonPressMask;
    case ButtonRelease:
        return ButtonReleaseMask;
    case EnterNotify:
        return EnterWindowMask;
    case LeaveNotify:
        return LeaveWindowMask;
    case MotionNotify:
        return PointerMotionMask | ButtonMotionMask | Button1MotionMask |
               Button2MotionMask | Button3MotionMask | Button4MotionMask |
               Button5MotionMask;
    case Expose:
        return ExposureMask;
    case VisibilityNotify:
        return VisibilityChangeMask;
    case CreateNotify:
    case DestroyNotify:
    case MapNotify:
    case UnmapNotify:
    case ReparentNotify:
    case ConfigureNotify:
    case GravityNotify:
    case CirculateNotify:
        return StructureNotifyMask | SubstructureNotifyMask;
    case MapRequest:
    case ConfigureRequest:
    case CirculateRequest:
        return SubstructureRedirectMask;
    case FocusIn:
    case FocusOut:
        return FocusChangeMask;
    case PropertyNotify:
        return PropertyChangeMask;
    case ColormapNotify:
        return ColormapChangeMask;
    default:
        return 0;
    }
}

static Bool checkTypedPredicate(Window w, int type, long mask, XEvent *event)
{
    return event->type == type;
}

static Bool checkTypedWindowPredicate(Window w,
                                      int type,
                                      long mask,
                                      XEvent *event)
{
    return event->type == type && event->xany.window == w;
}

static Bool checkMaskedWindowPredicate(Window w,
                                       int type,
                                       long mask,
                                       XEvent *event)
{
    return (eventMaskForType(event->type) & mask) && event->xany.window == w;
}

static Bool checkMaskedPredicate(Window w, int type, long mask, XEvent *event)
{
    (void) w;
    (void) type;
    return (eventMaskForType(event->type) & mask) != 0;
}

static Bool checkTypedEvent(Display *display,
                            Window w,
                            int type,
                            long mask,
                            XEvent *event,
                            Bool(predicate)(Window, int, long, XEvent *))
{
    if (displayEventQueueLength(display) == 0)
        pumpEventsSafe();
    int qlen = 0;
    LockDisplay(display);

    /* Flush dispatch commands parked while a handoff was pending, so a client
     * pumping only through XCheckTypedEvent still drives them.
     */
    mainDispatchRunDeferred();
    if (removeMatchingPutBackEvent(display, w, type, mask, event, predicate)) {
        UnlockDisplay(display);
        return True;
    }
    getEventQueueLength(&qlen);
    if (qlen > 0) {
        SDL_Event *tmp = calloc((size_t) qlen, sizeof(SDL_Event));
        if (!tmp) {
            UnlockDisplay(display);
            handleOutOfMemory(0, display, 0, 0);
            return False;
        }
        qlen = sdlPeepEventsForXlibDrain(tmp, qlen, SDL_GETEVENT,
                                         SDL_FIRSTEVENT, SDL_LASTEVENT);
        if (qlen < 0) {
            LOG("Unable to read event queue: %s\n", SDL_GetError());
            free(tmp);
            UnlockDisplay(display);
            return False;
        }
        for (int i = 0; i < qlen; i++)
            READ_EVENT_IN_PIPE(display);
        Bool foundMatch = False;
        for (int i = 0; i < qlen; i++) {
            XEvent convertedEvent;
            if (convertEvent(display, &tmp[i], &convertedEvent, True) != 0)
                continue;
            if (!foundMatch && predicate(w, type, mask, &convertedEvent)) {
                memcpy(event, &convertedEvent, sizeof(XEvent));
                foundMatch = True;
            } else if (!appendPutBackEventWithTap(display, &convertedEvent,
                                                  True)) {
                free(tmp);
                UnlockDisplay(display);
                return False;
            }
        }
        int remainingSdlEvents = 0;
        if (getEventQueueLength(&remainingSdlEvents)) {
            int desiredQlen = countPutBackEvents(display) + remainingSdlEvents;
            if (displayEventQueueLength(display) != desiredQlen)
                resetEventWakeups(display, desiredQlen);
        }
        free(tmp);
        if (foundMatch) {
            UnlockDisplay(display);
            return True;
        }
        if (removeMatchingPutBackEvent(display, w, type, mask, event,
                                       predicate)) {
            UnlockDisplay(display);
            return True;
        }
    }
    UnlockDisplay(display);
    return False;
}

static Bool checkIfEvent(Display *display,
                         XEvent *event,
                         Bool (*predicate)(Display *, XEvent *, char *),
                         char *arg)
{
    if (displayEventQueueLength(display) == 0)
        pumpEventsSafe();
    int qlen = 0;
    LockDisplay(display);
    if (removeMatchingPutBackIfEvent(display, event, predicate, arg)) {
        UnlockDisplay(display);
        return True;
    }
    getEventQueueLength(&qlen);
    if (qlen > 0) {
        SDL_Event *tmp = calloc((size_t) qlen, sizeof(SDL_Event));
        if (!tmp) {
            UnlockDisplay(display);
            handleOutOfMemory(0, display, 0, 0);
            return False;
        }
        qlen = sdlPeepEventsForXlibDrain(tmp, qlen, SDL_GETEVENT,
                                         SDL_FIRSTEVENT, SDL_LASTEVENT);
        if (qlen < 0) {
            LOG("Unable to read event queue: %s\n", SDL_GetError());
            free(tmp);
            UnlockDisplay(display);
            return False;
        }
        for (int i = 0; i < qlen; i++)
            READ_EVENT_IN_PIPE(display);
        Bool foundMatch = False;
        for (int i = 0; i < qlen; i++) {
            XEvent convertedEvent;
            if (convertEvent(display, &tmp[i], &convertedEvent, True) != 0)
                continue;
            if (!foundMatch && predicate(display, &convertedEvent, arg)) {
                memcpy(event, &convertedEvent, sizeof(XEvent));
                foundMatch = True;
            } else if (!appendPutBackEventWithTap(display, &convertedEvent,
                                                  True)) {
                free(tmp);
                UnlockDisplay(display);
                return False;
            }
        }
        int remainingSdlEvents = 0;
        if (getEventQueueLength(&remainingSdlEvents)) {
            int desiredQlen = countPutBackEvents(display) + remainingSdlEvents;
            if (displayEventQueueLength(display) != desiredQlen)
                resetEventWakeups(display, desiredQlen);
        }
        free(tmp);
        if (foundMatch) {
            UnlockDisplay(display);
            return True;
        }
        if (removeMatchingPutBackIfEvent(display, event, predicate, arg)) {
            UnlockDisplay(display);
            return True;
        }
    }
    UnlockDisplay(display);
    return False;
}

int XWindowEvent(Display *display, Window w, long mask, XEvent *event)
{
    while (!XCheckWindowEvent(display, w, mask, event)) {
        drawWindowDataToScreen();
        pumpEventsSafe();
        SDL_Delay(1);
    }
    return 0;
}

Bool XCheckIfEvent(register Display *display,
                   register XEvent *event /* XEvent to be filled in. */,
                   Bool (*predicate)(Display * /* display */,
                                     XEvent * /* event */,
                                     char * /* arg */
                                     ),
                   char *arg /* argument passed to predicate */)
{
    return checkIfEvent(display, event, predicate, arg);
}

int XIfEvent(register Display *display,
             register XEvent *event,
             Bool (*predicate)(Display * /* display */,
                               XEvent * /* event */,
                               char * /* arg */
                               ),
             char *arg)
{
    while (!checkIfEvent(display, event, predicate, arg)) {
        drawWindowDataToScreen();
        pumpEventsSafe();
        SDL_Delay(1);
    }
    return 0;
}

Bool XCheckTypedEvent(Display *display, int type, XEvent *event)
{
    return checkTypedEvent(display, 0, type, 0, event, &checkTypedPredicate);
}

Bool XCheckTypedWindowEvent(Display *display, Window w, int type, XEvent *event)
{
    return checkTypedEvent(display, w, type, 0, event,
                           &checkTypedWindowPredicate);
}

Bool XCheckWindowEvent(Display *display, Window w, long mask, XEvent *event)
{
    return checkTypedEvent(display, w, 0, mask, event,
                           &checkMaskedWindowPredicate);
}

Bool XCheckMaskEvent(Display *display, long mask, XEvent *event)
{
    return checkTypedEvent(display, None, 0, mask, event,
                           &checkMaskedPredicate);
}

int XMaskEvent(Display *display, long mask, XEvent *event)
{
    while (!XCheckMaskEvent(display, mask, event)) {
        drawWindowDataToScreen();
        pumpEventsSafe();
        SDL_Delay(1);
    }
    return 0;
}

int XPeekEvent(Display *display, XEvent *event)
{
    XNextEvent(display, event);
    XPutBackEvent(display, event);
    return 0;
}

int XPeekIfEvent(Display *display,
                 XEvent *event,
                 Bool (*predicate)(Display *, XEvent *, char *),
                 char *arg)
{
    XIfEvent(display, event, predicate, arg);
    XPutBackEvent(display, event);
    return 0;
}

#undef ENQUEUE_EVENT_IN_PIPE
#undef READ_EVENT_IN_PIPE
