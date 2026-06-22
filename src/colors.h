#ifndef _COLORS_H_
#define _COLORS_H_

#include "sdl-compat.h"
#include <X11/Xlib.h>

// #if SDL_BYTEORDER != SDL_BIG_ENDIAN
// #  define RED_SHIFT   24
// #  define GREEN_SHIFT 16
// #  define BLUE_SHIFT  8
// #  define ALPHA_SHIFT 0
// #else
// #  define RED_SHIFT   0
// #  define GREEN_SHIFT 8
// #  define BLUE_SHIFT  16
// #  define ALPHA_SHIFT 24
// #endif

/* Xlib-facing pixels are stored as logical ARGB values (A=24, R=16, G=8, B=0).
 * SDL render targets use SDL_PIXELFORMAT_RGBA8888, but every renderer write /
 * read goes through SDL_SetRenderDrawColor, SDL_GetRGBA, or explicit channel
 * extraction, so the logical pixel value is never memcpy'd as native RGBA
 * storage. Keeping these shifts endian-independent preserves X color semantics
 * while SDL handles its texture byte layout.
 */
#define RED_SHIFT 16
#define GREEN_SHIFT 8
#define BLUE_SHIFT 0
#define ALPHA_SHIFT 24

#define GET_RED_FROM_COLOR(color) ((Uint8) ((color >> RED_SHIFT) & 0xFF))
#define GET_GREEN_FROM_COLOR(color) ((Uint8) ((color >> GREEN_SHIFT) & 0xFF))
#define GET_BLUE_FROM_COLOR(color) ((Uint8) ((color >> BLUE_SHIFT) & 0xFF))
#define GET_ALPHA_FROM_COLOR(color) ((Uint8) ((color >> ALPHA_SHIFT) & 0xFF))

/* Core X11 has no notion of alpha: pixel values written through Xlib are
 * opaque by definition and the upper byte is conventionally zero. SDL2
 * textures are RGBA, so a literal copy renders X11 pixels as fully
 * transparent. Promote alpha == 0 to 0xFF to preserve the legacy
 * contract. Trade-off: callers that intentionally pass an ARGB pixel
 * value with alpha == 0 (a non-core Xrender/Composite usage) lose the
 * transparency. Use the XRender path when alpha is meaningful.
 */
static inline unsigned long colorWithOpaqueDefault(unsigned long color)
{
    if ((color & (0xFFul << ALPHA_SHIFT)) == 0)
        return color | (0xFFul << ALPHA_SHIFT);
    return color;
}

extern Colormap GREY_SCALE_COLORMAP;
extern Colormap REAL_COLOR_COLORMAP;

SDL_Color uLongToColor(XcPixelFormat pixelFormat, unsigned long color);
Bool initColorStorage(void);
void freeColorStorage(void);

#endif /* _COLORS_H_ */
