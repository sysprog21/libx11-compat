/* EWMH ClientMessage dispatch
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 *
 * Modern toolkits (GTK, Qt, xdotool, freerdp) ask the window manager for state
 * changes by sending ClientMessage events to the root window with specific
 * message_type atoms (_NET_WM_STATE, _NET_ACTIVE_WINDOW, _NET_CLOSE_WINDOW).
 * libx11-compat plays the WM role itself rather than relying on an external
 * manager, so XSendEvent intercepts those messages here and routes them to the
 * SDL window directly. Unrecognized message_types fall through to the regular
 * acceptsEventMask walk so future shims that select Substructure{Notify,
 * Redirect}Mask on the root can still observe them.
 *
 * Dynamic atoms are re-resolved on every dispatch: XCloseDisplay's
 * freeAtomStorage clears the dynamic atom table without resetting lastUsedAtom,
 * so a cached id would dangle into the next Display's renumbered allocation.
 * internalInternAtom is a linear walk with no allocation on hit, and root
 * ClientMessage sends are rare, so the cost is negligible.
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdlib.h>

#include "atoms.h"
#include "events-ewmh.h"
#include "events.h"
#include "net-atoms.h"
#include "window-internal.h"

Bool dispatchRootEwmhClientMessage(Display *display, XEvent *event)
{
    if (event->type != ClientMessage)
        return False;

    Atom messageType = event->xclient.message_type;
    Window target = (Window) event->xclient.window;
    Atom activeWindowAtom = internalInternAtom("_NET_ACTIVE_WINDOW");
    Atom closeWindowAtom = internalInternAtom("_NET_CLOSE_WINDOW");

    if (messageType == _NET_WM_STATE) {
        if (!IS_TYPE(target, WINDOW))
            return True;
        long action = event->xclient.data.l[0];
        Atom firstAtom = (Atom) event->xclient.data.l[1];
        Atom secondAtom = (Atom) event->xclient.data.l[2];
        applyNetWmStateAction(display, target, action, firstAtom);

        /* A toggle with both slots pointing at the same atom would cancel
         * itself out; skip the duplicate.
         */
        if (secondAtom != None && secondAtom != firstAtom)
            applyNetWmStateAction(display, target, action, secondAtom);
        return True;
    }

    if (messageType == activeWindowAtom) {
        if (IS_TYPE(target, WINDOW) && IS_MAPPED_TOP_LEVEL_WINDOW(target)) {
            SDL_Window *sdlWindow = GET_WINDOW_STRUCT(target)->sdlWindow;
            SDL_RaiseWindow(sdlWindow);
#if SDL_VERSION_ATLEAST(2, 0, 5)
            SDL_SetWindowInputFocus(sdlWindow);
#endif
        }
        return True;
    }

    if (messageType == closeWindowAtom) {
        if (!IS_TYPE(target, WINDOW))
            return True;

        /* Mirror SDL_WINDOWEVENT_CLOSE: hand the request off via
         * WM_DELETE_WINDOW when the target opted in, otherwise unmap. Shares
         * postWmDeleteIfHandled with the SDL close path so the two cannot
         * diverge.
         */
        if (!postWmDeleteIfHandled(display, target, (Time) CurrentTime) &&
            IS_MAPPED_TOP_LEVEL_WINDOW(target))
            XUnmapWindow(display, target);
        return True;
    }

    return False;
}
