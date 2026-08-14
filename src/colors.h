#ifndef _COLORS_H_
#define _COLORS_H_

#include <X11/Xlib.h>

#include "sdl-compat.h"

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

/* Core X11 has no notion of alpha: pixel values written through Xlib are opaque
 * by definition and the upper byte is conventionally zero. SDL2 textures are
 * RGBA, so a literal copy renders X11 pixels as fully transparent. Promote
 * alpha == 0 to 0xFF to preserve the legacy contract. Trade-off: callers that
 * intentionally pass an ARGB pixel value with alpha == 0 (a non-core
 * Xrender/Composite usage) lose the transparency. Use the XRender path when
 * alpha is meaningful.
 */
static inline unsigned long colorWithOpaqueDefault(unsigned long color)
{
    /* Branchless promote of a zero alpha byte to 0xFF: the mask is all-ones
     * when the alpha byte is zero and all-zero otherwise, so the OR is a no-op
     * for pixels that already carry alpha. Staying branch-free lets the per
     * pixel XPutImage swizzle loops that call through here vectorize.
     */
    unsigned long alpha = 0xFFul << ALPHA_SHIFT;
    return color | (alpha & -(unsigned long) ((color & alpha) == 0));
}

/* Map a logical ARGB pixel onto an SDL surface or texture format, promoting an
 * unset alpha the way colorWithOpaqueDefault describes. Every writer of X pixel
 * values into SDL storage goes through here so the promotion cannot be
 * forgotten at one site and applied at another.
 */
static inline Uint32 mapColorToPixel(XcPixelFormat format, unsigned long color)
{
    color = colorWithOpaqueDefault(color);
    return SDL_MapRGBA(format, GET_RED_FROM_COLOR(color),
                       GET_GREEN_FROM_COLOR(color), GET_BLUE_FROM_COLOR(color),
                       GET_ALPHA_FROM_COLOR(color));
}

extern Colormap GREY_SCALE_COLORMAP;
extern Colormap REAL_COLOR_COLORMAP;

SDL_Color uLongToColor(XcPixelFormat pixelFormat, unsigned long color);
Bool initColorStorage(void);
void freeColorStorage(void);

#endif /* _COLORS_H_ */
