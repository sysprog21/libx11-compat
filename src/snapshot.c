/* In-process window snapshot for the smoke-test pipeline.
 *
 * Why this exists: macOS screencapture deactivates the frontmost NSApp briefly
 * while it grabs pixels, which stalls SDL's Cocoa event pump just long enough
 * that synthetic mouse/wheel events queued by the in-process replay engine
 * never get drained. The result is screenshots that always show the pre-replay
 * state (verified empirically -- 8 wheel events pushed, 8 filtered through, 0
 * delivered to the X client). Capturing inside the target process, directly
 * from the SDL window surface, side-steps the entire NSApplication life-cycle.
 *
 * Threading: the replay parser runs on a detached pthread, but SDL's
 * SDL_GetWindowSurface requires the thread that owns the renderer (in
 * libx11-compat that is the X-client thread that called XOpenDisplay first).
 * snapshotRequestAndWait() solves this by pushing an SDL_USEREVENT with code
 * SNAPSHOT_EVENT_CODE; the main thread's convertEvent path detects that code
 * and calls snapshotHandleEvent(), which performs the surface read on the
 * correct thread and signals the waiter.
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdl-compat.h"

#include "drawing.h"
#include "events.h"
#include "replay-target.h"
#include "snapshot.h"
#include "window-internal.h"
#include "util.h"

/* Single-snapshot serialization. Two replay scripts cannot run in the same
 * process, and the replay parser issues snapshots sequentially, so a single
 * mutex + condvar suffices.
 */
static pthread_mutex_t snapshotMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t snapshotCond = PTHREAD_COND_INITIALIZER;
static int snapshotResult = 0;
static int snapshotDone = 0;

/* SDL requires user-event types to be registered via SDL_RegisterEvents to be
 * eligible for SDL_PushEvent + queue processing. Without registration the event
 * survives the push but never appears at the other end of the queue (verified
 * empirically: raw SDL_USEREVENT pushes silently dropped before convertEvent
 * saw them). Register lazily on first request so snapshot delivery shares the
 * per-process event-type registry with enqueueEvent's INTERNAL_EVENT_CODE pump.
 *
 * SDL_atomic-backed so snapshotOwnsEventType can read it from the SDL filter
 * dispatch path without holding snapshotMutex. Plain int reads raced the writer
 * that sets it under the mutex on first use; SDL atomics give an
 * acquire/release barrier matching the fix already applied to state-snapshot.
 */
static SDL_atomic_t snapshotEventType = {(int) (Uint32) -1};

/* Generation counter for in-flight requests. Each call to a request function
 * increments it inside snapshotMutex; the value is packed into the envelope on
 * the SDL_USEREVENT and re-checked inside the handler. A request that times out
 * leaves its event stranded in the queue; when the main thread eventually
 * drains it, the generation mismatch makes the handler skip signaling so it
 * does not satisfy a fresh waiter with the stale result.
 */
static uint64_t snapshotGeneration = 0;

/* Envelope passed via SDL_USEREVENT.user.data1. Carries the generation plus the
 * per-code payload (path for snapshot, dimensions for resize, coords for
 * focus-at). Heap-allocated in the request function and freed in the handler,
 * including the stale-event path so timed-out requests do not leak.
 */
typedef struct SnapshotEnvelope {
    uint64_t generation;
    int intA;
    int intB;
    char *path;
} SnapshotEnvelope;

Bool snapshotOwnsEventType(Uint32 eventType)
{
    Uint32 registered = (Uint32) SDL_AtomicGet(&snapshotEventType);
    return registered != (Uint32) -1 && eventType == registered;
}

/* Lazy-register the shared SDL user-event type, arm the done/result pair, and
 * advance the generation counter for a fresh round-trip. Performed under
 * snapshotMutex so two concurrent first-callers cannot each grab their own type
 * id or skew the generation.
 *
 * Returns the registered SDL event type plus the generation via the out params
 * on success; the function result is False when SDL_RegisterEvents failed and
 * the caller should bail with -2. The tag string is used only for the failure
 * LOG so each call site keeps its own diagnostic prefix.
 */
static Bool prepareSnapshotRoundTrip(const char *tag,
                                     Uint32 *eventTypeOut,
                                     uint64_t *generationOut)
{
    pthread_mutex_lock(&snapshotMutex);
    Uint32 registered = (Uint32) SDL_AtomicGet(&snapshotEventType);
    if (registered == (Uint32) -1) {
        Uint32 newType = SDL_RegisterEvents(1);
        if (newType == (Uint32) -1) {
            pthread_mutex_unlock(&snapshotMutex);
            LOG("%s: SDL_RegisterEvents failed: %s\n", tag, SDL_GetError());
            return False;
        }
        SDL_AtomicSet(&snapshotEventType, (int) newType);
        registered = newType;
    }
    snapshotDone = 0;
    snapshotResult = 0;
    snapshotGeneration++;
    *eventTypeOut = registered;
    *generationOut = snapshotGeneration;
    pthread_mutex_unlock(&snapshotMutex);
    return True;
}

/* Roll back the generation when SDL_PushEvent fails so a subsequent request
 * does not skip past the stale-event window.
 */
static void rollbackSnapshotGeneration(uint64_t generation)
{
    pthread_mutex_lock(&snapshotMutex);
    if (snapshotGeneration == generation)
        snapshotGeneration--;
    pthread_mutex_unlock(&snapshotMutex);
}

/* Test the incoming envelope's generation against the current counter under
 * snapshotMutex.
 *
 * Returns True when the event is stale and the handler should free the envelope
 * and skip signaling. The handler still owns the envelope in both branches.
 */
static Bool snapshotEventIsStale(uint64_t eventGeneration)
{
    pthread_mutex_lock(&snapshotMutex);
    Bool stale = eventGeneration != snapshotGeneration;
    pthread_mutex_unlock(&snapshotMutex);
    return stale;
}

/* Store the main-thread handler's result and wake the worker waiting in
 * waitSnapshotResult. Identical tail across all three handlers; the original
 * gotos remain for clarity at the call sites. The generation is rechecked
 * inside the mutex so a fresh request that bumped the counter while the handler
 * was executing has the stale result quietly dropped.
 */
static void signalSnapshotResult(uint64_t eventGeneration, int rc)
{
    pthread_mutex_lock(&snapshotMutex);
    if (eventGeneration == snapshotGeneration) {
        snapshotResult = rc;
        snapshotDone = 1;
        pthread_cond_signal(&snapshotCond);
    }
    pthread_mutex_unlock(&snapshotMutex);
}

/* Block up to 15s for the main thread to finish the round-trip. 15s covers
 * Motif/Xt reflow bursts (resize, freshly mapped XmMainWindow, focus-driven
 * traversal) without masking a genuinely stuck main thread. waitRcOut, if
 * non-NULL, receives the underlying pthread_cond_timedwait return so callers
 * can distinguish timeout from spurious wake in their diagnostic LOG.
 */
static int waitSnapshotResult(int *waitRcOut)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 15;
    pthread_mutex_lock(&snapshotMutex);
    int waitRc = 0;
    while (!snapshotDone && waitRc == 0)
        waitRc =
            pthread_cond_timedwait(&snapshotCond, &snapshotMutex, &deadline);
    int rc = snapshotDone ? snapshotResult : -4;
    pthread_mutex_unlock(&snapshotMutex);
    if (waitRcOut)
        *waitRcOut = waitRc;
    return rc;
}

int snapshotHandleEvent(const SDL_Event *event)
{
    SnapshotEnvelope *env = (SnapshotEnvelope *) event->user.data1;
    if (snapshotEventIsStale(env->generation)) {
        LOG("snapshot: dropping stale event gen=%llu cur=%llu\n",
            (unsigned long long) env->generation,
            (unsigned long long) snapshotGeneration);
        free(env->path);
        free(env);
        return -5;
    }
    char *path = env->path;
    int rc = 0;
    SDL_Surface *ownedSurface = NULL;
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
        Window xwin = getWindowFromId(winId);
        if (xwin != None) {
            SDL_Renderer *renderer = getWindowRenderer(xwin);
            WindowStruct *windowStruct = GET_WINDOW_STRUCT(xwin);
            if (windowStruct) {
                SDL_Rect rect = {
                    .x = 0,
                    .y = 0,
                    .w = (int) windowStruct->w,
                    .h = (int) windowStruct->h,
                };
                surface = getRenderSurfaceRect(renderer, &rect);
            }
            ownedSurface = surface;
        }
        if (!surface) {
            LOG("snapshot: render-surface fallback failed\n");
            rc = -3;
            goto signal;
        }
    }

    /* SDL_SaveBMP writes incrementally to the open file, so a runner that polls
     * for this path can observe a non-empty but truncated BMP and fail to
     * decode it (PIL "image file is truncated"). Write to a temp file and
     * rename into place; rename is atomic within one filesystem, so the reader
     * sees either no file or the complete one.
     */
    size_t pathLen = strlen(path);
    if (pathLen > SIZE_MAX - sizeof(".tmp")) {
        LOG("snapshot: path too long for temp suffix\n");
        rc = -3;
        goto signal;
    }
    char *tmpPath = malloc(pathLen + sizeof(".tmp"));
    if (!tmpPath) {
        LOG("snapshot: out of memory for temp path\n");
        rc = -3;
        goto signal;
    }
    memcpy(tmpPath, path, pathLen);
    memcpy(tmpPath + pathLen, ".tmp", sizeof(".tmp"));
    if (SDL_SaveBMP(surface, tmpPath) != 0) {
        LOG("snapshot: SDL_SaveBMP(%s) failed: %s\n", tmpPath, SDL_GetError());
        free(tmpPath);
        rc = -3;
        goto signal;
    }
    if (rename(tmpPath, path) != 0) {
        LOG("snapshot: rename(%s -> %s) failed: %s\n", tmpPath, path,
            strerror(errno));
        free(tmpPath);
        rc = -3;
        goto signal;
    }
    free(tmpPath);
    LOG("snapshot: wrote %s (%dx%d)\n", path, surface->w, surface->h);
signal:
    if (ownedSurface)
        SDL_FreeSurface(ownedSurface);
    signalSnapshotResult(env->generation, rc);
    free(path);
    free(env);
    return rc;
}

/* Resize round-trip uses the same global cond+mutex as snapshot since the
 * replay engine issues both serially. A single SDL_RegisterEvents type id is
 * shared with the snapshot path (the dispatch in convertEvent keys off
 * user.code, not event type).
 */
int snapshotHandleResizeEvent(Display *display, const SDL_Event *event)
{
    SnapshotEnvelope *env = (SnapshotEnvelope *) event->user.data1;
    if (snapshotEventIsStale(env->generation)) {
        LOG("resize: dropping stale event gen=%llu cur=%llu\n",
            (unsigned long long) env->generation,
            (unsigned long long) snapshotGeneration);
        free(env);
        return -5;
    }
    int width = env->intA;
    int height = env->intB;
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
    signalSnapshotResult(env->generation, rc);
    free(env);
    return rc;
}

int snapshotRequestResizeAndWait(int width, int height)
{
    if (width <= 0 || height <= 0)
        return -1;
    SnapshotEnvelope *env = calloc(1, sizeof(*env));
    if (!env)
        return -1;
    env->intA = width;
    env->intB = height;
    Uint32 eventType = 0;
    uint64_t generation = 0;
    if (!prepareSnapshotRoundTrip("resize", &eventType, &generation)) {
        free(env);
        return -2;
    }
    env->generation = generation;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = eventType;
    ev.user.code = RESIZE_EVENT_CODE;
    ev.user.data1 = env;
    int pushRc = SDL_PushEvent(&ev);
    LOG("resize: request %dx%d pushRc=%d\n", width, height, pushRc);
    if (pushRc <= 0) {
        rollbackSnapshotGeneration(generation);
        free(env);
        return -2;
    }
    wakeEventPipeForExternalEvent(NULL);
    int waitRc = 0;
    int rc = waitSnapshotResult(&waitRc);
    LOG("resize: wait done rc=%d wait_rc=%d\n", rc, waitRc);
    return rc;
}

int snapshotHandleFocusAtEvent(Display *display, const SDL_Event *event)
{
    SnapshotEnvelope *env = (SnapshotEnvelope *) event->user.data1;
    if (snapshotEventIsStale(env->generation)) {
        LOG("focus-at: dropping stale event gen=%llu cur=%llu\n",
            (unsigned long long) env->generation,
            (unsigned long long) snapshotGeneration);
        free(env);
        return -5;
    }

    int x = env->intA, y = env->intB;
    int rc = 0;
    int rootX = 0, rootY = 0;
    if (!replayTargetTranslateLocal(x, y, &rootX, &rootY)) {
        LOG("focus-at: replayTargetTranslateLocal(%d, %d) failed\n", x, y);
        rc = -1;
        goto signal;
    }

    Window target = getContainingWindow(SCREEN_WINDOW, rootX, rootY);
    /* Reject SCREEN_WINDOW because getContainingWindow falls back to the
     * starting window when no child contains the point, which would shift X
     * focus to the synthetic root. Reject None for the obvious reason.
     */
    if (target == None || target == SCREEN_WINDOW) {
        LOG("focus-at: no real window at (%d, %d) -> (%d, %d)\n", x, y, rootX,
            rootY);
        rc = -1;
        goto signal;
    }
    XSetInputFocus(display, target, RevertToNone, CurrentTime);
    LOG("focus-at: focus -> 0x%lx at (%d, %d)\n", target, rootX, rootY);
signal:
    signalSnapshotResult(env->generation, rc);
    free(env);
    return rc;
}

int snapshotRequestFocusAtAndWait(int x, int y)
{
    SnapshotEnvelope *env = calloc(1, sizeof(*env));
    if (!env)
        return -1;

    env->intA = x, env->intB = y;
    Uint32 eventType = 0;
    uint64_t generation = 0;
    if (!prepareSnapshotRoundTrip("focus-at", &eventType, &generation)) {
        free(env);
        return -2;
    }
    env->generation = generation;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = eventType;
    ev.user.code = FOCUS_AT_EVENT_CODE;
    ev.user.data1 = env;
    int pushRc = SDL_PushEvent(&ev);
    LOG("focus-at: request (%d, %d) pushRc=%d\n", x, y, pushRc);
    if (pushRc <= 0) {
        rollbackSnapshotGeneration(generation);
        free(env);
        return -2;
    }
    wakeEventPipeForExternalEvent(NULL);
    int waitRc = 0;
    int rc = waitSnapshotResult(&waitRc);
    LOG("focus-at: wait done rc=%d wait_rc=%d\n", rc, waitRc);
    return rc;
}

int snapshotRequestAndWait(const char *path)
{
    if (!path || !*path)
        return -1;
    SnapshotEnvelope *env = calloc(1, sizeof(*env));
    if (!env)
        return -1;
    env->path = strdup(path);
    if (!env->path) {
        free(env);
        return -1;
    }
    Uint32 eventType = 0;
    uint64_t generation = 0;
    if (!prepareSnapshotRoundTrip("snapshot", &eventType, &generation)) {
        free(env->path);
        free(env);
        return -2;
    }
    env->generation = generation;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = eventType;
    ev.user.code = SNAPSHOT_EVENT_CODE;
    ev.user.data1 = env;
    int pushRc = SDL_PushEvent(&ev);
    LOG("snapshot: request path=%s type=%u pushRc=%d\n", path, eventType,
        pushRc);
    if (pushRc <= 0) {
        rollbackSnapshotGeneration(generation);
        free(env->path);
        free(env);
        return -2;
    }
    wakeEventPipeForExternalEvent(NULL);
    int waitRc = 0;
    int rc = waitSnapshotResult(&waitRc);
    LOG("snapshot: wait done rc=%d wait_rc=%d\n", rc, waitRc);
    return rc;
}
