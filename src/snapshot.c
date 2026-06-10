/* In-process window snapshot for the smoke-test pipeline.
 *
 * Why this exists: macOS screencapture deactivates the frontmost
 * NSApp briefly while it grabs pixels, which stalls SDL's Cocoa event
 * pump just long enough that synthetic mouse/wheel events queued by
 * the in-process replay engine never get drained. The result is
 * screenshots that always show the pre-replay state (verified
 * empirically -- 8 wheel events pushed, 8 filtered through, 0
 * delivered to the X client). Capturing inside the target process,
 * directly from the SDL window surface, side-steps the entire
 * NSApplication life-cycle.
 *
 * Threading: the replay parser runs on a detached pthread, but SDL's
 * SDL_GetWindowSurface requires the thread that owns the renderer
 * (in libx11-compat that is the X-client thread that called
 * XOpenDisplay first). snapshotRequestAndWait() solves this by
 * pushing an SDL_USEREVENT with code SNAPSHOT_EVENT_CODE; the main
 * thread's convertEvent path detects that code and calls
 * snapshotHandleEvent(), which performs the surface read on the
 * correct thread and signals the waiter.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>
#include "drawing.h"
#include "events.h"
#include "replay-target.h"
#include "snapshot.h"
#include "window-internal.h"
#include "util.h"

/* Single-snapshot serialization. Two replay scripts cannot run in the
 * same process, and the replay parser issues snapshots sequentially,
 * so a single mutex + condvar suffices. */
static pthread_mutex_t snapshotMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t snapshotCond = PTHREAD_COND_INITIALIZER;
static int snapshotResult = 0;
static int snapshotDone = 0;
/* SDL requires user-event types to be registered via SDL_RegisterEvents
 * to be eligible for SDL_PushEvent + queue processing. Without
 * registration the event survives the push but never appears at the
 * other end of the queue (verified empirically -- raw SDL_USEREVENT
 * pushes silently dropped before convertEvent saw them). Register
 * lazily on first request so we share the per-process event-type
 * registry with enqueueEvent's INTERNAL_EVENT_CODE pump. */
static Uint32 snapshotEventType = (Uint32) -1;

Bool snapshotOwnsEventType(Uint32 eventType)
{
    return snapshotEventType != (Uint32) -1 && eventType == snapshotEventType;
}

int snapshotHandleEvent(const SDL_Event *event)
{
    char *path = (char *) event->user.data1;
    int rc = 0;
    Uint32 winId = replayTargetWindowId();
    SDL_Window *win = (winId != 0) ? SDL_GetWindowFromID(winId) : NULL;
    if (!win) {
        LOG("snapshot: no target window (winId=%u)\n", winId);
        rc = -1;
        goto signal;
    }
    drawWindowDataToScreen();
    SDL_Surface *surface = SDL_GetWindowSurface(win);
    if (!surface) {
        LOG("snapshot: SDL_GetWindowSurface failed: %s\n", SDL_GetError());
        rc = -3;
        goto signal;
    }
    if (SDL_SaveBMP(surface, path) != 0) {
        LOG("snapshot: SDL_SaveBMP(%s) failed: %s\n", path, SDL_GetError());
        rc = -3;
        goto signal;
    }
    LOG("snapshot: wrote %s (%dx%d)\n", path, surface->w, surface->h);
signal:
    pthread_mutex_lock(&snapshotMutex);
    snapshotResult = rc;
    snapshotDone = 1;
    pthread_cond_signal(&snapshotCond);
    pthread_mutex_unlock(&snapshotMutex);
    free(path);
    return rc;
}

/* Resize round-trip uses the same global cond+mutex as snapshot since
 * the replay engine issues both serially. A single SDL_RegisterEvents
 * type id is shared with the snapshot path (the dispatch in
 * convertEvent keys off user.code, not event type). */
int snapshotHandleResizeEvent(Display *display, const SDL_Event *event)
{
    int width = (int) (intptr_t) event->user.data1;
    int height = (int) (intptr_t) event->user.data2;
    int rc = 0;
    Uint32 winId = replayTargetWindowId();
    SDL_Window *win = (winId != 0) ? SDL_GetWindowFromID(winId) : NULL;
    if (!win) {
        LOG("resize: no target window (winId=%u)\n", winId);
        rc = -1;
    } else {
        Window xwin = getWindowFromId(winId);
        if (xwin == None) {
            LOG("resize: no X window for SDL target %u\n", winId);
            rc = -1;
            goto signal;
        }
        SDL_SetWindowSize(win, width, height);
        LOG("resize: target window %u -> %dx%d\n", winId, width, height);
        postSyntheticWindowResize(display, xwin, width, height);
    }
signal:
    pthread_mutex_lock(&snapshotMutex);
    snapshotResult = rc;
    snapshotDone = 1;
    pthread_cond_signal(&snapshotCond);
    pthread_mutex_unlock(&snapshotMutex);
    return rc;
}

int snapshotRequestResizeAndWait(int width, int height)
{
    if (width <= 0 || height <= 0)
        return -1;
    pthread_mutex_lock(&snapshotMutex);
    if (snapshotEventType == (Uint32) -1) {
        snapshotEventType = SDL_RegisterEvents(1);
        if (snapshotEventType == (Uint32) -1) {
            pthread_mutex_unlock(&snapshotMutex);
            LOG("resize: SDL_RegisterEvents failed: %s\n", SDL_GetError());
            return -2;
        }
    }
    snapshotDone = 0;
    snapshotResult = 0;
    pthread_mutex_unlock(&snapshotMutex);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = snapshotEventType;
    ev.user.code = RESIZE_EVENT_CODE;
    ev.user.data1 = (void *) (intptr_t) width;
    ev.user.data2 = (void *) (intptr_t) height;
    int pushRc = SDL_PushEvent(&ev);
    LOG("resize: request %dx%d pushRc=%d\n", width, height, pushRc);
    if (pushRc <= 0)
        return -2;
    wakeEventPipeForExternalEvent(NULL);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    /* Resize triggers a full reflow in Motif/Xt clients (ViolaWWW
     * re-renders every visible paragraph), so the main thread may not
     * drain SDL events for noticeably longer than a plain snapshot.
     * 15s is generous enough to cover that without masking a real
     * stuck-thread hang. */
    deadline.tv_sec += 15;
    pthread_mutex_lock(&snapshotMutex);
    int wait_rc = 0;
    while (!snapshotDone && wait_rc == 0)
        wait_rc =
            pthread_cond_timedwait(&snapshotCond, &snapshotMutex, &deadline);
    int rc = snapshotDone ? snapshotResult : -4;
    pthread_mutex_unlock(&snapshotMutex);
    LOG("resize: wait done rc=%d wait_rc=%d snapshotDone=%d\n", rc, wait_rc,
        snapshotDone);
    return rc;
}

int snapshotRequestAndWait(const char *path)
{
    if (!path || !*path)
        return -1;
    char *copy = strdup(path);
    if (!copy)
        return -1;
    pthread_mutex_lock(&snapshotMutex);
    /* Lazy registration inside the lock: two concurrent first callers
     * would otherwise each get their own type id from SDL, leaking one
     * id and pushing events that no one is registered to handle
     * (gemini-flagged race). */
    if (snapshotEventType == (Uint32) -1) {
        snapshotEventType = SDL_RegisterEvents(1);
        if (snapshotEventType == (Uint32) -1) {
            pthread_mutex_unlock(&snapshotMutex);
            LOG("snapshot: SDL_RegisterEvents failed: %s\n", SDL_GetError());
            free(copy);
            return -2;
        }
    }
    snapshotDone = 0;
    snapshotResult = 0;
    pthread_mutex_unlock(&snapshotMutex);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = snapshotEventType;
    ev.user.code = SNAPSHOT_EVENT_CODE;
    ev.user.data1 = copy;
    int pushRc = SDL_PushEvent(&ev);
    LOG("snapshot: request path=%s type=%u pushRc=%d\n", path,
        snapshotEventType, pushRc);
    if (pushRc <= 0) {
        free(copy);
        return -2;
    }
    wakeEventPipeForExternalEvent(NULL);
    /* Bound the wait so a stuck main thread doesn't hang the replay
     * indefinitely. The save itself is single-ms for a typical window,
     * but the main thread may not drain the SDL_USEREVENT for a while
     * if Motif is mid-reflow (e.g. right after a resize, or during
     * the initial-expose burst of a freshly mapped XmMainWindow). 15s
     * covers that without masking a genuine stuck-thread hang. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 15;
    pthread_mutex_lock(&snapshotMutex);
    int wait_rc = 0;
    while (!snapshotDone && wait_rc == 0)
        wait_rc =
            pthread_cond_timedwait(&snapshotCond, &snapshotMutex, &deadline);
    int rc = snapshotDone ? snapshotResult : -4;
    pthread_mutex_unlock(&snapshotMutex);
    LOG("snapshot: wait done rc=%d wait_rc=%d snapshotDone=%d\n", rc, wait_rc,
        snapshotDone);
    return rc;
}
