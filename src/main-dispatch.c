/*
 * Run work on the main event thread from a client worker thread.
 *
 * macOS requires all SDL window and event-pump calls on the thread that created
 * the windows (the first XOpenDisplay caller). An Xlib entry point invoked from
 * a background client thread that touches SDL would crash there. snapshot.c
 * already solved this for its three fixed round-trips (snapshot/resize/focus)
 * with a hand-written envelope, a shared condvar, and a generation counter to
 * drop stale events. This module generalizes the pattern to an arbitrary thunk
 * so any entry point can hop to the main thread with one call.
 *
 * The generation counter is gone: snapshot serializes every request through one
 * global cond, so a timed-out request can leave a stale event that must be
 * filtered by generation. Here each call carries its own mutex/cond on the
 * calling worker's stack and blocks until the main thread runs it, so there is
 * no shared slot, no staleness, and no contention between concurrent workers.
 */

#include <pthread.h>

#include "sdl-compat.h"

#include "display.h"
#include "events.h"
#include "main-dispatch.h"
#include "util.h"

/* Registered SDL user-event type carrying MAIN_DISPATCH_EVENT_CODE. SDL atomic
 * so the convertEvent dispatch path can read it without a lock, matching
 * snapshot.c. -1 (all bits set) means "not yet registered".
 */
static SDL_atomic_t mainDispatchEventType = {(int) (Uint32) -1};

Bool mainDispatchOwnsEventType(Uint32 eventType)
{
    Uint32 registered = (Uint32) SDL_AtomicGet(&mainDispatchEventType);
    return registered != (Uint32) -1 && eventType == registered;
}

/* One round-trip command. Lives on the calling worker's stack; the worker
 * blocks in runOnMainThread until done is set, so the pointer the main thread
 * receives stays valid for the whole exchange.
 */
typedef struct MainDispatchCmd {
    void (*fn)(void *);
    void *arg;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int done;
    int lockHandoff;
} MainDispatchCmd;

/* Lazily register the shared user-event type. Two first-callers may each call
 * SDL_RegisterEvents; the CAS keeps the first id and the loser's is abandoned
 * (SDL never reclaims a registered type, but leaking one id is harmless).
 * Returns (Uint32)-1 only when SDL_RegisterEvents itself fails.
 */
static Uint32 ensureMainDispatchEventType(void)
{
    Uint32 registered = (Uint32) SDL_AtomicGet(&mainDispatchEventType);
    if (registered != (Uint32) -1)
        return registered;
    Uint32 newType = SDL_RegisterEvents(1);
    if (newType == (Uint32) -1) {
        LOG("main-dispatch: SDL_RegisterEvents failed: %s\n", SDL_GetError());
        return (Uint32) -1;
    }
    if (!SDL_AtomicCAS(&mainDispatchEventType, (int) (Uint32) -1,
                       (int) newType))
        return (Uint32) SDL_AtomicGet(&mainDispatchEventType);
    return newType;
}

void runOnMainThread(void (*fn)(void *), void *arg)
{
    if (!fn)
        return;
    if (libx11CompatOnMainEventThread()) {
        fn(arg);
        return;
    }
    Uint32 eventType = ensureMainDispatchEventType();
    if (eventType == (Uint32) -1) {
        /* No channel to reach the main thread. fn touches SDL, which is exactly
         * what must not run off the main thread, so drop it rather than run it
         * here and crash on macOS. Registration only fails under SDL teardown
         * or event-type exhaustion, neither of which a live workload hits.
         */
        LOG("main-dispatch: no event type, dropping off-thread call\n");
        return;
    }

    MainDispatchCmd cmd = {
        fn, arg, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 0,
    };

    /* Shed any display lock this worker holds (an app-level XLockDisplay, or an
     * internal LockDisplay from an outer Xlib call) BEFORE publishing the
     * event. Two reasons: (1) the main thread needs that same process-global
     * lock to drain the queue and run the thunk, so holding it while blocked
     * would deadlock; (2) ordering -- if the event were pushed first, a
     * main-thread dispatch path that does not hold LockDisplay could run the
     * thunk while this worker still owns the monitor, defeating the handoff's
     * mutual exclusion. Publishing only after the shed guarantees the thunk
     * runs against a recorded handoff. Reacquire on every path after this point
     * (including push failure). A no-op for a worker holding no lock.
     *
     * The shed does not weaken XLockDisplay atomicity: the release/reacquire is
     * folded into the display lock's own monitor (src/display.c), which marks a
     * handoff pending so that while this worker is blocked only the main thread
     * (to run the thunk) and this worker (to reacquire) may take the freed
     * lock. A third thread that calls XLockDisplay waits until the handoff
     * completes, so it cannot interleave this worker's critical section. That
     * gate is part of ownership rather than a bolt-on, so there is no
     * check-then-act window.
     */
    int lockDepth = displayLockReleaseForHandoff();
    cmd.lockHandoff = lockDepth > 0;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = eventType;
    ev.user.code = MAIN_DISPATCH_EVENT_CODE;
    ev.user.data1 = &cmd;
    if (SDL_PushEvent(&ev) <= 0) {
        /* Never run the SDL-touching thunk on this worker; restore the shed
         * lock and drop the call.
         */
        LOG("main-dispatch: SDL_PushEvent failed (%s), dropping off-thread "
            "call\n",
            SDL_GetError());
        displayLockReacquire(lockDepth);
        return;
    }
    wakeEventPipeForExternalEvent(NULL);

    /* Block until the main thread drains the queue. The pipe wake plus the
     * ~33ms pump-wake timer guarantee it drains; add a timeout with heap-owned
     * commands if a stuck-main-thread hang ever appears.
     */
    pthread_mutex_lock(&cmd.mutex);
    while (!cmd.done)
        pthread_cond_wait(&cmd.cond, &cmd.mutex);
    pthread_mutex_unlock(&cmd.mutex);
    displayLockReacquire(lockDepth);
    pthread_mutex_destroy(&cmd.mutex);
    pthread_cond_destroy(&cmd.cond);
}

/* Mark the command complete and wake the blocked worker. Every path that takes
 * a MAIN_DISPATCH event off the SDL queue must either successfully re-queue it
 * or call this, or the worker waits on cmd.cond forever (there is no timeout).
 */
static void completeCmd(MainDispatchCmd *cmd)
{
    pthread_mutex_lock(&cmd->mutex);
    cmd->done = 1;
    pthread_cond_signal(&cmd->cond);
    pthread_mutex_unlock(&cmd->mutex);
}

void mainDispatchHandleEvent(SDL_Event *event)
{
    MainDispatchCmd *cmd = (MainDispatchCmd *) event->user.data1;
    if (!cmd->lockHandoff && displayLockHandoffPending()) {
        /* Defer a non-handoff-owner thunk until the pending handoff clears, so
         * it does not interleave the owner's critical section. If the re-queue
         * fails (SDL queue full/disabled), drop the op rather than hang the
         * worker. */
        if (SDL_PushEvent(event) <= 0) {
            LOG("main-dispatch: SDL_PushEvent failed while deferring (%s)\n",
                SDL_GetError());
            completeCmd(cmd);
        }
        return;
    }
    cmd->fn(cmd->arg);
    completeCmd(cmd);
}

/* Complete a MAIN_DISPATCH command WITHOUT running its thunk, to unblock the
 * worker when the event cannot be delivered to the main thread (a non-main
 * drainer's re-queue failed). The op is dropped: the worker's rc keeps its
 * initialized value, a failure, matching the fail-closed policy of the other
 * push-failure paths. Exposed so convertEvent can drop without reaching into
 * the private MainDispatchCmd layout.
 */
void mainDispatchDropCmd(SDL_Event *event)
{
    completeCmd((MainDispatchCmd *) event->user.data1);
}
