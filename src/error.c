#include "errors.h"
#include "sdl-compat.h"
#include <stdio.h>
#include <stdlib.h>
#include "display.h"

typedef int (*errorHandlerFunction)(Display *, XErrorEvent *);
static errorHandlerFunction error_handler = defaultErrorHandler;

/* Per-Display last request code tracking. The upstream _XDisplay struct has no
 * request_code slot, so a side table keyed by Display* lets independent Display
 * connections record their last request without clobbering each other.
 *
 * The list is guarded by lastRequestLock so concurrent threads sharing a
 * Display (a multi-threaded client that called XInitThreads) can safely record,
 * look up, and drop entries without racing on the linked-list links or on the
 * inserted entry's code field. The lock is lazily allocated on first use; if
 * SDL_CreateMutex fails the request-list helpers run unsynchronized rather than
 * crash. Lock and unlock are no-ops on NULL.
 *
 * Typical clients open one Display; a linked list is plenty. The fallback for
 * an unknown Display returns X_NoOperation, matching the previous behavior when
 * error_code lookup runs against a Display that has not yet recorded any
 * request.
 */
typedef struct LastRequestEntry {
    Display *display;
    unsigned char code;
    struct LastRequestEntry *next;
} LastRequestEntry;

static LastRequestEntry *lastRequestList = NULL;
static SDL_mutex *lastRequestLock = NULL;

/* Return a stable mutex pointer for the side table. The pointer is published
 * exactly once via atomic CAS so concurrent first-callers don't create and leak
 * duplicate mutexes (the prior naive single-check assignment could split
 * callers across different mutexes). The returned pointer must be used for both
 * lock and unlock; re-reading the global between Lock and Unlock would risk
 * unlocking a different mutex if a publication happened in between.
 */
static SDL_mutex *acquireLastRequestLock(void)
{
    SDL_mutex *cur = __atomic_load_n(&lastRequestLock, __ATOMIC_ACQUIRE);
    if (cur)
        return cur;
    SDL_mutex *fresh = SDL_CreateMutex();
    if (!fresh) {
        fprintf(stderr,
                "libx11-compat: SDL_CreateMutex failed; error.c side "
                "table will run unsynchronized: %s\n",
                SDL_GetError());
        return NULL;
    }
    SDL_mutex *expected = NULL;
    if (__atomic_compare_exchange_n(&lastRequestLock, &expected, fresh, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return fresh;
    /* Lost the publish race; free the local mutex and use the winner. */
    SDL_DestroyMutex(fresh);
    return expected;
}

static void lockSide(SDL_mutex *lk)
{
    if (lk)
        SDL_LockMutex(lk);
}

static void unlockSide(SDL_mutex *lk)
{
    if (lk)
        SDL_UnlockMutex(lk);
}

static LastRequestEntry *findLastRequestEntryLocked(Display *display)
{
    for (LastRequestEntry *e = lastRequestList; e; e = e->next) {
        if (e->display == display)
            return e;
    }
    return NULL;
}

void setLastRequestCode(Display *display, unsigned char requestCode)
{
    SDL_mutex *lk = acquireLastRequestLock();
    lockSide(lk);
    LastRequestEntry *entry = findLastRequestEntryLocked(display);
    if (!entry) {
        entry = malloc(sizeof(*entry));
        if (!entry) {
            unlockSide(lk);
            return;
        }
        entry->display = display;
        entry->next = lastRequestList;
        lastRequestList = entry;
    }
    entry->code = requestCode;
    unlockSide(lk);
}

unsigned char getLastRequestCode(Display *display)
{
    SDL_mutex *lk = acquireLastRequestLock();
    lockSide(lk);
    LastRequestEntry *entry = findLastRequestEntryLocked(display);
    /* Read the byte under the lock. Returning the pointer would let another
     * thread (releaseLastRequestCode) free the node between the unlock and the
     * caller's deref.
     */
    unsigned char code = entry ? entry->code : (unsigned char) X_NoOperation;
    unlockSide(lk);
    return code;
}

/* Drop a Display's per-Display state on XCloseDisplay so the side table doesn't
 * leak across long-running test harnesses.
 */
void releaseLastRequestCode(Display *display)
{
    SDL_mutex *lk = acquireLastRequestLock();
    lockSide(lk);
    LastRequestEntry **link = &lastRequestList;
    while (*link) {
        LastRequestEntry *e = *link;
        if (e->display == display) {
            *link = e->next;
            free(e);
            unlockSide(lk);
            return;
        }
        link = &e->next;
    }
    unlockSide(lk);
}

/* Called from XCloseDisplay's "last display closing" branch BEFORE SDL_Quit so
 * the destroy reaches a still-valid SDL subsystem. The function is idempotent
 * and safe when no entries remain.
 */
void freeLastRequestStorage(void)
{
    SDL_mutex *lk =
        __atomic_exchange_n(&lastRequestLock, NULL, __ATOMIC_ACQ_REL);
    if (lk) {
        SDL_LockMutex(lk);
        LastRequestEntry *e = lastRequestList;
        lastRequestList = NULL;
        while (e) {
            LastRequestEntry *next = e->next;
            free(e);
            e = next;
        }
        SDL_UnlockMutex(lk);
        SDL_DestroyMutex(lk);
    } else {
        /* Lock never created (SDL_CreateMutex failed); still drain the list. */
        LastRequestEntry *e = lastRequestList;
        lastRequestList = NULL;
        while (e) {
            LastRequestEntry *next = e->next;
            free(e);
            e = next;
        }
    }
}

errorHandlerFunction XSetErrorHandler(errorHandlerFunction handler)
{
    // https://tronche.com/gui/x/xlib/event-handling/protocol-errors/XSetErrorHandler.html
    errorHandlerFunction prev_error_handler = error_handler;
    if (!handler)
        handler = defaultErrorHandler;
    error_handler = handler;
    return prev_error_handler;
}

int defaultErrorHandler(Display *display, XErrorEvent *event)
{
    (void) display;
    static const struct {
        unsigned char code;
        const char *format;
    } errorMessages[] = {
        {BadAlloc,
         "Out of memory: Failed to allocate memory while processing "
         "request : %d\n"},
        {BadMatch,
         "Parameter mismatch: A parameter was not valid for request %d!\n"},
        {BadValue,
         "Parameter mismatch: An int parameter was out of range for "
         "request %d!\n"},
        {BadGC,
         "Parameter mismatch: A parameter was not a GC for request %d!\n"},
        {BadDrawable,
         "Parameter mismatch: A parameter was not a Drawable for "
         "request %d!\n"},
        {BadPixmap,
         "Parameter mismatch: A parameter was not a Pixmap for request %d!\n"},
        {BadWindow,
         "Parameter mismatch: A parameter was not a Window for request %d!\n"},
        {BadName, "Parameter invalid: Got an unknown name for request %d!\n"},
        {BadAtom, "Parameter invalid: Got an invalid Atom for request %d!\n"},
        {BadFont,
         "Parameter mismatch: A parameter was not a Font for request %d!\n"},
        {BadCursor,
         "Parameter mismatch: A parameter was not a Cursor for request %d!\n"},
        {BadColor,
         "Parameter invalid: A parameter did not name a defined "
         "color for request %d!\n"},
    };
    const char *format = NULL;
    for (size_t i = 0; i < sizeof(errorMessages) / sizeof(errorMessages[0]);
         i++) {
        if (errorMessages[i].code == event->error_code) {
            format = errorMessages[i].format;
            break;
        }
    }
    if (format) {
        fprintf(stderr, format, event->request_code);
    } else {
        fprintf(stderr, "An unknown error occurred for request %d: %u\n",
                event->request_code, event->error_code);
    }
    fflush(stdout);
    fflush(stderr);
    exit(1);
}

inline void handleOutOfMemory(int type,
                              Display *display,
                              unsigned long serial,
                              unsigned char minor_code)
{
    handleError(type, display, None, serial, BadAlloc, minor_code);
}
void handleError(int type,
                 Display *display,
                 XID resourceId,
                 unsigned long serial,
                 unsigned char error_code,
                 unsigned char minor_code)
{
    XErrorEvent event;
    event.type = type;
    event.display = display;
    event.resourceid = resourceId;
    event.serial = serial;
    event.error_code = error_code;
    event.request_code = getLastRequestCode(display);
    event.minor_code = minor_code;
    error_handler(display, &event);
}

unsigned char resourceTypeToErrorCode(XResourceType resourceType)
{
    switch (resourceType) {
    case WINDOW:
        return BadWindow;
    case DRAWABLE:
        return BadDrawable;
    case PIXMAP:
        return BadPixmap;
    case GRAPHICS_CONTEXT:
        return BadGC;
    case FONT:
        return BadFont;
    case CURSOR:
        return BadCursor;
    case COLORMAP:
        return BadColor;
    default:
        return BadMatch;
    }
}
