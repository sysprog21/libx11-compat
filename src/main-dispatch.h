#ifndef _MAIN_DISPATCH_H_
#define _MAIN_DISPATCH_H_

#include <X11/Xlib.h>
#include "sdl-compat.h"

/* Run fn(arg) on the main event thread: the one that owns the SDL windows and
 * runs the event pump, captured by the first XOpenDisplay. macOS requires all
 * window and event-pump work on that thread, so an Xlib entry point that
 * touches SDL from a client worker thread must funnel the work here.
 *
 * When the caller already is the main thread, or no main thread has been
 * captured yet (still single-threaded), fn runs inline with no queueing. Only
 * from a different thread is fn posted to the main thread via an SDL_USEREVENT;
 * the caller then blocks until the main thread has run it. Synchronous only: a
 * fire-and-forget variant is omitted until a call site needs one.
 *
 * Precondition: the main thread must be pumping events (the normal event-loop
 * state). If it is not, an off-thread caller blocks until it resumes; there is
 * no timeout. If the SDL event channel cannot be reached at all (registration
 * or push fails, only under SDL teardown), the call is dropped and logged
 * rather than run on the wrong thread, since fn touches SDL and would crash
 * off-main.
 */
void runOnMainThread(void (*fn)(void *), void *arg);

/* SDL3 delegation seam. SDL3 ships SDL_RunOnMainThread() (3.2.0) with the same
 * inline-on-main / queue-for-main semantics as this function, and
 * SDL_IsMainThread() matching libx11CompatOnMainEventThread (src/events.h).
 * This is deliberately the single funnel so a future SDL3 backend can delegate
 * its body to SDL_RunOnMainThread under SDL_VERSION_ATLEAST(3,2,0) with no
 * call-site changes. Do NOT delegate yet: SDL_RunOnMainThread does not wake a
 * main thread blocked in SDL_WaitEvent (libsdl-org/SDL#12390), and its
 * wait_complete=true path can deadlock. The event-pipe wake this implementation
 * uses (wakeEventPipeForExternalEvent) is the documented workaround, so it
 * stays load-bearing until #12390 is fixed in the linked SDL3.
 */

/* convertEvent dispatch for the registered MAIN_DISPATCH_EVENT_CODE user event.
 * mainDispatchOwnsEventType gates the branch to this module's event type;
 * mainDispatchHandleEvent runs the queued thunk and wakes the blocked worker.
 */
Bool mainDispatchOwnsEventType(Uint32 eventType);
void mainDispatchHandleEvent(SDL_Event *event);
/* Complete a queued MAIN_DISPATCH command without running its thunk, dropping
 * the op. convertEvent calls this when a non-main drainer cannot re-queue the
 * event, so the blocked worker unblocks (with a failure rc) instead of hanging
 * forever.
 */
void mainDispatchDropCmd(SDL_Event *event);
/* Run any commands parked while a display-lock handoff was pending, now that it
 * may have cleared. Called from the main-thread SDL drain so a handoff clearing
 * with no new MAIN_DISPATCH event still flushes them. No-op off the main
 * thread.
 */
void mainDispatchRunDeferred(void);

#endif /* _MAIN_DISPATCH_H_ */
