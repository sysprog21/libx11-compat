#include "X11/Xlib.h"
#include "X11/cursorfont.h"
#include "sdl-ttf-compat.h"
#include <limits.h>
#include <stdint.h>
#include "errors.h"
#include "drawing.h"
#include "colors.h"
#include "resource-types.h"
#include "display.h"
#include "window.h"

#define GET_CURSOR(cursorId) ((Cursor_ *) GET_XID_VALUE(cursorId))

typedef struct {
    SDL_Cursor *sdlCursor;
    int hotspot_x, hotspot_y;

    /* When non-NULL, srcBits/maskBits are stride-aligned packed bit arrays (1
     * bit per pixel) describing the original source and (optional) mask
     * pixmaps. XRecolorCursor uses them to rebuild the SDL color cursor with
     * new fg/bg colors.
     */
    unsigned char *srcBits;
    unsigned char *maskBits;
    int width;
    int height;
} Cursor_;

static Uint32 mapXColorOrDefault(XcPixelFormat format,
                                 const XColor *color,
                                 Uint8 fallback)
{
    Uint8 r = color ? (Uint8) (color->red >> 8) : fallback;
    Uint8 g = color ? (Uint8) (color->green >> 8) : fallback;
    Uint8 b = color ? (Uint8) (color->blue >> 8) : fallback;
    return SDL_MapRGBA(format, r, g, b, 0xFF);
}

static SDL_Cursor *buildColorCursorFromBits(const unsigned char *srcBits,
                                            const unsigned char *maskBits,
                                            int width,
                                            int height,
                                            int hotspot_x,
                                            int hotspot_y,
                                            const XColor *foreground_color,
                                            const XColor *background_color)
{
    if (!srcBits || width <= 0 || height <= 0)
        return NULL;
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);
    if (!surface)
        return NULL;
    Uint32 fg =
        mapXColorOrDefault(XC_SURFACE_FORMAT(surface), foreground_color, 0x00);
    Uint32 bg =
        mapXColorOrDefault(XC_SURFACE_FORMAT(surface), background_color, 0xFF);
    Uint32 transparent = SDL_MapRGBA(XC_SURFACE_FORMAT(surface), 0, 0, 0, 0);
    int stride = (width + 7) / 8;
    Uint32 *pixels = (Uint32 *) surface->pixels;
    int pitchInPixels = surface->pitch / 4;
    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            int byteIndex = py * stride + (px >> 3);
            int bitIndex = px & 7;
            unsigned int s = (srcBits[byteIndex] >> bitIndex) & 1u;
            unsigned int m =
                maskBits ? (maskBits[byteIndex] >> bitIndex) & 1u : 1u;
            Uint32 value = transparent;
            if (m != 0)
                value = s != 0 ? fg : bg;
            pixels[py * pitchInPixels + px] = value;
        }
    }
    SDL_Cursor *sdlCursor =
        SDL_CreateColorCursor(surface, hotspot_x, hotspot_y);
    SDL_FreeSurface(surface);
    return sdlCursor;
}

static unsigned char *packBitsFromImage(XImage *image)
{
    if (!image || image->width <= 0 || image->height <= 0)
        return NULL;

    /* width + 7 must not signed-overflow, and stride * height must fit in
     * size_t. Reject anything past those limits so the bits[py*stride +
     * (px>>3)] write below can never run past the end of the allocation.
     */
    if (image->width > INT_MAX - 7)
        return NULL;
    size_t stride = (size_t) ((image->width + 7) / 8);
    if (stride != 0 && (size_t) image->height > SIZE_MAX / stride)
        return NULL;
    size_t total = stride * (size_t) image->height;
    unsigned char *bits = calloc(total, 1);
    if (!bits)
        return NULL;
    for (int py = 0; py < image->height; py++) {
        for (int px = 0; px < image->width; px++) {
            unsigned long v = XGetPixel(image, px, py);
            if (v != 0)
                bits[(size_t) py * stride + (size_t) (px >> 3)] |=
                    (unsigned char) (1u << (px & 7));
        }
    }
    return bits;
}

static SDL_SystemCursor systemCursorForFontShape(unsigned int shape)
{
    switch (shape) {
    case XC_xterm:
        return SDL_SYSTEM_CURSOR_IBEAM;
    case XC_watch:
    case XC_clock:
        return SDL_SYSTEM_CURSOR_WAIT;
    case XC_cross:
    case XC_crosshair:
    case XC_tcross:
        return SDL_SYSTEM_CURSOR_CROSSHAIR;
    case XC_hand1:
    case XC_hand2:
        return SDL_SYSTEM_CURSOR_HAND;
    case XC_fleur:
    case XC_sizing:
        return SDL_SYSTEM_CURSOR_SIZEALL;
    case XC_sb_h_double_arrow:
    case XC_left_side:
    case XC_right_side:
        return SDL_SYSTEM_CURSOR_SIZEWE;
    case XC_sb_v_double_arrow:
    case XC_top_side:
    case XC_bottom_side:
        return SDL_SYSTEM_CURSOR_SIZENS;
    case XC_top_left_corner:
    case XC_bottom_right_corner:
        return SDL_SYSTEM_CURSOR_SIZENWSE;
    case XC_top_right_corner:
    case XC_bottom_left_corner:
        return SDL_SYSTEM_CURSOR_SIZENESW;
    default:
        return SDL_SYSTEM_CURSOR_ARROW;
    }
}

static Cursor allocateCursor(Display *display,
                             SDL_Cursor *sdlCursor,
                             unsigned int x,
                             unsigned int y)
{
    XID cursorId = ALLOC_XID();
    Cursor_ *cursor = cursorId != None ? calloc(1, sizeof(Cursor_)) : NULL;
    if (cursorId == None || !cursor) {
        if (sdlCursor)
            SDL_FreeCursor(sdlCursor);
        if (cursorId != None)
            FREE_XID(cursorId);
        handleOutOfMemory(0, display, 0, 0);
        return None;
    }
    SET_XID_TYPE(cursorId, CURSOR);
    cursor->sdlCursor = sdlCursor;
    cursor->hotspot_x = (int) x;
    cursor->hotspot_y = (int) y;
    SET_XID_VALUE(cursorId, cursor);
    return cursorId;
}

static Cursor createPixmapCursor(Display *display,
                                 SDL_Texture *source,
                                 SDL_Texture *mask,
                                 _Xconst XColor *foreground_color,
                                 _Xconst XColor *background_color,
                                 unsigned int x,
                                 unsigned int y)
{
    /* Custom cursor bitmaps are not yet rendered; fall back to the system arrow
     * so the call always succeeds with a usable cursor.
     */
    (void) source;
    (void) mask;
    (void) foreground_color;
    (void) background_color;
    SDL_Cursor *sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    return allocateCursor(display, sdlCursor, x, y);
}

Cursor XCreatePixmapCursor(Display *display,
                           Pixmap source,
                           Pixmap mask,
                           XColor *foreground_color,
                           XColor *background_color,
                           unsigned int x,
                           unsigned int y)
{
    // https://tronche.com/gui/x/xlib/pixmap-and-cursor/XCreatePixmapCursor.html
    SET_X_SERVER_REQUEST(display, X_CreateCursor);
    TYPE_CHECK(source, PIXMAP, display, None);

    /* Validate the mask up front so any error path runs before the srcImg
     * allocation below.
     */
    if (mask != None)
        TYPE_CHECK(mask, PIXMAP, display, None);

    /* Pull the source (and optional mask) bits out via XGetImage so the
     * conversion is independent of whether the pixmap lives in a renderer or a
     * surface.
     */
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(source, renderer);
    if (!renderer)
        return None;
    SDL_Rect viewport;
    SDL_RenderGetViewport(renderer, &viewport);
    int width = viewport.w;
    int height = viewport.h;
    if (width <= 0 || height <= 0)
        return None;

    /* The X11 spec requires the source and mask pixmaps to have the same
     * dimensions. XGetImage trusts the (x, y, w, h) tuple, so a smaller mask
     * would walk off its backing surface; reject the call here instead.
     */
    if (mask != None) {
        SDL_Texture *maskTexture = GET_PIXMAP_TEXTURE(mask);
        int maskWidth = 0;
        int maskHeight = 0;
        if (!maskTexture ||
            SDL_QueryTexture(maskTexture, NULL, NULL, &maskWidth,
                             &maskHeight) != 0 ||
            maskWidth != width || maskHeight != height) {
            handleError(0, display, mask, 0, BadMatch, 0);
            return None;
        }
    }

    XImage *srcImg =
        XGetImage(display, source, 0, 0, width, height, AllPlanes, ZPixmap);
    if (!srcImg)
        return None;
    XImage *maskImg = NULL;
    if (mask != None) {
        maskImg =
            XGetImage(display, mask, 0, 0, width, height, AllPlanes, ZPixmap);
    }

    unsigned char *srcBits = packBitsFromImage(srcImg);
    unsigned char *maskBits = maskImg ? packBitsFromImage(maskImg) : NULL;
    srcImg->f.destroy_image(srcImg);
    if (maskImg)
        maskImg->f.destroy_image(maskImg);
    if (!srcBits) {
        free(maskBits);
        return None;
    }

    SDL_Cursor *sdlCursor =
        buildColorCursorFromBits(srcBits, maskBits, width, height, (int) x,
                                 (int) y, foreground_color, background_color);
    if (!sdlCursor)
        sdlCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
    Cursor cursorId = allocateCursor(display, sdlCursor, x, y);
    if (cursorId == None) {
        free(srcBits);
        free(maskBits);
        return None;
    }
    Cursor_ *c = GET_CURSOR(cursorId);
    c->srcBits = srcBits;
    c->maskBits = maskBits;
    c->width = width;
    c->height = height;
    return cursorId;
}

Cursor XCreateGlyphCursor(Display *display,
                          Font source_font,
                          Font mask_font,
                          unsigned int source_char,
                          unsigned int mask_char,
                          XColor _Xconst *foreground_color,
                          XColor _Xconst *background_color)
{
    // https://tronche.com/gui/x/xlib/pixmap-and-cursor/XCreateGlyphCursor.html
    SET_X_SERVER_REQUEST(display, X_CreateGlyphCursor);

    /* Glyph rasterization is not implemented; fall back to the same arrow
     * placeholder as createPixmapCursor's NULL-source path.
     */
    (void) source_font;
    (void) mask_font;
    (void) source_char;
    (void) mask_char;
    return createPixmapCursor(display, NULL, NULL, foreground_color,
                              background_color, 0, 0);
}

int XFreeCursor(Display *display, Cursor cursor)
{
    // https://tronche.com/gui/x/xlib/pixmap-and-cursor/XFreeCursor.html
    SET_X_SERVER_REQUEST(display, X_FreeCursor);
    TYPE_CHECK(cursor, CURSOR, display, 0);
    Cursor_ *c = GET_CURSOR(cursor);
    SDL_FreeCursor(c->sdlCursor);
    free(c->srcBits);
    free(c->maskBits);
    free(c);
    FREE_XID(cursor);
    return 1;
}

int XDefineCursor(Display *display, Window window, Cursor cursor)
{
    // https://tronche.com/gui/x/xlib/window/XDefineCursor.html
    SET_X_SERVER_REQUEST(display, X_ChangeWindowAttributes);
    TYPE_CHECK(window, WINDOW, display, 0);
    if (cursor != None)
        TYPE_CHECK(cursor, CURSOR, display, 0);
    GET_WINDOW_STRUCT(window)->cursor = cursor;
    if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
        SDL_Cursor *sdlCursor =
            cursor == None ? NULL : GET_CURSOR(cursor)->sdlCursor;
        SDL_SetCursor(sdlCursor ? sdlCursor : SDL_GetDefaultCursor());
    }
    return 1;
}

int XUndefineCursor(Display *display, Window window)
{
    // https://tronche.com/gui/x/xlib/window/XUndefineCursor.html
    return XDefineCursor(display, window, None);
}

Cursor XCreateFontCursor(Display *display, unsigned int shape)
{
    // https://tronche.com/gui/x/xlib/pixmap-and-cursor/XCreateFontCursor.html
    SET_X_SERVER_REQUEST(display, X_CreateGlyphCursor);
    SDL_Cursor *sdlCursor =
        SDL_CreateSystemCursor(systemCursorForFontShape(shape));
    return allocateCursor(display, sdlCursor, 0, 0);
}

int XRecolorCursor(Display *display,
                   Cursor cursor,
                   XColor *foreground,
                   XColor *background)
{
    // https://tronche.com/gui/x/xlib/pixmap-and-cursor/XRecolorCursor.html
    SET_X_SERVER_REQUEST(display, X_RecolorCursor);
    TYPE_CHECK(cursor, CURSOR, display, 0);
    Cursor_ *c = GET_CURSOR(cursor);

    /* Without the original bits (font / system / fallback cursors) the cursor
     * cannot be recolored; preserve the existing cursor and report success.
     */
    if (!c->srcBits)
        return 1;
    SDL_Cursor *rebuilt = buildColorCursorFromBits(
        c->srcBits, c->maskBits, c->width, c->height, c->hotspot_x,
        c->hotspot_y, foreground, background);
    if (!rebuilt)
        return 1;
    SDL_Cursor *previous = c->sdlCursor;
    c->sdlCursor = rebuilt;

    /* Re-attach the new cursor everywhere the old one was active. SDL tracks
     * one "current" cursor, so a SetCursor is enough.
     */
    if (SDL_GetCursor() == previous)
        SDL_SetCursor(rebuilt);
    if (previous)
        SDL_FreeCursor(previous);
    return 1;
}
