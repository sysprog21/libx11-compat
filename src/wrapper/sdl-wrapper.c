#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

/* ASan (and other interceptor-based sanitizers) aborts on dlopen with
 * RTLD_DEEPBIND because deep binding bypasses their symbol interception. Drop
 * the flag in sanitizer builds. The escape hatch LIBX11_COMPAT_NO_RTLD_DEEPBIND
 * lets the build system force the same downgrade when a sanitizer variant the
 * wrapper does not auto-detect is in play.
 *
 * Caveat: RTLD_DEEPBIND was protecting against the real libSDL2 looking up
 * SDL_* symbols via RTLD_DEFAULT and finding this wrapper's exports instead
 * (which would recurse). RTLD_LOCAL on the dlopen sites doesn't fully replace
 * that; it only hides the loaded SDL's symbols from later lookups, it doesn't
 * stop libSDL2 itself from peeking back into the global scope where the wrapper
 * lives. In practice libSDL2 calls its own internal symbols directly (resolved
 * against its own .so at its link time) rather than via RTLD_DEFAULT, so the
 * recursion has not been observed. If a future SDL release reintroduces
 * RTLD_DEFAULT lookups for its own symbols, sanitizer runs will need to link
 * the real libSDL2 directly and skip the wrapper.
 */
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(LIBX11_COMPAT_NO_RTLD_DEEPBIND) ||                               \
    defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) ||         \
    defined(__SANITIZE_HWADDRESS__) || __has_feature(address_sanitizer) ||   \
    __has_feature(hwaddress_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#undef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

/* The lazy caches below (the shared handle plus the per-wrapper realFunc
 * statics expanded by the SDL_WRAP macros) are accessed via __atomic_load_n /
 * __atomic_store_n with ACQUIRE/RELEASE ordering so concurrent first calls have
 * a well-defined outcome under the C memory model rather than UB. The race is
 * benign in practice because dlopen reference-counts the underlying library and
 * dlsym is idempotent, so both racers see the same handle and pointer; the
 * atomics give the compiler permission to reason about it instead of folding
 * the load into a single read.
 */
static void *realSdlHandle(void)
{
    static void *handle;
    void *cached = __atomic_load_n(&handle, __ATOMIC_ACQUIRE);
    if (cached)
        return cached;

    const char *override = getenv("LIBX11_COMPAT_REAL_SDL2");
    const char *candidates[] = {
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

    void *opened = NULL;
    if (override && override[0])
        opened = dlopen(override, RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
    for (size_t i = 0; !opened && candidates[i]; i++)
        opened = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!opened) {
        fprintf(stderr, "libX11-compat: failed to load real SDL2: %s\n",
                dlerror());
        abort();
    }
    /* Publish via CAS so racing first-callers don't each keep their own dlopen
     * refcount alive. The loser dlcloses to balance its dlopen and uses the
     * winner's handle. POSIX dlopen reference-counts the underlying library, so
     * the loser's dlclose just decrements that count back to where the winner
     * left it.
     */
    void *expected = NULL;
    if (__atomic_compare_exchange_n(&handle, &expected, opened, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return opened;
    dlclose(opened);
    return expected;
}

static void *realSdlSymbol(const char *name)
{
    void *symbol = dlsym(realSdlHandle(), name);
    if (!symbol) {
        fprintf(stderr, "libX11-compat: failed to resolve SDL2 symbol %s: %s\n",
                name, dlerror());
        abort();
    }
    return symbol;
}

#define SDL_WRAP(ret, name, args, callargs)                             \
    ret SDLCALL name args                                               \
    {                                                                   \
        typedef ret(SDLCALL * RealFunc) args;                           \
        static RealFunc realFunc;                                       \
        RealFunc cached = __atomic_load_n(&realFunc, __ATOMIC_ACQUIRE); \
        if (!cached) {                                                  \
            cached = (RealFunc) realSdlSymbol(#name);                   \
            __atomic_store_n(&realFunc, cached, __ATOMIC_RELEASE);      \
        }                                                               \
        return cached callargs;                                         \
    }

#define SDL_WRAP_VOID(name, args, callargs)                             \
    void SDLCALL name args                                              \
    {                                                                   \
        typedef void(SDLCALL * RealFunc) args;                          \
        static RealFunc realFunc;                                       \
        RealFunc cached = __atomic_load_n(&realFunc, __ATOMIC_ACQUIRE); \
        if (!cached) {                                                  \
            cached = (RealFunc) realSdlSymbol(#name);                   \
            __atomic_store_n(&realFunc, cached, __ATOMIC_RELEASE);      \
        }                                                               \
        cached callargs;                                                \
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
SDL_WRAP_VOID(SDL_FlushEvent, (Uint32 type), (type))
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
SDL_WRAP(SDL_bool, SDL_HasEvent, (Uint32 type), (type))
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
SDL_WRAP(int,
         SDL_PeepEvents,
         (SDL_Event * events,
          int numevents,
          SDL_eventaction action,
          Uint32 minType,
          Uint32 maxType),
         (events, numevents, action, minType, maxType))
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
SDL_WRAP(int, SDL_PushEvent, (SDL_Event * event), (event))
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
SDL_WRAP(int, SDL_WaitEvent, (SDL_Event * event), (event))
SDL_WRAP(int, SDL_WarpMouseGlobal, (int x, int y), (x, y))
SDL_WRAP_VOID(SDL_WarpMouseInWindow,
              (SDL_Window * window, int x, int y),
              (window, x, y))
SDL_WRAP(Uint32, SDL_WasInit, (Uint32 flags), (flags))
SDL_WRAP_VOID(SDL_free, (void *mem), (mem))
SDL_WRAP(void *, SDL_memset, (void *dst, int c, size_t len), (dst, c, len))

#undef SDL_WRAP
#undef SDL_WRAP_VOID
