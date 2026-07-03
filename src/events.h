#ifndef _EVENTS_H_
#define _EVENTS_H_

#include <X11/Xlib.h>
#include "sdl-compat.h"
#include "input.h"

#define SEND_EVENT_CODE 1
#define INTERNAL_EVENT_CODE 2
/* Snapshot the replay target window's backing surface to a path. Carried on
 * SDL_USEREVENT.user.data1 (path, allocated by replay thread, freed by the
 * main-thread handler). Used by the in-process smoke-snapshot path to bypass
 * macOS screencapture, which deactivates NSApp briefly and stalls the SDL event
 * pump just long enough that synthetic input goes undelivered.
 */
#define SNAPSHOT_EVENT_CODE 3
/* Resize the replay target window. user.data1 carries width, user.data2 carries
 * height, each as intptr_t. Runs synchronously on the main thread
 * (SDL_SetWindowSize is main-thread only on macOS). Used by the replay engine's
 * resize command so the smoke can exercise ViolaWWW's ConfigureNotify reflow
 * without depending on the OS window manager.
 */
#define RESIZE_EVENT_CODE 4
/* Flush pending top-level backing textures to their SDL windows. Posted by the
 * drawing layer after X drawing operations that happen away from a blocking
 * XNextEvent/XFlush path, such as Motif popup menu expose redraws.
 */
#define PRESENT_EVENT_CODE 5
/* Set keyboard focus to the X subwindow under a replay-target-local point.
 * user.data1 carries the target-local x, user.data2 the y, each as intptr_t.
 * Runs synchronously on the main thread so the window-tree walk and
 * XSetInputFocus mutation never race with main-thread mutators. Used by the
 * replay engine's focus-at verb.
 */
#define FOCUS_AT_EVENT_CODE 6

#define HAS_EVENT_MASK(window, mask) \
    ((GET_WINDOW_STRUCT(window)->eventMask & mask) == mask)

int initEventPipe(Display *display);
void closeEventPipe(Display *display);
void captureMainEventThreadIfUnset(void);
void releaseMainEventThread(void);
/* Refresh SDL's cached input state (mouse buttons, position, modifiers) when
 * the caller is on the main event thread. XQueryPointer relies on this so a
 * client busy-polling pointer state, like xwpe's button-release wait, observes
 * a real release instead of spinning forever on a stale pressed bitmask.
 */
void pumpEventsSafe(void);
void wakeEventPipeForExternalEvent(Display *display);
unsigned int convertModifierState(Uint16 mod);
Bool postEvent(Display *display, Window eventWindow, unsigned int eventId, ...);
/* Post a WM_DELETE_WINDOW ClientMessage if the window opted into the protocol
 * via WM_PROTOCOLS; returns True when posted. Shared by the SDL window-close
 * path (convertEvent) and the EWMH _NET_CLOSE_WINDOW path (events-ewmh.c) so
 * the two cannot diverge.
 */
Bool postWmDeleteIfHandled(Display *display, Window window, Time time);
Bool postReparentUnmapNotify(Display *display,
                             Window eventWindow,
                             Window oldParent);
Bool enqueueEvent(Display *display, Window eventWindow, void *event);
void discardQueuedEventsForWindow(Display *display, Window window);
Bool postCrossingEvent(Display *display,
                       Window window,
                       int type,
                       int mode,
                       int detail,
                       unsigned int state);
Bool postFocusEvent(Display *display, Window window, int type, int detail);

void postFocusChange(Display *display,
                     FocusKind oldKind,
                     Window oldFocus,
                     FocusKind newKind,
                     Window newFocus);
void postExposeEvent(Display *display,
                     Window window,
                     const SDL_Rect *damagedAreaList,
                     size_t numAreas);
void postExposeEventsForMappedChildren(Display *display,
                                       Window window,
                                       const SDL_Rect *damagedAreaList,
                                       size_t numAreas);
void postSyntheticWindowResize(Display *display,
                               Window eventWindow,
                               int width,
                               int height);
void clearActivePointerWindow(void);
void clearPointerStateForWindow(Window window);
void releaseButtonGrabsForWindow(Window window);

#endif /* _EVENTS_H_ */
