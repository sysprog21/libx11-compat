#ifndef _LIBX11_COMPAT_SNAPSHOT_INTERNAL_H_
#define _LIBX11_COMPAT_SNAPSHOT_INTERNAL_H_

#include <X11/Xlib.h>
#include "sdl-compat.h"

/* Request a snapshot of the replay target SDL window's backing surface to
 * path (BMP, format chosen by SDL_SaveBMP). Blocks the calling thread
 * until the main thread finishes the save and signals completion, then
 * returns. Safe to call from any thread; the actual file I/O always
 * happens on the thread that runs the X event loop (the same thread that
 * owns the SDL renderer), satisfying SDL's main-thread requirement for
 * SDL_GetWindowSurface.
 *
 * Returns 0 on success, -1 if no target window is cached, -2 if the
 * SDL_Event push failed, -3 if the save itself failed. The reason the
 * call is synchronous (and not fire-and-forget): the smoke test runner
 * inspects the saved file immediately after replay completes, so the
 * BMP must exist by the time replayStartIfRequested's background thread
 * returns from runScript.
 */
int snapshotRequestAndWait(const char *path);

/* Handle a SNAPSHOT_EVENT_CODE SDL_USEREVENT on the main thread. Reads
 * the path from event->user.data1, saves the cached replay target's
 * window surface to it, frees the path string, and signals any waiter
 * in snapshotRequestAndWait. Returns 0 on success, non-zero on
 * snapshot failure (still signals the waiter so the call returns).
 */
int snapshotHandleEvent(const SDL_Event *event);

/* Resize the cached replay target window to (width, height). Blocks the
 * caller until SDL_SetWindowSize completes on the main thread, since
 * SDL_SetWindowSize is main-thread-only on macOS. Used by the replay
 * engine's resize command. Returns 0 on success, -1 if no target
 * window, -2 if the SDL_Event push failed.
 */
int snapshotRequestResizeAndWait(int width, int height);
Bool snapshotOwnsEventType(Uint32 eventType);

/* Handle a RESIZE_EVENT_CODE SDL_USEREVENT on the main thread.
 * Decodes width/height from data1/data2, calls SDL_SetWindowSize,
 * signals the waiter. Returns 0 on success.
 */
int snapshotHandleResizeEvent(Display *display, const SDL_Event *event);

/* Resolve the X subwindow containing the replay-target-local point (x, y) and
 * call XSetInputFocus on it. Blocks the caller until the lookup and focus
 * mutation complete on the main thread, since both the window tree walk
 * (getContainingWindow) and the focus state (keyboardFocus / postFocusChange)
 * are unguarded against the main-thread mutators that build them. Used by the
 * replay engine's focus-at verb. Returns 0 on success, -1 if the translation
 * or containment lookup fails, -2 if the SDL_Event push failed.
 */
int snapshotRequestFocusAtAndWait(int x, int y);

/* Handle a FOCUS_AT_EVENT_CODE SDL_USEREVENT on the main thread. Runs the
 * tree walk and the XSetInputFocus call, then signals the waiter. */
int snapshotHandleFocusAtEvent(Display *display, const SDL_Event *event);

#endif
