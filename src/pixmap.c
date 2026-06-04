#include "X11/Xlib.h"
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include "drawing.h"
#include "errors.h"
#include "resource-types.h"
#include "display.h"
#include "colors.h"

Pixmap XCreatePixmapFromBitmapData(Display *display,
                                   Drawable d,
                                   char *data,
                                   unsigned int width,
                                   unsigned int height,
                                   unsigned long fg,
                                   unsigned long bg,
                                   unsigned int depth);

static Bool isSupportedPixmapDepth(unsigned int depth)
{
    return depth == 1 || depth == 16 || depth == 24 || depth == 32;
}

static Uint32 mapPixel(SDL_PixelFormat *format, unsigned long pixel)
{
    return SDL_MapRGBA(format, GET_RED_FROM_COLOR(pixel),
                       GET_GREEN_FROM_COLOR(pixel), GET_BLUE_FROM_COLOR(pixel),
                       GET_ALPHA_FROM_COLOR(pixel));
}

static Bool bitmapBitIsSet(const char *data,
                           unsigned int x,
                           unsigned int y,
                           unsigned int width)
{
    /* Caller validates width > 0 and width <= UINT_MAX - 7 before calling. */
    unsigned int bytesPerRow = (width + 7) / 8;
    unsigned char byte = (unsigned char) data[(size_t) y * bytesPerRow + x / 8];
    return (byte & (1u << (x % 8))) != 0;
}

static Pixmap createPixmapFromPixels(Display *display,
                                     unsigned int width,
                                     unsigned int height,
                                     const Uint32 *pixels,
                                     unsigned int depth)
{
    Pixmap pixmap = XCreatePixmap(display, SCREEN_WINDOW, width, height, depth);
    if (pixmap == None) {
        return None;
    }
    SDL_Texture *texture = GET_PIXMAP_TEXTURE(pixmap);
    /* SDL_UpdateTexture takes pitch as int; reject widths whose RGBA8888
     * row size would not fit, before the implicit cast truncates it.
     */
    if (width > (unsigned int) INT_MAX / sizeof(Uint32)) {
        XFreePixmap(display, pixmap);
        return None;
    }
    if (SDL_UpdateTexture(texture, NULL, pixels,
                          (int) (width * sizeof(Uint32))) != 0) {
        LOG("SDL_UpdateTexture failed in %s: %s\n", __func__, SDL_GetError());
        XFreePixmap(display, pixmap);
        return None;
    }
    return pixmap;
}

Pixmap XCreatePixmap(Display *display,
                     Drawable drawable,
                     unsigned int width,
                     unsigned int height,
                     unsigned int depth)
{
    // https://tronche.com/gui/x/xlib/pixmap-and-cursor/XCreatePixmap.html
    SET_X_SERVER_REQUEST(display, X_CreatePixmap);
    (void) drawable;
    if (width == 0 || height == 0) {
        LOG("Width and/or height are 0 in XCreatePixmap: w = %u, h = %u\n",
            width, height);
        handleError(0, display, None, 0, BadValue, 0);
        return None;
    }
    if (!isSupportedPixmapDepth(depth)) {
        LOG("Got unsupported depth (%u) in XCreatePixmap\n", depth);
        handleError(0, display, None, 0, BadValue, 0);
        return None;
    }
    XID pixmap = ALLOC_XID();
    if (pixmap == None) {
        LOG("Out of memory: Could not allocate XID in XCreatePixmap!\n");
        handleOutOfMemory(0, display, 0, 0);
        return None;
    }
    LOG("%s: addr= %lu, w = %d, h = %d\n", __func__, pixmap, width, height);
    PixmapStruct *pixmapStruct = malloc(sizeof(PixmapStruct));
    if (!pixmapStruct) {
        FREE_XID(pixmap);
        handleOutOfMemory(0, display, 0, 0);
        return None;
    }
    SDL_Texture *texture = SDL_CreateTexture(
        GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer, SDL_PIXELFORMAT_RGBA8888,
        SDL_TEXTUREACCESS_TARGET, (int) width, (int) height);
    if (!texture) {
        fprintf(stderr, "SDL_CreateTexture failed in XCreatePixmap: %s\n",
                SDL_GetError());
        free(pixmapStruct);
        FREE_XID(pixmap);
        handleOutOfMemory(0, display, 0, 0);
        return None;
    }
    pixmapStruct->texture = texture;
    pixmapStruct->width = width;
    pixmapStruct->height = height;
    pixmapStruct->depth = depth;
    SET_XID_TYPE(pixmap, PIXMAP);
    SET_XID_VALUE(pixmap, pixmapStruct);

    SDL_Renderer *renderer;
    GET_RENDERER(pixmap, renderer);
    applySdlDrawState(renderer, NULL, SDL_BLENDMODE_NONE, 0);
    SDL_RenderClear(renderer);
    return pixmap;
}

int XFreePixmap(Display *display, Pixmap pixmap)
{
    // https://tronche.com/gui/x/xlib/pixmap-and-cursor/XFreePixmap.html
    SET_X_SERVER_REQUEST(display, X_FreePixmap);
    TYPE_CHECK(pixmap, PIXMAP, display, 0);
    PixmapStruct *pixmapStruct = GET_PIXMAP_STRUCT(pixmap);
    FREE_XID(pixmap);
    SDL_DestroyTexture(pixmapStruct->texture);
    free(pixmapStruct);
    return 1;
}

Pixmap XCreateBitmapFromData(Display *display,
                             Drawable d,
                             _Xconst char *data,
                             unsigned int width,
                             unsigned int height)
{
    return XCreatePixmapFromBitmapData(display, d, (char *) data, width, height,
                                       0xFFFFFFFF, 0xFF000000, 1);
}

Pixmap XCreatePixmapFromBitmapData(Display *display,
                                   Drawable d,
                                   char *data,
                                   unsigned int width,
                                   unsigned int height,
                                   unsigned long fg,
                                   unsigned long bg,
                                   unsigned int depth)
{
    SET_X_SERVER_REQUEST(display, X_CreatePixmap);
    TYPE_CHECK(d, DRAWABLE, display, None);
    if (width == 0 || height == 0 || !data) {
        handleError(0, display, None, 0, BadValue, 0);
        return None;
    }
    if (!isSupportedPixmapDepth(depth)) {
        handleError(0, display, None, 0, BadValue, 0);
        return None;
    }

    /* Guard against width*height overflow before alloc, and against
     * (width + 7) wrapping inside bitmapBitIsSet. Both feed into pointer
     * arithmetic that would otherwise walk off the heap. */
    if (width > UINT_MAX - 7 ||
        width > SIZE_MAX / sizeof(Uint32) / (size_t) height) {
        handleError(0, display, None, 0, BadValue, 0);
        return None;
    }
    SDL_PixelFormat *format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA8888);
    if (!format) {
        handleOutOfMemory(0, display, 0, 0);
        return None;
    }
    Uint32 foreground = mapPixel(format, fg);
    Uint32 background = mapPixel(format, bg);
    Uint32 *pixels = malloc(sizeof(Uint32) * (size_t) width * (size_t) height);
    if (!pixels) {
        SDL_FreeFormat(format);
        handleOutOfMemory(0, display, 0, 0);
        return None;
    }
    for (unsigned int y = 0; y < height; y++) {
        for (unsigned int x = 0; x < width; x++) {
            pixels[(size_t) y * width + x] =
                bitmapBitIsSet(data, x, y, width) ? foreground : background;
        }
    }

    Pixmap pixmap =
        createPixmapFromPixels(display, width, height, pixels, depth);
    free(pixels);
    SDL_FreeFormat(format);
    return pixmap;
}

XPixmapFormatValues *XListPixmapFormats(Display *dpy, int *count)
{
    (void) dpy;
    static const XPixmapFormatValues formats[] = {
        {.depth = 32, .bits_per_pixel = 32, .scanline_pad = 32},
        {.depth = 24, .bits_per_pixel = 32, .scanline_pad = 32},
        {.depth = 16, .bits_per_pixel = 16, .scanline_pad = 16},
        {.depth = 1, .bits_per_pixel = 1, .scanline_pad = 8},
    };
    XPixmapFormatValues *copy = malloc(sizeof(formats));
    if (!copy) {
        return NULL;
    }
    memcpy(copy, formats, sizeof(formats));
    if (count) {
        *count = (int) (sizeof(formats) / sizeof(formats[0]));
    }
    return copy;
}
