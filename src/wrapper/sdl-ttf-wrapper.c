#include <SDL_ttf.h>

#include "dlwrap.h"

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

static void *realTtfHandle(void)
{
    static void *handle;
    static const char *const candidates[] = {
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
    return dlwrapOpen(&handle, "LIBX11_COMPAT_REAL_SDL2_TTF", candidates,
                      "real SDL2_ttf");
}

static void *realTtfSymbol(const char *name)
{
    return dlwrapSym(realTtfHandle(), name, "SDL2_ttf");
}

#define TTF_WRAP(ret, name, args, callargs) \
    DLWRAP_THUNK(realTtfSymbol, ret, name, args, callargs)

#define TTF_WRAP_VOID(name, args, callargs) \
    DLWRAP_THUNK_VOID(realTtfSymbol, name, args, callargs)

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
