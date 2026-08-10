/*
 * Window manager hint and state helpers
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 *
 * Implements the in-process WM role for Motif (_MOTIF_WM_HINTS), EWMH
 * (_NET_WM_STATE), and ICCCM transient_for + modal pairing. These helpers
 * translate stored window properties into SDL window flags (resizable /
 * bordered / fullscreen / modal-for / maximized / minimized / always-on-top)
 * and mirror state-machine actions back into the stored properties so
 * XGetWindowProperty readers see what a real WM would publish.
 */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "atoms.h"
#include "net-atoms.h"
#include "window.h"
#include "events.h"

const NetWmStateHonoredAtom netWmStateHonored[] = {
    {_NET_WM_STATE_MODAL, offsetof(NetWmStateSet, modal)},
    {_NET_WM_STATE_FULLSCREEN, offsetof(NetWmStateSet, fullscreen)},
    {_NET_WM_STATE_MAXIMIZED_HORZ, offsetof(NetWmStateSet, maximized)},
    {_NET_WM_STATE_MAXIMIZED_VERT, offsetof(NetWmStateSet, maximized)},
    {_NET_WM_STATE_HIDDEN, offsetof(NetWmStateSet, hidden)},
#if SDL_VERSION_ATLEAST(2, 0, 16)
    /* Gated with its consumer: applyNetWmStateFromProperty only calls
     * SDL_SetWindowAlwaysOnTop from 2.0.16 on, and below that the apply is
     * compiled out entirely. Advertising the substate there would echo the atom
     * back into the window's _NET_WM_STATE while nothing ever raises the
     * window, which is precisely the "advertised but not honored" drift this
     * table exists to prevent.
     */
    {_NET_WM_STATE_ABOVE, offsetof(NetWmStateSet, above)},
#endif
};
const size_t netWmStateHonoredCount = ARRAY_LENGTH(netWmStateHonored);

static Bool isNetWmStateAtomList(WindowProperty *prop)
{
    return prop && prop->dataFormat == 32 && prop->type == XA_ATOM &&
           prop->data;
}

static void readNetWmState(WindowStruct *windowStruct, NetWmStateSet *state)
{
    memset(state, 0, sizeof(*state));

    WindowProperty *prop =
        findProperty(&windowStruct->properties, _NET_WM_STATE, NULL);
    if (!isNetWmStateAtomList(prop))
        return;

    Atom *atoms = (Atom *) prop->data;
    for (unsigned int i = 0; i < prop->dataLength; i++) {
        for (size_t k = 0; k < netWmStateHonoredCount; k++) {
            if (atoms[i] != netWmStateHonored[k].atom)
                continue;
            *(Bool *) ((char *) state + netWmStateHonored[k].offset) = True;
            break;
        }
    }
}

static Bool netWmStateHasAtom(WindowStruct *windowStruct, Atom stateAtom)
{
    WindowProperty *prop =
        findProperty(&windowStruct->properties, _NET_WM_STATE, NULL);
    if (!isNetWmStateAtomList(prop))
        return False;

    Atom *atoms = (Atom *) prop->data;
    for (unsigned int i = 0; i < prop->dataLength; i++)
        if (atoms[i] == stateAtom)
            return True;

    return False;
}

Bool topLevelHasMotifFunctionHints(Window window)
{
    if (!IS_TYPE(window, WINDOW))
        return False;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    WindowProperty *prop =
        findProperty(&windowStruct->properties, _MOTIF_WM_HINTS, NULL);
    if (!prop || prop->dataFormat != 32 || prop->dataLength < 2)
        return False;

    long *values = (long *) prop->data;
    return (values[0] & MWM_HINTS_FUNCTIONS) != 0;
}

void applyMotifWmHintsFromProperty(Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        runWindowOpOnMain(applyMotifWmHintsFromProperty, window);
        return;
    }
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window))
        return;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    WindowProperty *prop =
        findProperty(&windowStruct->properties, _MOTIF_WM_HINTS, NULL);
    if (!prop || prop->dataFormat != 32 || prop->dataLength < 1)
        return;

    long *values = (long *) prop->data;
    long flags = values[0];
    if (flags == 0)
        return;

    SDL_Window *sdlWindow = windowStruct->sdlWindow;
    if ((flags & MWM_HINTS_FUNCTIONS) && prop->dataLength >= 2) {
        long functions = values[1];
        Bool allBit = (functions & MWM_FUNC_ALL) != 0;
        Bool resize = allBit ? !(functions & MWM_FUNC_RESIZE)
                             : (functions & MWM_FUNC_RESIZE) != 0;
#if SDL_VERSION_ATLEAST(2, 0, 5)
        SDL_SetWindowResizable(sdlWindow, resize ? SDL_TRUE : SDL_FALSE);
#else
        (void) resize;
#endif
    }

    if ((flags & MWM_HINTS_DECORATIONS) && prop->dataLength >= 3) {
        long decorations = values[2];
        Bool allBit = (decorations & MWM_DECOR_ALL) != 0;
        Bool border = allBit ? !(decorations & MWM_DECOR_BORDER)
                             : (decorations & MWM_DECOR_BORDER) != 0;
        Bool title = allBit ? !(decorations & MWM_DECOR_TITLE)
                            : (decorations & MWM_DECOR_TITLE) != 0;

        /* SDL has only one knob (bordered=on/off) for the whole frame while
         * Motif distinguishes BORDER (frame) from TITLE (title bar). Drop SDL's
         * border only when BOTH are absent so a window that asked for a title
         * bar but no frame still gets the title bar, matching what a real Motif
         * WM would render.
         */
        SDL_SetWindowBordered(sdlWindow,
                              (border || title) ? SDL_TRUE : SDL_FALSE);
    }
}

Bool windowIsModal(Window window)
{
    if (!IS_TYPE(window, WINDOW))
        return False;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    NetWmStateSet state;
    readNetWmState(windowStruct, &state);
    if (state.modal)
        return True;

    WindowProperty *mwm =
        findProperty(&windowStruct->properties, _MOTIF_WM_HINTS, NULL);
    if (mwm && mwm->dataFormat == 32 && mwm->dataLength >= 4) {
        long *values = (long *) mwm->data;
        if ((values[0] & MWM_HINTS_INPUT_MODE) &&
            values[3] == MWM_INPUT_FULL_APPLICATION_MODAL)
            return True;
    }

    return False;
}

void applyTransientForRelationship(Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        runWindowOpOnMain(applyTransientForRelationship, window);
        return;
    }
    if (!IS_TYPE(window, WINDOW))
        return;

    WindowStruct *childStruct = GET_WINDOW_STRUCT(window);
    Window parent = None;
    WindowProperty *prop =
        findProperty(&childStruct->properties, XA_WM_TRANSIENT_FOR, NULL);
    if (prop && prop->dataFormat == 32 && prop->dataLength >= 1 && prop->data)
        parent = *((Window *) prop->data);
    else
        parent = childStruct->deferredTransientParent;
    if (parent == None || parent == window)
        return;

    /* ICCCM 4.1.2.6 says WM_TRANSIENT_FOR points at the "main top-level" but in
     * practice some Xt / Motif toolkits store a sub-widget XID (e.g. the owner
     * form or canvas). Walk up to the top-level shell so SDL_SetWindowModalFor
     * receives a window with an SDL backing.
     */
    while (IS_TYPE(parent, WINDOW) && parent != SCREEN_WINDOW &&
           !IS_TOP_LEVEL(parent)) {
        Window grandparent = GET_PARENT(parent);
        if (grandparent == None || grandparent == parent)
            break;
        parent = grandparent;
    }
    if (parent == SCREEN_WINDOW)
        return;

    /* Defer until both windows are realized: SDL_SetWindowModalFor needs the
     * parent's SDL_Window handle to exist, and Motif XmDialogShell writes
     * WM_TRANSIENT_FOR before its top-level peer is mapped.
     */
    childStruct->deferredTransientParent = parent;
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window))
        return;
    if (!IS_TYPE(parent, WINDOW) || !IS_MAPPED_TOP_LEVEL_WINDOW(parent))
        return;

    if (!windowIsModal(window)) {
        /* Transient but non-modal: SDL2 lacks a non-modal parent primitive.
         * Leave the relationship recorded so a later modal write can re-apply,
         * but do not call SDL_SetWindowModalFor. macOS / Windows still honor
         * SDL_RaiseWindow ordering. If a prior modal binding is live, drop it
         * now so the child stops receiving modal blocking from the host WM.
         */
#if SDL_VERSION_ATLEAST(2, 0, 5)
        if (childStruct->deferredTransientApplied && childStruct->sdlWindow)
            SDL_SetWindowModalFor(childStruct->sdlWindow, NULL);
#endif
        childStruct->deferredTransientApplied = False;
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 5)
    SDL_Window *parentSdl = GET_WINDOW_STRUCT(parent)->sdlWindow;
    SDL_Window *childSdl = childStruct->sdlWindow;
    if (parentSdl && childSdl) {
        SDL_SetWindowModalFor(childSdl, parentSdl);
        childStruct->deferredTransientApplied = True;
    }
#endif
}

void clearTransientForRelationship(Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        runWindowOpOnMain(clearTransientForRelationship, window);
        return;
    }
    if (!IS_TYPE(window, WINDOW))
        return;

    WindowStruct *childStruct = GET_WINDOW_STRUCT(window);
#if SDL_VERSION_ATLEAST(2, 0, 5)
    if (childStruct->deferredTransientApplied && childStruct->sdlWindow)
        SDL_SetWindowModalFor(childStruct->sdlWindow, NULL);
#endif
    childStruct->deferredTransientParent = None;
    childStruct->deferredTransientApplied = False;
}

void applyNetWmStateFromProperty(Window window)
{
    if (!libx11CompatOnMainEventThread()) {
        runWindowOpOnMain(applyNetWmStateFromProperty, window);
        return;
    }
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window))
        return;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    NetWmStateSet state;
    readNetWmState(windowStruct, &state);

    SDL_Window *sdlWindow = windowStruct->sdlWindow;
    if (!state.hidden)
        SDL_RestoreWindow(sdlWindow);
    XC_SetWindowFullscreen(sdlWindow, state.fullscreen);
    if (state.maximized)
        SDL_MaximizeWindow(sdlWindow);
    if (state.hidden)
        SDL_MinimizeWindow(sdlWindow);
#if SDL_VERSION_ATLEAST(2, 0, 16)
    SDL_SetWindowAlwaysOnTop(sdlWindow, state.above ? SDL_TRUE : SDL_FALSE);
#else
    (void) state;
#endif
}

void applyNetWmStateAction(Display *display,
                           Window window,
                           long action,
                           Atom stateAtom)
{
    if (stateAtom == None)
        return;
    if (!IS_TYPE(window, WINDOW))
        return;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    WindowProperty *prop =
        findProperty(&windowStruct->properties, _NET_WM_STATE, NULL);
    Bool currentlyOn = netWmStateHasAtom(windowStruct, stateAtom);

    Bool desiredOn;
    switch (action) {
    case 0: /* remove */
        desiredOn = False;
        break;
    case 1: /* add */
        desiredOn = True;
        break;
    case 2: /* toggle */
        desiredOn = !currentlyOn;
        break;
    default:
        return;
    }

    /* Mirror the post-action state into the stored _NET_WM_STATE property so
     * XGetWindowProperty readers see the same view the WM would.
     */
    if (currentlyOn == desiredOn && prop) {
        applyNetWmStateFromProperty(window);
        if (stateAtom == _NET_WM_STATE_MODAL)
            applyTransientForRelationship(window);
        return; /* property already matches; avoid useless writes */
    }

    /* Only treat an existing property as the source atom list when it is
     * actually atoms. A client could have stored format=8/16 data under
     * _NET_WM_STATE; iterating that as `Atom *` would read past the smaller
     * per-element backing buffer.
     */
    Bool propIsAtomList = isNetWmStateAtomList(prop);
    size_t existing =
        (propIsAtomList && prop && prop->data) ? prop->dataLength : 0;
    size_t newCount = existing;
    if (desiredOn && !currentlyOn) {
        if (existing >= (size_t) INT_MAX ||
            existing + 1 > SIZE_MAX / sizeof(Atom))
            return; /* would overflow XChangeProperty's int count */
        newCount = existing + 1;
    } else if (!desiredOn && currentlyOn) {
        newCount = existing - 1;
    }

    Atom *newAtoms = NULL;
    if (newCount > 0) {
        newAtoms = malloc(sizeof(Atom) * newCount);
        if (!newAtoms)
            return;
        size_t j = 0;
        if (propIsAtomList && prop && prop->data) {
            Atom *atoms = (Atom *) prop->data;
            for (unsigned int i = 0; i < prop->dataLength; i++) {
                if (!desiredOn && atoms[i] == stateAtom)
                    continue;
                newAtoms[j++] = atoms[i];
            }
        }
        if (desiredOn && !currentlyOn)
            newAtoms[j++] = stateAtom;
    }
    XChangeProperty(display, window, _NET_WM_STATE, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *) newAtoms,
                    (int) newCount);
    free(newAtoms);

    applyNetWmStateFromProperty(window);

    /* Modal hint changed: reapply transient pairing so SDL learns about it. */
    if (stateAtom == _NET_WM_STATE_MODAL)
        applyTransientForRelationship(window);
}

/* Name reported for the in-process window manager, read by clients off the
 * check window's _NET_WM_NAME.
 */
#define EWMH_WM_NAME "libx11-compat"

/* Not serialized against a concurrent XOpenDisplay: two threads opening a
 * display at the same moment could both miss the stored check window and create
 * one, leaving the loser as a hidden internal root child until teardown. That
 * matches the rest of the open path, which already touches numDisplaysOpen,
 * SCREEN_WINDOW and the atom table without a lock, so a mutex here would buy
 * nothing until the whole path is serialized.
 */
void publishEwmhWmSupport(Display *display)
{
    if (!display || SCREEN_WINDOW == None)
        return;

    Atom supportingCheck = internalInternAtom("_NET_SUPPORTING_WM_CHECK");
    Atom supported = internalInternAtom("_NET_SUPPORTED");
    Atom closeWindow = internalInternAtom("_NET_CLOSE_WINDOW");
    Atom utf8String = internalInternAtom("UTF8_STRING");
    if (supportingCheck == None || supported == None || closeWindow == None ||
        utf8String == None)
        return;

    /* The root property store is the only record of the check window, so a
     * second display opened against the same root finds it here instead of
     * through a side table that a display close could leave dangling.
     */
    WindowStruct *rootStruct = GET_WINDOW_STRUCT(SCREEN_WINDOW);
    WindowProperty *stored =
        findProperty(&rootStruct->properties, supportingCheck, NULL);
    if (stored && stored->dataFormat == 32 && stored->dataLength == 1 &&
        stored->data) {
        Window prior = (Window) ((long *) stored->data)[0];
        if (IS_TYPE(prior, WINDOW) && GET_WINDOW_STRUCT(prior)->internal)
            return;
    }

    /* Advertise exactly what the shim acts on: the substates and window types
     * come from the honored tables the handlers read, and the fixed entries are
     * the messages events-ewmh.c dispatches plus the two properties
     * window-property.c turns into a real SDL title and icon. Listing anything
     * else (workarea, client list, desktops) would make clients wait on state
     * changes that never arrive. Sizing the buffer off the same array the fixed
     * entries are copied from keeps the two from disagreeing.
     * _NET_ACTIVE_WINDOW is deliberately absent even though events-ewmh.c acts
     * on its ClientMessage: EWMH also defines it as a root property the manager
     * keeps current, and nothing here maintains that. Advertising it would make
     * a client that reads the root property (gdk_x11_screen_get_active_window,
     * xdotool getactivewindow) see no active window at all, or poll for an
     * update that never lands. _NET_CLOSE_WINDOW has no property side, so it
     * stays. Publishing the property is the way to earn the atom back.
     */
    const Atom fixedSupported[] = {
        supported,    supportingCheck, _NET_WM_STATE, _NET_WM_WINDOW_TYPE,
        _NET_WM_NAME, _NET_WM_ICON,    closeWindow,
    };
    size_t capacity = ARRAY_LENGTH(fixedSupported) + netWmStateHonoredCount +
                      netWmWindowTypeHonoredCount;

    /* Format-32 property data is stored and returned as an array of long, the
     * Xlib client convention this shim follows.
     */
    long *list = malloc(sizeof(long) * capacity);
    if (!list)
        return;

    size_t count = 0;
    for (size_t i = 0; i < ARRAY_LENGTH(fixedSupported); i++)
        list[count++] = (long) fixedSupported[i];
    for (size_t i = 0; i < netWmStateHonoredCount; i++)
        list[count++] = (long) netWmStateHonored[i].atom;
    for (size_t i = 0; i < netWmWindowTypeHonoredCount; i++)
        list[count++] = (long) netWmWindowTypeHonored[i];

    /* InputOnly and never mapped: the window exists only to carry the two
     * properties. createInternalWindow marks it internal, which keeps it out of
     * CreateNotify, XQueryTree and the state snapshot, so it cannot be mistaken
     * for a client top-level. Created only once the advertisement is ready to
     * write, since a check window published without _NET_SUPPORTED would make
     * every later call take the reuse path above and never finish the job.
     */
    XSetWindowAttributes attributes;
    attributes.override_redirect = True;
    Window check = createInternalWindow(display, SCREEN_WINDOW, -1, -1, 1, 1, 0,
                                        0, InputOnly, CopyFromParent,
                                        CWOverrideRedirect, &attributes);
    if (check == None) {
        free(list);
        return;
    }

    /* The root's _NET_SUPPORTING_WM_CHECK is written last and is the commit
     * point: it is both what a client tests first and what the reuse path above
     * reads. Everything that can still fail (the property stores allocate)
     * happens before it, and a failure tears the half-built check window back
     * down, so the next call retries from a clean root rather than parking
     * forever on a handshake that never got its atom list.
     */
    long checkId = (long) check;
    Bool published =
        XChangeProperty(display, check, supportingCheck, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *) &checkId, 1) &&
        XChangeProperty(display, check, _NET_WM_NAME, utf8String, 8,
                        PropModeReplace, (const unsigned char *) EWMH_WM_NAME,
                        (int) strlen(EWMH_WM_NAME)) &&
        XChangeProperty(display, SCREEN_WINDOW, supported, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *) list, (int) count) &&
        XChangeProperty(display, SCREEN_WINDOW, supportingCheck, XA_WINDOW, 32,
                        PropModeReplace, (unsigned char *) &checkId, 1);
    free(list);
    if (!published) {
        /* Both root properties go, not just the one this call wrote. The reuse
         * path above already tolerates a _NET_SUPPORTING_WM_CHECK naming a dead
         * window, so one can be sitting there from an earlier cycle; leaving it
         * would keep the root advertising a window ID that resolves to nothing,
         * which reads to a client as a live WM with no check window behind it.
         * Deleting an absent property is a no-op.
         */
        XDestroyWindow(display, check);
        XDeleteProperty(display, SCREEN_WINDOW, supported);
        XDeleteProperty(display, SCREEN_WINDOW, supportingCheck);
    }
}
