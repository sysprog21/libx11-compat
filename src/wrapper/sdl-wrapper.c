#include <SDL.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>

#include "dlwrap.h"

static void *realSdlHandle(void)
{
    static void *handle;
    static const char *const candidates[] = {
#ifdef LIBX11_COMPAT_SDL2_DYLIB
        LIBX11_COMPAT_SDL2_DYLIB,
#endif
#if defined(__APPLE__)
        "libSDL2-2.0.0.dylib",
        "libSDL2.dylib",
#else
        "libSDL2-2.0.so.0",
        "libSDL2.so.0",
        "libSDL2.so",
#endif
        NULL,
    };
    return dlwrapOpen(&handle, "LIBX11_COMPAT_REAL_SDL2", candidates,
                      "real SDL2");
}

static void *realSdlSymbol(const char *name)
{
    return dlwrapSym(realSdlHandle(), name, "SDL2");
}

#define SDL_WRAP(ret, name, args, callargs) \
    DLWRAP_THUNK(realSdlSymbol, ret, name, args, callargs)

#define SDL_WRAP_VOID(name, args, callargs) \
    DLWRAP_THUNK_VOID(realSdlSymbol, name, args, callargs)

/* Lazily resolve a symbol into a function-local cache slot using the same
 * atomic ACQUIRE/RELEASE dance as the SDL_WRAP macros, then bind it to a
 * variable named "cached". Type is a typedef'd function-pointer type, slot is a
 * static variable of that type, and resolve is the expression that produces the
 * symbol (realSdlSymbol(...) or dlsym(...)).
 */
#define CACHED_SYMBOL(Type, slot, resolve)                    \
    Type cached = __atomic_load_n(&(slot), __ATOMIC_ACQUIRE); \
    if (!cached) {                                            \
        cached = (Type) (resolve);                            \
        __atomic_store_n(&(slot), cached, __ATOMIC_RELEASE);  \
    }

SDL_WRAP(SDL_TimerID,
         SDL_AddTimer,
         (Uint32 interval, SDL_TimerCallback callback, void *param),
         (interval, callback, param))
SDL_WRAP(SDL_PixelFormat *,
         SDL_AllocFormat,
         (Uint32 pixel_format),
         (pixel_format))
SDL_WRAP(int, SDL_AtomicAdd, (SDL_atomic_t * a, int v), (a, v))
SDL_WRAP(int, SDL_AtomicGet, (SDL_atomic_t * a), (a))
SDL_WRAP(SDL_bool,
         SDL_AtomicCAS,
         (SDL_atomic_t * a, int oldv, int newv),
         (a, oldv, newv))
SDL_WRAP_VOID(SDL_AtomicLock, (SDL_SpinLock * lock), (lock))
SDL_WRAP(int, SDL_AtomicSet, (SDL_atomic_t * a, int v), (a, v))
SDL_WRAP_VOID(SDL_AtomicUnlock, (SDL_SpinLock * lock), (lock))
SDL_WRAP(SDL_Cursor *,
         SDL_CreateColorCursor,
         (SDL_Surface * surface, int hot_x, int hot_y),
         (surface, hot_x, hot_y))
SDL_WRAP(SDL_mutex *, SDL_CreateMutex, (void), ())
SDL_WRAP(
    int,
    SDL_ConvertPixels,
    (int width,
     int height,
     Uint32 src_format,
     const void *src,
     int src_pitch,
     Uint32 dst_format,
     void *dst,
     int dst_pitch),
    (width, height, src_format, src, src_pitch, dst_format, dst, dst_pitch))
SDL_WRAP(SDL_Surface *,
         SDL_CreateRGBSurface,
         (Uint32 flags,
          int width,
          int height,
          int depth,
          Uint32 Rmask,
          Uint32 Gmask,
          Uint32 Bmask,
          Uint32 Amask),
         (flags, width, height, depth, Rmask, Gmask, Bmask, Amask))
SDL_WRAP(SDL_Surface *,
         SDL_CreateRGBSurfaceFrom,
         (void *pixels,
          int width,
          int height,
          int depth,
          int pitch,
          Uint32 Rmask,
          Uint32 Gmask,
          Uint32 Bmask,
          Uint32 Amask),
         (pixels, width, height, depth, pitch, Rmask, Gmask, Bmask, Amask))
SDL_WRAP(SDL_Surface *,
         SDL_CreateRGBSurfaceWithFormat,
         (Uint32 flags, int width, int height, int depth, Uint32 format),
         (flags, width, height, depth, format))
SDL_WRAP(
    SDL_Surface *,
    SDL_CreateRGBSurfaceWithFormatFrom,
    (void *pixels, int width, int height, int depth, int pitch, Uint32 format),
    (pixels, width, height, depth, pitch, format))
SDL_WRAP(SDL_Renderer *,
         SDL_CreateSoftwareRenderer,
         (SDL_Surface * surface),
         (surface))
SDL_WRAP(SDL_Cursor *, SDL_CreateSystemCursor, (SDL_SystemCursor id), (id))
SDL_WRAP(SDL_Texture *,
         SDL_CreateTexture,
         (SDL_Renderer * renderer, Uint32 format, int access, int w, int h),
         (renderer, format, access, w, h))
SDL_WRAP(SDL_Texture *,
         SDL_CreateTextureFromSurface,
         (SDL_Renderer * renderer, SDL_Surface *surface),
         (renderer, surface))
SDL_WRAP(SDL_Window *,
         SDL_CreateWindow,
         (const char *title, int x, int y, int w, int h, Uint32 flags),
         (title, x, y, w, h, flags))
SDL_WRAP_VOID(SDL_Delay, (Uint32 ms), (ms))
SDL_WRAP_VOID(SDL_DestroyMutex, (SDL_mutex * mutex), (mutex))
SDL_WRAP_VOID(SDL_DestroyRenderer, (SDL_Renderer * renderer), (renderer))
SDL_WRAP_VOID(SDL_DestroyTexture, (SDL_Texture * texture), (texture))
SDL_WRAP_VOID(SDL_DestroyWindow, (SDL_Window * window), (window))
SDL_WRAP_VOID(SDL_DisableScreenSaver, (void), ())
SDL_WRAP_VOID(SDL_EnableScreenSaver, (void), ())
SDL_WRAP(int,
         SDL_FillRect,
         (SDL_Surface * dst, const SDL_Rect *rect, Uint32 color),
         (dst, rect, color))
SDL_WRAP_VOID(SDL_FreeCursor, (SDL_Cursor * cursor), (cursor))
SDL_WRAP_VOID(SDL_FreeFormat, (SDL_PixelFormat * format), (format))
SDL_WRAP_VOID(SDL_FreeSurface, (SDL_Surface * surface), (surface))
SDL_WRAP(char *, SDL_GetClipboardText, (void), ())
SDL_WRAP(int,
         SDL_GetCurrentDisplayMode,
         (int displayIndex, SDL_DisplayMode *mode),
         (displayIndex, mode))
SDL_WRAP(const char *, SDL_GetCurrentVideoDriver, (void), ())
SDL_WRAP(SDL_Cursor *, SDL_GetCursor, (void), ())
SDL_WRAP(SDL_Cursor *, SDL_GetDefaultCursor, (void), ())
SDL_WRAP(int,
         SDL_GetDesktopDisplayMode,
         (int displayIndex, SDL_DisplayMode *mode),
         (displayIndex, mode))
SDL_WRAP(const char *, SDL_GetError, (void), ())
SDL_WRAP(SDL_bool,
         SDL_GetEventFilter,
         (SDL_EventFilter * filter, void **userdata),
         (filter, userdata))
SDL_WRAP(Uint32, SDL_GetGlobalMouseState, (int *x, int *y), (x, y))
SDL_WRAP(SDL_Keycode, SDL_GetKeyFromScancode, (SDL_Scancode s), (s))
SDL_WRAP(SDL_Scancode, SDL_GetScancodeFromKey, (SDL_Keycode k), (k))
SDL_WRAP(SDL_Keymod, SDL_GetModState, (void), ())
SDL_WRAP(SDL_Window *, SDL_GetMouseFocus, (void), ())
SDL_WRAP(Uint32, SDL_GetMouseState, (int *x, int *y), (x, y))
SDL_WRAP(int, SDL_GetNumVideoDisplays, (void), ())
SDL_WRAP(Uint32, SDL_GetTicks, (void), ())
SDL_WRAP_VOID(
    SDL_GetRGB,
    (Uint32 pixel, const SDL_PixelFormat *format, Uint8 *r, Uint8 *g, Uint8 *b),
    (pixel, format, r, g, b))
SDL_WRAP_VOID(SDL_GetRGBA,
              (Uint32 pixel,
               const SDL_PixelFormat *format,
               Uint8 *r,
               Uint8 *g,
               Uint8 *b,
               Uint8 *a),
              (pixel, format, r, g, b, a))
SDL_WRAP(int,
         SDL_GetRenderDrawBlendMode,
         (SDL_Renderer * renderer, SDL_BlendMode *blendMode),
         (renderer, blendMode))
SDL_WRAP(SDL_Texture *,
         SDL_GetRenderTarget,
         (SDL_Renderer * renderer),
         (renderer))
SDL_WRAP(SDL_bool, SDL_GetRelativeMouseMode, (void), ())
SDL_WRAP(Uint32, SDL_GetWindowFlags, (SDL_Window * window), (window))
SDL_WRAP(SDL_Window *, SDL_GetWindowFromID, (Uint32 id), (id))
SDL_WRAP(Uint32, SDL_GetWindowID, (SDL_Window * window), (window))
SDL_WRAP_VOID(SDL_GetWindowPosition,
              (SDL_Window * window, int *x, int *y),
              (window, x, y))
SDL_WRAP_VOID(SDL_GetWindowSize,
              (SDL_Window * window, int *w, int *h),
              (window, w, h))
SDL_WRAP(int,
         SDL_GetRendererOutputSize,
         (SDL_Renderer * renderer, int *w, int *h),
         (renderer, w, h))
SDL_WRAP(SDL_Surface *, SDL_GetWindowSurface, (SDL_Window * window), (window))
SDL_WRAP(const char *, SDL_GetWindowTitle, (SDL_Window * window), (window))
SDL_WRAP(SDL_bool, SDL_HasClipboardText, (void), ())
SDL_WRAP(int, SDL_Init, (Uint32 flags), (flags))
SDL_WRAP(SDL_bool,
         SDL_IntersectRect,
         (const SDL_Rect *A, const SDL_Rect *B, SDL_Rect *result),
         (A, B, result))
SDL_WRAP(int, SDL_LockMutex, (SDL_mutex * mutex), (mutex))
SDL_WRAP(int, SDL_LockSurface, (SDL_Surface * surface), (surface))
SDL_WRAP(Uint32,
         SDL_MapRGBA,
         (const SDL_PixelFormat *format, Uint8 r, Uint8 g, Uint8 b, Uint8 a),
         (format, r, g, b, a))
SDL_WRAP_VOID(SDL_MaximizeWindow, (SDL_Window * window), (window))
SDL_WRAP_VOID(SDL_MinimizeWindow, (SDL_Window * window), (window))
/* sdl2-compat (the SDL2 API implemented over SDL3) crashes inside SDL_PushEvent
 * / SDL_PeepEvents when an SDL_TEXTINPUT or SDL_TEXTEDITING event is *pushed*
 * into its queue: SDL2 carries the text inline in a fixed char[] field, SDL3
 * carries it as a heap pointer, and that translation does not survive the push
 * direction (it hands SDL3 a NULL event). Real input flows the other way, SDL3
 * -> SDL2, and is unaffected; only synthetic injection of a text event hits
 * this. To keep that injection working without crashing, the wrapper diverts
 * these events into a small side queue and merges them back in through the
 * wrapped SDL queue APIs below. The registered event filter is invoked on the
 * way in, exactly as SDL would; when a side-queued filtered event is later
 * removed through SDL APIs, a narrow libX11-compat hook undoes the X event
 * wakeup accounting for that removed entry. On a classic SDL2 runtime no text
 * push ever lands here in practice (only synthetic injection does), so the
 * workaround is inert for real workloads.
 *
 * Limitations, acceptable because real code never injects text events: the side
 * queue drains before real SDL events, so a text event injected after another
 * event can be returned slightly early; and only the inline-text types are
 * handled (SDL_TEXTEDITING_EXT carries a caller-owned char* that a flat
 * SDL_Event copy would dangle, so it is left to the real path).
 */
enum { TEXT_SIDEQ_CAP = 64 };

typedef struct TextSideEvent {
    SDL_Event event;
    SDL_EventFilter filter;
    void *filterUserdata;
    bool ranFilter;
} TextSideEvent;

static TextSideEvent textSideQueue[TEXT_SIDEQ_CAP];
static int textSideHead;
static int textSideCount;
static pthread_mutex_t textSideMutex = PTHREAD_MUTEX_INITIALIZER;

static bool isTextEventType(Uint32 type)
{
    return type == SDL_TEXTINPUT || type == SDL_TEXTEDITING;
}

/* Mirror SDL_PushEvent's filter step: returns 0 if the registered filter asks
 * to drop the event, non-zero otherwise (including when no filter is set).
 */
static int invokeRealEventFilter(SDL_Event *event,
                                 SDL_EventFilter *filter,
                                 void **userdata)
{
    typedef SDL_bool(SDLCALL * GetFilterFunc)(SDL_EventFilter *, void **);
    static GetFilterFunc getFilter;
    CACHED_SYMBOL(GetFilterFunc, getFilter,
                  realSdlSymbol("SDL_GetEventFilter"));
    *filter = NULL;
    *userdata = NULL;
    if (cached(filter, userdata) == SDL_TRUE && *filter)
        return (*filter)(*userdata, event);
    return 1;
}

static void undoSideQueueAccounting(SDL_EventFilter filter, void *userdata)
{
    typedef void (*HookFunc)(SDL_EventFilter, void *);
    static HookFunc hook;
    CACHED_SYMBOL(HookFunc, hook,
                  dlsym(RTLD_DEFAULT, "libx11CompatSideQueueEventRemoved"));
    if (cached)
        cached(filter, userdata);
}

static bool sideQueueDrainShouldReclaim(void)
{
    typedef int (*HookFunc)(void);
    static HookFunc hook;
    CACHED_SYMBOL(HookFunc, hook,
                  dlsym(RTLD_DEFAULT, "libx11CompatSdlPeepEventsIsXlibDrain"));
    return !cached || !cached();
}

/* Append a copy to the ring. Returns 1 on success, -1 if the ring is full. */
static int sideQueueRawEnqueue(const SDL_Event *event,
                               bool ranFilter,
                               SDL_EventFilter filter,
                               void *filterUserdata)
{
    int result;
    pthread_mutex_lock(&textSideMutex);
    if (textSideCount < TEXT_SIDEQ_CAP) {
        int tail = (textSideHead + textSideCount) % TEXT_SIDEQ_CAP;
        textSideQueue[tail].event = *event;
        textSideQueue[tail].filter = filter;
        textSideQueue[tail].filterUserdata = filterUserdata;
        textSideQueue[tail].ranFilter = ranFilter;
        textSideCount++;
        result = 1;
    } else {
        result = -1;
    }
    pthread_mutex_unlock(&textSideMutex);
    return result;
}

/* Divert an inline-text event pushed via SDL_PushEvent into the side queue.
 * Returns SDL_PushEvent's result (1 queued, 0 filtered out, -1 queue full), or
 * INT_MIN to signal "not a diverted event, push normally".
 */
static int sideQueueTextPush(SDL_Event *event)
{
    if (!event || !isTextEventType(event->type))
        return INT_MIN;
    pthread_mutex_lock(&textSideMutex);
    bool full = textSideCount >= TEXT_SIDEQ_CAP;
    pthread_mutex_unlock(&textSideMutex);
    if (full)
        return -1;
    SDL_EventFilter filter = NULL;
    void *filterUserdata = NULL;
    if (invokeRealEventFilter(event, &filter, &filterUserdata) == 0)
        return 0;
    int enqueued =
        sideQueueRawEnqueue(event, filter != NULL, filter, filterUserdata);
    /* The filter (onSdlEvent) already bumped the wake-pipe and qlen accounting;
     * if a concurrent push filled the ring in the gap and we drop the event,
     * undo that accounting so XEventsQueued does not report a phantom event.
     */
    if (enqueued < 0 && filter != NULL)
        undoSideQueueAccounting(filter, filterUserdata);
    return enqueued;
}

/* Drain (GET) or copy (PEEK) side-queued text events whose type falls in
 * [minType, maxType] into the caller's buffer, FIFO order, up to numevents. A
 * non-removing PEEK is forced when events is NULL (the SDL count form).
 * Returns how many were placed (or would be placed, for the count form).
 */
/* When reclaim is true the caller consumes the drained events itself and
 * libX11's Xlib drain will NOT read their wake-pipe bytes, so we reclaim the
 * push-time accounting here. When reclaim is false the events are handed back
 * to libX11's SDL_PeepEvents drain, which reads one pipe byte per returned
 * event (its qlen count includes ours), so reclaiming here too would
 * double-decrement and steal another event's wake byte.
 */
static int sideQueueDrain(SDL_Event *events,
                          int numevents,
                          SDL_eventaction action,
                          Uint32 minType,
                          Uint32 maxType,
                          bool reclaim)
{
    if (numevents <= 0)
        return 0;
    bool remove = (action == SDL_GETEVENT) && events != NULL;
    int taken = 0;
    TextSideEvent removed[TEXT_SIDEQ_CAP];
    int removedCount = 0;
    pthread_mutex_lock(&textSideMutex);
    int remaining = textSideCount;
    int readIdx = textSideHead;
    TextSideEvent kept[TEXT_SIDEQ_CAP];
    int keptCount = 0;
    for (int scanned = 0; scanned < remaining; scanned++) {
        TextSideEvent *e = &textSideQueue[readIdx];
        bool match = e->event.type >= minType && e->event.type <= maxType &&
                     taken < numevents;
        if (match) {
            if (events)
                events[taken] = e->event;
            taken++;
            if (remove && reclaim && e->ranFilter)
                removed[removedCount++] = *e;
            if (!remove)
                kept[keptCount++] = *e;
        } else {
            kept[keptCount++] = *e;
        }
        readIdx = (readIdx + 1) % TEXT_SIDEQ_CAP;
    }
    if (remove) {
        /* Rebuild the ring with only the untaken entries. */
        for (int i = 0; i < keptCount; i++)
            textSideQueue[i] = kept[i];
        textSideHead = 0;
        textSideCount = keptCount;
    }
    pthread_mutex_unlock(&textSideMutex);
    for (int i = 0; i < removedCount; i++)
        undoSideQueueAccounting(removed[i].filter, removed[i].filterUserdata);
    return taken;
}

static void sideQueueRemoveRange(Uint32 minType, Uint32 maxType)
{
    TextSideEvent removed[TEXT_SIDEQ_CAP];
    int removedCount = 0;
    pthread_mutex_lock(&textSideMutex);
    int remaining = textSideCount;
    int readIdx = textSideHead;
    TextSideEvent kept[TEXT_SIDEQ_CAP];
    int keptCount = 0;
    for (int scanned = 0; scanned < remaining; scanned++) {
        TextSideEvent *e = &textSideQueue[readIdx];
        if (e->event.type < minType || e->event.type > maxType)
            kept[keptCount++] = *e;
        else if (e->ranFilter)
            removed[removedCount++] = *e;
        readIdx = (readIdx + 1) % TEXT_SIDEQ_CAP;
    }
    for (int i = 0; i < keptCount; i++)
        textSideQueue[i] = kept[i];
    textSideHead = 0;
    textSideCount = keptCount;
    pthread_mutex_unlock(&textSideMutex);
    for (int i = 0; i < removedCount; i++)
        undoSideQueueAccounting(removed[i].filter, removed[i].filterUserdata);
}

static bool sideQueueHasRange(Uint32 minType, Uint32 maxType)
{
    bool found = false;
    pthread_mutex_lock(&textSideMutex);
    int readIdx = textSideHead;
    for (int scanned = 0; scanned < textSideCount; scanned++) {
        TextSideEvent *e = &textSideQueue[readIdx];
        if (e->event.type >= minType && e->event.type <= maxType) {
            found = true;
            break;
        }
        readIdx = (readIdx + 1) % TEXT_SIDEQ_CAP;
    }
    pthread_mutex_unlock(&textSideMutex);
    return found;
}

void SDLCALL SDL_FlushEvent(Uint32 type)
{
    sideQueueRemoveRange(type, type);
    typedef void(SDLCALL * RealFunc)(Uint32);
    static RealFunc realFunc;
    CACHED_SYMBOL(RealFunc, realFunc, realSdlSymbol("SDL_FlushEvent"));
    cached(type);
}

SDL_bool SDLCALL SDL_HasEvent(Uint32 type)
{
    if (sideQueueHasRange(type, type))
        return SDL_TRUE;
    typedef SDL_bool(SDLCALL * RealFunc)(Uint32);
    static RealFunc realFunc;
    CACHED_SYMBOL(RealFunc, realFunc, realSdlSymbol("SDL_HasEvent"));
    return cached(type);
}

/* Range forms must consult the side queue too, so a flush/has spanning the
 * text-event types stays correct.
 */
void SDLCALL SDL_FlushEvents(Uint32 minType, Uint32 maxType)
{
    sideQueueRemoveRange(minType, maxType);
    typedef void(SDLCALL * RealFunc)(Uint32, Uint32);
    static RealFunc realFunc;
    CACHED_SYMBOL(RealFunc, realFunc, realSdlSymbol("SDL_FlushEvents"));
    cached(minType, maxType);
}

SDL_bool SDLCALL SDL_HasEvents(Uint32 minType, Uint32 maxType)
{
    if (sideQueueHasRange(minType, maxType))
        return SDL_TRUE;
    typedef SDL_bool(SDLCALL * RealFunc)(Uint32, Uint32);
    static RealFunc realFunc;
    CACHED_SYMBOL(RealFunc, realFunc, realSdlSymbol("SDL_HasEvents"));
    return cached(minType, maxType);
}

int SDLCALL SDL_PeepEvents(SDL_Event *events,
                           int numevents,
                           SDL_eventaction action,
                           Uint32 minType,
                           Uint32 maxType)
{
    typedef int(SDLCALL * RealFunc)(SDL_Event *, int, SDL_eventaction, Uint32,
                                    Uint32);
    static RealFunc realFunc;
    CACHED_SYMBOL(RealFunc, realFunc, realSdlSymbol("SDL_PeepEvents"));
    if (action == SDL_ADDEVENT) {
        /* SDL_ADDEVENT is the queue's other enqueue door; route inline-text
         * events through the side queue so they never reach sdl2-compat's
         * crashing translation. SDL_ADDEVENT does not run the event filter, so
         * use the raw enqueue here (unlike the SDL_PushEvent path).
         */
        if (!events)
            return cached(events, numevents, action, minType, maxType);
        int added = 0;
        for (int i = 0; i < numevents; i++) {
            int r;
            if (isTextEventType(events[i].type))
                r = sideQueueRawEnqueue(&events[i], false, NULL, NULL);
            else
                r = cached(&events[i], 1, SDL_ADDEVENT, minType, maxType);
            if (r < 0)
                return added > 0 ? added : r;
            added += r;
        }
        return added;
    }
    if (action != SDL_GETEVENT && action != SDL_PEEKEVENT)
        return cached(events, numevents, action, minType, maxType);

    /* events == NULL is SDL's count form: count matching side entries without
     * removing them (sideQueueDrain forces PEEK when events is NULL), then add
     * the real queue's count. numevents is per-SDL ignored for counting, so cap
     * the side scan at the ring size.
     */
    int sideMax = events ? numevents : TEXT_SIDEQ_CAP;
    bool reclaim = action == SDL_GETEVENT && sideQueueDrainShouldReclaim();
    int taken =
        sideQueueDrain(events, sideMax, action, minType, maxType, reclaim);
    int realNum = events ? numevents - taken : numevents;
    int got = cached(events ? events + taken : NULL, realNum, action, minType,
                     maxType);
    if (got < 0)
        return taken > 0 ? taken : got;
    return taken + got;
}
SDL_WRAP(SDL_bool,
         SDL_PixelFormatEnumToMasks,
         (Uint32 format,
          int *bpp,
          Uint32 *Rmask,
          Uint32 *Gmask,
          Uint32 *Bmask,
          Uint32 *Amask),
         (format, bpp, Rmask, Gmask, Bmask, Amask))
SDL_WRAP_VOID(SDL_PumpEvents, (void), ())
int SDLCALL SDL_PushEvent(SDL_Event *event)
{
    int diverted = sideQueueTextPush(event);
    if (diverted != INT_MIN)
        return diverted;
    typedef int(SDLCALL * RealFunc)(SDL_Event *);
    static RealFunc realFunc;
    CACHED_SYMBOL(RealFunc, realFunc, realSdlSymbol("SDL_PushEvent"));
    return cached(event);
}
SDL_WRAP(int,
         SDL_QueryTexture,
         (SDL_Texture * texture, Uint32 *format, int *access, int *w, int *h),
         (texture, format, access, w, h))
SDL_WRAP_VOID(SDL_Quit, (void), ())
SDL_WRAP_VOID(SDL_RaiseWindow, (SDL_Window * window), (window))
SDL_WRAP(SDL_RWops *,
         SDL_RWFromFile,
         (const char *file, const char *mode),
         (file, mode))
SDL_WRAP(int,
         SDL_SaveBMP_RW,
         (SDL_Surface * surface, SDL_RWops *dst, int freedst),
         (surface, dst, freedst))
SDL_WRAP(Uint32, SDL_RegisterEvents, (int numevents), (numevents))
SDL_WRAP(SDL_bool, SDL_RemoveTimer, (SDL_TimerID id), (id))
SDL_WRAP(int, SDL_RenderClear, (SDL_Renderer * renderer), (renderer))
SDL_WRAP(int,
         SDL_RenderCopy,
         (SDL_Renderer * renderer,
          SDL_Texture *texture,
          const SDL_Rect *srcrect,
          const SDL_Rect *dstrect),
         (renderer, texture, srcrect, dstrect))
SDL_WRAP(int,
         SDL_RenderDrawLine,
         (SDL_Renderer * renderer, int x1, int y1, int x2, int y2),
         (renderer, x1, y1, x2, y2))
SDL_WRAP(int,
         SDL_RenderDrawLines,
         (SDL_Renderer * renderer, const SDL_Point *points, int count),
         (renderer, points, count))
SDL_WRAP(int,
         SDL_RenderDrawPoint,
         (SDL_Renderer * renderer, int x, int y),
         (renderer, x, y))
SDL_WRAP(int,
         SDL_RenderDrawRect,
         (SDL_Renderer * renderer, const SDL_Rect *rect),
         (renderer, rect))
SDL_WRAP(int,
         SDL_RenderFillRect,
         (SDL_Renderer * renderer, const SDL_Rect *rect),
         (renderer, rect))
SDL_WRAP(int,
         SDL_RenderFillRects,
         (SDL_Renderer * renderer, const SDL_Rect *rects, int count),
         (renderer, rects, count))
SDL_WRAP_VOID(SDL_RenderGetViewport,
              (SDL_Renderer * renderer, SDL_Rect *rect),
              (renderer, rect))
SDL_WRAP_VOID(SDL_RenderPresent, (SDL_Renderer * renderer), (renderer))
SDL_WRAP(int,
         SDL_RenderReadPixels,
         (SDL_Renderer * renderer,
          const SDL_Rect *rect,
          Uint32 format,
          void *pixels,
          int pitch),
         (renderer, rect, format, pixels, pitch))
SDL_WRAP(int,
         SDL_RenderSetClipRect,
         (SDL_Renderer * renderer, const SDL_Rect *rect),
         (renderer, rect))
SDL_WRAP(int,
         SDL_RenderSetViewport,
         (SDL_Renderer * renderer, const SDL_Rect *rect),
         (renderer, rect))
SDL_WRAP_VOID(SDL_RestoreWindow, (SDL_Window * window), (window))
SDL_WRAP_VOID(SDL_SetCursor, (SDL_Cursor * cursor), (cursor))
SDL_WRAP(int, SDL_SetClipboardText, (const char *text), (text))
SDL_WRAP_VOID(SDL_SetEventFilter,
              (SDL_EventFilter filter, void *userdata),
              (filter, userdata))
SDL_WRAP(SDL_bool,
         SDL_SetHint,
         (const char *name, const char *value),
         (name, value))
SDL_WRAP_VOID(SDL_SetMainReady, (void), ())
SDL_WRAP(int,
         SDL_SetRenderDrawBlendMode,
         (SDL_Renderer * renderer, SDL_BlendMode blendMode),
         (renderer, blendMode))
SDL_WRAP(int,
         SDL_SetRenderDrawColor,
         (SDL_Renderer * renderer, Uint8 r, Uint8 g, Uint8 b, Uint8 a),
         (renderer, r, g, b, a))
SDL_WRAP(int,
         SDL_SetRenderTarget,
         (SDL_Renderer * renderer, SDL_Texture *texture),
         (renderer, texture))
#if SDL_VERSION_ATLEAST(2, 0, 22)
SDL_WRAP_VOID(SDL_SetTextInputRect, (const SDL_Rect *rect), (rect))
#else
SDL_WRAP_VOID(SDL_SetTextInputRect, (SDL_Rect * rect), (rect))
#endif
SDL_WRAP(int,
         SDL_SetTextureBlendMode,
         (SDL_Texture * texture, SDL_BlendMode blendMode),
         (texture, blendMode))
SDL_WRAP(int,
         SDL_SetTextureScaleMode,
         (SDL_Texture * texture, SDL_ScaleMode scaleMode),
         (texture, scaleMode))
#if SDL_VERSION_ATLEAST(2, 0, 16)
SDL_WRAP_VOID(SDL_SetWindowAlwaysOnTop,
              (SDL_Window * window, SDL_bool on_top),
              (window, on_top))
#endif
SDL_WRAP_VOID(SDL_SetWindowBordered,
              (SDL_Window * window, SDL_bool bordered),
              (window, bordered))
SDL_WRAP(int,
         SDL_SetWindowFullscreen,
         (SDL_Window * window, Uint32 flags),
         (window, flags))
#if SDL_VERSION_ATLEAST(2, 0, 5)
SDL_WRAP(int, SDL_SetWindowInputFocus, (SDL_Window * window), (window))
SDL_WRAP(int,
         SDL_SetWindowModalFor,
         (SDL_Window * modalWindow, SDL_Window *parentWindow),
         (modalWindow, parentWindow))
SDL_WRAP_VOID(SDL_SetWindowResizable,
              (SDL_Window * window, SDL_bool resizable),
              (window, resizable))
#endif
SDL_WRAP_VOID(SDL_SetWindowGrab,
              (SDL_Window * window, SDL_bool grabbed),
              (window, grabbed))
SDL_WRAP_VOID(SDL_SetWindowIcon,
              (SDL_Window * window, SDL_Surface *icon),
              (window, icon))
SDL_WRAP_VOID(SDL_SetWindowMaximumSize,
              (SDL_Window * window, int max_w, int max_h),
              (window, max_w, max_h))
SDL_WRAP_VOID(SDL_SetWindowMinimumSize,
              (SDL_Window * window, int min_w, int min_h),
              (window, min_w, min_h))
SDL_WRAP_VOID(SDL_SetWindowPosition,
              (SDL_Window * window, int x, int y),
              (window, x, y))
SDL_WRAP_VOID(SDL_SetWindowSize,
              (SDL_Window * window, int w, int h),
              (window, w, h))
SDL_WRAP_VOID(SDL_SetWindowTitle,
              (SDL_Window * window, const char *title),
              (window, title))
SDL_WRAP_VOID(SDL_ShowWindow, (SDL_Window * window), (window))
SDL_WRAP_VOID(SDL_StopTextInput, (void), ())
SDL_WRAP(SDL_threadID, SDL_ThreadID, (void), ())
SDL_WRAP(int, SDL_UnlockMutex, (SDL_mutex * mutex), (mutex))
SDL_WRAP_VOID(SDL_UnlockSurface, (SDL_Surface * surface), (surface))
SDL_WRAP(int,
         SDL_UpdateTexture,
         (SDL_Texture * texture,
          const SDL_Rect *rect,
          const void *pixels,
          int pitch),
         (texture, rect, pixels, pitch))
SDL_WRAP(int, SDL_UpdateWindowSurface, (SDL_Window * window), (window))
SDL_WRAP(int,
         SDL_UpdateWindowSurfaceRects,
         (SDL_Window * window, const SDL_Rect *rects, int numrects),
         (window, rects, numrects))
SDL_WRAP(int,
         SDL_UpperBlit,
         (SDL_Surface * src,
          const SDL_Rect *srcrect,
          SDL_Surface *dst,
          SDL_Rect *dstrect),
         (src, srcrect, dst, dstrect))
SDL_WRAP(int,
         SDL_UpperBlitScaled,
         (SDL_Surface * src,
          const SDL_Rect *srcrect,
          SDL_Surface *dst,
          SDL_Rect *dstrect),
         (src, srcrect, dst, dstrect))
int SDLCALL SDL_WaitEvent(SDL_Event *event)
{
    typedef int(SDLCALL * PeepFunc)(SDL_Event *, int, SDL_eventaction, Uint32,
                                    Uint32);
    static PeepFunc realPeep;
    CACHED_SYMBOL(PeepFunc, realPeep, realSdlSymbol("SDL_PeepEvents"));
    SDL_Event discard;
    SDL_Event *dst = event ? event : &discard;
    for (;;) {
        /* Return an already-queued side event before pumping, so unrelated host
         * events are not admitted ahead of it. reclaim = true: libX11's Xlib
         * drain does not run for a direct SDL_WaitEvent consumer, so the
         * push-time wake byte is reclaimed here. Real events come from
         * sdl2-compat's own queue, which never holds our side-queued text.
         */
        if (sideQueueDrain(dst, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT,
                           true) > 0)
            return 1;
        SDL_PumpEvents();
        int got = cached(dst, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT);
        if (got != 0)
            return got > 0 ? 1 : 0;
        SDL_Delay(1);
    }
}
SDL_WRAP(int, SDL_WarpMouseGlobal, (int x, int y), (x, y))
SDL_WRAP_VOID(SDL_WarpMouseInWindow,
              (SDL_Window * window, int x, int y),
              (window, x, y))
SDL_WRAP(Uint32, SDL_WasInit, (Uint32 flags), (flags))
SDL_WRAP_VOID(SDL_free, (void *mem), (mem))
SDL_WRAP(void *, SDL_memset, (void *dst, int c, size_t len), (dst, c, len))

#undef SDL_WRAP
#undef SDL_WRAP_VOID
