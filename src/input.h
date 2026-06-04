#ifndef INPUT_H
#define INPUT_H

#include "X11/Xlib.h"
#include "window.h"

Window getKeyboardFocus();
void setKeyboardFocus(Window window);
Window getGrabbedPointerWindow(void);
Bool getPointerGrabOwnerEvents(void);

/* Active keyboard grab installed via XGrabKeyboard. Returns the
 * grab_window or None if no grab is active. While a grab is active the
 * event layer routes key events to the grab window so modal Motif
 * dialogs (Help popup, etc.) receive keystrokes regardless of focus. */
Window getGrabbedKeyboardWindow(void);
Bool getKeyboardGrabOwnerEvents(void);

/* Look up an active passive key grab. Returns the grab_window that
 * registered the grab via XGrabKey, or None if no grab matches. The
 * event layer uses this to route grabbed key events to the grab window
 * instead of the focus window. */
Window findKeyGrabWindow(int keycode, unsigned int modifiers);

#endif /* INPUT_H */
