#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/Xatom.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atoms.h"
#include "events.h"
#include "selection.h"
#include "window.h"

/* Minimal in-process ICCCM selection bookkeeping. Owner state is per
 * (display, selection atom); CLIPBOARD also mirrors SDL's clipboard so
 * external SDL code can interoperate. PRIMARY/SECONDARY have no SDL
 * equivalent and report no owner. */

typedef struct SelectionEntry {
    Atom selection;
    Window owner;
    Time time;
    struct SelectionEntry *next;
} SelectionEntry;

/* Per-display selection table. Display* keys are opaque pointers so we
 * keep a small registry rather than touching the upstream _XPrivate
 * layout. freeSelectionStorage prunes the list when a display closes. */
typedef struct DisplayTable {
    Display *display;
    SelectionEntry *head;
    struct DisplayTable *next;
} DisplayTable;

static DisplayTable *displayTables = NULL;

static DisplayTable *findTable(Display *display)
{
    for (DisplayTable *t = displayTables; t; t = t->next) {
        if (t->display == display)
            return t;
    }
    return NULL;
}

static DisplayTable *ensureTable(Display *display)
{
    DisplayTable *t = findTable(display);
    if (t)
        return t;
    t = calloc(1, sizeof(*t));
    if (!t)
        return NULL;
    t->display = display;
    t->next = displayTables;
    displayTables = t;
    return t;
}

static SelectionEntry *findInTable(DisplayTable *table, Atom selection)
{
    if (!table)
        return NULL;
    for (SelectionEntry *e = table->head; e; e = e->next) {
        if (e->selection == selection)
            return e;
    }
    return NULL;
}

static SelectionEntry *findEntry(Display *display, Atom selection)
{
    return findInTable(findTable(display), selection);
}

static SelectionEntry *ensureEntry(Display *display, Atom selection)
{
    DisplayTable *t = ensureTable(display);
    SelectionEntry *e = findInTable(t, selection);
    if (e)
        return e;
    if (!t)
        return NULL;
    e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->selection = selection;
    e->owner = None;
    e->next = t->head;
    t->head = e;
    return e;
}

void freeSelectionStorage(Display *display)
{
    DisplayTable **link = &displayTables;
    while (*link && (*link)->display != display)
        link = &(*link)->next;
    DisplayTable *t = *link;
    if (!t)
        return;
    *link = t->next;
    SelectionEntry *e = t->head;
    while (e) {
        SelectionEntry *next = e->next;
        free(e);
        e = next;
    }
    free(t);
}

static Atom clipboardAtom(Display *display)
{
    static Atom cached = None;
    if (cached == None && display)
        cached = XInternAtom(display, "CLIPBOARD", False);
    return cached;
}

static Atom utf8StringAtom(Display *display)
{
    static Atom cached = None;
    if (cached == None && display)
        cached = XInternAtom(display, "UTF8_STRING", False);
    return cached;
}

static Bool postSelectionClear(Display *display,
                               Window owner,
                               Atom selection,
                               Time time)
{
    if (owner == None)
        return False;
    XSelectionClearEvent *event = malloc(sizeof(*event));
    if (!event)
        return False;
    memset(event, 0, sizeof(*event));
    event->type = SelectionClear;
    event->display = display;
    event->window = owner;
    event->selection = selection;
    event->time = time;
    return enqueueEvent(display, owner, event);
}

static Bool postSelectionRequest(Display *display,
                                 Window owner,
                                 Window requestor,
                                 Atom selection,
                                 Atom target,
                                 Atom property,
                                 Time time)
{
    XSelectionRequestEvent *event = malloc(sizeof(*event));
    if (!event)
        return False;
    memset(event, 0, sizeof(*event));
    event->type = SelectionRequest;
    event->display = display;
    event->owner = owner;
    event->requestor = requestor;
    event->selection = selection;
    event->target = target;
    event->property = property;
    event->time = time;
    return enqueueEvent(display, owner, event);
}

static Bool postSelectionNotify(Display *display,
                                Window requestor,
                                Atom selection,
                                Atom target,
                                Atom property,
                                Time time)
{
    XSelectionEvent *event = malloc(sizeof(*event));
    if (!event)
        return False;
    memset(event, 0, sizeof(*event));
    event->type = SelectionNotify;
    event->display = display;
    event->requestor = requestor;
    event->selection = selection;
    event->target = target;
    event->property = property;
    event->time = time;
    return enqueueEvent(display, requestor, event);
}

Window XGetSelectionOwner(Display *display, Atom selection)
{
    SelectionEntry *e = findEntry(display, selection);
    if (e && e->owner != None)
        return e->owner;
    if (display && selection == clipboardAtom(display) &&
        SDL_HasClipboardText() == SDL_TRUE) {
        /* SDL holds the clipboard but no local window has claimed it.
         * Report root so callers can still XConvertSelection through us. */
        return RootWindow(display, 0);
    }
    return None;
}

int XSetSelectionOwner(Display *display,
                       Atom selection,
                       Window owner,
                       Time time)
{
    SelectionEntry *e = ensureEntry(display, selection);
    if (!e)
        return 0;
    /* ICCCM 2.1: a timestamp older than the current owner's must not
     * displace it. CurrentTime always wins (treated as "now"). */
    if (e->owner != None && time != CurrentTime && e->time != CurrentTime &&
        (long) (time - e->time) < 0) {
        return 1;
    }
    if (e->owner != None && e->owner != owner) {
        postSelectionClear(display, e->owner, selection, time);
    }
    e->owner = owner;
    e->time = time;
    /* Note: actual clipboard contents are not mirrored to SDL here; an
     * owner is expected to respond to SelectionRequest with the data. */
    return 1;
}

int XConvertSelection(Display *display,
                      Atom selection,
                      Atom target,
                      Atom property,
                      Window requestor,
                      Time time)
{
    SelectionEntry *e = findEntry(display, selection);
    Window owner = e ? e->owner : None;
    Atom effectiveProperty = property == None ? target : property;

    if (owner == None && display && selection == clipboardAtom(display) &&
        SDL_HasClipboardText() == SDL_TRUE) {
        /* No local owner but SDL has clipboard text; synthesize the
         * conversion here so XConvertSelection works in single-process
         * configurations. */
        char *text = SDL_GetClipboardText();
        Atom utf8 = utf8StringAtom(display);
        if (text && (target == XA_STRING || target == utf8)) {
            XChangeProperty(display, requestor, effectiveProperty, target, 8,
                            PropModeReplace, (unsigned char *) text,
                            (int) strlen(text));
            postSelectionNotify(display, requestor, selection, target,
                                effectiveProperty, time);
            SDL_free(text);
            return 1;
        }
        if (text)
            SDL_free(text);
    }

    if (owner == None) {
        postSelectionNotify(display, requestor, selection, target, None, time);
        return 1;
    }
    postSelectionRequest(display, owner, requestor, selection, target,
                         effectiveProperty, time);
    return 1;
}
