#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <SDL2/SDL.h>
#include "events.h"
#include "errors.h"
#include "input.h"
#include "input-method.h"
#include "display.h"
#include "atoms.h"
#include "util.h"
#include "drawing.h"
#include "window-internal.h"
#include "X11/Xlibint.h"

int eventFds[2];
#define READ_EVENT_FD eventFds[0]
#define WRITE_EVENT_FD eventFds[1]
unsigned long lastEventSerial = 1;
static SDL_mutex *eventQueueLengthLock = NULL;

/* SDL_PumpEvents is only safe on the thread that owns SDL windows. XOpenDisplay
 * captures that owner before client threads can issue Xlib requests. */
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

typedef struct PutBackEvent {
    Display *display;
    XEvent event;
    struct PutBackEvent *next;
} PutBackEvent;

static PutBackEvent *putBackEvents = NULL;

static void updateWindowRenderTargets(Display *display);
int convertEvent(Display *display,
                 SDL_Event *sdlEvent,
                 XEvent *xEvent,
                 Bool freeInternalEvents);

/* Pipe write/read are best-effort wake-up signals; the authoritative
 * "event ready" tracker is GET_DISPLAY(display)->qlen plus the
 * putBackEvents linked list. Both file descriptors are non-blocking
 * after initEventPipe so a full or empty pipe never stalls the event
 * loop on a slow client or a phantom qlen. */
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

static Bool getRectIntersection(const SDL_Rect *rect1,
                                const SDL_Rect *rect2,
                                SDL_Rect *rectOut)
{
    if (!SDL_IntersectRect(rect1, rect2, rectOut)) {
        return False;
    }
    rectOut->x -= rect2->x;
    rectOut->y -= rect2->y;
    return True;
}

void postExposeEvent(Display *display,
                     Window window,
                     const SDL_Rect *damagedAreaList,
                     size_t numAreas)
{
    size_t i = numAreas, j;
    while (i-- > 0) {
        postEvent(display, window, Expose, &damagedAreaList[i], i);
    }
    SDL_Rect *childDamagedAreaList = malloc(sizeof(SDL_Rect) * numAreas);
    if (!childDamagedAreaList)
        return;
    Window *children = GET_CHILDREN(window);
    for (i = 0; i < GET_WINDOW_STRUCT(window)->children.length; i++) {
        if (!IS_INPUT_ONLY(children[i]) &&
            GET_WINDOW_STRUCT(children[i])->mapState == Mapped) {
            WindowStruct *childWindowStruct = GET_WINDOW_STRUCT(children[i]);
            SDL_Rect childWindowRect = {
                childWindowStruct->x,
                childWindowStruct->y,
                childWindowStruct->w,
                childWindowStruct->h,
            };
            size_t numChildAreas = 0;
            for (j = 0; j < numAreas; j++) {
                if (getRectIntersection(&damagedAreaList[j], &childWindowRect,
                                        &childDamagedAreaList[numChildAreas])) {
                    numChildAreas++;
                }
            }
            if (numChildAreas > 0) {
                postExposeEvent(display, children[i], childDamagedAreaList,
                                numChildAreas);
            }
        }
    }
    free(childDamagedAreaList);
}

void postExposeEventsForMappedChildren(Display *display,
                                       Window window,
                                       const SDL_Rect *damagedAreaList,
                                       size_t numAreas)
{
    if (numAreas == 0 || !damagedAreaList || !IS_TYPE(window, WINDOW))
        return;

    SDL_Rect *childDamagedAreaList = malloc(sizeof(SDL_Rect) * numAreas);
    if (!childDamagedAreaList)
        return;

    Window *children = GET_CHILDREN(window);
    for (size_t i = 0; i < GET_WINDOW_STRUCT(window)->children.length; i++) {
        if (IS_INPUT_ONLY(children[i]) ||
            GET_WINDOW_STRUCT(children[i])->mapState != Mapped) {
            continue;
        }

        WindowStruct *childWindowStruct = GET_WINDOW_STRUCT(children[i]);
        SDL_Rect childWindowRect = {
            childWindowStruct->x,
            childWindowStruct->y,
            childWindowStruct->w,
            childWindowStruct->h,
        };
        size_t numChildAreas = 0;
        for (size_t j = 0; j < numAreas; j++) {
            if (getRectIntersection(&damagedAreaList[j], &childWindowRect,
                                    &childDamagedAreaList[numChildAreas])) {
                numChildAreas++;
            }
        }
        if (numChildAreas > 0) {
            postExposeEvent(display, children[i], childDamagedAreaList,
                            numChildAreas);
        }
    }
    free(childDamagedAreaList);
}

static int onSdlEvent(void *userdata, SDL_Event *event)
{
    if (SCREEN_WINDOW == None || !IS_TYPE(SCREEN_WINDOW, WINDOW))
        return 0;

    switch (event->type) {
        //        case SDL_QUIT:
    case SDL_WINDOWEVENT:
        if (!GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlWindow ||
            event->window.windowID ==
                SDL_GetWindowID(GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlWindow)) {
            return 0;
        }
        /* Fall through to enqueue non-screen window events. */
    default:
        ENQUEUE_EVENT_IN_PIPE((Display *) userdata);
    }
    return 1;
}

static Bool getEventQueueLength(int *qlen)
{
    /* Motif expose/configure bursts and resize-driven repaints routinely
     * push the queue past 25 entries in a single SDL_PumpEvents tick;
     * callers used the returned length both to size a follow-up GETEVENT
     * drain and to feed XEventsQueued, so undercounting throttled Xt's
     * main loop and dropped events behind a quiet 25-event ceiling.
     *
     * The cap is just a peek buffer; size it large enough for realistic
     * bursts but bounded so a runaway SDL queue can't blow the stack. */
    enum { PEEK_CAP = 256 };
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

static Bool enqueuePutBackEvent(Display *display, const XEvent *event)
{
    PutBackEvent *node = malloc(sizeof(PutBackEvent));
    if (!node) {
        handleOutOfMemory(0, display, 0, 0);
        return False;
    }
    node->display = display;
    memcpy(&node->event, event, sizeof(XEvent));
    node->next = putBackEvents;
    putBackEvents = node;
    /* Match the SDL filter's pipe-byte accounting so that clients
     * select()ing on ConnectionNumber actually wake up for put-backs. */
    ENQUEUE_EVENT_IN_PIPE(display);
    return True;
}

static Bool appendPutBackEvent(Display *display, const XEvent *event)
{
    PutBackEvent *node = malloc(sizeof(PutBackEvent));
    if (!node) {
        handleOutOfMemory(0, display, 0, 0);
        return False;
    }
    node->display = display;
    memcpy(&node->event, event, sizeof(XEvent));
    node->next = NULL;

    PutBackEvent **link = &putBackEvents;
    while (*link)
        link = &(*link)->next;
    *link = node;
    ENQUEUE_EVENT_IN_PIPE(display);
    return True;
}

static Bool popPutBackEvent(Display *display, XEvent *event)
{
    PutBackEvent **link = &putBackEvents;
    while (*link) {
        PutBackEvent *node = *link;
        if (node->display == display) {
            *link = node->next;
            memcpy(event, &node->event, sizeof(XEvent));
            free(node);
            /* Match the pipe byte enqueuePutBackEvent wrote so the wake-up
             * accounting stays consistent. */
            READ_EVENT_IN_PIPE(display);
            return True;
        }
        link = &node->next;
    }
    return False;
}

static int countPutBackEvents(Display *display)
{
    int count = 0;
    for (PutBackEvent *node = putBackEvents; node; node = node->next) {
        if (node->display == display)
            count++;
    }
    return count;
}

static int drainSdlEventsToPutBack(Display *display)
{
    int qlen = 0;
    getEventQueueLength(&qlen);
    if (qlen <= 0) {
        int putBackCount = countPutBackEvents(display);
        if (displayEventQueueLength(display) > putBackCount) {
            resetEventWakeups(display, putBackCount);
        }
        return putBackCount;
    }

    SDL_Event *events = calloc((size_t) qlen, sizeof(SDL_Event));
    if (!events) {
        handleOutOfMemory(0, display, 0, 0);
        return countPutBackEvents(display);
    }

    qlen = SDL_PeepEvents(events, qlen, SDL_GETEVENT, SDL_FIRSTEVENT,
                          SDL_LASTEVENT);
    if (qlen < 0) {
        LOG("Unable to read event queue: %s\n", SDL_GetError());
        free(events);
        return countPutBackEvents(display);
    }

    for (int i = 0; i < qlen; i++) {
        READ_EVENT_IN_PIPE(display);
    }
    for (int i = 0; i < qlen; i++) {
        XEvent converted;
        if (convertEvent(display, &events[i], &converted, True) == 0)
            appendPutBackEvent(display, &converted);
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
        int translatedX = event->x_root;
        int translatedY = event->y_root;
        Window child = None;
        XTranslateCoordinates(display, SCREEN_WINDOW, window, event->x_root,
                              event->y_root, &translatedX, &translatedY,
                              &child);
        event->x = translatedX;
        event->y = translatedY;
        event->subwindow = child;
    }
    event->mode = mode;
    event->detail = detail;
    event->same_screen = True;
    event->focus = getKeyboardFocus() == window;
    event->state = state;
}

static void translateRootPointToWindow(Display *display,
                                       Window root,
                                       Window window,
                                       int rootX,
                                       int rootY,
                                       int *windowX,
                                       int *windowY)
{
    *windowX = rootX;
    *windowY = rootY;
    if (window != None && window != root && IS_TYPE(window, WINDOW)) {
        Window child = None;
        XTranslateCoordinates(display, root, window, rootX, rootY, windowX,
                              windowY, &child);
    }
}

static void translateSdlPointToRoot(Display *display,
                                    Window sdlWindow,
                                    int localX,
                                    int localY,
                                    int *rootX,
                                    int *rootY)
{
    *rootX = localX;
    *rootY = localY;
    if (sdlWindow != None && sdlWindow != SCREEN_WINDOW &&
        IS_TYPE(sdlWindow, WINDOW)) {
        Window child = None;
        XTranslateCoordinates(display, sdlWindow, SCREEN_WINDOW, localX, localY,
                              rootX, rootY, &child);
    }
}

static Bool windowSelectsAny(Window window, long mask)
{
    return IS_TYPE(window, WINDOW) &&
           (GET_WINDOW_STRUCT(window)->eventMask & mask) != 0;
}

static Window directChildContainingPoint(Window window, int x, int y)
{
    if (!IS_TYPE(window, WINDOW))
        return None;

    Window *children = GET_CHILDREN(window);
    for (size_t i = GET_WINDOW_STRUCT(window)->children.length; i > 0; i--) {
        Window child = children[i - 1];
        int childX, childY, childW, childH;
        GET_WINDOW_POS(child, childX, childY);
        GET_WINDOW_DIMS(child, childW, childH);
        if (x >= childX && x < childX + childW && y >= childY &&
            y < childY + childH) {
            return child;
        }
    }
    return None;
}

static Window selectPointerEventWindow(Display *display,
                                       Window root,
                                       int rootX,
                                       int rootY,
                                       long mask,
                                       Window *subwindowReturn,
                                       int *eventXReturn,
                                       int *eventYReturn)
{
    Window deepest = getContainingWindow(root, rootX, rootY);
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
        *subwindowReturn = directChildContainingPoint(
            eventWindow, *eventXReturn, *eventYReturn);
    return eventWindow;
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
    PutBackEvent **link = &putBackEvents;
    while (*link) {
        PutBackEvent *node = *link;
        if (node->display == display &&
            predicate(w, type, mask, &node->event)) {
            *link = node->next;
            memcpy(event, &node->event, sizeof(XEvent));
            free(node);
            READ_EVENT_IN_PIPE(display);
            return True;
        }
        link = &node->next;
    }
    return False;
}

static Bool removeMatchingPutBackIfEvent(Display *display,
                                         XEvent *event,
                                         Bool (*predicate)(Display *,
                                                           XEvent *,
                                                           char *),
                                         char *arg)
{
    PutBackEvent **link = &putBackEvents;
    while (*link) {
        PutBackEvent *node = *link;
        if (node->display == display && predicate(display, &node->event, arg)) {
            *link = node->next;
            memcpy(event, &node->event, sizeof(XEvent));
            free(node);
            READ_EVENT_IN_PIPE(display);
            return True;
        }
        link = &node->next;
    }
    return False;
}

void discardQueuedEventsForWindow(Display *display, Window window)
{
    PutBackEvent **link = &putBackEvents;
    while (*link) {
        PutBackEvent *node = *link;
        if (node->display == display && node->event.xany.window == window) {
            *link = node->next;
            free(node);
            READ_EVENT_IN_PIPE(display);
            continue;
        }
        link = &node->next;
    }

    int qlen = 0;
    getEventQueueLength(&qlen);
    if (qlen <= 0)
        return;

    SDL_Event *events = calloc((size_t) qlen, sizeof(SDL_Event));
    if (!events) {
        handleOutOfMemory(0, display, 0, 0);
        return;
    }
    qlen = SDL_PeepEvents(events, qlen, SDL_GETEVENT, SDL_FIRSTEVENT,
                          SDL_LASTEVENT);
    if (qlen < 0) {
        LOG("Unable to read event queue: %s\n", SDL_GetError());
        free(events);
        return;
    }

    for (int i = 0; i < qlen; i++) {
        READ_EVENT_IN_PIPE(display);
    }
    for (int i = 0; i < qlen; i++) {
        XEvent converted;
        if (convertEvent(display, &events[i], &converted, True) != 0)
            continue;
        if (converted.xany.window == window)
            continue;
        appendPutBackEvent(display, &converted);
    }
    int remainingSdlEvents = 0;
    if (getEventQueueLength(&remainingSdlEvents)) {
        int desiredQlen = countPutBackEvents(display) + remainingSdlEvents;
        if (displayEventQueueLength(display) != desiredQlen)
            resetEventWakeups(display, desiredQlen);
    }
    free(events);
}

/* SDL_SetEventFilter only stores one (callback, userdata) pair, so we
 * keep a registry of every Display that has installed onSdlEvent. The
 * active slot always points at the most recent open; closing the
 * active slot hands it off to whichever display is still open
 * (most-recently-opened wins) instead of nulling the filter and
 * stranding the remaining display(s). The registry uses the project's
 * dynamic Array helper so multi-display flows are not silently capped
 * at an arbitrary number of opens.
 */
static Array trackedDisplays = {NULL, 0, 0};

static void trackDisplay(Display *display)
{
    if (findInArray(&trackedDisplays, display) >= 0) {
        return;
    }
    if (!insertArray(&trackedDisplays, display)) {
        LOG("Failed to track display %p for SDL event-filter handoff\n",
            (void *) display);
    }
}

static void untrackDisplay(Display *display)
{
    ssize_t index = findInArray(&trackedDisplays, display);
    if (index >= 0) {
        removeArray(&trackedDisplays, (size_t) index, True);
    }
    if (trackedDisplays.length == 0) {
        freeArray(&trackedDisplays);
    }
}

int initEventPipe(Display *display)
{
    captureMainEventThreadIfUnset();
    lastEventSerial = 1;
    if (!eventQueueLengthLock) {
        eventQueueLengthLock = SDL_CreateMutex();
        if (!eventQueueLengthLock) {
            LOG("Could not create the event queue lock: %s", SDL_GetError());
            return -1;
        }
    }
    int flags;
    if (pipe(eventFds) == -1) {
        LOG("Could not create the event pipe: %s", strerror(errno));
        return -1;
    }
    /* Real X11 clients select() / poll() on the connection FD then call
     * XNextEvent in a non-blocking mode. The pipe is just a wake-up
     * signal; reads happen inside XNextEvent itself, so make the FD
     * non-blocking so a desynced qlen does not stall the whole event
     * loop on a missing byte. The original spelling used F_SETFD
     * (FD_CLOEXEC) instead of F_SETFL (O_NONBLOCK), silently leaving the
     * FD in blocking mode. */
    flags = fcntl(READ_EVENT_FD, F_GETFL);
    fcntl(READ_EVENT_FD, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(WRITE_EVENT_FD, F_GETFL);
    fcntl(WRITE_EVENT_FD, F_SETFL, flags | O_NONBLOCK);
    FILE *event_write = fdopen(WRITE_EVENT_FD, "w");
    if (!event_write) {
        LOG("Could not create the write input of the event pipe: %s",
            strerror(errno));
        return -1;
    }
    FILE *event_read = fdopen(READ_EVENT_FD, "r");
    if (!event_read) {
        LOG("Could not create the read output of the event pipe: %s",
            strerror(errno));
        return -1;
    }
    int qlen;
    getEventQueueLength(&qlen);
    LOG("Events in queue = %d at initialisation\n", qlen);
    for (; qlen > 0; qlen--) {
        ENQUEUE_EVENT_IN_PIPE(display);
    }
    SDL_SetEventFilter(onSdlEvent, display);
    trackDisplay(display);
    return READ_EVENT_FD;
}

/* Detach the SDL event filter before XCloseDisplay frees the Display.
 * SDL_SetEventFilter only tracks the latest registration, so closing a
 * display whose pointer is still installed as filter userdata leaves a
 * dangling reference; the next SDL_PumpEvents would deliver an event
 * through onSdlEvent and dereference freed memory (AddressSanitizer
 * caught this as a heap-use-after-free during test_extensions when a
 * nested XOpenDisplay/XCloseDisplay pair handed the slot off and the
 * outer display then pushed an event). When other displays remain
 * open we hand the slot to one of them so their event pipe keeps
 * draining; only the final close nulls the filter.
 */
void closeEventPipe(Display *display)
{
    untrackDisplay(display);
    SDL_EventFilter currentFilter = NULL;
    void *currentUserdata = NULL;
    SDL_GetEventFilter(&currentFilter, &currentUserdata);
    if (currentFilter != onSdlEvent || currentUserdata != display) {
        if (trackedDisplays.length == 0 && eventQueueLengthLock) {
            SDL_DestroyMutex(eventQueueLengthLock);
            eventQueueLengthLock = NULL;
        }
        return;
    }
    if (trackedDisplays.length > 0) {
        SDL_SetEventFilter(onSdlEvent,
                           trackedDisplays.array[trackedDisplays.length - 1]);
    } else {
        SDL_SetEventFilter(NULL, NULL);
        if (eventQueueLengthLock) {
            SDL_DestroyMutex(eventQueueLengthLock);
            eventQueueLengthLock = NULL;
        }
    }
}

unsigned int convertModifierState(Uint16 mod)
{
    unsigned int state = 0;
    if (HAS_VALUE(mod, KMOD_SHIFT)) {
        state |= ShiftMask;
    }
    if (HAS_VALUE(mod, KMOD_CTRL)) {
        state |= ControlMask;
    }
    if (HAS_VALUE(mod, KMOD_CAPS)) {
        state |= LockMask;
    }
    /* X11 convention: Mod1Mask is Alt, Mod2Mask is NumLock. Motif
     * accelerators (Alt-F for File, Alt-E for Edit, etc.) test for
     * Mod1; without this, accelerators silently never fire. The previous
     * mapping put NumLock on Mod1, leaving Alt unreachable. */
    if (HAS_VALUE(mod, KMOD_ALT)) {
        state |= Mod1Mask;
    }
    if (HAS_VALUE(mod, KMOD_NUM)) {
        state |= Mod2Mask;
    }
    return state;
}

static unsigned int pointerButtonState = 0;
static Window activePointerWindow = None;

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

int convertEvent(Display *display,
                 SDL_Event *sdlEvent,
                 XEvent *xEvent,
                 Bool freeInternalEvents)
{
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
        xEvent->xkey.root = getWindowFromId(sdlEvent->key.windowID);
        xEvent->xkey.state = convertModifierState(sdlEvent->key.keysym.mod);
        xEvent->xkey.keycode = (unsigned int) sdlEvent->key.keysym.sym & 0xFF;
        /* Route priority for key events:
         *  1. Active XGrabKeyboard (modal dialogs like Motif's Help popup)
         *  2. Passive XGrabKey match (Motif accelerators)
         *  3. The keyboard-focus window
         *  4. The event's root window as a last resort.
         * Items 1 and 2 are independent of focus; item 3 is the normal
         * path. */
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
        xEvent->xkey.time = sdlEvent->key.timestamp;
        SDL_GetMouseState(&xEvent->xkey.x, &xEvent->xkey.y);
        xEvent->xkey.x_root =
            xEvent->xkey.x;  // Because root and window are the same.
        xEvent->xkey.y_root = xEvent->xkey.y;
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
         * convertEvent below; let SDL handle it. */
    case SDL_MOUSEBUTTONUP:
        if (sdlEvent->type == SDL_MOUSEBUTTONUP) {
            LOG("SDL_MOUSEBUTTONUP\n");
            type = ButtonRelease;
        }
        FILL_STANDARD_VALUES(xbutton);
        Window sdlButtonWindow = getWindowFromId(sdlEvent->button.windowID);
        xEvent->xbutton.root = SCREEN_WINDOW;
        xEvent->xbutton.time = sdlEvent->button.timestamp;
        translateSdlPointToRoot(display, sdlButtonWindow, sdlEvent->button.x,
                                sdlEvent->button.y, &xEvent->xbutton.x_root,
                                &xEvent->xbutton.y_root);
        xEvent->xbutton.button = convertSdlMouseButton(sdlEvent->button.button);
        unsigned int buttonState = buttonMaskForXButton(xEvent->xbutton.button);
        unsigned int previousButtonState = pointerButtonState;
        xEvent->xbutton.state =
            convertModifierState(SDL_GetModState()) | previousButtonState;
        if (freeInternalEvents) {
            if (type == ButtonPress)
                pointerButtonState |= buttonState;
            else
                pointerButtonState &= ~buttonState;
        }
        long buttonMask =
            type == ButtonPress ? ButtonPressMask : ButtonReleaseMask;
        if (type == ButtonRelease && activePointerWindow != None &&
            windowSelectsAny(activePointerWindow, buttonMask)) {
            eventWindow = activePointerWindow;
            translateRootPointToWindow(display, xEvent->xbutton.root,
                                       eventWindow, xEvent->xbutton.x_root,
                                       xEvent->xbutton.y_root,
                                       &xEvent->xbutton.x, &xEvent->xbutton.y);
            xEvent->xbutton.subwindow = directChildContainingPoint(
                eventWindow, xEvent->xbutton.x, xEvent->xbutton.y);
        } else {
            eventWindow = selectPointerEventWindow(
                display, xEvent->xbutton.root, xEvent->xbutton.x_root,
                xEvent->xbutton.y_root, buttonMask, &xEvent->xbutton.subwindow,
                &xEvent->xbutton.x, &xEvent->xbutton.y);
        }
        if (freeInternalEvents && type == ButtonPress &&
            previousButtonState == 0 && eventWindow != None) {
            activePointerWindow = eventWindow;
        }
        if (freeInternalEvents && pointerButtonState == 0)
            activePointerWindow = None;
        if (eventWindow == None)
            return -1;
        xEvent->xbutton.window = eventWindow;
        xEvent->xbutton.same_screen = True;
        break;
    case SDL_MOUSEMOTION:
        LOG("SDL_MOUSEMOTION\n");
        type = MotionNotify;
        FILL_STANDARD_VALUES(xmotion);
        Window sdlMotionWindow = getWindowFromId(sdlEvent->motion.windowID);
        xEvent->xmotion.root = SCREEN_WINDOW;
        xEvent->xmotion.time = sdlEvent->motion.timestamp;
        translateSdlPointToRoot(display, sdlMotionWindow, sdlEvent->motion.x,
                                sdlEvent->motion.y, &xEvent->xmotion.x_root,
                                &xEvent->xmotion.y_root);
        long motionMask = PointerMotionMask | ButtonMotionMask |
                          Button1MotionMask | Button2MotionMask |
                          Button3MotionMask | Button4MotionMask |
                          Button5MotionMask | PointerMotionHintMask;
        if (activePointerWindow != None &&
            windowSelectsAny(activePointerWindow, motionMask)) {
            eventWindow = activePointerWindow;
            translateRootPointToWindow(display, xEvent->xmotion.root,
                                       eventWindow, xEvent->xmotion.x_root,
                                       xEvent->xmotion.y_root,
                                       &xEvent->xmotion.x, &xEvent->xmotion.y);
            xEvent->xmotion.subwindow = directChildContainingPoint(
                eventWindow, xEvent->xmotion.x, xEvent->xmotion.y);
        } else {
            eventWindow = selectPointerEventWindow(
                display, xEvent->xmotion.root, xEvent->xmotion.x_root,
                xEvent->xmotion.y_root, motionMask, &xEvent->xmotion.subwindow,
                &xEvent->xmotion.x, &xEvent->xmotion.y);
        }
        if (eventWindow == None)
            return -1;
        xEvent->xmotion.window = eventWindow;
        xEvent->xmotion.state =
            convertModifierState(SDL_GetModState()) | pointerButtonState;
        xEvent->xmotion.is_hint =
            HAS_EVENT_MASK(eventWindow, PointerMotionHintMask) ? NotifyHint
                                                               : NotifyNormal;
        xEvent->xmotion.same_screen = True;
        break;
    case SDL_WINDOWEVENT:
        eventWindow = getWindowFromId(sdlEvent->window.windowID);
        switch (sdlEvent->window.event) {
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
                GET_WINDOW_STRUCT(eventWindow)->x = sdlEvent->window.data1;
                GET_WINDOW_STRUCT(eventWindow)->y = sdlEvent->window.data2;
            }
            /* fall through: MOVED, RESIZED and SIZE_CHANGED share a single
             * ConfigureNotify dispatch below; the unified handler keys off
             * sdlEvent->window.event to decide which fields to query.
             */
        case SDL_WINDOWEVENT_RESIZED:
            if (sdlEvent->window.event == SDL_WINDOWEVENT_RESIZED) {
                LOG("Window %d resized to %dx%d\n", sdlEvent->window.windowID,
                    sdlEvent->window.data1, sdlEvent->window.data2);
            }
            /* fall through */
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            if (sdlEvent->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                LOG("Window %d size changed to %dx%d\n",
                    sdlEvent->window.windowID, sdlEvent->window.data1,
                    sdlEvent->window.data2);
            }
            type = ConfigureNotify;
            FILL_STANDARD_VALUES(xconfigure);
            xEvent->xconfigure.event = eventWindow;
            xEvent->xconfigure.window = xEvent->xconfigure.event;
            if (sdlEvent->window.event == SDL_WINDOWEVENT_MOVED) {
                xEvent->xconfigure.x = sdlEvent->window.data1;
                xEvent->xconfigure.y = sdlEvent->window.data2;
            } else {
                SDL_GetWindowPosition(
                    SDL_GetWindowFromID(sdlEvent->window.windowID),
                    &xEvent->xconfigure.x, &xEvent->xconfigure.y);
            }
            if (sdlEvent->window.event == SDL_WINDOWEVENT_RESIZED ||
                sdlEvent->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                xEvent->xconfigure.width = sdlEvent->window.data1;
                xEvent->xconfigure.height = sdlEvent->window.data2;
                /* After unification, every mapped top-level window draws
                 * into a per-window backing texture on the SCREEN
                 * renderer. SDL_GetWindowSize now reports the new size,
                 * so resize the backing texture to match before the
                 * next present reads at the new dimensions. */
                if (eventWindow != None) {
                    GET_WINDOW_STRUCT(eventWindow)->w =
                        (unsigned int) sdlEvent->window.data1;
                    GET_WINDOW_STRUCT(eventWindow)->h =
                        (unsigned int) sdlEvent->window.data2;
                    resizeWindowTexture(eventWindow);
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
            if (sdlEvent->window.event == SDL_WINDOWEVENT_LEAVE) {
                LOG("Mouse left window %d\n", sdlEvent->window.windowID);
                type = LeaveNotify;
            }
            fillCrossingEvent(display, &xEvent->xcrossing, eventWindow, type,
                              NotifyNormal, NotifyAncestor,
                              convertModifierState(SDL_GetModState()));
            xEvent->xcrossing.time = sdlEvent->window.timestamp;
            break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            LOG("Window %d gained keyboard focus\n", sdlEvent->window.windowID);
            type = FocusIn;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            if (sdlEvent->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
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
            static Atom WM_PROTOCOLS = None;
            static Atom WM_DELETE_WINDOW = None;
            if (WM_PROTOCOLS == None) {
                WM_PROTOCOLS = internalInternAtom("WM_PROTOCOLS");
                WM_DELETE_WINDOW = internalInternAtom("WM_DELETE_WINDOW");
            }
            WindowProperty *windowProperty =
                findProperty(&GET_WINDOW_STRUCT(eventWindow)->properties,
                             WM_PROTOCOLS, NULL);
            Bool clientHandlesDelete = False;
            if (windowProperty && windowProperty->type == XA_ATOM) {
                for (size_t i = 0; i < windowProperty->dataLength; i++) {
                    if (((Atom *) windowProperty->data)[i] ==
                        WM_DELETE_WINDOW) {
                        postEvent(display, eventWindow, ClientMessage, 32,
                                  WM_PROTOCOLS, WM_DELETE_WINDOW,
                                  (Time) sdlEvent->window.timestamp);
                        clientHandlesDelete = True;
                        break;
                    }
                }
            }
            if (!clientHandlesDelete) {
                /* Real X11 kills the client connection here. Route through
                 * the IO error hook so a client that installed one can
                 * intercept; otherwise it terminates. */
                triggerIOError(display);
            }
            return -1;
            break;
        default:
            LOG("Window %d got unknown event %d\n", sdlEvent->window.windowID,
                sdlEvent->window.event);
            return -1;
        }
        break;
    case SDL_QUIT: /**< User-requested quit */
        LOG("SDL_QUIT\n");
        /* Cmd+Q / SIGTERM / dock-quit: emulate a fatal IO error so any
         * client-installed XIOErrorHandler runs first. */
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
    case SDL_SYSWMEVENT: /**< System specific event */
        LOG("SDL_SYSWMEVENT\n");
        return -1;
    case SDL_TEXTEDITING: /**< Keyboard text editing (composition) */
        LOG("SDL_TEXTEDITING\n");
        return -1;
    case SDL_TEXTINPUT: /**< Keyboard text input */
        LOG("SDL_TEXTINPUT\n");
        return -1;
    case SDL_MOUSEWHEEL: /**< Mouse wheel motion */
        LOG("SDL_MOUSEWHEEL\n");
        {
            int wy = sdlEvent->wheel.y;
            int wx = sdlEvent->wheel.x;
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
            SDL_GetMouseState(&mx, &my);
            xEvent->xbutton.time = sdlEvent->wheel.timestamp;
            translateSdlPointToRoot(display, sdlWheelWindow, mx, my,
                                    &xEvent->xbutton.x_root,
                                    &xEvent->xbutton.y_root);
            eventWindow = selectPointerEventWindow(
                display, xEvent->xbutton.root, xEvent->xbutton.x_root,
                xEvent->xbutton.y_root, ButtonPressMask,
                &xEvent->xbutton.subwindow, &xEvent->xbutton.x,
                &xEvent->xbutton.y);
            if (eventWindow == None)
                return -1;
            xEvent->xbutton.window = eventWindow;
            xEvent->xbutton.state =
                convertModifierState(SDL_GetModState()) | pointerButtonState;
            xEvent->xbutton.button = wheelButton;
            xEvent->xbutton.same_screen = True;
            break;
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
        if (sdlEvent->type == SDL_FINGERUP) {
            type = ButtonRelease;
        }
        FILL_STANDARD_VALUES(xbutton);
        eventWindow = SCREEN_WINDOW;
        xEvent->xbutton.root = eventWindow;
        xEvent->xbutton.window =
            xEvent->xbutton.root;  // The event window is always the SDL Window.
        xEvent->xbutton.subwindow = None;
        xEvent->xbutton.time = sdlEvent->tfinger.timestamp;
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
    case SDL_DOLLARGESTURE:
        LOG("SDL_DOLLARGESTURE\n");
        return -1;
    case SDL_DOLLARRECORD:
        LOG("SDL_DOLLARRECORD\n");
        return -1;
    case SDL_MULTIGESTURE:
        LOG("SDL_MULTIGESTURE\n");
        return -1;
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
                     * struct -- otherwise this memcpy reads past the end.
                     * See src/xshm.c XShmPutImage for the only current caller.
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
                return 0;
            }
        }
        return -1;
    }
    FILL_STANDARD_VALUES(xany);
    xEvent->xany.window = eventWindow;
    xEvent->type = type;
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
        /* Real X11 implicitly flushes the request queue when the client
         * blocks on input. Mirror that here so accumulated drawing
         * reaches the screen before we go to sleep. */
        drawWindowDataToScreen();
        if (SDL_WaitEvent(&event) != 1) {
            LOG("SDL_WaitEvent failed: %s, retrying...\n", SDL_GetError());
            fflush(stderr);
            continue;
        }
        /* Drain the wake-up byte the SDL filter wrote for this event so
         * a follow-up select(ConnectionNumber) reflects the real queue
         * depth. With the non-blocking pipe, a stale qlen no longer
         * forces XNextEvent to fabricate an Expose. */
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
        /* Do NOT fabricate a fake Expose: a wrong event type confuses
         * Motif's translation tables and Xt's event dispatcher far
         * worse than a brief spin in this loop. Wait for the next real
         * event instead. */
    }
}

Bool enqueueEvent(Display *display, Window eventWindow, void *event)
{
    static Uint32 sendEventType = (Uint32) -1;
    if (sendEventType == ((Uint32) -1)) {
        sendEventType = SDL_RegisterEvents(1);
    }
    if (sendEventType != ((Uint32) -1)) {
        SDL_Event sdlEvent;
        SDL_zero(sdlEvent);
        sdlEvent.type = sendEventType;
        sdlEvent.user.code = INTERNAL_EVENT_CODE;
        sdlEvent.user.data1 = event;
        sdlEvent.user.data2 = (void *) eventWindow;
        LOG("Enqueuing event\n");
        SDL_PushEvent(&sdlEvent);
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
    if (event_mask == NoEventMask) {
        return True;
    }
    return IS_TYPE(window, WINDOW) &&
           (GET_WINDOW_STRUCT(window)->eventMask & event_mask) != 0;
}

Status XSendEvent(Display *display,
                  Window window,
                  Bool propagate,
                  long event_mask,
                  XEvent *event_send)
{
    // https://tronche.com/gui/x/xlib/event-handling/XSendEvent.html
    SET_X_SERVER_REQUEST(display, X_SendEvent);
    TYPE_CHECK(window, WINDOW, display, 0);
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
    if (sendEventType == ((Uint32) -1)) {
        sendEventType = SDL_RegisterEvents(1);
    }
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
    if (mode == QueuedAlready) {
        return displayEventQueueLength(display);
    }
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
    /* Real X11 batches drawing on the server and flushes here. We accumulate
     * primitives in the renderer's back buffer and only present on flush so
     * partial frames don't reach the screen. */
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
    if (GET_WINDOW_STRUCT(window)->mapState != Mapped) {
        return VisibilityFullyObscured;
    }
    Window parent = GET_PARENT(window);
    if (parent == None) {
        return VisibilityUnobscured;
    }
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
 * StructureNotifyMask) cannot share one allocation -- the second dispatch
 * would double-free. enqueueAlias clones src, patches the per-event "event"
 * field to identify the recipient, and enqueues the copy. */
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

/* Fan-out helper for DestroyNotify, ConfigureNotify, MapNotify, UnmapNotify
 * and GravityNotify: each can be reported to the parent (SubstructureNotify)
 * and to the window itself (StructureNotify).
 *
 *   FANOUT_FAIL     -- enqueue failed; `event` already freed, caller breaks.
 *   FANOUT_CONSUMED -- delivered to parent only; caller must NOT set eventData
 *                      (ownership transferred to the queue).
 *   FANOUT_KEEP     -- `event` still owned by caller; it must store it in
 *                      eventData so the self delivery happens. The "event"
 *                      field has been patched to `self`. */
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
        XExposeEvent *event = malloc(sizeof(XExposeEvent));
        if (!event)
            break;
        event->type = eventId;
        event->send_event = False;
        event->display = display;
        event->window = eventWindow;
        event->x = exposeRect->x;
        event->y = exposeRect->y;
        event->width = exposeRect->w;
        event->height = exposeRect->h;
        event->count = va_arg(args, size_t);
        eventData = event;
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
        if (HAS_VALUE(event->value_mask, CWX)) {
            event->x = windowChanges->x;
        }
        if (HAS_VALUE(event->value_mask, CWY)) {
            event->y = windowChanges->y;
        }
        GET_WINDOW_DIMS(eventWindow, event->width, event->height);
        if (HAS_VALUE(event->value_mask, CWWidth)) {
            event->width = windowChanges->width;
        }
        if (HAS_VALUE(event->value_mask, CWHeight)) {
            event->height = windowChanges->height;
        }
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
        /* Deliver clones to every recipient except the last; the last keeps
         * the original allocation via eventData. */
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
        /* calloc instead of malloc: the data union has five long slots
         * and the spec lets clients consult any of them. malloc left
         * data.l[1..4] full of heap bytes, which fed Xt's WM_DELETE_WINDOW
         * handler garbage in the timestamp slot. send_event = True
         * matches what real X servers stamp on synthesized ICCCM
         * messages, so Motif's compare against e.send_event in
         * WM_PROTOCOLS callbacks now sees the expected value. */
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
    case GraphicsExpose:
        //            memcpy(&xEvent->xgraphicsexpose, allocEvent,
        //            sizeof(XGraphicsExposeEvent)); break;
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
    /* enqueueEvent only owns the heap event once it actually pushes onto
     * the SDL queue. If SDL_RegisterEvents has never succeeded the call
     * fails without taking ownership, so free here to avoid a leak. */
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
    if (displayEventQueueLength(display) == 0) {
        pumpEventsSafe();
    }
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
        qlen = SDL_PeepEvents(tmp, qlen, SDL_GETEVENT, SDL_FIRSTEVENT,
                              SDL_LASTEVENT);
        if (qlen < 0) {
            LOG("Unable to read event queue: %s\n", SDL_GetError());
            free(tmp);
            UnlockDisplay(display);
            return False;
        }
        for (int i = 0; i < qlen; i++) {
            READ_EVENT_IN_PIPE(display);
        }
        LOG("FOUND %d events in SDL queue\n", qlen);
        Bool foundMatch = False;
        for (int i = 0; i < qlen; i++) {
            XEvent convertedEvent;
            if (convertEvent(display, &tmp[i], &convertedEvent, True) != 0) {
                LOG("i = %d, SDL event_type = %d skipped\n", i, tmp[i].type);
                continue;
            }
            LOG("i = %d, SDL event_type = %d, X event_type = %d\n", i,
                tmp[i].type, convertedEvent.type);
            if (!foundMatch && predicate(w, type, mask, &convertedEvent)) {
                memcpy(event, &convertedEvent, sizeof(XEvent));
                foundMatch = True;
            } else if (!appendPutBackEvent(display, &convertedEvent)) {
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
    if (displayEventQueueLength(display) == 0) {
        pumpEventsSafe();
    }
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
        qlen = SDL_PeepEvents(tmp, qlen, SDL_GETEVENT, SDL_FIRSTEVENT,
                              SDL_LASTEVENT);
        if (qlen < 0) {
            LOG("Unable to read event queue: %s\n", SDL_GetError());
            free(tmp);
            UnlockDisplay(display);
            return False;
        }
        for (int i = 0; i < qlen; i++) {
            READ_EVENT_IN_PIPE(display);
        }
        Bool foundMatch = False;
        for (int i = 0; i < qlen; i++) {
            XEvent convertedEvent;
            if (convertEvent(display, &tmp[i], &convertedEvent, True) != 0)
                continue;
            if (!foundMatch && predicate(display, &convertedEvent, arg)) {
                memcpy(event, &convertedEvent, sizeof(XEvent));
                foundMatch = True;
            } else if (!appendPutBackEvent(display, &convertedEvent)) {
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
