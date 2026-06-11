#include <SDL_ttf.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

/* See sdl-wrapper.c for the rationale and caveat. Same trigger set so
 * both shims downgrade together.
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

#ifdef TTF_GetError
#undef TTF_GetError
#endif

#ifndef SDL_TTF_VERSION_ATLEAST
#define SDL_TTF_VERSION_ATLEAST(x, y, z)                              \
    ((SDL_TTF_MAJOR_VERSION >= (x)) &&                                \
     (SDL_TTF_MAJOR_VERSION > (x) || SDL_TTF_MINOR_VERSION >= (y)) && \
     (SDL_TTF_MAJOR_VERSION > (x) || SDL_TTF_MINOR_VERSION > (y) ||   \
      SDL_TTF_PATCHLEVEL >= (z)))
#endif

/* See sdl-wrapper.c for the rationale: atomic load/store with
 * ACQUIRE/RELEASE ordering on the shared handle plus the per-wrapper
 * realFunc caches so racing first-callers are well-defined under the C
 * memory model.
 */
static void *realTtfHandle(void)
{
    static void *handle;
    void *cached = __atomic_load_n(&handle, __ATOMIC_ACQUIRE);
    if (cached)
        return cached;

    const char *override = getenv("LIBX11_COMPAT_REAL_SDL2_TTF");
    const char *candidates[] = {
#ifdef LIBX11_COMPAT_SDL2_TTF_DYLIB
        LIBX11_COMPAT_SDL2_TTF_DYLIB,
#endif
#if defined(__APPLE__)
        "libSDL2_ttf-2.0.0.dylib",
        "libSDL2_ttf.dylib",
#else
        "libSDL2_ttf-2.0.so.0",
        "libSDL2_ttf.so.0",
        "libSDL2_ttf.so",
#endif
        NULL,
    };

    void *opened = NULL;
    if (override && override[0])
        opened = dlopen(override, RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
    for (size_t i = 0; !opened && candidates[i]; i++)
        opened = dlopen(candidates[i], RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!opened) {
        fprintf(stderr, "libX11-compat: failed to load real SDL2_ttf: %s\n",
                dlerror());
        abort();
    }
    /* CAS so racing first-callers don't each pin an extra dlopen
     * refcount; loser dlcloses and uses the winner's handle. See
     * sdl-wrapper.c for the full rationale.
     */
    void *expected = NULL;
    if (__atomic_compare_exchange_n(&handle, &expected, opened, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return opened;
    dlclose(opened);
    return expected;
}

static void *realTtfSymbol(const char *name)
{
    void *symbol = dlsym(realTtfHandle(), name);
    if (!symbol) {
        fprintf(stderr,
                "libX11-compat: failed to resolve SDL2_ttf symbol %s: %s\n",
                name, dlerror());
        abort();
    }
    return symbol;
}

#define TTF_WRAP(ret, name, args, callargs)                             \
    ret SDLCALL name args                                               \
    {                                                                   \
        typedef ret(SDLCALL * RealFunc) args;                           \
        static RealFunc realFunc;                                       \
        RealFunc cached = __atomic_load_n(&realFunc, __ATOMIC_ACQUIRE); \
        if (!cached) {                                                  \
            cached = (RealFunc) realTtfSymbol(#name);                   \
            __atomic_store_n(&realFunc, cached, __ATOMIC_RELEASE);      \
        }                                                               \
        return cached callargs;                                         \
    }

#define TTF_WRAP_VOID(name, args, callargs)                             \
    void SDLCALL name args                                              \
    {                                                                   \
        typedef void(SDLCALL * RealFunc) args;                          \
        static RealFunc realFunc;                                       \
        RealFunc cached = __atomic_load_n(&realFunc, __ATOMIC_ACQUIRE); \
        if (!cached) {                                                  \
            cached = (RealFunc) realTtfSymbol(#name);                   \
            __atomic_store_n(&realFunc, cached, __ATOMIC_RELEASE);      \
        }                                                               \
        cached callargs;                                                \
    }

TTF_WRAP_VOID(TTF_CloseFont, (TTF_Font * font), (font))
TTF_WRAP(int, TTF_FontAscent, (const TTF_Font *font), (font))
TTF_WRAP(int, TTF_FontDescent, (const TTF_Font *font), (font))
#if SDL_TTF_VERSION_ATLEAST(2, 20, 0)
TTF_WRAP(const char *, TTF_FontFaceFamilyName, (const TTF_Font *font), (font))
#else
TTF_WRAP(char *, TTF_FontFaceFamilyName, (const TTF_Font *font), (font))
#endif
TTF_WRAP(int, TTF_FontFaceIsFixedWidth, (const TTF_Font *font), (font))
TTF_WRAP(const char *, TTF_GetError, (void), ())
TTF_WRAP(int, TTF_GetFontStyle, (const TTF_Font *font), (font))
#if SDL_TTF_VERSION_ATLEAST(2, 20, 0)
TTF_WRAP(int, TTF_GlyphIsProvided, (TTF_Font * font, Uint16 ch), (font, ch))
#else
TTF_WRAP(int,
         TTF_GlyphIsProvided,
         (const TTF_Font *font, Uint16 ch),
         (font, ch))
#endif
TTF_WRAP(int,
         TTF_GlyphMetrics,
         (TTF_Font * font,
          Uint16 ch,
          int *minx,
          int *maxx,
          int *miny,
          int *maxy,
          int *advance),
         (font, ch, minx, maxx, miny, maxy, advance))
TTF_WRAP(int, TTF_Init, (void), ())
TTF_WRAP(TTF_Font *,
         TTF_OpenFont,
         (const char *file, int ptsize),
         (file, ptsize))
TTF_WRAP_VOID(TTF_Quit, (void), ())
TTF_WRAP(SDL_Surface *,
         TTF_RenderUTF8_Blended,
         (TTF_Font * font, const char *text, SDL_Color fg),
         (font, text, fg))
TTF_WRAP(SDL_Surface *,
         TTF_RenderUTF8_Solid,
         (TTF_Font * font, const char *text, SDL_Color fg),
         (font, text, fg))
TTF_WRAP(int,
         TTF_SizeUTF8,
         (TTF_Font * font, const char *text, int *w, int *h),
         (font, text, w, h))
TTF_WRAP(int, TTF_WasInit, (void), ())

#undef TTF_WRAP
#undef TTF_WRAP_VOID
