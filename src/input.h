#ifndef INPUT_H
#define INPUT_H

#include "X11/Xlib.h"
#include "window.h"

/* Focus-target classification. The window leaf stays collapsed to None for
 * routing, but the kind preserves None vs PointerRoot for XGetInputFocus and
 * focus-change root detail.
 */
typedef enum {
    FocusKindNone,
    FocusKindPointerRoot,
    FocusKindWindow,
} FocusKind;

Window getKeyboardFocus();
FocusKind getKeyboardFocusKind(void);
Bool isKeyboardFocusFromHost(void);
Bool isKeyboardFocusFromClient(void);
void setKeyboardFocus(Window window);
void syncKeyboardFocusFromHost(Window window);

extern int revertTo;

/* If "window" is the current keyboard focus, re-target focus per the stored
 * revert_to value (RevertToParent / RevertToPointerRoot / RevertToNone) and
 * emit the focus-change events that XSetInputFocus would have fired. Call from
 * destroyWindow before the window's resources are freed so the event payloads
 * see live state.
 */
void revertKeyboardFocusForDestroyedWindow(Display *display, Window window);
Window getGrabbedPointerWindow(void);
Bool getPointerGrabOwnerEvents(void);
unsigned int getPointerGrabEventMask(void);
Bool activatePassiveButtonGrab(Display *display,
                               Window root,
                               int root_x,
                               int root_y,
                               unsigned int button,
                               unsigned int state);
Bool pointerGrabIsPassive(void);
void releasePassivePointerGrab(Display *display);

/* Active keyboard grab installed via XGrabKeyboard.
 *
 * Returns the grab_window or None if no grab is active. While a grab is active
 * the event layer routes key events to the grab window so modal Motif dialogs
 * (Help popup, etc.) receive keystrokes regardless of focus.
 */
Window getGrabbedKeyboardWindow(void);
Bool getKeyboardGrabOwnerEvents(void);

/* Look up an active passive key grab.
 *
 * Returns the grab_window that registered the grab via XGrabKey, or None if no
 * grab matches. The event layer uses this to route grabbed key events to the
 * grab window instead of the focus window.
 */
Window findKeyGrabWindow(int keycode, unsigned int modifiers);

#endif /* INPUT_H */
