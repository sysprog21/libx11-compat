#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include "sdl-compat.h"
#include "events.h"
#include "events-ewmh.h"
#include "events-expose.h"
#include "errors.h"
#include "input.h"
#include "input-method.h"
#include "display.h"
#include "atoms.h"
#include "util.h"
#include "drawing.h"
#include "window-internal.h"
#include "replay-target.h"
#include "snapshot.h"
#include "state-snapshot.h"
#include "timeline.h"
#include "X11/Xlibint.h"

int eventFds[2] = {-1, -1};
#define READ_EVENT_FD eventFds[0]
#define WRITE_EVENT_FD eventFds[1]
unsigned long lastEventSerial = 1;
static SDL_mutex *eventQueueLengthLock = NULL;

/* Concurrency: SDL invokes onSdlEvent on its own event thread while Xlib API
 * entries run on the application thread. Each shared piece of state has its own
 * mutex; helper wrappers tolerate a NULL lock so static initializers and
 * pre-init code do not deadlock. The locks are created in initEventPipe and
 * destroyed when the last display closes.
 */
static SDL_mutex *putBackEventsLock = NULL;
static SDL_mutex *trackedDisplaysLock = NULL;
static SDL_mutex *activePointerWindowLock = NULL;
static Uint32 xtWakeEventType = (Uint32) -1;
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

/* clearWindowTreeWithoutExpose, postFullWindowExpose, and
 * postSyntheticWindowResize live in src/events-expose.c alongside the fan-out
 * expose helpers.
 */

/* SDL_PumpEvents is only safe on the thread that owns SDL windows. XOpenDisplay
 * captures that owner before client threads can issue Xlib requests.
 */
static SDL_threadID mainEventThreadId = 0;
static Bool mainEventThreadCaptured = False;

void captureMainEventThreadIfUnset(void)
{
    if (!mainEventThreadCaptured) {
        mainEventThreadId = SDL_ThreadID();
        mainEventThreadCaptured = True;
    }
}

void releaseMainEventThread(void)
{
    /* Allow a later XOpenDisplay to capture a new owner thread. */
    mainEventThreadCaptured = False;
    mainEventThreadId = 0;
}

static void pumpEventsSafe(void)
{
    if (!mainEventThreadCaptured) {
        /* Pre-initialization callers are still single-threaded. */
        SDL_PumpEvents();
        return;
    }
    if (SDL_ThreadID() != mainEventThreadId) {
        LOG("SDL_PumpEvents skipped: called from thread %lu, main is %lu\n",
            (unsigned long) SDL_ThreadID(), (unsigned long) mainEventThreadId);
        return;
    }
    SDL_PumpEvents();
}

static Uint32 xtWakeTimerCallback(XC_TIMER_CALLBACK_PARAMS)
{
    (void) param;
    if (xtWakeEventType == (Uint32) -1)
        return interval;

    /* Rate-limit: if the consumer hasn't drained the previous wake event, don't
     * queue another. Otherwise an idle (or slow-dispatching) client accumulates
     * wake events in SDL's queue and the corresponding pipe bytes via
     * onSdlEvent's accounting, both of which would surface as phantom "events
     * pending" to XEventsQueued and to external select(ConnectionNumber)
     * consumers.
     */
    if (SDL_HasEvent(xtWakeEventType))
        return interval;

    SDL_Event event;
    SDL_zero(event);
    event.type = xtWakeEventType;
    SDL_PushEvent(&event);
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
static void incrementDisplayEventQueueLength(Display *display)
{
    if (eventQueueLengthLock)
        SDL_LockMutex(eventQueueLengthLock);
    GET_DISPLAY(display)->qlen++;
    if (eventQueueLengthLock)
        SDL_UnlockMutex(eventQueueLengthLock);
}

static void decrementDisplayEventQueueLength(Display *display)
{
    if (eventQueueLengthLock)
        SDL_LockMutex(eventQueueLengthLock);
    if (GET_DISPLAY(display)->qlen > 0)
        GET_DISPLAY(display)->qlen--;
    if (eventQueueLengthLock)
        SDL_UnlockMutex(eventQueueLengthLock);
}

static int displayEventQueueLength(Display *display)
{
    if (eventQueueLengthLock)
        SDL_LockMutex(eventQueueLengthLock);
    int qlen = GET_DISPLAY(display)->qlen;
    if (eventQueueLengthLock)
        SDL_UnlockMutex(eventQueueLengthLock);
    return qlen;
}

static void setDisplayEventQueueLength(Display *display, int qlen)
{
    if (eventQueueLengthLock)
        SDL_LockMutex(eventQueueLengthLock);
    GET_DISPLAY(display)->qlen = qlen < 0 ? 0 : qlen;
    if (eventQueueLengthLock)
        SDL_UnlockMutex(eventQueueLengthLock);
}

static void discardPipeWakeups(void)
{
    char buffer[64];
    while (read(READ_EVENT_FD, buffer, sizeof(buffer)) > 0) {
    }
}

static void resetEventWakeups(Display *display, int qlen)
{
    char buffer[64];
    memset(buffer, 'e', sizeof(buffer));
    discardPipeWakeups();
    int remaining = qlen;
    while (remaining > 0) {
        size_t chunk = (size_t) remaining;
        if (chunk > sizeof(buffer))
            chunk = sizeof(buffer);
        ssize_t written = write(WRITE_EVENT_FD, buffer, chunk);
        if (written <= 0)
            break;
        remaining -= (int) written;
    }
    setDisplayEventQueueLength(display, qlen);
}

#define ENQUEUE_EVENT_IN_PIPE(display)                               \
    do {                                                             \
        char buffer = 'e';                                           \
        ssize_t _w = write(WRITE_EVENT_FD, &buffer, sizeof(buffer)); \
        (void) _w;                                                   \
        incrementDisplayEventQueueLength(display);                   \
    } while (0)
#define READ_EVENT_IN_PIPE(display)                                \
    do {                                                           \
        char buffer;                                               \
        ssize_t _r = read(READ_EVENT_FD, &buffer, sizeof(buffer)); \
        (void) _r;                                                 \
        decrementDisplayEventQueueLength(display);                 \
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
        char buffer[64];
        memset(buffer, 'e', sizeof(buffer));
        discardPipeWakeups();
        int remaining = maxDesiredQlen;
        while (remaining > 0) {
            size_t chunk = (size_t) remaining;
            if (chunk > sizeof(buffer))
                chunk = sizeof(buffer);
            ssize_t written = write(WRITE_EVENT_FD, buffer, chunk);
            if (written <= 0)
                break;
            remaining -= (int) written;
        }
    }
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
    XC_CASE_WINDOWEVENT:
        if (!GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlWindow ||
            event->window.windowID ==
                SDL_GetWindowID(GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlWindow)) {
            return 0;
        }
        /* Fall through to enqueue non-screen window events. */
        ENQUEUE_EVENT_IN_PIPE((Display *) userdata);
        break;
    default:
        ENQUEUE_EVENT_IN_PIPE((Display *) userdata);
        break;
    }
    return 1;
}

static Bool getEventQueueLength(int *qlen)
{
    /* Motif expose/configure bursts and resize-driven repaints routinely push
     * the queue past 25 entries in a single SDL_PumpEvents tick; callers used
     * the returned length both to size a follow-up GETEVENT drain and to feed
     * XEventsQueued, so undercounting throttled Xt's main loop and dropped
     * events behind a quiet 25-event ceiling.
     *
     * The cap is just a peek buffer; size it large enough for realistic bursts
     * but bounded so a runaway SDL queue can't blow the stack.
     */
    enum { PEEK_CAP = 4096 };
    SDL_Event tmp[PEEK_CAP];
    *qlen = SDL_PeepEvents(tmp, PEEK_CAP, SDL_PEEKEVENT, SDL_FIRSTEVENT,
                           SDL_LASTEVENT);
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
    int qlen = 0;
    getEventQueueLength(&qlen);
    if (qlen <= 0) {
        int putBackCount = countPutBackEvents(display);
        if (displayEventQueueLength(display) > putBackCount)
            resetEventWakeups(display, putBackCount);
        return putBackCount;
    }

    SDL_Event *events = calloc((size_t) qlen, sizeof(SDL_Event));
    if (!events) {
        handleOutOfMemory(0, display, 0, 0);
        return countPutBackEvents(display);
    }

    qlen = sdlPeepEventsForXlibDrain(events, qlen, SDL_GETEVENT, SDL_FIRSTEVENT,
                                     SDL_LASTEVENT);
    if (qlen < 0) {
        LOG("Unable to read event queue: %s\n", SDL_GetError());
        free(events);
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
    return countPutBackEvents(display);
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
    event.xexpose.serial = lastEventSerial;
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
    event->serial = lastEventSerial;
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

static Bool windowSelectsAny(Window window, long mask)
{
    return IS_TYPE(window, WINDOW) &&
           (GET_WINDOW_STRUCT(window)->eventMask & mask) != 0;
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
                                       Window clickTopLevel,
                                       int rootX,
                                       int rootY,
                                       long mask,
                                       Window *subwindowReturn,
                                       int *eventXReturn,
                                       int *eventYReturn)
{
    /* SDL tells us which top-level the pointer event belongs to (the window the
     * user actually clicked on screen). Trust that over a global stacking
     * search: a top-level's logical position in the window model can diverge
     * from its real on-screen placement (e.g. an image window the toolkit
     * created over the toolbox in the model but that the host placed elsewhere
     * on screen), and a global getContainingWindow would then misroute a click
     * on the visible window to whichever top-level overlaps it in the model.
     * Descend from clickTopLevel using coordinates relative to it; rootX/rootY
     * were derived from the same logical origin, so the offset cancels out.
     */
    Window deepest = None;
    if (clickTopLevel != None && clickTopLevel != SCREEN_WINDOW &&
        IS_TYPE(clickTopLevel, WINDOW) && GET_PARENT(clickTopLevel) == root) {
        int tlx = 0, tly = 0, tlw = 0, tlh = 0;
        GET_WINDOW_POS(clickTopLevel, tlx, tly);
        GET_WINDOW_DIMS(clickTopLevel, tlw, tlh);
        int localX = rootX - tlx, localY = rootY - tly;
        /* Only constrain to the reported top-level when the point is actually
         * inside it. A drag-release outside the captured window arrives with
         * that window's id but out-of-bounds coordinates; fall through to the
         * global search so it lands on the window really under the pointer.
         */
        if (localX >= 0 && localX < tlw && localY >= 0 && localY < tlh)
            deepest = getContainingWindow(clickTopLevel, localX, localY);
    }
    if (deepest == None)
        deepest = getContainingWindow(root, rootX, rootY);
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
        *subwindowReturn = getDirectChildContainingPoint(
            eventWindow, *eventXReturn, *eventYReturn);
    return eventWindow;
}

static Bool routePointerGrabEvent(Display *display,
                                  Window root,
                                  Window clickTopLevel,
                                  int rootX,
                                  int rootY,
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
            display, root, clickTopLevel, rootX, rootY, mask, subwindowReturn,
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
    event->serial = lastEventSerial;
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

void discardQueuedEventsForWindow(Display *display, Window window)
{
    int removedPutBackEvents = 0;
    lockPutBackEvents();
    PutBackEvent **link = &putBackEvents;
    while (*link) {
        PutBackEvent *node = *link;
        if (node->display == display && node->event.xany.window == window) {
            *link = node->next;
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
    if (qlen <= 0)
        return;

    SDL_Event *events = calloc((size_t) qlen, sizeof(SDL_Event));
    if (!events) {
        handleOutOfMemory(0, display, 0, 0);
        return;
    }
    qlen = sdlPeepEventsForXlibDrain(events, qlen, SDL_GETEVENT, SDL_FIRSTEVENT,
                                     SDL_LASTEVENT);
    if (qlen < 0) {
        LOG("Unable to read event queue: %s\n", SDL_GetError());
        free(events);
        return;
    }

    for (int i = 0; i < qlen; i++)
        READ_EVENT_IN_PIPE(display);
    for (int i = 0; i < qlen; i++) {
        XEvent converted;
        if (convertEvent(display, &events[i], &converted, True) != 0)
            continue;
        if (converted.xany.window == window)
            continue;
        appendPutBackEventWithTap(display, &converted, True);
    }
    int remainingSdlEvents = 0;
    if (getEventQueueLength(&remainingSdlEvents)) {
        int desiredQlen = countPutBackEvents(display) + remainingSdlEvents;
        if (displayEventQueueLength(display) != desiredQlen)
            resetEventWakeups(display, desiredQlen);
    }
    free(events);
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
    lastEventSerial = 1;
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
        if (xtWakeEventType == (Uint32) -1)
            xtWakeEventType = SDL_RegisterEvents(1);
        if (xtWakeEventType != (Uint32) -1)
            xtWakeTimer = SDL_AddTimer(33, xtWakeTimerCallback, NULL);
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
    if (activePointerWindow == window)
        activePointerWindow = None;
    if (pointerHoverWindow == window)
        pointerHoverWindow = None;
    unlockActivePointerWindow();
}

static Bool postPointerCrossingEvents(Display *display,
                                      int rootX,
                                      int rootY,
                                      unsigned int state,
                                      Time time)
{
    Window newHoverWindow = getContainingWindow(SCREEN_WINDOW, rootX, rootY);
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

int convertEvent(Display *display,
                 SDL_Event *sdlEvent,
                 XEvent *xEvent,
                 Bool freeInternalEvents)
{
    if (sdlEvent->type == xtWakeEventType)
        return -1;

    Bool sendEvent = False;
    Window eventWindow = None;
    int type = -1;
    unsigned long serial = lastEventSerial;
#define FILL_STANDARD_VALUES(eventStruct)       \
    xEvent->eventStruct.type = type;            \
    xEvent->eventStruct.serial = serial;        \
    xEvent->eventStruct.send_event = sendEvent; \
    xEvent->eventStruct.display = display
    switch (sdlEvent->type) {
    case SDL_KEYDOWN:
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
         * -> 116
         * == 't', SDLK_KP_0 0x40000062 -> 98 == 'b'). XkbKeycodeToKeysym uses
         * the same truncation so decoding round-trips for the common (ASCII)
         * keys. XKeysymToKeycode guards the reverse direction (it refuses a
         * keycode that does not round-trip) so Motif cannot bind a special key
         * onto a letter; pressing one of the rare aliasing scancode keys still
         * types the colliding character, which is accepted given the 8-bit
         * limit.
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
            eventWindow = eventWindow == None ? xEvent->xkey.root : eventWindow;
        }
        xEvent->xkey.window = eventWindow;
        xEvent->xkey.subwindow = None;
        xEvent->xkey.time = XC_EVENT_TIME_MS(sdlEvent->key.timestamp);
        int pointerX = 0, pointerY = 0;
        if (!replayTargetReadPointer(&pointerX, &pointerY))
            SDL_GetMouseState(&pointerX, &pointerY);
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
        translateSdlPointToRoot(display, sdlButtonWindow, sdlEvent->button.x,
                                sdlEvent->button.y, &xEvent->xbutton.x_root,
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
        /* Explicit XGrabPointer routing owns the event when
         * routePointerGrabEvent returns True; otherwise fall back to the normal
         * pointer-window selection (with a sticky ButtonRelease delivery to the
         * last button-press recipient).
         */
        if (!routePointerGrabEvent(display, xEvent->xbutton.root,
                                   sdlButtonWindow, xEvent->xbutton.x_root,
                                   xEvent->xbutton.y_root, buttonMask,
                                   &eventWindow, &xEvent->xbutton.subwindow,
                                   &xEvent->xbutton.x, &xEvent->xbutton.y)) {
            if (type == ButtonRelease && activePointerSnapshot != None &&
                windowSelectsAny(activePointerSnapshot, buttonMask)) {
                eventWindow = activePointerSnapshot;
                translateRootPointToWindow(
                    display, xEvent->xbutton.root, eventWindow,
                    xEvent->xbutton.x_root, xEvent->xbutton.y_root,
                    &xEvent->xbutton.x, &xEvent->xbutton.y);
                xEvent->xbutton.subwindow = getDirectChildContainingPoint(
                    eventWindow, xEvent->xbutton.x, xEvent->xbutton.y);
            } else {
                eventWindow = selectPointerEventWindow(
                    display, xEvent->xbutton.root, sdlButtonWindow,
                    xEvent->xbutton.x_root, xEvent->xbutton.y_root, buttonMask,
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
        translateSdlPointToRoot(display, sdlMotionWindow, sdlEvent->motion.x,
                                sdlEvent->motion.y, &xEvent->xmotion.x_root,
                                &xEvent->xmotion.y_root);
        unsigned int motionButtonState = pointerButtonStateSnapshot();
        unsigned int motionState =
            convertModifierState(SDL_GetModState()) | motionButtonState;
        Bool crossingQueued = postPointerCrossingEvents(
            display, xEvent->xmotion.x_root, xEvent->xmotion.y_root,
            motionState, XC_EVENT_TIME_MS(sdlEvent->motion.timestamp));
        long motionMask = motionMaskForButtonState(motionButtonState);
        /* Explicit XGrabPointer routing owns the event when
         * routePointerGrabEvent returns True; otherwise the implicit
         * button-press grab (snapshot != None) wins, falling back to regular
         * pointer-window selection.
         */
        if (!routePointerGrabEvent(display, xEvent->xmotion.root,
                                   sdlMotionWindow, xEvent->xmotion.x_root,
                                   xEvent->xmotion.y_root, motionMask,
                                   &eventWindow, &xEvent->xmotion.subwindow,
                                   &xEvent->xmotion.x, &xEvent->xmotion.y)) {
            lockActivePointerWindow();
            Window snapshot = activePointerWindow;
            unlockActivePointerWindow();
            if (snapshot != None && windowSelectsAny(snapshot, motionMask)) {
                eventWindow = snapshot;
                translateRootPointToWindow(
                    display, xEvent->xmotion.root, eventWindow,
                    xEvent->xmotion.x_root, xEvent->xmotion.y_root,
                    &xEvent->xmotion.x, &xEvent->xmotion.y);
                xEvent->xmotion.subwindow = getDirectChildContainingPoint(
                    eventWindow, xEvent->xmotion.x, xEvent->xmotion.y);
            } else {
                eventWindow = selectPointerEventWindow(
                    display, xEvent->xmotion.root, sdlMotionWindow,
                    xEvent->xmotion.x_root, xEvent->xmotion.y_root, motionMask,
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
                GET_WINDOW_STRUCT(eventWindow)->mapState = Mapped;
                markWindowNeedsPresent(eventWindow);
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
                int logicalX = sdlEvent->window.data1;
                int logicalY = sdlEvent->window.data2;
                topLevelWindowLogicalPosition(eventWindow, logicalX, logicalY,
                                              &logicalX, &logicalY);
                GET_WINDOW_STRUCT(eventWindow)->x = logicalX;
                GET_WINDOW_STRUCT(eventWindow)->y = logicalY;
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
                    topLevelWindowLogicalPosition(
                        eventWindow, xEvent->xconfigure.x, xEvent->xconfigure.y,
                        &xEvent->xconfigure.x, &xEvent->xconfigure.y);
                }
            } else {
                SDL_GetWindowPosition(
                    SDL_GetWindowFromID(sdlEvent->window.windowID),
                    &xEvent->xconfigure.x, &xEvent->xconfigure.y);
                if (eventWindow != None) {
                    topLevelWindowLogicalPosition(
                        eventWindow, xEvent->xconfigure.x, xEvent->xconfigure.y,
                        &xEvent->xconfigure.x, &xEvent->xconfigure.y);
                }
            }
            /* The X11 window and its backing are kept at the logical (point)
             * size; presentation scales up to the physical surface. A RESIZED
             * event carries the new logical size in data1/data2 on both SDL2
             * and SDL3. SDL3 additionally fires SDL_WINDOWEVENT_SIZE_CHANGED as
             * PIXEL_SIZE_CHANGED, whose data1/data2 are physical pixels (2x on
             * a HiDPI display); stamping that onto the X11 geometry would
             * disagree with everything measured in logical units and break
             * client layout (Motif wraps every line after a couple of
             * characters). So treat only the logical resize as authoritative
             * for X11 dimensions.
             */
            Bool logicalResize =
                XC_WINDOW_SUBEVENT(sdlEvent) == SDL_WINDOWEVENT_RESIZED;
#ifndef LIBX11_COMPAT_SDL3
            /* SDL2 SIZE_CHANGED is also in logical units. */
            logicalResize = logicalResize || XC_WINDOW_SUBEVENT(sdlEvent) ==
                                                 SDL_WINDOWEVENT_SIZE_CHANGED;
#endif
            if (logicalResize) {
                xEvent->xconfigure.width = sdlEvent->window.data1;
                xEvent->xconfigure.height = sdlEvent->window.data2;
                if (eventWindow != None) {
                    GET_WINDOW_STRUCT(eventWindow)->w =
                        (unsigned int) sdlEvent->window.data1;
                    GET_WINDOW_STRUCT(eventWindow)->h =
                        (unsigned int) sdlEvent->window.data2;
                    resizeWindowTexture(eventWindow);
                    /* The top-level's own cached visibleRegion is its (0,0,w,h)
                     * frame; it goes stale on every resize driven by SDL
                     * outside of configureWindow.
                     */
                    invalidateVisibleRegionForTopLevel(eventWindow);
                    clearWindowTreeWithoutExpose(display, eventWindow);
                    postFullWindowExpose(display, eventWindow);
                }
            } else {
                SDL_GetWindowSize(
                    SDL_GetWindowFromID(sdlEvent->window.windowID),
                    &xEvent->xconfigure.width, &xEvent->xconfigure.height);
            }
            xEvent->xconfigure.border_width =
                GET_WINDOW_STRUCT(eventWindow)->borderWidth;
            xEvent->xconfigure.above = None;
            xEvent->xconfigure.override_redirect =
                GET_WINDOW_STRUCT(eventWindow)->overrideRedirect;
            break;
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
        case SDL_WINDOWEVENT_FOCUS_LOST:
            if (XC_WINDOW_SUBEVENT(sdlEvent) == SDL_WINDOWEVENT_FOCUS_LOST) {
                LOG("Window %d lost keyboard focus\n",
                    sdlEvent->window.windowID);
                type = FocusOut;
            }
            FILL_STANDARD_VALUES(xfocus);
            xEvent->xfocus.window = eventWindow;
            xEvent->xfocus.mode = NotifyNormal;
            xEvent->xfocus.detail = NotifyAncestor;
            break;
        case SDL_WINDOWEVENT_CLOSE:
            LOG("Window %d closed\n", sdlEvent->window.windowID);
            /* Re-resolve per call: freeAtomStorage drops dynamic atoms on the
             * last XCloseDisplay without resetting lastUsedAtom, so a static
             * cache would dangle to a stale id across a close/open cycle.
             */
            Atom wmProtocolsAtom = internalInternAtom("WM_PROTOCOLS");
            Atom wmDeleteWindowAtom = internalInternAtom("WM_DELETE_WINDOW");
            WindowProperty *windowProperty =
                findProperty(&GET_WINDOW_STRUCT(eventWindow)->properties,
                             wmProtocolsAtom, NULL);
            Bool clientHandlesDelete = False;
            if (windowProperty && windowProperty->type == XA_ATOM &&
                windowProperty->dataFormat == 32 && windowProperty->data) {
                for (size_t i = 0; i < windowProperty->dataLength; i++) {
                    if (((Atom *) windowProperty->data)[i] ==
                        wmDeleteWindowAtom) {
                        postEvent(display, eventWindow, ClientMessage, 32,
                                  wmProtocolsAtom, wmDeleteWindowAtom,
                                  (Time) XC_EVENT_TIME_MS(
                                      sdlEvent->window.timestamp));
                        clientHandlesDelete = True;
                        break;
                    }
                }
            }
            /* Real X11 kills the client connection here. Route through the IO
             * error hook so a client that installed one can intercept;
             * otherwise it terminates.
             */
            if (!clientHandlesDelete)
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
        /* Cmd+Q / SIGTERM / dock-quit: emulate a fatal IO error so any
         * client-installed XIOErrorHandler runs first.
         */
        triggerIOError(display);
        return -1;
    case SDL_APP_TERMINATING: /**< The application is being terminated by the OS
                                 Called on iOS in applicationWillTerminate()
                                 Called on Android in onDestroy()
                            */
        LOG("SDL_APP_TERMINATING\n");
        return -1;
    case SDL_APP_LOWMEMORY: /**< The application is low on memory, free memory
                               if possible. Called on iOS in
                               applicationDidReceiveMemoryWarning() Called on
                               Android in onLowMemory()
                            */
        LOG("SDL_APP_LOWMEMORY\n");
        return -1;
    case SDL_APP_WILLENTERBACKGROUND: /**< The application is about to enter the
                                 background Called on iOS in
                                 applicationWillResignActive() Called on Android
                                 in onPause()
                            */
        LOG("SDL_APP_WILLENTERBACKGROUND\n");
        return -1;
    case SDL_APP_DIDENTERBACKGROUND: /**< The application did enter the
                                 background and may not get CPU for some time
                                 Called on iOS in
                                 applicationDidEnterBackground() Called on
                                 Android in onPause()
                            */
        LOG("SDL_APP_DIDENTERBACKGROUND\n");
        return -1;
    case SDL_APP_WILLENTERFOREGROUND: /**< The application is about to enter the
                                 foreground Called on iOS in
                                 applicationWillEnterForeground() Called on
                                 Android in onResume()
                            */
        LOG("SDL_APP_WILLENTERFOREGROUND\n");
        return -1;
    case SDL_APP_DIDENTERFOREGROUND: /**< The application is now interactive
                                 Called on iOS in applicationDidBecomeActive()
                                 Called on Android in onResume()
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
        return -1;
    case SDL_TEXTINPUT: /**< Keyboard text input */
        LOG("SDL_TEXTINPUT\n");
        return -1;
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
            if (sdlEvent->wheel.which == SDL_TOUCH_MOUSEID &&
                replayTargetReadPointer(&mx, &my)) {
                /* injected pos wins */
            } else {
                SDL_GetMouseState(&mx, &my);
            }
            xEvent->xbutton.time = XC_EVENT_TIME_MS(sdlEvent->wheel.timestamp);
            translateSdlPointToRoot(display, sdlWheelWindow, mx, my,
                                    &xEvent->xbutton.x_root,
                                    &xEvent->xbutton.y_root);
            if (!routePointerGrabEvent(
                    display, xEvent->xbutton.root, sdlWheelWindow,
                    xEvent->xbutton.x_root, xEvent->xbutton.y_root,
                    ButtonPressMask, &eventWindow, &xEvent->xbutton.subwindow,
                    &xEvent->xbutton.x, &xEvent->xbutton.y)) {
                eventWindow = selectPointerEventWindow(
                    display, xEvent->xbutton.root, sdlWheelWindow,
                    xEvent->xbutton.x_root, xEvent->xbutton.y_root,
                    ButtonPressMask, &xEvent->xbutton.subwindow,
                    &xEvent->xbutton.x, &xEvent->xbutton.y);
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
                                system */
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
    case SDL_CONTROLLERDEVICEADDED: /**< A new Game controller has been inserted
                                       into the system */
        LOG("SDL_CONTROLLERDEVICEADDED\n");
        return -1;
    case SDL_CONTROLLERDEVICEREMOVED: /**< An opened Game controller has been
                                         removed */
        LOG("SDL_CONTROLLERDEVICEREMOVED\n");
        return -1;
    case SDL_CONTROLLERDEVICEREMAPPED: /**< The controller mapping was updated
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
                serial = GET_DISPLAY(display)->request;
                allocEvent->serial = serial;
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
                 * and keyboard events.
                 */
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
            lastEventSerial++;
            LOG("Leaving XNextEvent\n");
            return 0;
        }
        if (convertResult < 0) {
            /* Silently swallowed SDL event (e.g. exposed). */
            lastEventSerial++;
            continue;
        }
        LOG("Got unknown SDL event %d, dropping.\n", event.type);
        lastEventSerial++;
        /* Do NOT fabricate a fake Expose: a wrong event type confuses Motif's
         * translation tables and Xt's event dispatcher far worse than a brief
         * spin in this loop. Wait for the next real event instead.
         */
    }
}

Bool enqueueEvent(Display *display, Window eventWindow, void *event)
{
    static Uint32 sendEventType = (Uint32) -1;
    if (sendEventType == ((Uint32) -1))
        sendEventType = SDL_RegisterEvents(1);
    if (sendEventType != ((Uint32) -1)) {
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
    event_send->xany.serial = lastEventSerial;
    event_send->xany.display = display;
    lastEventSerial++;
    static Uint32 sendEventType = (Uint32) -1;
    if (sendEventType == ((Uint32) -1))
        sendEventType = SDL_RegisterEvents(1);
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
        SDL_PushEvent(&sdlEvent);
        return 1;
    }
    return 0;
}

Bool XFilterEvent(XEvent *event, Window w)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XFilterEvent.3.xhtml
    if (event->type == KeyPress || event->type == KeyRelease) {
        // filter event if there is no keyboard focus
        return getKeyboardFocus() == None;
    }
    // We don't get an event from sdl, if an IM gets it before us.
    return False;
}

int XEventsQueued(Display *display, int mode)
{
    // https://tronche.com/gui/x/xlib/event-handling/XEventsQueued.html
    //    SET_X_SERVER_REQUEST(display, XCB_);
    if (mode == QueuedAlready)
        return displayEventQueueLength(display);
    if (mode == QueuedAfterFlush) {
        XFlush(display);
    } else {
        pumpEventsSafe();
    }
    int putBackCount = countPutBackEvents(display);
    int queued = displayEventQueueLength(display);
    if (queued <= putBackCount)
        return putBackCount;
    return drainSdlEventsToPutBack(display);
}

int XFlush(Display *display)
{
    // https://tronche.com/gui/x/xlib/event-handling/XFlush.html
    pumpEventsSafe();
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
 *   FANOUT_CONSUMED -- delivered to parent only; caller must NOT set eventData
 *                      (ownership transferred to the queue).
 *   FANOUT_KEEP     -- event still owned by caller; it must store it in
 *                      eventData so the self delivery happens. The "event"
 *                      field has been patched to self.
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
        event.xexpose.serial = GET_DISPLAY(display)->request;
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
        event->serial = lastEventSerial;
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
        event->serial = lastEventSerial;
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
        event->serial = lastEventSerial;
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
        event->serial = lastEventSerial;
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
        event->serial = lastEventSerial;
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
                   register XEvent *event, /* XEvent to be filled in. */
                   Bool (*predicate)(Display * /* display */,
                                     XEvent * /* event */,
                                     char * /* arg */
                                     ),     /* function to call */
                   char *arg)
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
