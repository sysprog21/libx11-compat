#ifndef SELECTION_H
#define SELECTION_H

#include <X11/Xlib.h>

void freeSelectionStorage(Display *display);
void resetSelectionAtomCache(void);

/* Intercept an event delivered to the hidden outbound-clipboard requestor
 * window.
 *
 * Returns True when the event is consumed (the caller must drop it); False to
 * deliver it normally. type is the X event type; event points at the matching X
 * event struct (XSelectionEvent / XPropertyEvent).
 */
Bool clipboardConsumeInternalEvent(Display *display,
                                   Window window,
                                   int type,
                                   void *event);

/* React to SDL_CLIPBOARDUPDATE: if an external app changed the host clipboard,
 * revoke any X client's stale CLIPBOARD/PRIMARY ownership.
 */
void clipboardHandleExternalUpdate(Display *display);

#endif /* SELECTION_H */
