#ifndef LIBX11_COMPAT_SDL_COMPAT_H
#define LIBX11_COMPAT_SDL_COMPAT_H

/* Single include chokepoint for the SDL core API. Every source file includes
 * this instead of <SDL2/SDL.h> so the SDL2 / SDL3 divergence lives in one
 * place. The default build (no LIBX11_COMPAT_SDL3) is a pass-through to SDL2
 * and must stay byte-for-byte equivalent to including <SDL2/SDL.h> directly.
 *
 * Under LIBX11_COMPAT_SDL3 this header reshapes the SDL3 API back into the SDL2
 * spelling the rest of the tree was written against. It covers only the symbols
 * this codebase actually uses; it is not a general SDL2-on-SDL3 shim. Two rules
 * keep it honest:
 *   1. inline wrappers that reuse an SDL3 function whose name collides with the
 *      SDL2 name are defined BEFORE the #define that renames the call sites, so
 *      the wrapper body still binds the real SDL3 symbol;
 *   2. only divergent symbols appear here; anything source-identical across
 *      SDL2 and SDL3 is left untouched.
 */

#ifdef LIBX11_COMPAT_SDL3

/* SDL3 ships an "old names" compatibility layer (SDL_oldnames.h) that aliases
 * the SDL2 spelling to the SDL3 name for everything that was a pure rename: the
 * SDLK_* / KMOD_* constants, the SDL_EVENT_* enum, atomics, cursor enums,
 * SDL_mutex, and so on. Enabling it carries the bulk of the migration so this
 * header only has to deal with the symbols whose SIGNATURE or semantics
 * changed. Where an old-names alias is actively wrong (it renames but does not
 * convert, e.g. SDL_RenderCopy's SDL_Rect -> SDL_FRect), we #undef it and
 * install a converting wrapper below.
 */
#define SDL_ENABLE_OLD_NAMES
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdbool.h>

/* SDL3 folded timers into core init and dropped SDL_INIT_TIMER. Define it to a
 * no-op bit so XOpenDisplay can pass SDL_INIT_VIDEO | SDL_INIT_TIMER unchanged
 * across both backends; on SDL3 the timer thread needs no subsystem init.
 */
#ifndef SDL_INIT_TIMER
#define SDL_INIT_TIMER 0
#endif

/* SDL2's headers transitively dragged in the libc declarations (stdlib, string)
 * that much of the tree relies on without including them directly. SDL3
 * deliberately stopped leaking libc, so re-provide the same surface here to
 * keep those source files compiling unchanged.
 */
#include <stdlib.h>
#include <string.h>

/* Pixel-format handle. SDL2 surfaces carry a SDL_PixelFormat* details struct;
 * SDL3 surfaces carry a SDL_PixelFormat enum and expose the details through
 * SDL_GetPixelFormatDetails. XcPixelFormat is the neutral handle the colour
 * helpers below consume, so call sites pass XC_SURFACE_FORMAT() or
 * xcAllocFormat rather than touching surface->format directly.
 */
typedef const SDL_PixelFormatDetails *XcPixelFormat;
#define XC_SURFACE_FORMAT(s) SDL_GetPixelFormatDetails((s)->format)
#define XC_SURFACE_FMT_ENUM(s) ((s)->format)
#define XC_SURFACE_BYTESPERPIXEL(s) \
    (SDL_GetPixelFormatDetails((s)->format)->bytes_per_pixel)
#define xcFreeFormat(f) ((void) (f))

static inline XcPixelFormat xcAllocFormat(SDL_PixelFormat format)
{
    return SDL_GetPixelFormatDetails(format);
}

/* Colour map/unmap: SDL3 added a palette argument and takes the details struct.
 * The neutral handle is already the details struct, so pass NULL for the
 * (unused, non-indexed) palette.
 */
static inline Uint32 xc_MapRGBA(XcPixelFormat f,
                                Uint8 r,
                                Uint8 g,
                                Uint8 b,
                                Uint8 a)
{
    return SDL_MapRGBA(f, NULL, r, g, b, a);
}

static inline Uint32 xc_MapRGB(XcPixelFormat f, Uint8 r, Uint8 g, Uint8 b)
{
    return SDL_MapRGB(f, NULL, r, g, b);
}

static inline void xc_GetRGB(Uint32 pixel,
                             XcPixelFormat f,
                             Uint8 *r,
                             Uint8 *g,
                             Uint8 *b)
{
    SDL_GetRGB(pixel, f, NULL, r, g, b);
}

static inline void xc_GetRGBA(Uint32 pixel,
                              XcPixelFormat f,
                              Uint8 *r,
                              Uint8 *g,
                              Uint8 *b,
                              Uint8 *a)
{
    SDL_GetRGBA(pixel, f, NULL, r, g, b, a);
}

/* Surface creation: SDL3 folds masks/depth into a single format enum. */
static inline SDL_Surface *xc_CreateRGBSurface(Uint32 flags,
                                               int width,
                                               int height,
                                               int depth,
                                               Uint32 rmask,
                                               Uint32 gmask,
                                               Uint32 bmask,
                                               Uint32 amask)
{
    (void) flags;
    return SDL_CreateSurface(
        width, height,
        SDL_GetPixelFormatForMasks(depth, rmask, gmask, bmask, amask));
}

static inline SDL_Surface *xc_CreateRGBSurfaceWithFormat(Uint32 flags,
                                                         int width,
                                                         int height,
                                                         int depth,
                                                         Uint32 format)
{
    (void) flags;
    (void) depth;
    return SDL_CreateSurface(width, height, (SDL_PixelFormat) format);
}

static inline SDL_Surface *xc_CreateRGBSurfaceFrom(void *pixels,
                                                   int width,
                                                   int height,
                                                   int depth,
                                                   int pitch,
                                                   Uint32 rmask,
                                                   Uint32 gmask,
                                                   Uint32 bmask,
                                                   Uint32 amask)
{
    return SDL_CreateSurfaceFrom(
        width, height,
        SDL_GetPixelFormatForMasks(depth, rmask, gmask, bmask, amask), pixels,
        pitch);
}

static inline SDL_Surface *xc_CreateRGBSurfaceWithFormatFrom(void *pixels,
                                                             int width,
                                                             int height,
                                                             int depth,
                                                             int pitch,
                                                             Uint32 format)
{
    (void) depth;
    return SDL_CreateSurfaceFrom(width, height, (SDL_PixelFormat) format,
                                 pixels, pitch);
}

/* Renderer: SDL3 renamed the copy/draw calls and moved geometry to floats.
 * These wrappers keep the SDL2 int return convention (0 success / -1 error) the
 * call sites test with `< 0` / `!= 0`.
 */
static inline int xc_RenderCopy(SDL_Renderer *renderer,
                                SDL_Texture *texture,
                                const SDL_Rect *src,
                                const SDL_Rect *dst)
{
    SDL_FRect fsrc, fdst;
    if (src) {
        fsrc.x = (float) src->x;
        fsrc.y = (float) src->y;
        fsrc.w = (float) src->w;
        fsrc.h = (float) src->h;
    }
    if (dst) {
        fdst.x = (float) dst->x;
        fdst.y = (float) dst->y;
        fdst.w = (float) dst->w;
        fdst.h = (float) dst->h;
    }
    return SDL_RenderTexture(renderer, texture, src ? &fsrc : NULL,
                             dst ? &fdst : NULL)
               ? 0
               : -1;
}

static inline int xc_RenderFillRect(SDL_Renderer *renderer,
                                    const SDL_Rect *rect)
{
    if (!rect)
        return SDL_RenderFillRect(renderer, NULL) ? 0 : -1;
    SDL_FRect f = {(float) rect->x, (float) rect->y, (float) rect->w,
                   (float) rect->h};
    return SDL_RenderFillRect(renderer, &f) ? 0 : -1;
}

static inline int xc_RenderFillRects(SDL_Renderer *renderer,
                                     const SDL_Rect *rects,
                                     int count)
{
    if (count <= 0)
        return 0;
    if (!rects || (size_t) count > SIZE_MAX / sizeof(SDL_FRect))
        return -1;
    SDL_FRect stackRects[64];
    SDL_FRect *frects =
        count <= 64 ? stackRects
                    : (SDL_FRect *) SDL_malloc(sizeof(SDL_FRect) * count);
    if (!frects)
        return -1;
    for (int i = 0; i < count; i++) {
        frects[i].x = (float) rects[i].x;
        frects[i].y = (float) rects[i].y;
        frects[i].w = (float) rects[i].w;
        frects[i].h = (float) rects[i].h;
    }
    bool ok = SDL_RenderFillRects(renderer, frects, count);
    if (frects != stackRects)
        SDL_free(frects);
    return ok ? 0 : -1;
}

static inline int xc_RenderDrawRect(SDL_Renderer *renderer,
                                    const SDL_Rect *rect)
{
    if (!rect)
        return SDL_RenderRect(renderer, NULL) ? 0 : -1;
    SDL_FRect f = {(float) rect->x, (float) rect->y, (float) rect->w,
                   (float) rect->h};
    return SDL_RenderRect(renderer, &f) ? 0 : -1;
}

/* SDL3 SDL_RenderReadPixels returns a fresh surface instead of filling caller
 * memory. Convert it back into the SDL2-style (format, pixels, pitch) buffer.
 */
static inline int xc_RenderReadPixels(SDL_Renderer *renderer,
                                      const SDL_Rect *rect,
                                      Uint32 format,
                                      void *pixels,
                                      int pitch)
{
    SDL_Surface *s = SDL_RenderReadPixels(renderer, rect);
    if (!s)
        return -1;
    int rc = -1;
    if (s->pixels && pixels) {
        /* The caller's buffer is sized for the requested rect at `pitch`; SDL3
         * may hand back a surface that differs (clip/scale). Clamp the convert
         * extent to what the destination can hold so a larger source cannot
         * overflow the caller's buffer.
         */
        int w = s->w;
        int h = s->h;
        if (rect) {
            if (rect->w < w)
                w = rect->w;
            if (rect->h < h)
                h = rect->h;
        }
        int bpp = SDL_BYTESPERPIXEL((SDL_PixelFormat) format);
        if (bpp > 0) {
            int dstMaxW = pitch / bpp;
            if (dstMaxW > 0 && dstMaxW < w)
                w = dstMaxW;
        }
        if (w > 0 && h > 0)
            rc = SDL_ConvertPixels(w, h, s->format, s->pixels, s->pitch,
                                   (SDL_PixelFormat) format, pixels, pitch)
                     ? 0
                     : -1;
    }
    SDL_DestroySurface(s);
    return rc;
}

/* Accelerated per-window renderer. SDL3 dropped the driver index and the
 * SDL_RENDERER_* flag word (renderer selection is by name, vsync is a separate
 * call), so ignore the SDL2 flags and request the best available renderer.
 * Returns NULL on failure, which drives the software present fallback.
 */
static inline SDL_Renderer *xc_CreateRenderer(SDL_Window *win, Uint32 flags)
{
    (void) flags;
    return SDL_CreateRenderer(win, NULL);
}
/* SDL3 dropped the SDL_RENDERER_* flag word; xc_CreateRenderer ignores flags
 * there, so this token is only a portable spelling for the call site.
 */
#define XC_RENDERER_ACCELERATED 0u

/* SDL3 flipped the success convention (int 0 -> bool true) for these calls, but
 * the call sites still test the SDL2 way (`< 0` / `!= 0` / `== 0`). Each
 * wrapper restores the int 0/-1 result. The four render entry points that
 * old-names already aliased are #undef'd below before being redefined.
 */
static inline int xc_SetRenderTarget(SDL_Renderer *renderer,
                                     SDL_Texture *texture)
{
    return SDL_SetRenderTarget(renderer, texture) ? 0 : -1;
}

static inline int xc_RenderDrawLine(SDL_Renderer *renderer,
                                    int x1,
                                    int y1,
                                    int x2,
                                    int y2)
{
    return SDL_RenderLine(renderer, (float) x1, (float) y1, (float) x2,
                          (float) y2)
               ? 0
               : -1;
}

static inline int xc_RenderDrawPoint(SDL_Renderer *renderer, int x, int y)
{
    return SDL_RenderPoint(renderer, (float) x, (float) y) ? 0 : -1;
}

static inline int xc_RenderSetClipRect(SDL_Renderer *renderer,
                                       const SDL_Rect *rect)
{
    return SDL_SetRenderClipRect(renderer, rect) ? 0 : -1;
}

static inline SDL_bool xc_RenderIsClipEnabled(SDL_Renderer *renderer)
{
    return SDL_RenderClipEnabled(renderer) ? SDL_TRUE : SDL_FALSE;
}

static inline void xc_RenderGetClipRect(SDL_Renderer *renderer, SDL_Rect *rect)
{
    SDL_GetRenderClipRect(renderer, rect);
}

static inline int xc_RenderSetViewport(SDL_Renderer *renderer,
                                       const SDL_Rect *rect)
{
    return SDL_SetRenderViewport(renderer, rect) ? 0 : -1;
}

static inline int xc_RenderSetScale(SDL_Renderer *renderer,
                                    float scaleX,
                                    float scaleY)
{
    return SDL_SetRenderScale(renderer, scaleX, scaleY) ? 0 : -1;
}

static inline int xc_LockSurface(SDL_Surface *surface)
{
    return SDL_LockSurface(surface) ? 0 : -1;
}

static inline int xc_UpdateTexture(SDL_Texture *texture,
                                   const SDL_Rect *rect,
                                   const void *pixels,
                                   int pitch)
{
    return SDL_UpdateTexture(texture, rect, pixels, pitch) ? 0 : -1;
}

static inline int xc_SaveBMP(SDL_Surface *surface, const char *file)
{
    return SDL_SaveBMP(surface, file) ? 0 : -1;
}

/* SDL3 dropped the x,y window-creation arguments; place via properties. */
static inline SDL_Window *xc_CreateWindow(const char *title,
                                          int x,
                                          int y,
                                          int w,
                                          int h,
                                          Uint64 flags)
{
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props)
        return NULL;
    if (title)
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                              title);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, x);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, y);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER,
                          (Sint64) flags);
    SDL_Window *win = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    return win;
}

/* SDL3 SDL_Init returns bool; map to the SDL2 int 0/-1 the call sites test. */
static inline int xc_Init(Uint32 flags)
{
    return SDL_Init(flags) ? 0 : -1;
}

/* SDL3 grew a modifier-state out-parameter on this query. */
static inline SDL_Scancode xc_GetScancodeFromKey(SDL_Keycode key)
{
    return SDL_GetScancodeFromKey(key, NULL);
}

/* SDL2 SDL_RenderDrawLines took SDL_Point; SDL3 SDL_RenderLines takes
 * SDL_FPoint, so the old-names alias is a type mismatch, not a rename.
 */
static inline int xc_RenderDrawLines(SDL_Renderer *renderer,
                                     const SDL_Point *points,
                                     int count)
{
    if (count <= 0)
        return 0;
    if (!points || (size_t) count > SIZE_MAX / sizeof(SDL_FPoint))
        return -1;
    SDL_FPoint stackPts[64];
    SDL_FPoint *fpts =
        count <= 64 ? stackPts
                    : (SDL_FPoint *) SDL_malloc(sizeof(SDL_FPoint) * count);
    if (!fpts)
        return -1;
    for (int i = 0; i < count; i++) {
        fpts[i].x = (float) points[i].x;
        fpts[i].y = (float) points[i].y;
    }
    bool ok = SDL_RenderLines(renderer, fpts, count);
    if (fpts != stackPts)
        SDL_free(fpts);
    return ok ? 0 : -1;
}

/* SDL3 SDL_BlitSurfaceScaled grew a scale-mode argument. */
static inline int xc_BlitScaled(SDL_Surface *src,
                                const SDL_Rect *srcrect,
                                SDL_Surface *dst,
                                SDL_Rect *dstrect)
{
    return SDL_BlitSurfaceScaled(src, srcrect, dst, dstrect,
                                 SDL_SCALEMODE_LINEAR)
               ? 0
               : -1;
}

/* SDL3 removed SDL_QueryTexture; size comes from SDL_GetTextureSize and the
 * format/access live on the texture properties.
 */
static inline int xc_QueryTexture(SDL_Texture *texture,
                                  Uint32 *format,
                                  int *access,
                                  int *w,
                                  int *h)
{
    /* Resolve everything that can fail before touching any out-param, so a
     * failed query leaves the caller's variables untouched.
     */
    float fw = 0.0f, fh = 0.0f;
    if (!SDL_GetTextureSize(texture, &fw, &fh))
        return -1;
    SDL_PropertiesID props = SDL_GetTextureProperties(texture);
    if (format)
        *format = (Uint32) SDL_GetNumberProperty(
            props, SDL_PROP_TEXTURE_FORMAT_NUMBER, SDL_PIXELFORMAT_UNKNOWN);
    if (access)
        *access = (int) SDL_GetNumberProperty(
            props, SDL_PROP_TEXTURE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STATIC);
    if (w)
        *w = (int) fw;
    if (h)
        *h = (int) fh;
    return 0;
}

/* --- Renames applied after the wrappers above are defined. Only symbols whose
 * signature or semantics changed appear here; the SDL_oldnames.h layer carries
 * every pure rename (SDLK_*, KMOD_*, SDL_EVENT_*, atomics, cursor enums, ...).
 * The four #undef'd entries are old-names aliases that rename without the
 * required type conversion.
 */
#undef SDL_RenderCopy
#define SDL_RenderCopy xc_RenderCopy
#undef SDL_RenderDrawRect
#define SDL_RenderDrawRect xc_RenderDrawRect
#undef SDL_RenderDrawLines
#define SDL_RenderDrawLines xc_RenderDrawLines
#undef SDL_BlitScaled
#define SDL_BlitScaled xc_BlitScaled
#undef SDL_RenderDrawLine
#define SDL_RenderDrawLine xc_RenderDrawLine
#undef SDL_RenderDrawPoint
#define SDL_RenderDrawPoint xc_RenderDrawPoint
#undef SDL_RenderSetClipRect
#define SDL_RenderSetClipRect xc_RenderSetClipRect
#undef SDL_RenderIsClipEnabled
#define SDL_RenderIsClipEnabled xc_RenderIsClipEnabled
#undef SDL_RenderGetClipRect
#define SDL_RenderGetClipRect xc_RenderGetClipRect
#undef SDL_RenderSetViewport
#define SDL_RenderSetViewport xc_RenderSetViewport
#undef SDL_RenderSetScale
#define SDL_RenderSetScale xc_RenderSetScale

#define SDL_SetRenderTarget xc_SetRenderTarget
#define SDL_LockSurface xc_LockSurface
#define SDL_UpdateTexture xc_UpdateTexture
#define SDL_SaveBMP xc_SaveBMP

#define SDL_MapRGBA xc_MapRGBA
#define SDL_MapRGB xc_MapRGB
#define SDL_GetRGB xc_GetRGB
#define SDL_GetRGBA xc_GetRGBA
#define SDL_CreateRGBSurface xc_CreateRGBSurface
#define SDL_CreateRGBSurfaceWithFormat xc_CreateRGBSurfaceWithFormat
#define SDL_CreateRGBSurfaceFrom xc_CreateRGBSurfaceFrom
#define SDL_CreateRGBSurfaceWithFormatFrom xc_CreateRGBSurfaceWithFormatFrom
#define SDL_RenderFillRect xc_RenderFillRect
#define SDL_RenderFillRects xc_RenderFillRects
#define SDL_RenderReadPixels xc_RenderReadPixels
#define SDL_QueryTexture xc_QueryTexture
#define SDL_CreateWindow xc_CreateWindow
#define SDL_Init xc_Init
#define SDL_GetScancodeFromKey(k) xc_GetScancodeFromKey(k)
#define SDL_SetWindowGrab SDL_SetWindowMouseGrab

/* SDL3 reports mouse coordinates as floats; the tree works in integer pixels.
 */
static inline SDL_MouseButtonFlags xc_GetMouseState(int *x, int *y)
{
    float fx = 0.0f, fy = 0.0f;
    SDL_MouseButtonFlags buttons = SDL_GetMouseState(&fx, &fy);
    if (x)
        *x = (int) fx;
    if (y)
        *y = (int) fy;
    return buttons;
}

static inline SDL_MouseButtonFlags xc_GetGlobalMouseState(int *x, int *y)
{
    float fx = 0.0f, fy = 0.0f;
    SDL_MouseButtonFlags buttons = SDL_GetGlobalMouseState(&fx, &fy);
    if (x)
        *x = (int) fx;
    if (y)
        *y = (int) fy;
    return buttons;
}

/* SDL3 SDL_WarpMouseGlobal returns bool; preserve the SDL2 int 0/-1 result. */
static inline int xc_WarpMouseGlobal(int x, int y)
{
    return SDL_WarpMouseGlobal((float) x, (float) y) ? 0 : -1;
}

/* libX11-compat tracks a per-display X event count (`qlen`) that the SDL event
 * filter bumps on push and the consume path drains. SDL_FlushEvent removes
 * events without consuming them; the SDL2 build reconciled this inside the
 * dlopen wrapper by calling the exported hook below for each removed event. The
 * SDL3 build links libSDL3 directly, so reconcile here instead. The lib never
 * calls SDL_FlushEvent itself, so this only fires for client flushes.
 */
extern void libx11CompatSideQueueEventRemoved(SDL_EventFilter filter,
                                              void *userdata);
extern int libx11CompatSdlPeepEventsIsXlibDrain(void);

/* Decrement the lib's per-display qlen (and drain a pipe byte) once per event
 * that left SDL's queue without going through the lib's own consume path.
 */
static inline void xc_DrainQlen(int count)
{
    if (count <= 0)
        return;
    SDL_EventFilter filter = NULL;
    void *userdata = NULL;
    if (!SDL_GetEventFilter(&filter, &userdata) || !filter)
        return;
    for (int i = 0; i < count; i++)
        libx11CompatSideQueueEventRemoved(filter, userdata);
}

/* SDL_PeepEvents with a NULL array is SDL's count form: it returns the exact
 * number of matching events without a stack buffer or truncation.
 */
static inline void xc_FlushEvent(Uint32 type)
{
    xc_DrainQlen(SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, type, type));
    SDL_FlushEvent(type);
}

static inline void xc_FlushEvents(Uint32 minType, Uint32 maxType)
{
    xc_DrainQlen(SDL_PeepEvents(NULL, 0, SDL_PEEKEVENT, minType, maxType));
    SDL_FlushEvents(minType, maxType);
}

/* Client retrieval via SDL_PeepEvents(GETEVENT) removes counted events; the
 * lib's own drain bumps a thread-local guard so only client calls reconcile.
 */
static inline int xc_PeepEvents(SDL_Event *events,
                                int numevents,
                                SDL_EventAction action,
                                Uint32 minType,
                                Uint32 maxType)
{
    int n = SDL_PeepEvents(events, numevents, action, minType, maxType);
    if (action == SDL_GETEVENT && n > 0 &&
        !libx11CompatSdlPeepEventsIsXlibDrain())
        xc_DrainQlen(n);
    return n;
}

static inline bool xc_WaitEvent(SDL_Event *event)
{
    bool r = SDL_WaitEvent(event);
    if (r && !libx11CompatSdlPeepEventsIsXlibDrain())
        xc_DrainQlen(1);
    return r;
}

static inline bool xc_PollEvent(SDL_Event *event)
{
    bool r = SDL_PollEvent(event);
    if (r && !libx11CompatSdlPeepEventsIsXlibDrain())
        xc_DrainQlen(1);
    return r;
}

/* SDL3 made text input per-window; target the focused window. */
static inline void xc_StopTextInput(void)
{
    SDL_Window *w = SDL_GetKeyboardFocus();
    if (w)
        SDL_StopTextInput(w);
}

static inline void xc_StartTextInput(void)
{
    SDL_Window *w = SDL_GetKeyboardFocus();
    if (w)
        SDL_StartTextInput(w);
}

static inline void xc_SetTextInputRect(const SDL_Rect *rect)
{
    SDL_Window *w = SDL_GetKeyboardFocus();
    if (w)
        SDL_SetTextInputArea(w, rect, 0);
}

/* SDL3 split the modal relationship into parent + modal flag. */
static inline int xc_SetWindowModalFor(SDL_Window *modal, SDL_Window *parent)
{
    if (parent) {
        SDL_SetWindowParent(modal, parent);
        SDL_SetWindowModal(modal, true);
    } else {
        SDL_SetWindowModal(modal, false);
        SDL_SetWindowParent(modal, NULL);
    }
    return 0;
}

/* SDL3 display-mode and display-count queries key off display IDs. */
static inline int xc_GetCurrentDisplayMode(int displayIndex,
                                           SDL_DisplayMode *mode)
{
    (void) displayIndex;
    const SDL_DisplayMode *m =
        SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!m)
        return -1;
    *mode = *m;
    return 0;
}

static inline int xc_GetNumVideoDisplays(void)
{
    int count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&count);
    if (displays)
        SDL_free(displays);
    return count;
}

/* SDL2 addressed displays by a contiguous index; SDL3 uses opaque display IDs,
 * so map the index through the live display list.
 */
static inline int xc_GetDesktopDisplayMode(int displayIndex,
                                           SDL_DisplayMode *mode)
{
    int count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&count);
    if (!displays || displayIndex < 0 || displayIndex >= count) {
        SDL_free(displays);
        return -1;
    }
    const SDL_DisplayMode *m =
        SDL_GetDesktopDisplayMode(displays[displayIndex]);
    SDL_free(displays);
    if (!m)
        return -1;
    *mode = *m;
    return 0;
}

#define SDL_GetMouseState xc_GetMouseState
#define SDL_GetGlobalMouseState xc_GetGlobalMouseState
#define SDL_WarpMouseGlobal xc_WarpMouseGlobal
#define SDL_FlushEvent xc_FlushEvent
#define SDL_FlushEvents xc_FlushEvents
#define SDL_PeepEvents xc_PeepEvents
#define SDL_WaitEvent xc_WaitEvent
#define SDL_PollEvent xc_PollEvent
#define SDL_StopTextInput() xc_StopTextInput()
#define SDL_StartTextInput() xc_StartTextInput()
#define SDL_SetTextInputRect(rect) xc_SetTextInputRect(rect)
#define SDL_SetWindowModalFor xc_SetWindowModalFor
#define SDL_GetCurrentDisplayMode xc_GetCurrentDisplayMode
#define SDL_GetDesktopDisplayMode xc_GetDesktopDisplayMode
#define SDL_GetNumVideoDisplays() xc_GetNumVideoDisplays()

/* SDL3 made SDL_ThreadID a type; the current-thread query is now a renamed
 * function. Map the call form without disturbing the (old-names) type alias.
 */
#define SDL_ThreadID() SDL_GetCurrentThreadID()

/* SDL3 removed SDL_SetWindowInputFocus; raising is the closest analogue. */
#undef SDL_SetWindowInputFocus
#define SDL_SetWindowInputFocus(win) SDL_RaiseWindow(win)

/* SDL3 SDL_SetWindowFullscreen takes a bool, not a flag word. */
#define XC_SetWindowFullscreen(win, on) SDL_SetWindowFullscreen((win), (on))

/* SDL3 event filters return bool; SDL3 timer callbacks gained a timer-id
 * argument and reordered their parameters. These let the callback definitions
 * stay backend-neutral.
 */
#define XC_EVENTFILTER_RET bool
#define XC_TIMER_CALLBACK_PARAMS \
    void *param, SDL_TimerID xcTimerID, Uint32 interval

/* Window/key event field access differs structurally; the helpers below let the
 * call sites stay backend-neutral.
 */
#define XC_WINDOW_SUBEVENT(ev) ((ev)->type)
#define XC_CASE_WINDOWEVENT \
    case SDL_EVENT_WINDOW_FIRST ... SDL_EVENT_WINDOW_LAST
#define XC_EVENT_KEYSYM(ev) ((ev)->key.key)
#define XC_EVENT_KEYMOD(ev) ((ev)->key.mod)
#define XC_EVENT_SET_KEYMOD(ev, v) ((ev)->key.mod = (v))
#define XC_EVENT_SET_KEYSYM(ev, v) ((ev)->key.key = (v))
#define XC_EVENT_SET_SCANCODE(ev, v) ((ev)->key.scancode = (v))
#define XC_EVENT_SET_KEY_PRESSED(ev, p) ((ev)->key.down = (p))
#define XC_EVENT_SET_BUTTON_PRESSED(ev, p) ((ev)->button.down = (p))
#define XC_EVENT_TIME_MS(ts) ((Uint32) SDL_NS_TO_MS(ts))
#define XC_NOW_EVENT_TS() SDL_GetTicksNS()

/* Construct an SDL window event (used by the test harness). In SDL3 the
 * sub-event IS the top-level type, so the placeholder init is a no-op and the
 * sub-event assignment writes the type.
 */
#define XC_INIT_WINDOW_EVENT(evptr) ((void) 0)
#define XC_SET_WINDOW_SUBEVENT(evptr, sub) ((evptr)->type = (sub))
#define XC_SET_WHEEL_Y(evptr, v) ((evptr)->wheel.y = (v))
#define XC_SET_WHEEL_X(evptr, v) ((evptr)->wheel.x = (v))

/* SDL3 has no global relative-mouse query; this library never enables relative
 * mode, so it is always off.
 */
#define SDL_GetRelativeMouseMode() false

/* SDL3 SDL_TextInputEvent.text is a const char* rather than an inline char[],
 * so a synthetic text event points at the caller-owned string (which must
 * outlive the push, e.g. a literal) instead of copying into the struct.
 */
#define XC_SET_TEXT_EVENT(ev, s) ((ev).text.text = (s))
#define XC_TEXT_EVENT_TEXT(evptr) ((evptr)->text.text)
#define XC_SET_EDITING_EVENT(ev, s) ((ev).edit.text = (s))
#define XC_EDITING_EVENT_TEXT(evptr) ((evptr)->edit.text)

/* SDL3 defaults texture scaling to linear; SDL2 defaulted to nearest. Pin glyph
 * textures explicitly where needed; window-surface blits use linear above so
 * Retina presentation does not pixel-double text.
 */
#define XC_SCALEMODE_NEAREST SDL_SCALEMODE_NEAREST
#define XC_SCALEMODE_LINEAR SDL_SCALEMODE_LINEAR

#else /* SDL2 */

#include <SDL2/SDL.h>

typedef SDL_PixelFormat *XcPixelFormat;
#define XC_SURFACE_FORMAT(s) ((s)->format)
#define XC_SURFACE_FMT_ENUM(s) ((s)->format->format)
#define XC_SURFACE_BYTESPERPIXEL(s) ((s)->format->BytesPerPixel)
#define xcAllocFormat(f) SDL_AllocFormat(f)
#define xcFreeFormat(f) SDL_FreeFormat(f)

/* Accelerated per-window renderer; see the SDL3 wrapper above. SDL2 keeps the
 * driver index (-1 = first supporting the flags) and the SDL_RENDERER_* flags.
 */
static inline SDL_Renderer *xc_CreateRenderer(SDL_Window *win, Uint32 flags)
{
    return SDL_CreateRenderer(win, -1, flags);
}
#define XC_RENDERER_ACCELERATED SDL_RENDERER_ACCELERATED

#define XC_WINDOW_SUBEVENT(ev) ((ev)->window.event)
#define XC_CASE_WINDOWEVENT case SDL_WINDOWEVENT
#define XC_EVENT_KEYSYM(ev) ((ev)->key.keysym.sym)
#define XC_EVENT_KEYMOD(ev) ((ev)->key.keysym.mod)
#define XC_EVENT_SET_KEYMOD(ev, v) ((ev)->key.keysym.mod = (v))
#define XC_EVENT_SET_KEYSYM(ev, v) ((ev)->key.keysym.sym = (v))
#define XC_EVENT_SET_SCANCODE(ev, v) ((ev)->key.keysym.scancode = (v))
#define XC_EVENT_SET_KEY_PRESSED(ev, p) \
    ((ev)->key.state = (p) ? SDL_PRESSED : SDL_RELEASED)
#define XC_EVENT_SET_BUTTON_PRESSED(ev, p) \
    ((ev)->button.state = (p) ? SDL_PRESSED : SDL_RELEASED)
#define XC_EVENT_TIME_MS(ts) ((Uint32) (ts))
#define XC_NOW_EVENT_TS() SDL_GetTicks()
#define XC_SetWindowFullscreen(win, on) \
    SDL_SetWindowFullscreen((win), (on) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0)
#define XC_EVENTFILTER_RET int
#define XC_TIMER_CALLBACK_PARAMS Uint32 interval, void *param
#define XC_INIT_WINDOW_EVENT(evptr) ((evptr)->type = SDL_WINDOWEVENT)
#define XC_SET_WINDOW_SUBEVENT(evptr, sub) ((evptr)->window.event = (sub))
#define XC_SET_WHEEL_Y(evptr, v) ((evptr)->wheel.preciseY = (v))
#define XC_SET_WHEEL_X(evptr, v) ((evptr)->wheel.preciseX = (v))
#define XC_SET_TEXT_EVENT(ev, s) \
    ((void) snprintf((ev).text.text, sizeof((ev).text.text), "%s", (s)))
#define XC_TEXT_EVENT_TEXT(evptr) ((evptr)->text.text)
#define XC_SET_EDITING_EVENT(ev, s) \
    ((void) snprintf((ev).edit.text, sizeof((ev).edit.text), "%s", (s)))
#define XC_EDITING_EVENT_TEXT(evptr) ((evptr)->edit.text)
#define XC_SCALEMODE_NEAREST SDL_ScaleModeNearest
#define XC_SCALEMODE_LINEAR SDL_ScaleModeLinear

#endif /* LIBX11_COMPAT_SDL3 */

#if defined(__linux__)
/* Native X11 window id of an SDL window, or 0 when it is not an X11 window (the
 * dummy driver). SDL2 implements this in the wrapper shim so SDL_syswm.h and
 * its X11 headers stay out of the compat Xlib sources; SDL3 exposes it through
 * window properties.
 */
#ifdef LIBX11_COMPAT_SDL3
static inline unsigned long sdlX11WindowHandle(SDL_Window *window)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props)
        return 0;
    return (unsigned long) SDL_GetNumberProperty(
        props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
}
#else
unsigned long sdlX11WindowHandle(SDL_Window *window);
#endif
#endif

/* Native NSWindow of an SDL window on macOS, or NULL when it is not a Cocoa
 * window (the dummy driver). Under SDL3 it is a window property; under SDL2 the
 * wrapper shim implements it so SDL_syswm.h and its Cocoa types stay out of the
 * compat sources (mirrors sdlX11WindowHandle above).
 */
#if defined(__APPLE__)
#ifdef LIBX11_COMPAT_SDL3
static inline void *sdlCocoaWindowHandle(SDL_Window *window)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    if (!props)
        return NULL;
    return SDL_GetPointerProperty(props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                                  NULL);
}
#else
void *sdlCocoaWindowHandle(SDL_Window *window);
#endif
#endif

#endif
