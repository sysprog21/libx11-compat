/*
 * Replay-time state introspection.
 *
 * Marshals the in-process focus, grab, mapped-window, geometry and
 * named-property surface into a UiSnapshot struct, serialized to JSON. The
 * marshal runs on the main / X-client event thread (via a SDL_USEREVENT
 * round-trip identical to src/snapshot.c) so reads of WindowStruct and the
 * focus / grab globals are race-free with respect to window destruction.
 *
 * Property reads go through WindowStruct.properties directly rather than
 * XGetWindowProperty so the snapshot can distinguish a stored property value
 * from one synthesized at read time (Motif, _MOTIF_WM_HINTS:
 * src/window.c:XGetWindowProperty fabricates defaults when the client has not
 * stored its own value). The recorded UiPropSource lets the property-drift
 * verifier tell those two states apart.
 */

#include "state-snapshot.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "sdl-compat.h"
#include <X11/Xatom.h>
#include "display.h"
#include "events.h"
#include "input.h"
#include "net-atoms.h"
#include "resource-types.h"
#include "timeline.h"
#include "util.h"
#include "window-internal.h"
#include "window.h"

/* Shared with snapshot.c's SDL_USEREVENT contract; both round-trips use the
 * same condvar so they cannot interleave.
 */
#define STATE_SNAPSHOT_EVENT_CODE 0x57415453 /* 'STAW' */

static pthread_mutex_t stateMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t stateCond = PTHREAD_COND_INITIALIZER;
static int stateDone = 0;
static int stateResult = 0;
/* SDL_atomic-backed so stateSnapshotOwnsEventType() can read it from the SDL
 * filter / dispatch path without holding stateMutex. SDL atomics give us an
 * acquire/release barrier; the previous plain int read raced the writer that
 * sets it under the mutex on first use.
 */
static SDL_atomic_t stateEventType = {(int) (Uint32) -1};

/* Generation counter for in-flight requests. Each call to
 * stateSnapshotRequestAndWait increments it inside stateMutex; the value is
 * passed as user.data2 on the SDL_USEREVENT and re-checked inside
 * stateSnapshotHandleEvent. A request that times out leaves its SDL_USEREVENT
 * stranded in the queue; when the main thread eventually drains it, the
 * generation mismatch makes us free the stale path and skip the condvar signal
 * so it does not prematurely satisfy a fresh waiter on the next request.
 */
static uint64_t stateGeneration = 0;

/* Tracked atoms. Index here is just the position in the windows[].properties
 * record; the actual atom value is the literal from atom-list.h / net-atoms.h.
 */
static const Atom TRACKED_ATOMS[] = {
    XA_WM_HINTS,   XA_WM_CLASS,         XA_WM_NAME,      _NET_WM_NAME,
    _NET_WM_STATE, _NET_WM_WINDOW_TYPE, _MOTIF_WM_HINTS,
};
#define TRACKED_ATOM_COUNT \
    ((int) (sizeof(TRACKED_ATOMS) / sizeof(TRACKED_ATOMS[0])))

Bool stateSnapshotOwnsEventType(Uint32 eventType)
{
    Uint32 registered = (Uint32) SDL_AtomicGet(&stateEventType);
    return registered != (Uint32) -1 && eventType == registered;
}

static uint64_t nowMs(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
        return 0;
    return (uint64_t) t.tv_sec * 1000ULL + (uint64_t) (t.tv_nsec / 1000000L);
}

static Bool isPopupAtomSet(Atom typeAtom)
{
    return typeAtom == _NET_WM_WINDOW_TYPE_MENU ||
           typeAtom == _NET_WM_WINDOW_TYPE_POPUP_MENU ||
           typeAtom == _NET_WM_WINDOW_TYPE_DROPDOWN_MENU ||
           typeAtom == _NET_WM_WINDOW_TYPE_TOOLTIP ||
           typeAtom == _NET_WM_WINDOW_TYPE_NOTIFICATION ||
           typeAtom == _NET_WM_WINDOW_TYPE_COMBO;
}

static void copyName(char *dst, size_t dstSize, const char *src)
{
    /* Use for known C strings only (e.g. ws->windowName). For raw X property
     * data use copyBoundedString, which does not call strlen on the source.
     */
    if (!dst || dstSize == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(src);
    if (len >= dstSize)
        len = dstSize - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* X property values such as WM_CLASS / WM_NAME are byte arrays whose
 * NUL-termination is not guaranteed (WM_CLASS in particular is two NUL-
 * separated runs without a guarantee of a trailing NUL). Using strlen on the
 * raw bytes is an OOB read into adjacent property storage. Bound on the
 * explicit byte length and clamp at the first NUL within that range.
 */
static void copyBoundedString(char *dst,
                              size_t dstSize,
                              const void *src,
                              size_t srcBytes)
{
    if (!dst || dstSize == 0)
        return;
    if (!src || srcBytes == 0) {
        dst[0] = '\0';
        return;
    }
    const void *terminator = memchr(src, '\0', srcBytes);
    size_t effective =
        terminator ? (size_t) ((const char *) terminator - (const char *) src)
                   : srcBytes;
    if (effective >= dstSize)
        effective = dstSize - 1;
    memcpy(dst, src, effective);
    dst[effective] = '\0';
}

/* Map an X property dataFormat (8/16/32) to its on-the-wire byte size on this
 * host.
 *
 * Returns 0 for unsupported values so writePropertyRecord can refuse to
 * fabricate a byte count from a corrupt or unknown format.
 */
static size_t formatBytesFor(int dataFormat)
{
    switch (dataFormat) {
    case 8:
        return 1;
    case 16:
        return 2;
    case 32:
        return sizeof(long);
    default:
        return 0;
    }
}

static void writePropertyRecord(UiSnapshotProperty *out,
                                Atom atom,
                                const WindowProperty *stored)
{
    out->atom = atom;
    if (!stored) {
        out->format = 0;
        out->length = 0;
        out->source = UI_PROP_SOURCE_NONE;
        out->truncated = 0;
        return;
    }
    out->format = stored->dataFormat;
    out->length = stored->dataLength;
    out->source = UI_PROP_SOURCE_STORED;
    out->truncated = 0;
    size_t formatBytes = formatBytesFor(stored->dataFormat);
    if (formatBytes == 0) {
        /* Unsupported format: do not fabricate a byte count. Recording length /
         * format from the source is enough for the property-drift verifier to
         * flag the anomaly; copying zero bytes avoids a misleading hex dump and
         * the under-sized total a 1-byte fallback would have produced.
         */
        LOG("state-snapshot: skipping property atom=%lu unsupported "
            "format=%d\n",
            (unsigned long) atom, stored->dataFormat);
        return;
    }
    size_t totalBytes = (size_t) stored->dataLength * formatBytes;
    size_t copy = totalBytes;
    if (copy > UI_SNAPSHOT_PROP_BYTES_MAX) {
        copy = UI_SNAPSHOT_PROP_BYTES_MAX;
        out->truncated = 1;
    }
    if (copy && stored->data)
        memcpy(out->bytes, stored->data, copy);
}

static void populateWindowEntry(UiSnapshotWindow *entry, Window window)
{
    WindowStruct *ws = GET_WINDOW_STRUCT(window);
    entry->window = window;
    entry->parent = ws->parent;
    entry->x = ws->x;
    entry->y = ws->y;
    entry->w = ws->w;
    entry->h = ws->h;
    entry->sdl_window_id = ws->sdlWindow ? SDL_GetWindowID(ws->sdlWindow) : 0;
    entry->sdl_flags = ws->sdlWindow ? SDL_GetWindowFlags(ws->sdlWindow) : 0;
    entry->override_redirect = ws->overrideRedirect ? 1 : 0;
    entry->map_state = (int) ws->mapState;
    entry->event_mask = ws->eventMask;
    entry->wm_class[0] = '\0';
    entry->wm_name[0] = '\0';
    /* wm_name: prefer the cached windowName, fall back to WM_NAME storage.
     * wm_class: read from XA_WM_CLASS storage if present.
     */
    if (ws->windowName)
        copyName(entry->wm_name, sizeof(entry->wm_name), ws->windowName);

    entry->property_count = 0;
    int slots = TRACKED_ATOM_COUNT;
    if (slots >
        (int) (sizeof(entry->properties) / sizeof(entry->properties[0])))
        slots =
            (int) (sizeof(entry->properties) / sizeof(entry->properties[0]));
    for (int i = 0; i < slots; i++) {
        Atom atom = TRACKED_ATOMS[i];
        WindowProperty *stored = findProperty(&ws->properties, atom, NULL);
        writePropertyRecord(&entry->properties[entry->property_count], atom,
                            stored);
        /* Use the explicit byte length (dataLength * format byte width) so a
         * non-NUL-terminated property value cannot drive strlen past the end of
         * the property storage.
         */
        if (atom == XA_WM_CLASS && stored && stored->data &&
            stored->dataLength > 0 && stored->dataFormat == 8) {
            copyBoundedString(entry->wm_class, sizeof(entry->wm_class),
                              stored->data, (size_t) stored->dataLength);
        }
        if (atom == XA_WM_NAME && stored && stored->data &&
            stored->dataLength > 0 && stored->dataFormat == 8 &&
            entry->wm_name[0] == '\0') {
            copyBoundedString(entry->wm_name, sizeof(entry->wm_name),
                              stored->data, (size_t) stored->dataLength);
        }
        entry->property_count++;
    }
}

static void emitJsonStringContent(FILE *fp, const char *text)
{
    for (const unsigned char *p = (const unsigned char *) text; *p; p++) {
        switch (*p) {
        case '"':
        case '\\':
            fputc('\\', fp);
            fputc(*p, fp);
            break;
        case '\b':
            fputs("\\b", fp);
            break;
        case '\f':
            fputs("\\f", fp);
            break;
        case '\n':
            fputs("\\n", fp);
            break;
        case '\r':
            fputs("\\r", fp);
            break;
        case '\t':
            fputs("\\t", fp);
            break;
        default:
            if (*p < 0x20)
                fprintf(fp, "\\u%04x", (unsigned) *p);
            else
                fputc(*p, fp);
            break;
        }
    }
}

static void emitWindowEntry(FILE *fp, const UiSnapshotWindow *entry, Bool last)
{
    fprintf(fp,
            "    {\"window\":%lu,\"parent\":%lu,\"x\":%d,\"y\":%d,\"w\":%u,"
            "\"h\":%u,\"sdl_window_id\":%u,\"sdl_flags\":%u,"
            "\"override_redirect\":%d,\"map_state\":%d,\"event_mask\":%ld,"
            "\"wm_class\":\"",
            (unsigned long) entry->window, (unsigned long) entry->parent,
            entry->x, entry->y, entry->w, entry->h,
            (unsigned) entry->sdl_window_id, (unsigned) entry->sdl_flags,
            entry->override_redirect, entry->map_state, entry->event_mask);
    emitJsonStringContent(fp, entry->wm_class);
    fprintf(fp, "\",\"wm_name\":\"");
    emitJsonStringContent(fp, entry->wm_name);
    fprintf(fp, "\",\"properties\":[");
    for (int i = 0; i < entry->property_count; i++) {
        const UiSnapshotProperty *p = &entry->properties[i];
        fprintf(
            fp,
            "%s{\"atom\":%lu,\"format\":%d,\"length\":%lu,\"source\":\"%s\","
            "\"truncated\":%d,\"hex\":\"",
            i == 0 ? "" : ",", (unsigned long) p->atom, p->format, p->length,
            p->source == UI_PROP_SOURCE_NONE     ? "none"
            : p->source == UI_PROP_SOURCE_STORED ? "stored"
                                                 : "synthesized",
            p->truncated);
        /* Match writePropertyRecord: unsupported formats emit an empty hex
         * string because the byte buffer was never populated. Otherwise the
         * dumped zeros would lie about the property's contents and trip
         * property-drift comparisons against the same data on another run.
         */
        size_t formatBytes = formatBytesFor(p->format);
        size_t totalBytes = (size_t) p->length * formatBytes;
        if (totalBytes > UI_SNAPSHOT_PROP_BYTES_MAX)
            totalBytes = UI_SNAPSHOT_PROP_BYTES_MAX;
        for (size_t k = 0; k < totalBytes; k++)
            fprintf(fp, "%02x", p->bytes[k]);
        fprintf(fp, "\"}");
    }
    fprintf(fp, "]}%s\n", last ? "" : ",");
}

static int writeSnapshotJson(const char *path, const UiSnapshot *snap)
{
    FILE *fp = fopen(path, "w");
    if (!fp)
        return -1;
    fprintf(fp,
            "{\n  \"captured_t_ms\":%llu,\n  \"focused_window\":%lu,\n  "
            "\"revert_to\":%d,\n  \"pointer_grab\":%lu,\n  "
            "\"keyboard_grab\":%lu,\n  \"mapped_window_count\":%d,\n  "
            "\"popup_window_count\":%d,\n  \"event_queue_depth\":%d,\n  "
            "\"truncation_flagged\":%d,\n  \"window_count\":%d,\n  "
            "\"windows\":[\n",
            (unsigned long long) snap->captured_t_ms,
            (unsigned long) snap->focused_window, snap->revert_to,
            (unsigned long) snap->pointer_grab,
            (unsigned long) snap->keyboard_grab, snap->mapped_window_count,
            snap->popup_window_count, snap->event_queue_depth,
            snap->truncation_flagged, snap->window_count);
    for (int i = 0; i < snap->window_count; i++)
        emitWindowEntry(fp, &snap->windows[i], i == snap->window_count - 1);
    fprintf(fp, "  ]\n}\n");
    fclose(fp);
    return 0;
}

static void populateSnapshot(Display *display, UiSnapshot *snap)
{
    memset(snap, 0, sizeof(*snap));
    snap->captured_t_ms = nowMs();
    snap->focused_window = getKeyboardFocus();
    snap->revert_to = revertTo;
    snap->pointer_grab = getGrabbedPointerWindow();
    snap->keyboard_grab = getGrabbedKeyboardWindow();
    snap->event_queue_depth = display ? GET_DISPLAY(display)->qlen : 0;
    if (SCREEN_WINDOW == None) {
        snap->window_count = 0;
        return;
    }
    WindowStruct *screen = GET_WINDOW_STRUCT(SCREEN_WINDOW);
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    for (size_t i = 0; i < screen->children.length; i++) {
        Window child = children[i];
        WindowStruct *cws = GET_WINDOW_STRUCT(child);
        if (cws->mapState == Mapped)
            snap->mapped_window_count++;
        if (cws->overrideRedirect && cws->mapState == Mapped)
            snap->popup_window_count++;
        if (snap->window_count >= UI_SNAPSHOT_MAX_WINDOWS) {
            snap->truncation_flagged = 1;
            continue;
        }
        UiSnapshotWindow *entry = &snap->windows[snap->window_count++];
        populateWindowEntry(entry, child);
        /* Also count popup windows by _NET_WM_WINDOW_TYPE atom set (some Motif
         * menus set the atom without override-redirect). EWMH allows the
         * property to be a list of atoms in priority order, so walk every
         * stored element (bounded by what we copied into bytes), not just the
         * first one.
         */
        for (int p = 0; p < entry->property_count; p++) {
            const UiSnapshotProperty *prop = &entry->properties[p];
            if (prop->atom != _NET_WM_WINDOW_TYPE || prop->format != 32 ||
                prop->length == 0)
                continue;
            if (cws->overrideRedirect || cws->mapState != Mapped)
                continue;
            size_t storedAtoms =
                (size_t) UI_SNAPSHOT_PROP_BYTES_MAX / sizeof(Atom);
            if ((size_t) prop->length < storedAtoms)
                storedAtoms = (size_t) prop->length;
            Bool isPopup = False;
            for (size_t a = 0; a < storedAtoms && !isPopup; a++) {
                Atom value = 0;
                memcpy(&value, prop->bytes + a * sizeof(Atom), sizeof(Atom));
                if (isPopupAtomSet(value))
                    isPopup = True;
            }
            if (isPopup) {
                /* Avoid double-counting override-redirect popups (already
                 * filtered above) and avoid double-counting when two
                 * _NET_WM_WINDOW_TYPE entries are present on the same window:
                 * break out of the property loop on the first hit.
                 */
                snap->popup_window_count++;
                break;
            }
        }
    }
}

int stateSnapshotHandleEvent(Display *display, const SDL_Event *event)
{
    char *path = (char *) event->user.data1;
    uint64_t eventGeneration = (uint64_t) (uintptr_t) event->user.data2;
    /* If the request that posted this event already timed out, the worker has
     * moved on (or has dispatched a fresh request with its own path and
     * generation). Free the stale path, do not touch stateDone, do not signal.
     * Otherwise a fresh waiter would observe stateDone == 1 from this stale
     * event and return without its own snapshot being written.
     */
    pthread_mutex_lock(&stateMutex);
    Bool stale = eventGeneration != stateGeneration;
    pthread_mutex_unlock(&stateMutex);
    if (stale) {
        LOG("state-snapshot: dropping stale event gen=%llu cur=%llu\n",
            (unsigned long long) eventGeneration,
            (unsigned long long) stateGeneration);
        free(path);
        return -5;
    }
    UiSnapshot snap;
    populateSnapshot(display, &snap);
    int rc = writeSnapshotJson(path, &snap);
    if (rc != 0)
        LOG("state-snapshot: write %s failed (rc=%d)\n", path, rc);
    pthread_mutex_lock(&stateMutex);
    /* Re-check the generation now that we have the mutex; a concurrent
     * stateSnapshotRequestAndWait could have bumped it between our pre-write
     * check and the signal.
     */
    if (eventGeneration == stateGeneration) {
        stateResult = rc;
        stateDone = 1;
        pthread_cond_signal(&stateCond);
    }
    pthread_mutex_unlock(&stateMutex);
    free(path);
    return rc;
}

int stateSnapshotRequestAndWait(const char *path)
{
    if (!path || !*path)
        return -1;
    char *copy = strdup(path);
    if (!copy)
        return -1;
    pthread_mutex_lock(&stateMutex);
    Uint32 registered = (Uint32) SDL_AtomicGet(&stateEventType);
    if (registered == (Uint32) -1) {
        Uint32 newType = SDL_RegisterEvents(1);
        if (newType == (Uint32) -1) {
            pthread_mutex_unlock(&stateMutex);
            LOG("state-snapshot: SDL_RegisterEvents failed: %s\n",
                SDL_GetError());
            free(copy);
            return -2;
        }
        SDL_AtomicSet(&stateEventType, (int) newType);
        registered = newType;
    }
    stateDone = 0;
    stateResult = 0;
    /* Bump the generation under the mutex so any stale event from a prior
     * timed-out request lands as eventGeneration != stateGeneration in
     * stateSnapshotHandleEvent and gets dropped instead of satisfying us.
     */
    stateGeneration++;
    uint64_t generation = stateGeneration;
    pthread_mutex_unlock(&stateMutex);
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = registered;
    ev.user.code = STATE_SNAPSHOT_EVENT_CODE;
    ev.user.data1 = copy;
    ev.user.data2 = (void *) (uintptr_t) generation;
    int pushRc = SDL_PushEvent(&ev);
    if (pushRc <= 0) {
        /* Roll back the generation so a subsequent request does not skip past
         * the stale-event window.
         */
        pthread_mutex_lock(&stateMutex);
        if (stateGeneration == generation)
            stateGeneration--;
        pthread_mutex_unlock(&stateMutex);
        free(copy);
        return -2;
    }
    wakeEventPipeForExternalEvent(NULL);
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 15;
    pthread_mutex_lock(&stateMutex);
    int wait_rc = 0;
    while (!stateDone && wait_rc == 0)
        wait_rc = pthread_cond_timedwait(&stateCond, &stateMutex, &deadline);
    int rc = stateDone ? stateResult : -4;
    pthread_mutex_unlock(&stateMutex);
    return rc;
}
