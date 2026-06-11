#ifndef _EVENTS_H_
#define _EVENTS_H_

#include <X11/Xlib.h>
#include <SDL2/SDL.h>

#define SEND_EVENT_CODE 1
#define INTERNAL_EVENT_CODE 2
/* Snapshot the replay target window's backing surface to a path. Carried on
 * SDL_USEREVENT.user.data1 (path, allocated by replay thread, freed by the
 * main-thread handler). Used by the in-process smoke-snapshot path to
 * bypass macOS screencapture, which deactivates NSApp briefly and stalls
 * the SDL event pump just long enough that synthetic input goes
 * undelivered.
 */
#define SNAPSHOT_EVENT_CODE 3
/* Resize the replay target window. user.data1 carries width, user.data2
 * carries height, each as intptr_t. Runs synchronously on the main thread
 * (SDL_SetWindowSize is main-thread only on macOS). Used by the replay
 * engine's resize command so the smoke can exercise ViolaWWW's
 * ConfigureNotify reflow without depending on the OS window manager.
 */
#define RESIZE_EVENT_CODE 4
/* Flush pending top-level backing textures to their SDL windows. Posted by the
 * drawing layer after X drawing operations that happen away from a blocking
 * XNextEvent/XFlush path, such as Motif popup menu expose redraws. */
#define PRESENT_EVENT_CODE 5

#define HAS_EVENT_MASK(window, mask) \
    ((GET_WINDOW_STRUCT(window)->eventMask & mask) == mask)

int initEventPipe(Display *display);
void closeEventPipe(Display *display);
void captureMainEventThreadIfUnset(void);
void releaseMainEventThread(void);
void wakeEventPipeForExternalEvent(Display *display);
unsigned int convertModifierState(Uint16 mod);
Bool postEvent(Display *display, Window eventWindow, unsigned int eventId, ...);
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

/* Focus-target classification. The window leaf stays collapsed to None
 * for routing, but postFocusChange needs the distinction to pick
 * NotifyDetailNone vs NotifyPointerRoot on the root events per Xlib 10.7.
 */
typedef enum {
    FocusKindNone,
    FocusKindPointerRoot,
    FocusKindWindow,
} FocusKind;

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
