#ifndef LIBX11_COMPAT_SDL_TTF_COMPAT_H
#define LIBX11_COMPAT_SDL_TTF_COMPAT_H

/* Include chokepoint for SDL_ttf, mirroring sdl-compat.h. The default build is
 * a pass-through to SDL2_ttf; under LIBX11_COMPAT_SDL3 it reshapes the
 * SDL_ttf 3.x API into the 2.x spelling the tree was written against. The
 * inline wrappers preserve the SDL2_ttf return conventions (0 success / -1
 * error) that the call sites test against, and are defined before the renaming
 * #defines so a wrapper that reuses a surviving name still binds the real
 * function.
 */

#include "sdl-compat.h"

#ifdef LIBX11_COMPAT_SDL3

#include <SDL3_ttf/SDL_ttf.h>

static inline int xc_TTF_Init(void)
{
    return TTF_Init() ? 0 : -1;
}

static inline int xc_TTF_GlyphMetrics(TTF_Font *font,
                                      Uint32 ch,
                                      int *minx,
                                      int *maxx,
                                      int *miny,
                                      int *maxy,
                                      int *advance)
{
    return TTF_GetGlyphMetrics(font, ch, minx, maxx, miny, maxy, advance) ? 0
                                                                          : -1;
}

static inline int xc_TTF_SizeUTF8(TTF_Font *font,
                                  const char *text,
                                  int *w,
                                  int *h)
{
    return TTF_GetStringSize(font, text, 0, w, h) ? 0 : -1;
}

static inline SDL_Surface *xc_TTF_RenderUTF8_Solid(TTF_Font *font,
                                                   const char *text,
                                                   SDL_Color fg)
{
    return TTF_RenderText_Solid(font, text, 0, fg);
}

static inline SDL_Surface *xc_TTF_RenderUTF8_Blended(TTF_Font *font,
                                                     const char *text,
                                                     SDL_Color fg)
{
    return TTF_RenderText_Blended(font, text, 0, fg);
}

#define TTF_Init xc_TTF_Init
#define TTF_GlyphMetrics xc_TTF_GlyphMetrics
#define TTF_SizeUTF8 xc_TTF_SizeUTF8
#define TTF_RenderUTF8_Solid xc_TTF_RenderUTF8_Solid
#define TTF_RenderUTF8_Blended xc_TTF_RenderUTF8_Blended
#define TTF_FontAscent TTF_GetFontAscent
#define TTF_FontDescent TTF_GetFontDescent
#define TTF_FontFaceFamilyName TTF_GetFontFamilyName
#define TTF_FontFaceIsFixedWidth TTF_FontIsFixedWidth
#define TTF_GlyphIsProvided TTF_FontHasGlyph
#define TTF_GetError SDL_GetError

#else /* SDL2 */

#include <SDL2/SDL_ttf.h>

#endif /* LIBX11_COMPAT_SDL3 */

#ifndef SDL_TTF_VERSION_ATLEAST
#define SDL_TTF_VERSION_ATLEAST(x, y, z)                              \
    ((SDL_TTF_MAJOR_VERSION >= (x)) &&                                \
     (SDL_TTF_MAJOR_VERSION > (x) || SDL_TTF_MINOR_VERSION >= (y)) && \
     (SDL_TTF_MAJOR_VERSION > (x) || SDL_TTF_MINOR_VERSION > (y) ||   \
      SDL_TTF_PATCHLEVEL >= (z)))
#endif

static inline int xc_TTF_GlyphIsProvidedUcs4(TTF_Font *font, Uint32 ch)
{
#ifdef LIBX11_COMPAT_SDL3
    return TTF_FontHasGlyph(font, ch);
#elif SDL_TTF_VERSION_ATLEAST(2, 0, 18)
    return TTF_GlyphIsProvided32(font, ch);
#else
    if (ch > 0xffff)
        return 0;
    return TTF_GlyphIsProvided(font, (Uint16) ch);
#endif
}

#endif
