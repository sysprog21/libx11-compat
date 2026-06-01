#ifndef _EVENTS_H_
#define _EVENTS_H_

#include <X11/Xlib.h>
#include <SDL2/SDL.h>

#define SEND_EVENT_CODE 1
#define INTERNAL_EVENT_CODE 2

#define HAS_EVENT_MASK(window, mask) \
    ((GET_WINDOW_STRUCT(window)->eventMask & mask) == mask)

int initEventPipe(Display *display);
void closeEventPipe(Display *display);
void captureMainEventThreadIfUnset(void);
void releaseMainEventThread(void);
unsigned int convertModifierState(Uint16 mod);
Bool postEvent(Display *display, Window eventWindow, unsigned int eventId, ...);
Bool postReparentUnmapNotify(Display *display,
                             Window eventWindow,
                             Window oldParent);
Bool enqueueEvent(Display *display, Window eventWindow, void *event);
Bool postCrossingEvent(Display *display,
                       Window window,
                       int type,
                       int mode,
                       int detail,
                       unsigned int state);
void postExposeEvent(Display *display,
                     Window window,
                     const SDL_Rect *damagedAreaList,
                     size_t numAreas);

#endif /* _EVENTS_H_ */
