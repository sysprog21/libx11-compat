/*
 * No-op SDL_ttf stubs for the text-free wasm example builds (clock, moire,
 * mandel).
 *
 * Those examples draw with core Xlib only and never open a font, yet
 * XOpenDisplay's font-storage init pulls font.o/xft.o into the link, and those
 * reference the SDL_ttf functions below. Linking Emscripten's real SDL_ttf port
 * to satisfy them drags in freetype and harfbuzz (about 1.6 MB of the roughly
 * 2.7 MB artifact), so the text-free link drops those ports and uses these
 * stubs instead.
 *
 * TTF_Init reports success so XOpenDisplay proceeds and font storage
 * initializes (which needs no real face). Every face open returns NULL, so a
 * scalable-font path would fail gracefully rather than render garbage; the
 * text-free examples never reach it. An example that actually draws text (life)
 * keeps the full font stack instead of this stub. TTF_GetError is a macro for
 * SDL_GetError, which stays linked.
 */
#include <SDL2/SDL_ttf.h>

static int ttfInitialized = 0;

int TTF_Init(void)
{
    ttfInitialized = 1;
    return 0;
}

int TTF_WasInit(void)
{
    return ttfInitialized;
}

void TTF_Quit(void)
{
    ttfInitialized = 0;
}

TTF_Font *TTF_OpenFont(const char *file, int ptsize)
{
    (void) file;
    (void) ptsize;
    return NULL;
}

void TTF_CloseFont(TTF_Font *font)
{
    (void) font;
}

int TTF_FontAscent(const TTF_Font *font)
{
    (void) font;
    return 0;
}

int TTF_FontDescent(const TTF_Font *font)
{
    (void) font;
    return 0;
}

const char *TTF_FontFaceFamilyName(const TTF_Font *font)
{
    (void) font;
    return NULL;
}

int TTF_FontFaceIsFixedWidth(const TTF_Font *font)
{
    (void) font;
    return 0;
}

int TTF_GetFontStyle(const TTF_Font *font)
{
    (void) font;
    return TTF_STYLE_NORMAL;
}

int TTF_GlyphIsProvided(TTF_Font *font, Uint16 ch)
{
    (void) font;
    (void) ch;
    return 0;
}

int TTF_GlyphIsProvided32(TTF_Font *font, Uint32 ch)
{
    (void) font;
    (void) ch;
    return 0;
}

int TTF_GlyphMetrics(TTF_Font *font,
                     Uint16 ch,
                     int *minx,
                     int *maxx,
                     int *miny,
                     int *maxy,
                     int *advance)
{
    (void) font;
    (void) ch;
    if (minx)
        *minx = 0;
    if (maxx)
        *maxx = 0;
    if (miny)
        *miny = 0;
    if (maxy)
        *maxy = 0;
    if (advance)
        *advance = 0;
    return -1;
}

SDL_Surface *TTF_RenderUTF8_Solid(TTF_Font *font, const char *text, SDL_Color fg)
{
    (void) font;
    (void) text;
    (void) fg;
    return NULL;
}

SDL_Surface *TTF_RenderUTF8_Blended(TTF_Font *font,
                                    const char *text,
                                    SDL_Color fg)
{
    (void) font;
    (void) text;
    (void) fg;
    return NULL;
}

int TTF_SizeUTF8(TTF_Font *font, const char *text, int *w, int *h)
{
    (void) font;
    (void) text;
    if (w)
        *w = 0;
    if (h)
        *h = 0;
    return -1;
}
