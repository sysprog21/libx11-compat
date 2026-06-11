/* Window manager hint and state helpers
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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "net-atoms.h"
#include "window.h"

typedef struct NetWmStateSet {
    Bool modal;
    Bool fullscreen;
    Bool maximized;
    Bool hidden;
    Bool above;
} NetWmStateSet;

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
        if (atoms[i] == _NET_WM_STATE_MODAL)
            state->modal = True;
        else if (atoms[i] == _NET_WM_STATE_FULLSCREEN)
            state->fullscreen = True;
        else if (atoms[i] == _NET_WM_STATE_MAXIMIZED_HORZ ||
                 atoms[i] == _NET_WM_STATE_MAXIMIZED_VERT)
            state->maximized = True;
        else if (atoms[i] == _NET_WM_STATE_HIDDEN)
            state->hidden = True;
        else if (atoms[i] == _NET_WM_STATE_ABOVE)
            state->above = True;
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

void applyMotifWmHintsFromProperty(Window window)
{
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

void applyTransientForRelationship(Display *display, Window window)
{
    (void) display;
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
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window))
        return;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    NetWmStateSet state;
    readNetWmState(windowStruct, &state);

    SDL_Window *sdlWindow = windowStruct->sdlWindow;
    if (!state.hidden)
        SDL_RestoreWindow(sdlWindow);
    SDL_SetWindowFullscreen(
        sdlWindow, state.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
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
            applyTransientForRelationship(display, window);
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
        applyTransientForRelationship(display, window);
}
