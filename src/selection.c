#include <X11/Xlib.h>
#include <X11/Xlibint.h>
#include <X11/Xatom.h>
#include "sdl-compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atoms.h"
#include "events.h"
#include "selection.h"
#include "window.h"
#include "window-internal.h"
#include "util.h"

/* Minimal in-process ICCCM selection bookkeeping. Owner state is per (display,
 * selection atom); CLIPBOARD also mirrors SDL's clipboard so external SDL code
 * can interoperate. PRIMARY/SECONDARY have no SDL equivalent and report no
 * owner.
 */

typedef struct SelectionEntry {
    Atom selection;
    Window owner;
    Time time;
    struct SelectionEntry *next;
} SelectionEntry;

/* Per-display selection table. Display* keys are opaque pointers, so the table
 * lives in a small side registry rather than the upstream _XPrivate layout.
 * freeSelectionStorage prunes the list when a display closes.
 */
typedef struct DisplayTable {
    Display *display;
    SelectionEntry *head;

    /* Outbound mirror state. clipboardRequestor is a hidden, unmapped window
     * that stands in as the requestor for the SelectionRequest we fire at an X
     * owner so its data can reach SDL's clipboard. lastPushed is the text we
     * last wrote to SDL, kept only to recognize the SDL_CLIPBOARDUPDATE echo of
     * our own write and not mistake it for an external change. lastPushed is a
     * one-shot marker: it is cleared once its own echo is observed, so a later
     * external copy of the same text still revokes stale ownership. everPushed
     * stays set after the first push and disarms the first-fetch expiry guard.
     */
    Window clipboardRequestor;
    char *lastPushed;
    Bool everPushed;

    /* True between firing an outbound SelectionRequest and consuming its reply.
     * Gates the requestor's SelectionNotify handler so an unsolicited or
     * duplicate notify cannot re-push a stale property. fetchProperty is the
     * pool slot the current fetch asked the owner to write, fetchTarget is the
     * target currently being tried, and fetchGen rotates through the pool so a
     * superseded fetch uses a different slot; a late reply whose property is
     * not fetchProperty is ignored rather than pushed.
     */
    Bool clipboardFetchPending;
    Atom fetchProperty;
    Atom fetchTarget;
    unsigned fetchGen;

    /* INCR reassembly. When an owner answers with an INCR property (a size hint
     * rather than the data), the payload arrives in chunks as PropertyNotify
     * writes; incrBuf/incrLen accumulate them until a zero-length chunk ends
     * the transfer. incrProperty is the property the chunks land in.
     */
    Bool incrActive;
    Atom incrProperty;
    Bool incrLatin1;
    unsigned char *incrBuf;
    size_t incrLen;
    struct DisplayTable *next;
} DisplayTable;

/* Cap on a reassembled INCR clipboard so a buggy or hostile owner cannot grow
 * the buffer without bound. ponytail: a fixed 128 MiB ceiling; raise it if a
 * real workload copies more.
 */
#define CLIPBOARD_INCR_MAX (128u * 1024u * 1024u)

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

    free(t->lastPushed);
    free(t->incrBuf);

    /* The hidden requestor window, if any, is a child of the root and gets torn
     * down by destroyScreenWindow later in XCloseDisplay, so it needs no
     * explicit destroy here.
     */
    free(t);
}

/* Atom IDs come from the global atom table in atoms.c. That table is destroyed
 * by freeAtomStorage() when the last Display closes, so any cached ID becomes
 * stale across XCloseDisplay+XOpenDisplay cycles. resetSelectionAtomCache() is
 * invoked from XCloseDisplay alongside freeAtomStorage() to keep these in sync.
 */
static Atom cachedClipboardAtom = None;
static Atom cachedTargetsAtom = None;
static Atom cachedIncrAtom = None;

/* Each outbound fetch draws its receiving property from this small rotating
 * pool so a late reply from a superseded owner lands on a different property
 * than the current fetch and is ignored instead of overwriting the clipboard. A
 * fixed pool keeps atom use bounded (the atom table is a linear list, so an
 * unbounded unique-atom-per-fetch scheme would slow every intern app-wide).
 * ponytail: aliasing only recurs after the pool wraps, which needs more
 * concurrently unanswered fetches than exist here: the single in-process event
 * loop lets each owner answer within an iteration, so at most a couple are ever
 * outstanding. Widen the pool if that assumption ever stops holding.
 */
#define CLIPBOARD_FETCH_PROP_POOL 8
static Atom cachedFetchPropertyAtoms[CLIPBOARD_FETCH_PROP_POOL];

static Atom clipboardAtom(Display *display)
{
    if (cachedClipboardAtom == None && display)
        cachedClipboardAtom = XInternAtom(display, "CLIPBOARD", False);
    return cachedClipboardAtom;
}

/* UTF8_STRING is a pinned predefined atom (net-atoms.h), so unlike the
 * CLIPBOARD / TARGETS / INCR caches around it there is nothing to resolve or
 * invalidate. Kept as a function so the call sites stay uniform.
 */
static Atom utf8StringAtom(Display *display)
{
    (void) display;
    return UTF8_STRING;
}

static Atom targetsAtom(Display *display)
{
    if (cachedTargetsAtom == None && display)
        cachedTargetsAtom = XInternAtom(display, "TARGETS", False);
    return cachedTargetsAtom;
}

static Atom incrAtom(Display *display)
{
    if (cachedIncrAtom == None && display)
        cachedIncrAtom = XInternAtom(display, "INCR", False);
    return cachedIncrAtom;
}

/* Private property (pool slot) the outbound fetch asks the owner to deposit its
 * data in, on the hidden requestor window. Namespaced so it cannot collide with
 * a property a real client uses.
 */
static Atom fetchPropertyAtom(Display *display, unsigned index)
{
    index %= CLIPBOARD_FETCH_PROP_POOL;
    if (cachedFetchPropertyAtoms[index] == None && display) {
        char name[48];
        snprintf(name, sizeof(name), "_LIBX11_COMPAT_CLIPBOARD_RECV_%u", index);
        cachedFetchPropertyAtoms[index] = XInternAtom(display, name, False);
    }
    return cachedFetchPropertyAtoms[index];
}

void resetSelectionAtomCache(void)
{
    cachedClipboardAtom = None;
    cachedTargetsAtom = None;
    cachedIncrAtom = None;
    for (unsigned i = 0; i < CLIPBOARD_FETCH_PROP_POOL; i++)
        cachedFetchPropertyAtoms[i] = None;
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
    if (enqueueEvent(display, owner, event))
        return True;
    free(event);
    return False;
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
    if (enqueueEvent(display, owner, event))
        return True;
    free(event);
    return False;
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
    if (enqueueEvent(display, requestor, event))
        return True;
    free(event);
    return False;
}

/* Lazily create the hidden requestor window. It is never mapped, so it stays
 * invisible; PropertyChangeMask lets the owner's property writes reach us as
 * PropertyNotify (needed for the INCR ping-pong). createInternalWindow marks it
 * internal so it emits no CreateNotify/DestroyNotify and is filtered out of
 * XQueryTree and the state snapshot.
 */
static Bool ensureClipboardRequestor(Display *display, DisplayTable *table)
{
    if (!table)
        return False;
    if (table->clipboardRequestor != None)
        return True;

    /* InputOnly: it never draws, so it needs no backing texture. */
    XSetWindowAttributes attrs;
    attrs.event_mask = PropertyChangeMask;
    attrs.override_redirect = True;
    Window w = createInternalWindow(display, SCREEN_WINDOW, -1, -1, 1, 1, 0, 0,
                                    InputOnly, CopyFromParent,
                                    CWEventMask | CWOverrideRedirect, &attrs);
    if (w == None)
        return False;
    table->clipboardRequestor = w;
    return True;
}

/* Copy len bytes of selection text to SDL's clipboard, remembering it so the
 * resulting SDL_CLIPBOARDUPDATE echo is not mistaken for an external change.
 * When latin1 is set the source is ISO Latin-1 (an XA_STRING reply) and is
 * transcoded to UTF-8, since SDL's clipboard is UTF-8; UTF8_STRING data is
 * already UTF-8 and copied verbatim.
 */
static void pushTextToSdlClipboard(DisplayTable *table,
                                   const unsigned char *data,
                                   size_t len,
                                   Bool latin1)
{
    char *buf;
    if (latin1) {
        /* Each byte >= 0x80 becomes two UTF-8 bytes, so worst case doubles. */
        size_t out = 0;
        for (size_t i = 0; i < len; i++)
            out += (data && data[i] >= 0x80) ? 2 : 1;
        buf = malloc(out + 1);
        if (!buf)
            return;
        size_t j = 0;
        for (size_t i = 0; i < len; i++) {
            unsigned char c = data ? data[i] : 0;
            if (c < 0x80) {
                buf[j++] = (char) c;
            } else {
                buf[j++] = (char) (0xC0 | (c >> 6));
                buf[j++] = (char) (0x80 | (c & 0x3F));
            }
        }
        buf[j] = '\0';
    } else {
        buf = malloc(len + 1);
        if (!buf)
            return;
        if (len > 0 && data)
            memcpy(buf, data, len);
        buf[len] = '\0';
    }
    free(table->lastPushed);
    table->lastPushed = buf;
    table->everPushed = True;

    /* SDL_SetClipboardText's success value differs across SDL2 (0) and SDL3
     * (true), so its return is not checked here; the echo guard tolerates a
     * rejected write by simply comparing against what we last intended to set.
     */
    SDL_SetClipboardText(buf);
}

/* Discard any in-flight INCR reassembly state. */
static void clipboardResetIncr(DisplayTable *table)
{
    free(table->incrBuf);
    table->incrBuf = NULL;
    table->incrLen = 0;
    table->incrActive = False;
    table->incrProperty = None;
    table->incrLatin1 = False;
}

/* Append one INCR chunk, enforcing the size cap.
 *
 * Returns False on overflow or out of memory; the caller then abandons the
 * transfer.
 */
static Bool clipboardAppendIncr(DisplayTable *table,
                                const unsigned char *data,
                                size_t len)
{
    if (len > CLIPBOARD_INCR_MAX - table->incrLen)
        return False;
    unsigned char *grown = realloc(table->incrBuf, table->incrLen + len);
    if (!grown)
        return False;
    table->incrBuf = grown;
    if (len > 0 && data)
        memcpy(grown + table->incrLen, data, len);
    table->incrLen += len;
    return True;
}

/* Handle one PropertyNotify(NewValue) chunk of an active INCR transfer. A
 * zero-length chunk ends it and pushes the reassembled text; every chunk is
 * acked by deleting the property so the owner sends the next one.
 */
static void clipboardHandleIncrChunk(Display *display,
                                     DisplayTable *table,
                                     Window window,
                                     Atom property)
{
    WindowStruct *ws = GET_WINDOW_STRUCT(window);
    WindowProperty *prop = findProperty(&ws->properties, property, NULL);
    if (!prop)
        return; /* spurious notify for an already-consumed property; ignore */

    /* Every path below reads prop and then acks by deleting the property so the
     * owner sends the next chunk; the ack is deferred to the single exit here.
     * A zero-length chunk terminates the transfer, a non-text or oversized
     * chunk aborts it, and any other chunk is appended.
     */
    if (prop->dataFormat != 8 ||
        (prop->type != utf8StringAtom(display) && prop->type != XA_STRING)) {
        LOG("INCR chunk had non-text type/format; aborting transfer.\n");
        clipboardResetIncr(table);
    } else if (prop->dataLength == 0) {
        pushTextToSdlClipboard(table, table->incrBuf, table->incrLen,
                               table->incrLatin1);
        clipboardResetIncr(table);
    } else if (!clipboardAppendIncr(table, prop->data, prop->dataLength)) {
        LOG("INCR chunk too large; aborting transfer.\n");
        clipboardResetIncr(table);
    } else {
        /* Chunks of one transfer share a type; remember it for the terminator's
         * transcode decision. XA_STRING is Latin-1, UTF8_STRING is UTF-8.
         */
        table->incrLatin1 = (prop->type == XA_STRING);
    }
    XDeleteProperty(display, window, property);
}

/* Kick an asynchronous fetch of an X owner's selection into SDL's clipboard.
 * Posts a SelectionRequest for UTF8_STRING; the reply is picked up later by
 * clipboardConsumeInternalEvent when the owner responds. Never blocks.
 */
static void startClipboardFetch(Display *display,
                                DisplayTable *table,
                                Window owner,
                                Atom selection,
                                Time time)
{
    if (!ensureClipboardRequestor(display, table))
        return;
    if (owner == table->clipboardRequestor)
        return;

    /* A fresh fetch supersedes any transfer still in progress. */
    table->clipboardFetchPending = False;
    table->fetchTarget = None;
    clipboardResetIncr(table);
    Atom property = fetchPropertyAtom(display, table->fetchGen++);
    table->fetchProperty = property;
    table->fetchTarget = utf8StringAtom(display);
    if (postSelectionRequest(display, owner, table->clipboardRequestor,
                             selection, table->fetchTarget, property, time))
        table->clipboardFetchPending = True;
}

/* An X client that still believes it owns selection no longer does once the
 * host clipboard changed under it. Tell it and drop the stale ownership.
 */
static void expireSelectionOwner(Display *display,
                                 DisplayTable *table,
                                 Atom selection)
{
    SelectionEntry *e = findInTable(table, selection);
    if (e && e->owner != None && e->owner != table->clipboardRequestor) {
        postSelectionClear(display, e->owner, selection, CurrentTime);
        e->owner = None;

        /* An in-flight fetch or INCR transfer from this now-revoked owner would
         * only push stale data; abandon it.
         */
        table->clipboardFetchPending = False;
        table->fetchTarget = None;
        clipboardResetIncr(table);
    }
}

/* Consume the reply the caller has already attributed to the current fetch:
 * enter INCR reassembly if the owner answered with a size hint, otherwise
 * mirror the text to SDL. Non-INCR non-text targets are dropped (text-only, see
 * clipboardConsumeInternalEvent).
 */
static void clipboardHandleFetchReply(Display *display,
                                      DisplayTable *table,
                                      Window window,
                                      Atom property)
{
    WindowStruct *ws = GET_WINDOW_STRUCT(window);
    WindowProperty *prop = findProperty(&ws->properties, property, NULL);
    if (!prop)
        return; /* notify without the promised data */
    if (prop->type == incrAtom(display)) {
        /* The property holds a 32-bit size hint, not the data. Reject a
         * malformed hint rather than enter a streaming state that may never
         * terminate cleanly.
         */
        if (prop->dataFormat != 32 || prop->dataLength < 1) {
            LOG("INCR size hint malformed; dropping.\n");
            return;
        }

        /* Start reassembly and ack by deleting the hint; the owner then streams
         * the payload as PropertyNotify(NewValue) chunks.
         */
        clipboardResetIncr(table);
        table->incrActive = True;
        table->incrProperty = property;
        XDeleteProperty(display, window, property);
        return;
    }

    /* Only 8-bit text can go to SDL's text clipboard. A reply with a wider
     * format or a non-text type (an image/PNG target, say) is dropped rather
     * than mirrored with a wrong byte length. An empty (zero-length) text
     * property is valid and mirrors an empty clipboard. ponytail: text-only by
     * design. SDL2 has no image clipboard at all and the SDL2-spelled shim does
     * not wrap SDL3's SDL_SetClipboardData, so mirroring image targets would
     * need a whole TARGETS negotiation plus an SDL3-only sink that no in-tree
     * workload asks for; wire that up only when one does. The dropped target is
     * named below so it is visible.
     */
    if (prop->dataFormat != 8 ||
        (prop->type != utf8StringAtom(display) && prop->type != XA_STRING)) {
        const char *targetName = getAtomName(display, prop->type);
        LOG("Clipboard fetch reply target %s (format %d) is not text; dropping "
            "(image and other non-text targets are not mirrored).\n",
            targetName ? targetName : "<unknown>", prop->dataFormat);
        XDeleteProperty(display, window, property);
        return;
    }

    /* A single-shot reply is bounded by the same cap as an INCR transfer so a
     * client cannot pin hundreds of MiB on the hidden requestor.
     */
    if (prop->dataLength > CLIPBOARD_INCR_MAX) {
        LOG("Clipboard fetch reply of %u bytes exceeds the cap; dropping.\n",
            prop->dataLength);
        XDeleteProperty(display, window, property);
        return;
    }
    pushTextToSdlClipboard(table, prop->data, prop->dataLength,
                           prop->type == XA_STRING);

    /* Release the received bytes now that they are mirrored; otherwise the
     * reply lingers stored on the requestor, one copy per pool slot.
     */
    XDeleteProperty(display, window, property);
}

Bool clipboardConsumeInternalEvent(Display *display,
                                   Window window,
                                   int type,
                                   void *event)
{
    DisplayTable *table = findTable(display);
    if (!table || window == None || window != table->clipboardRequestor)
        return False;

    /* The requestor XID travels to the owner in SelectionRequest.requestor, so
     * a reply can still be queued after the window was torn down. Drop the
     * stale handle rather than dereference a freed WindowStruct.
     */
    if (!IS_TYPE(window, WINDOW)) {
        table->clipboardRequestor = None;
        table->clipboardFetchPending = False;
        table->fetchTarget = None;
        clipboardResetIncr(table);
        return True;
    }
    if (type == SelectionNotify) {
        XSelectionEvent *sel = event;
        if (!table->clipboardFetchPending)
            return True; /* unsolicited or duplicate notify */

        /* A decline (property None) carries no pool slot, so it cannot prove
         * which fetch it belongs to. Retry legacy STRING once for the current
         * owner when the UTF8 request is declined; stale declines may add one
         * extra request, but stale replies still cannot push without matching
         * fetchProperty while a fetch is pending.
         */
        if (sel->property == None) {
            if (sel->target != table->fetchTarget)
                return True;
            if (table->fetchTarget == utf8StringAtom(display)) {
                SelectionEntry *e = findInTable(table, sel->selection);
                if (e && e->owner != None &&
                    e->owner != table->clipboardRequestor &&
                    postSelectionRequest(display, e->owner,
                                         table->clipboardRequestor,
                                         sel->selection, XA_STRING,
                                         table->fetchProperty, sel->time)) {
                    table->fetchTarget = XA_STRING;
                } else {
                    table->clipboardFetchPending = False;
                    table->fetchTarget = None;
                }
            } else {
                table->clipboardFetchPending = False;
                table->fetchTarget = None;
            }
            return True;
        }

        /* A reply on a stale pool slot belongs to a superseded fetch; drop it
         * and keep waiting for the current one.
         */
        if (sel->property != table->fetchProperty)
            return True;
        table->clipboardFetchPending = False;
        table->fetchTarget = None;
        clipboardHandleFetchReply(display, table, window, sel->property);
        return True;
    }
    if (type == PropertyNotify) {
        XPropertyEvent *pe = event;
        if (pe->state == PropertyNewValue && table->incrActive &&
            pe->atom == table->incrProperty) {
            clipboardHandleIncrChunk(display, table, window, pe->atom);
            return True;
        }

        /* An INCR owner is waiting for our per-chunk XDeleteProperty ack (a
         * PropertyDelete on the requestor) to send the next chunk, so deliver
         * those while a transfer is active. Every other PropertyNotify on this
         * hidden window (NewValue chunks, and the cleanup deletes of one-shot
         * replies) is ours to swallow, so a client never sees traffic for a
         * window it never created.
         */
        if (pe->state == PropertyDelete && table->incrActive)
            return False;
        return True;
    }
    return True;
}

void clipboardHandleExternalUpdate(Display *display)
{
    DisplayTable *table = findTable(display);
    if (!table)
        return;

    /* Before the first push there is nothing of ours to compare against, so the
     * echo guard below cannot tell our own write from an external one. If a
     * fetch or INCR transfer is also in flight then we are actively populating
     * SDL from the current X owner, and the update is almost certainly the
     * spurious one SDL raises at startup for the pre-existing pasteboard
     * content (or content we are about to overwrite): expiring the very owner
     * we are fetching from would abort the transfer. Skip it. Once we have
     * pushed once, the echo guard distinguishes real external changes even
     * during a later fetch.
     */
    if (!table->everPushed &&
        (table->clipboardFetchPending || table->incrActive))
        return;

    /* Ignore the single update our own SDL_SetClipboardText just triggered.
     * Compare the current text directly rather than gating on
     * SDL_HasClipboardText, which is false for an empty string and would let
     * our own empty push look external. The marker is one-shot: clear it once
     * its echo is seen so a later external copy of the same text still expires
     * the stale owner.
     */
    if (table->lastPushed) {
        char *current = SDL_GetClipboardText();
        if (current) {
            Bool ours = strcmp(current, table->lastPushed) == 0;
            SDL_free(current);
            if (ours) {
                free(table->lastPushed);
                table->lastPushed = NULL;
                return;
            }
        }
    }

    /* Only CLIPBOARD is mirrored to SDL, so only its owner is affected by a
     * host clipboard change; PRIMARY is an independent selection and is left
     * alone.
     */
    expireSelectionOwner(display, table, clipboardAtom(display));
    free(table->lastPushed);
    table->lastPushed = NULL;
}

Window XGetSelectionOwner(Display *display, Atom selection)
{
    SelectionEntry *e = findEntry(display, selection);
    if (e && e->owner != None)
        return e->owner;
    if (display && selection == clipboardAtom(display) &&
        SDL_HasClipboardText() == SDL_TRUE) {
        /* SDL holds the clipboard but no local window has claimed it. Report
         * root so callers can still XConvertSelection through libx11-compat.
         */
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

    /* ICCCM 2.1: a timestamp older than the current owner's must not displace
     * it. CurrentTime always wins (treated as "now").
     */
    if (e->owner != None && time != CurrentTime && e->time != CurrentTime &&
        (long) (time - e->time) < 0) {
        return 1;
    }
    if (e->owner != None && e->owner != owner)
        postSelectionClear(display, e->owner, selection, time);
    e->owner = owner;
    e->time = time;

    /* Mirror an X owner's CLIPBOARD out to SDL so external host apps can paste.
     * The data is not available synchronously, so this only kicks off an async
     * SelectionRequest; the reply arrives via clipboardConsumeInternalEvent.
     * PRIMARY has no SDL equivalent, so only CLIPBOARD is pushed. Clearing the
     * owner (owner == None) abandons any in-flight fetch or INCR transfer so a
     * late reply from the old owner cannot push stale data.
     */
    if (display && selection == clipboardAtom(display)) {
        DisplayTable *table = findTable(display);
        if (owner == None) {
            if (table) {
                table->clipboardFetchPending = False;
                table->fetchTarget = None;
                clipboardResetIncr(table);
            }
        } else {
            startClipboardFetch(display, table, owner, selection, time);
        }
    }
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
        /* No local owner but SDL has clipboard text; synthesize the conversion
         * here so XConvertSelection works in single-process configurations.
         */
        char *text = SDL_GetClipboardText();
        Atom utf8 = utf8StringAtom(display);
        Atom targets = targetsAtom(display);

        /* Only advertise TARGETS once SDL actually has retrievable text.
         * SDL_HasClipboardText() above can disagree with a follow-up
         * SDL_GetClipboardText() returning NULL (race, OOM), and a TARGETS
         * reply listing UTF8_STRING/STRING when no text exists would lie to the
         * requestor. Fall through to the no-data path instead.
         */
        if (target == targets && text) {
            Atom supportedTargets[] = {targets, utf8, XA_STRING};
            XChangeProperty(
                display, requestor, effectiveProperty, XA_ATOM, 32,
                PropModeReplace, (unsigned char *) supportedTargets,
                (int) (sizeof(supportedTargets) / sizeof(supportedTargets[0])));
            postSelectionNotify(display, requestor, selection, target,
                                effectiveProperty, time);
            SDL_free(text);
            return 1;
        }
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
