#ifndef _MAC_LIVE_RESIZE_H_
#define _MAC_LIVE_RESIZE_H_

/* macOS blocks the application's event loop inside a modal Cocoa run loop for
 * the whole duration of a live window-resize drag. SDL delivers no resize or
 * expose events during that window, so nothing repaints and the compositor
 * upscales the last framebuffer to the growing window - visibly blurring text.
 *
 * Start registers for the AppKit live-resize notifications
 * (NSWindowWillStartLiveResize / NSWindowDidEndLiveResize) with the default
 * notification center. A CFRunLoopObserver on the main run loop's common modes
 * (which include the event-tracking mode used by the resize drag) is armed only
 * for the duration of an actual drag: the WillStart handler adds it and the
 * DidEnd handler removes it, so it never ticks in steady state (no stuck resize
 * cursor, no wasted presents after the mouse is released). While armed the
 * observer calls libx11CompatPresentDuringLiveResize on each iteration of the
 * modal loop, re-fitting the backing to the live window size and presenting a
 * sharp 1:1 frame; the DidEnd handler fires one final present. Stop
 * unregisters the notifications and removes the observer. On non-Apple
 * platforms both start/stop are no-ops.
 */
void libx11CompatStartLiveResizeObserver(void);
void libx11CompatStopLiveResizeObserver(void);

/* Implemented in src/events.c. Re-fits every mapped top-level to its live SDL
 * window size and presents. Safe to call repeatedly and only acts on the main
 * SDL-owning thread. Returns 1 if the live window size changed this tick (a
 * reflow ran), 0 otherwise.
 */
int libx11CompatPresentDuringLiveResize(void);

/* Implemented in src/events.c. Like libx11CompatPresentDuringLiveResize, but
 * when forceReflow is nonzero it runs the (expensive) client reflow immediately
 * for the current window size instead of waiting for the size to settle. Used
 * for the final present at drag end (NSWindowDidEndLiveResize / idle-disarm) so
 * the window lands on fully reflowed content. Main-thread-only.
 */
int libx11CompatPresentDuringLiveResizeEx(int forceReflow);

/* Implemented in src/events.c. Clears the deferred-reflow coalesce state (last
 * seen size + settle counter) so a fresh drag starts clean. Called from the
 * observer arm path. Main-thread-only.
 */
void libx11CompatResetLiveResizeCoalesce(void);

/* Implemented in src/events.c. Emits one line to the runtime live-resize trace
 * (LIBX11_COMPAT_LIVE_RESIZE_TRACE) tagged as an observer event; no-op when the
 * trace is off. Used to mark run-loop-observer boundaries (tick in/out) that
 * live outside events.c. Main-thread-only.
 */
void libx11CompatLiveResizeTrace(const char *tag);

/* Client-registered live-resize reflow hook.
 *
 * On macOS the client's own event loop is blocked inside the modal Cocoa resize
 * loop for the whole drag, so it never processes the ConfigureNotify and never
 * reflows; the grown area would otherwise show a guessed margin fill. A client
 * (e.g. xwpe) whose reflow+repaint is re-entrant and does not itself call
 * XNextEvent can register a callback here. The run-loop observer path
 * (libx11CompatPresentDuringLiveResize) then invokes it on each drag tick with
 * the live physical-pixel window size, so the client recomputes its own grid
 * and paints real content into the backing before the frame is presented.
 *
 * The callback runs on the main SDL-owning thread, synchronously, from the
 * observer path. It may issue Xlib drawing calls back into the shim. Registered
 * once at client startup; the setter simply stores the pointer.
 *
 * The declaration is intentionally callable from a client that also builds
 * against a real libX11 (where this symbol is absent) via a weak reference.
 */
typedef void (*LibX11CompatLiveResizeReflowFn)(int newPixelWidth,
                                               int newPixelHeight);
void libx11CompatRegisterLiveResizeReflow(LibX11CompatLiveResizeReflowFn fn);

/* True once a reflow callback has been registered. The present path uses this
 * to skip its guessed margin fill (the client now paints the whole new area).
 */
int libx11CompatHasLiveResizeReflow(void);

/* Invoke the registered reflow callback with the given live pixel size, if any.
 * Exposed so unit tests can exercise the dispatch seam without the observer or
 * a real SDL surface. No-op when nothing is registered.
 */
void libx11CompatInvokeLiveResizeReflow(int newPixelWidth, int newPixelHeight);

/* True while a live-resize present frame is unwinding on the main thread (i.e.
 * for the whole duration of libx11CompatPresentDuringLiveResize). Any Xlib call
 * re-entered from the observer/reflow callback (e.g. XCloseDisplay) uses this
 * to detect the in-present state and defer teardown that would be unsafe from
 * inside the CFRunLoop observer. Main-thread-only.
 */
int libx11CompatInLiveResizePresent(void);

/* Runs a live-resize-deferred XCloseDisplay, if one was requested re-entrantly
 * from the observer. No-op otherwise. Call once per observer tick, on the main
 * thread, AFTER the in-present flag is cleared.
 */
void libx11CompatRunDeferredDisplayClose(void);

/* One-time glitch-free live-resize configuration for SDL's own CAMetalLayer
 * (Phase 2B). Sets contentsGravity = kCAGravityTopLeft and contentsScale =
 * NSWindow.backingScaleFactor, leaving the layer's contents untouched so SDL
 * keeps presenting real frames via nextDrawable. During the modal resize drag
 * AppKit then pins the stale drawable top-left instead of stretching it (no
 * glyph distortion) while contentsScale keeps drawable pixels 1:1 on Retina.
 *
 * This replaces the earlier CGImage-upload approach, which fought the
 * CAMetalLayer's internal CAImageQueue (red tint) and broke the modal-loop exit
 * (stuck resize cursor). Because contents is never replaced there is no
 * drag-end cleanup and no black window; the setting is harmless in steady state
 * and idempotent, so the present path may call it on every live-resize frame.
 * nsWindow is the opaque NSWindow* from sdlCocoaWindowHandle. No-op on
 * non-Apple platforms or when the handle is not a Cocoa window.
 * Main-thread-only.
 */
void libx11CompatConfigureLiveResizeLayer(void *nsWindow);

#endif /* _MAC_LIVE_RESIZE_H_ */
