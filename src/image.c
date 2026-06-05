#include <limits.h>
#include <stdint.h>
#include "X11/Xlib.h"
#include "errors.h"
#include "drawing.h"
#include "resource-types.h"
#include "window.h"
#include "display.h"
#include "gc.h"
#include "colors.h"
#include "image.h"

/* Cached scratch buffer and staging texture for XPutImage. x11perf
 * issues thousands of identically-sized XPutImage calls per benchmark;
 * a per-call malloc + SDL_CreateTexture + SDL_DestroyTexture chain
 * dominates the wall time, so we reuse both across calls and only grow
 * (or recreate) when the requested geometry or renderer changes. */
static struct {
    Uint32 *pixels;
    size_t pixelCapacity;
    SDL_Texture *texture;
    SDL_Renderer *textureRenderer;
    int textureWidth;
    int textureHeight;
} putImageScratch;

static Uint32 *ensurePutImageScratchBuffer(size_t pixelsNeeded)
{
    /* Refuse anything that would overflow size_t when scaled to bytes.
     * Caller is also expected to validate width/height before this, but
     * a second line of defense is cheap. */
    if (pixelsNeeded > SIZE_MAX / sizeof(Uint32))
        return NULL;
    if (pixelsNeeded > putImageScratch.pixelCapacity) {
        Uint32 *grown =
            realloc(putImageScratch.pixels, pixelsNeeded * sizeof(Uint32));
        if (!grown)
            return NULL;
        putImageScratch.pixels = grown;
        putImageScratch.pixelCapacity = pixelsNeeded;
    }
    return putImageScratch.pixels;
}

static SDL_Texture *ensurePutImageStagingTexture(SDL_Renderer *renderer,
                                                 int width,
                                                 int height)
{
    if (putImageScratch.texture &&
        (putImageScratch.textureRenderer != renderer ||
         putImageScratch.textureWidth != width ||
         putImageScratch.textureHeight != height)) {
        SDL_DestroyTexture(putImageScratch.texture);
        putImageScratch.texture = NULL;
    }
    if (!putImageScratch.texture) {
        putImageScratch.texture =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_STREAMING, width, height);
        if (!putImageScratch.texture)
            return NULL;
        putImageScratch.textureRenderer = renderer;
        putImageScratch.textureWidth = width;
        putImageScratch.textureHeight = height;
    }
    return putImageScratch.texture;
}

void freeImageStorage(void)
{
    if (putImageScratch.texture) {
        SDL_DestroyTexture(putImageScratch.texture);
        putImageScratch.texture = NULL;
    }
    putImageScratch.textureRenderer = NULL;
    putImageScratch.textureWidth = 0;
    putImageScratch.textureHeight = 0;
    free(putImageScratch.pixels);
    putImageScratch.pixels = NULL;
    putImageScratch.pixelCapacity = 0;
}

/* X color encoding to RGBA8888 packed pixel. With colors.h shifts
 * R=16, G=8, B=0, A=24, the per-channel "extract and re-pack" sequence
 * collapses to a single left-rotate by 8 bits. Compilers lower this to
 * one ROL on x86/ARM. Macro form so the per-pixel hot loop stays
 * inlined even at -O0.
 *
 * Evaluates `color` twice. Pass a pure expression (a named local, an
 * indexed array read, or a memcpy result) only -- never a side-
 * effecting one like *src++. */
#define X_COLOR_TO_RGBA8888(color) \
    (((Uint32) (color) << 8) | ((Uint32) (color) >> 24))

void invalidatePutImageStagingTexture(SDL_Renderer *renderer)
{
    if (putImageScratch.texture &&
        putImageScratch.textureRenderer == renderer) {
        SDL_DestroyTexture(putImageScratch.texture);
        putImageScratch.texture = NULL;
        putImageScratch.textureRenderer = NULL;
        putImageScratch.textureWidth = 0;
        putImageScratch.textureHeight = 0;
    }
}

SDL_Renderer *getPutImageStagingTextureRenderer(void)
{
    return putImageScratch.textureRenderer;
}

#ifdef XPutPixel
#undef XPutPixel
#endif
#ifdef XGetPixel
#undef XGetPixel
#endif
#ifdef XSubImage
#undef XSubImage
#endif
#ifdef XDestroyImage
#undef XDestroyImage
#endif

// Inspired by
// https://github.com/csulmone/X11/blob/59029dc09211926a5c95ff1dd2b828574fefcde6/libX11-1.5.0/src/ImUtil.c

static int imageByteOrder(Display *display)
{
    if (display) {
        return ImageByteOrder(display);
    }
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    return MSBFirst;
#else
    return LSBFirst;
#endif
}

static int bitsPerPixelForDepth(unsigned int depth, int format)
{
    if (format != ZPixmap)
        return 1;
    if (depth <= 1)
        return 1;
    if (depth <= 8)
        return 8;
    if (depth <= 16)
        return 16;
    return 32;
}

static Bool isValidBitmapPad(int bitmapPad)
{
    return bitmapPad == 8 || bitmapPad == 16 || bitmapPad == 32;
}

static int paddedBytesPerLine(unsigned int width,
                              int bitsPerPixel,
                              int bitmapPad)
{
    if (!isValidBitmapPad(bitmapPad))
        return 0;
    if (bitsPerPixel <= 0)
        return 0;
    /* Compute in uint64_t so width * bitsPerPixel (worst case
     * ~2^32 * 32) and the padding round-up cannot wrap. The final
     * return is int, so reject anything that would not fit. */
    uint64_t bits = (uint64_t) width * (uint64_t) (unsigned int) bitsPerPixel;
    uint64_t pad = (uint64_t) (unsigned int) bitmapPad;
    if (bits > UINT64_MAX - (pad - 1))
        return 0;
    uint64_t paddedBits = (bits + pad - 1) & ~(pad - 1);
    uint64_t bytes = paddedBits / 8;
    if (bytes > (uint64_t) INT_MAX)
        return 0;
    return (int) bytes;
}

XImage *XCreateImage(Display *display,
                     Visual *visual,
                     unsigned int depth,
                     int format,
                     int offset,
                     char *data,
                     unsigned int width,
                     unsigned int height,
                     int bitmap_pad,
                     int bytes_per_line)
{
    // https://tronche.com/gui/x/xlib/utilities/XCreateImage.html
    /* Real Xlib only accepts bitmap_pad in {8, 16, 32}. Reject anything
     * else with BadValue so callers find bugs early instead of silently
     * getting a malformed scanline. */
    if (!isValidBitmapPad(bitmap_pad)) {
        handleError(0, display, None, 0, BadValue, 0);
        return NULL;
    }
    XImage *image = malloc(sizeof(XImage));
    if (!image) {
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    LOG("%s: w = %d, h = %d\n", __func__, (int) width, (int) height);
    image->width = width;
    image->height = height;
    image->xoffset = offset;
    image->format = format;
    image->data = data;
    image->byte_order = imageByteOrder(display);
    image->bitmap_unit = 8;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    image->bitmap_bit_order = MSBFirst;
#else
    image->bitmap_bit_order = LSBFirst;
#endif
    image->depth = depth;
    image->bitmap_pad = bitmap_pad;
    image->bytes_per_line = bytes_per_line;
    image->bits_per_pixel = bitsPerPixelForDepth(depth, format);
    /* Real Xlib happily accepts width==0 or bits_per_pixel==0 and
     * returns an XImage whose bytes_per_line is zero. paddedBytesPerLine
     * yields 0 in those cases now, so just trust its result rather than
     * aborting. */
    if (bytes_per_line == 0) {
        image->bytes_per_line =
            paddedBytesPerLine(width, image->bits_per_pixel, bitmap_pad);
    }

    XInitImage(image);
    return image;
}

char *getImageDataPointer(XImage *image, unsigned int x, unsigned int y)
{
    char *pointer = image->data;
    pointer += image->bytes_per_line * y;
    return pointer + ((image->bits_per_pixel + 7) / 8) * x;
}

static unsigned char bitMaskForImage(XImage *image, int x)
{
    int bit = image->bitmap_bit_order == MSBFirst ? 7 - (x & 7) : (x & 7);
    return (unsigned char) (1u << bit);
}

/* XYPixmap stores each bit-plane in its own contiguous slab; cap the plane
 * count at the unsigned-long bit width so `1UL << plane` cannot trigger UB
 * on a malformed image->depth. */
static int xyPixmapPlaneCount(const XImage *image)
{
    int maxPlane = (int) (sizeof(unsigned long) * CHAR_BIT);
    return image->depth < maxPlane ? image->depth : maxPlane;
}

static char *xyPixmapPlanePointer(XImage *image, int plane, int x, int y)
{
    size_t planeStride =
        (size_t) image->bytes_per_line * (size_t) image->height;
    return image->data + planeStride * (size_t) plane +
           image->bytes_per_line * y + x / 8;
}

static Bool imageCoordinatesValid(XImage *image, int x, int y)
{
    return image && image->data && x >= 0 && y >= 0 && x < image->width &&
           y < image->height;
}

/* Compute the data-buffer byte count for an internally-allocated XImage,
 * with overflow checks. Returns 0 on overflow or invalid input.
 *
 * For XYPixmap, the X protocol stores each bit-plane in its own slab of
 * (bytes_per_line * height) bytes, so the total is multiplied by depth.
 * Our other formats use a single slab. */
static size_t imageDataBufferSize(int format,
                                  int bytesPerLine,
                                  unsigned int height,
                                  unsigned int depth)
{
    if (bytesPerLine < 0)
        return 0;
    size_t bpl = (size_t) bytesPerLine;
    if (height != 0 && bpl > SIZE_MAX / (size_t) height)
        return 0;
    size_t bytes = bpl * (size_t) height;
    if (format == XYPixmap) {
        if (depth == 0)
            return 0;
        if (bytes != 0 && (size_t) depth > SIZE_MAX / bytes)
            return 0;
        bytes *= (size_t) depth;
    }
    return bytes;
}

int XPutPixel(XImage *image, int x, int y, unsigned long pixel)
{
    // https://tronche.com/gui/x/xlib/utilities/XPutPixel.html
    if (!imageCoordinatesValid(image, x, y)) {
        LOG("Invalid argument: Got invalid image/pixel in XPutPixel\n");
        return 0;
    }
    char *pointer;
    switch (image->format) {
    case ZPixmap:
        pointer = getImageDataPointer(image, x, y);
        switch (image->bits_per_pixel) {
        case 32: {
            Uint32 value = (Uint32) pixel;
            memcpy(pointer, &value, sizeof(value));
            break;
        }
        case 16: {
            Uint16 value = (Uint16) pixel;
            memcpy(pointer, &value, sizeof(value));
            break;
        }
        case 8:
            *pointer = (char) pixel;
            break;
        case 1:
            pointer = image->data + image->bytes_per_line * y + x / 8;
            if (pixel)
                *pointer |= bitMaskForImage(image, x);
            else
                *pointer &= (char) ~bitMaskForImage(image, x);
            break;
        }
        break;
    case XYBitmap:
        pointer = image->data + image->bytes_per_line * y + x / 8;
        if (pixel)
            *pointer |= bitMaskForImage(image, x);
        else
            *pointer &= (char) ~bitMaskForImage(image, x);
        break;
    case XYPixmap:
        if (x < 0 || y < 0 || x >= image->width || y >= image->height)
            return 0;
        for (int plane = 0; plane < xyPixmapPlaneCount(image); plane++) {
            pointer = xyPixmapPlanePointer(image, plane, x, y);
            if (pixel & (1UL << plane))
                *pointer |= bitMaskForImage(image, x);
            else
                *pointer &= (char) ~bitMaskForImage(image, x);
        }
        break;
    default:
        LOG("Warn: Got invalid format %d\n", image->format);
        return 0;
    }
    return 1;
}

unsigned long XGetPixel(XImage *image, int x, int y)
{
    // https://tronche.com/gui/x/xlib/utilities/XGetPixel.html
    if (!imageCoordinatesValid(image, x, y)) {
        LOG("Invalid argument: Got invalid image/pixel in XGetPixel\n");
        return 0;
    }
    char *pointer;
    switch (image->format) {
    case ZPixmap:
        pointer = getImageDataPointer(image, x, y);
        switch (image->bits_per_pixel) {
        case 32: {
            Uint32 value;
            memcpy(&value, pointer, sizeof(value));
            return value;
        }
        case 16: {
            Uint16 value;
            memcpy(&value, pointer, sizeof(value));
            return value;
        }
        case 8:
            return (unsigned char) *pointer;
        case 1:
            pointer = image->data + image->bytes_per_line * y + x / 8;
            return (*pointer & bitMaskForImage(image, x)) != 0;
        }
        break;
    case XYBitmap:
        pointer = image->data + image->bytes_per_line * y + x / 8;
        return (*pointer & bitMaskForImage(image, x)) != 0;
    case XYPixmap:
        if (x < 0 || y < 0 || x >= image->width || y >= image->height)
            return 0;
        {
            unsigned long pixel = 0;
            for (int plane = 0; plane < xyPixmapPlaneCount(image); plane++) {
                pointer = xyPixmapPlanePointer(image, plane, x, y);
                if (*pointer & bitMaskForImage(image, x))
                    pixel |= 1UL << plane;
            }
            return pixel;
        }
    default:
        LOG("Warn: Got invalid format %d\n", image->format);
    }
    return 0;
}

static int addPixel(XImage *image, long value)
{
    if (!image || !image->data) {
        return 0;
    }
    for (int y = 0; y < image->height; y++) {
        for (int x = 0; x < image->width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            if (!XPutPixel(image, x, y, pixel + (unsigned long) value)) {
                return 0;
            }
        }
    }
    return 1;
}

XImage *XSubImage(XImage *image,
                  int x,
                  int y,
                  unsigned int width,
                  unsigned int height)
{
    if (!image || !image->data) {
        return NULL;
    }
    if (x < 0 || y < 0 || image->width < 0 || image->height < 0 ||
        (unsigned int) x > (unsigned int) image->width ||
        (unsigned int) y > (unsigned int) image->height ||
        width > (unsigned int) image->width - (unsigned int) x ||
        height > (unsigned int) image->height - (unsigned int) y) {
        return NULL;
    }

    int bytesPerLine =
        paddedBytesPerLine(width, image->bits_per_pixel, image->bitmap_pad);
    /* paddedBytesPerLine returns 0 either for a legitimately empty image
     * or because the row would overflow int. The pixel loop below would
     * still iterate width*height times on a calloc(0, height) buffer, so
     * reject the overflow case explicitly. */
    if (bytesPerLine == 0 && width != 0) {
        return NULL;
    }
    size_t dataBytes = imageDataBufferSize(image->format, bytesPerLine, height,
                                           (unsigned int) image->depth);
    if (dataBytes == 0 && bytesPerLine != 0 && height != 0) {
        return NULL;
    }
    char *data = dataBytes ? calloc(1, dataBytes) : NULL;
    if (!data && dataBytes != 0) {
        return NULL;
    }
    XImage *subImage =
        XCreateImage(NULL, NULL, image->depth, image->format, image->xoffset,
                     data, width, height, image->bitmap_pad, bytesPerLine);
    if (!subImage) {
        free(data);
        return NULL;
    }
    for (unsigned int currY = 0; currY < height; currY++) {
        for (unsigned int currX = 0; currX < width; currX++) {
            XPutPixel(subImage, (int) currX, (int) currY,
                      XGetPixel(image, x + (int) currX, y + (int) currY));
        }
    }
    return subImage;
}

int destroyImage(XImage *image)
{
    // https://tronche.com/gui/x/xlib/utilities/XDestroyImage.html
    if (image->data) {
        free(image->data);
    }
    free(image);
    return 1;
}

int XDestroyImage(XImage *image)
{
    if (!image)
        return 0;
    if (image->f.destroy_image)
        return image->f.destroy_image(image);
    return destroyImage(image);
}

int XImageByteOrder(Display *display)
{
    return imageByteOrder(display);
}

int XPutImage(Display *display,
              Drawable drawable,
              GC gc,
              XImage *image,
              int src_x,
              int src_y,
              int dest_x,
              int dest_y,
              unsigned int width,
              unsigned int height)
{
    // https://tronche.com/gui/x/xlib/graphics/XPutImage.html
    SET_X_SERVER_REQUEST(display, X_PutImage);
    TYPE_CHECK(drawable, DRAWABLE, display, 0);
    if (IS_TYPE(drawable, WINDOW) && IS_INPUT_ONLY(drawable)) {
        handleError(0, display, drawable, 0, BadMatch, 0);
        return -1;
    }
    LOG("%s: Drawing %p on %lu\n", __func__, image, drawable);

    SDL_Renderer *renderer = NULL;
    GET_RENDERER(drawable, renderer);
    if (!renderer) {
        LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, drawable, 0, BadDrawable, 0);
        return -1;
    }

    /* Reject requests whose source rectangle does not lie wholly inside
     * the XImage. The old XGetPixel fallback silently returned 0 for
     * out-of-bounds pixels; the new direct fast paths read raw bytes
     * from image->data and would segfault. */
    if (!image || src_x < 0 || src_y < 0 || image->width < 0 ||
        image->height < 0 || (long) src_x + (long) width > image->width ||
        (long) src_y + (long) height > image->height) {
        handleError(0, display, drawable, 0, BadMatch, 0);
        return -1;
    }
    if (width > 0 && height > 0 && !image->data) {
        handleError(0, display, drawable, 0, BadMatch, 0);
        return -1;
    }

    /* Reject geometries that would either overflow the scratch-buffer
     * size or exceed what SDL_Rect (signed int) can address downstream. */
    if (width > (unsigned int) INT_MAX || height > (unsigned int) INT_MAX ||
        (height != 0 && width > SIZE_MAX / sizeof(Uint32) / (size_t) height)) {
        handleError(0, display, drawable, 0, BadValue, 0);
        return -1;
    }
    Uint32 *data =
        ensurePutImageScratchBuffer((size_t) width * (size_t) height);
    if (!data) {
        handleOutOfMemory(0, display, 0, 0);
        return -1;
    }

    GraphicContext *graphicContext =
        image->format == XYBitmap ? GET_GC(gc) : NULL;
    if (!image->data) {
        if (image->format == XYBitmap && graphicContext) {
            Uint32 background =
                GET_RED_FROM_COLOR(graphicContext->background) << 24 |
                GET_GREEN_FROM_COLOR(graphicContext->background) << 16 |
                GET_BLUE_FROM_COLOR(graphicContext->background) << 8 |
                GET_ALPHA_FROM_COLOR(graphicContext->background);
            for (unsigned int y = 0; y < height; y++) {
                Uint32 *dst = data + y * width;
                for (unsigned int x = 0; x < width; x++)
                    dst[x] = background;
            }
        } else {
            memset(data, 0, sizeof(Uint32) * width * height);
        }
    } else if (image->format == ZPixmap && image->bits_per_pixel == 32) {
        /* The fast 32-bit-load path requires the row base to be 4-byte
         * aligned. Xlib does not promise that for caller-supplied data,
         * so fall back to memcpy-per-pixel when either the stride or
         * the base pointer is misaligned. */
        Bool aligned = (image->bytes_per_line % (int) sizeof(Uint32)) == 0 &&
                       (((uintptr_t) image->data) % sizeof(Uint32)) == 0;
        for (unsigned int y = 0; y < height; y++) {
            const char *srcRow = image->data +
                                 image->bytes_per_line * (src_y + (int) y) +
                                 src_x * (int) sizeof(Uint32);
            Uint32 *dst = data + y * width;
            if (aligned) {
                const Uint32 *src = (const Uint32 *) srcRow;
                for (unsigned int x = 0; x < width; x++) {
                    dst[x] = X_COLOR_TO_RGBA8888(src[x]);
                }
            } else {
                for (unsigned int x = 0; x < width; x++) {
                    Uint32 color;
                    memcpy(&color, srcRow + x * sizeof(Uint32), sizeof(Uint32));
                    dst[x] = X_COLOR_TO_RGBA8888(color);
                }
            }
        }
    } else if (image->format == XYBitmap) {
        Uint32 foreground =
            GET_RED_FROM_COLOR(graphicContext->foreground) << 24 |
            GET_GREEN_FROM_COLOR(graphicContext->foreground) << 16 |
            GET_BLUE_FROM_COLOR(graphicContext->foreground) << 8 |
            GET_ALPHA_FROM_COLOR(graphicContext->foreground);
        Uint32 background =
            GET_RED_FROM_COLOR(graphicContext->background) << 24 |
            GET_GREEN_FROM_COLOR(graphicContext->background) << 16 |
            GET_BLUE_FROM_COLOR(graphicContext->background) << 8 |
            GET_ALPHA_FROM_COLOR(graphicContext->background);
        Bool msbFirst = image->bitmap_bit_order == MSBFirst;
        /* The 256-entry LUT below costs ~2us to build, only worth it
         * when the image is large enough to amortize over many bytes.
         * For small inputs the original per-pixel bit test wins. */
        if ((size_t) width * (size_t) height >= 1024) {
            Uint32 lut[256][8];
            for (int byte = 0; byte < 256; byte++) {
                for (int bit = 0; bit < 8; bit++) {
                    int mask = msbFirst ? (1 << (7 - bit)) : (1 << bit);
                    lut[byte][bit] = (byte & mask) ? foreground : background;
                }
            }
            for (unsigned int y = 0; y < height; y++) {
                const unsigned char *srcRow =
                    (const unsigned char *) image->data +
                    image->bytes_per_line * (src_y + (int) y);
                Uint32 *dst = data + y * width;
                unsigned int x = 0;
                unsigned int srcBitX = (unsigned int) src_x;
                /* Head: pixels before the first aligned source byte. */
                while (x < width && (srcBitX & 7) != 0) {
                    unsigned char byte = srcRow[srcBitX >> 3];
                    dst[x++] = lut[byte][srcBitX & 7];
                    srcBitX++;
                }
                /* Aligned middle: one source byte at a time produces
                 * eight destination pixels per memcpy. */
                while (x + 8 <= width) {
                    unsigned char byte = srcRow[srcBitX >> 3];
                    memcpy(&dst[x], lut[byte], sizeof(lut[byte]));
                    x += 8;
                    srcBitX += 8;
                }
                /* Tail: remaining pixels after the last full byte. */
                while (x < width) {
                    unsigned char byte = srcRow[srcBitX >> 3];
                    dst[x++] = lut[byte][srcBitX & 7];
                    srcBitX++;
                }
            }
        } else {
            for (unsigned int y = 0; y < height; y++) {
                Uint32 *dst = data + y * width;
                for (unsigned int x = 0; x < width; x++) {
                    const char *byte =
                        image->data +
                        image->bytes_per_line * (src_y + (int) y) +
                        (src_x + (int) x) / 8;
                    dst[x] = (*byte & bitMaskForImage(image, src_x + (int) x))
                                 ? foreground
                                 : background;
                }
            }
        }
    } else if (image->format == XYPixmap) {
        for (unsigned int y = 0; y < height; y++) {
            for (unsigned int x = 0; x < width; x++) {
                unsigned long color =
                    XGetPixel(image, src_x + (int) x, src_y + (int) y);
                data[y * width + x] = X_COLOR_TO_RGBA8888((Uint32) color);
            }
        }
    } else {
        for (unsigned int y = 0; y < height; y++) {
            for (unsigned int x = 0; x < width; x++) {
                unsigned long color = XGetPixel(image, src_x + x, src_y + y);
                if (graphicContext) {
                    color = color ? graphicContext->foreground
                                  : graphicContext->background;
                }
                data[y * width + x] = X_COLOR_TO_RGBA8888((Uint32) color);
            }
        }
    }

    SDL_Texture *texture =
        ensurePutImageStagingTexture(renderer, (int) width, (int) height);
    if (!texture) {
        LOG("SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }
    if (SDL_UpdateTexture(texture, NULL, data, width * sizeof(Uint32)) < 0) {
        LOG("SDL_UpdateTexture failed in %s: %s\n", __func__, SDL_GetError());
        return -1;
    }
    SDL_Rect dst = {dest_x, dest_y, width, height};
    ShapeGuard sg;
    shapeGuardBegin(&sg, drawable, renderer, &dst);
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (SDL_RenderCopy(renderer, texture, NULL, &dst) < 0) {
            clearRendererClip(renderer);
            shapeGuardEnd(&sg);
            LOG("SDL_RenderCopy failed: %s\n", SDL_GetError());
            return -1;
        }
    }
    clearRendererClip(renderer);
    /* If the shape composite failed mid-flight, mask-violating pixels
     * may still be on the renderer; skip the present so the next draw
     * recomposes from a fresh baseline rather than flashing stale
     * output. */
    Bool shapeOk = shapeGuardEnd(&sg);
    if (shapeOk)
        presentDrawableIfVisible(drawable);
    return 1;
}

XImage *XGetImage(Display *display,
                  Drawable drawable,
                  int x,
                  int y,
                  unsigned int width,
                  unsigned int height,
                  unsigned long plane_mask,
                  int format)
{
    // https://tronche.com/gui/x/xlib/graphics/XGetImage.html
    SET_X_SERVER_REQUEST(display, X_GetImage);
    LOG("%s: From %lu\n", __func__, drawable);
    if (IS_TYPE(drawable, WINDOW) && drawable == SCREEN_WINDOW) {
        /* Root-window readback would need a compositing pass: top-level
         * SDL_Windows present independently, the SCREEN renderer's
         * backing surface is never updated with their pixels, and there
         * is no single root framebuffer to capture from. Returning NULL
         * matches what Xlib does when the request is rejected. */
        LOG("XGetImage on SCREEN_WINDOW is not supported by this shim.\n");
        return NULL;
    }
    if (IS_TYPE(drawable, WINDOW) && IS_INPUT_ONLY(drawable)) {
        handleError(0, display, drawable, 0, BadMatch, 0);
        return NULL;
    }
    /* SDL_Rect carries signed int, and the per-row pointer math below uses
     * width and height as positive offsets; reject any dimension that would
     * cast to a negative int before allocations are attempted. */
    if (width > (unsigned int) INT_MAX || height > (unsigned int) INT_MAX) {
        handleError(0, display, drawable, 0, BadValue, 0);
        return NULL;
    }
    int depth = DefaultDepth(display, DefaultScreen(display));
    if (IS_TYPE(drawable, WINDOW)) {
        WindowStruct *windowStruct = GET_WINDOW_STRUCT(drawable);
        depth = windowStruct->depth == CopyFromParent
                    ? DefaultDepth(display, DefaultScreen(display))
                    : windowStruct->depth;
    } else if (IS_TYPE(drawable, PIXMAP)) {
        depth = (int) GET_PIXMAP_STRUCT(drawable)->depth;
    }
    int bitsPerPixel = bitsPerPixelForDepth((unsigned int) depth, format);
    int bytes_per_line = paddedBytesPerLine(width, bitsPerPixel, 32);
    /* paddedBytesPerLine returns 0 either for a legitimately empty image
     * or because the row size would overflow int. Either way, an extreme
     * width could make calloc(0, height) succeed even though the
     * width*height pixel loop below would walk off the buffer. */
    if (bytes_per_line == 0 && width != 0) {
        handleError(0, display, drawable, 0, BadValue, 0);
        return NULL;
    }
    size_t dataBytes = imageDataBufferSize(format, bytes_per_line, height,
                                           (unsigned int) depth);
    if (dataBytes == 0 && bytes_per_line != 0 && height != 0) {
        handleError(0, display, drawable, 0, BadValue, 0);
        return NULL;
    }
    char *data = dataBytes ? calloc(1, dataBytes) : NULL;
    if (!data && dataBytes != 0) {
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    XImage *image =
        XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                     depth, format, 0, data, width, height, 32, bytes_per_line);
    if (!image) {
        free(data);
        return NULL;
    }
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(drawable, renderer);
    if (!renderer) {
        LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, drawable, 0, BadDrawable, 0);
        XDestroyImage(image);
        return NULL;
    }
    SDL_Rect srcRect = {x, y, (int) width, (int) height};
    SDL_Surface *drawableSurface = getRenderSurfaceRect(renderer, &srcRect);
    if (!drawableSurface) {
        XDestroyImage(image);
        return NULL;
    }

    if (format == ZPixmap && image->bits_per_pixel == 32) {
        for (unsigned int currY = 0; currY < height; currY++) {
            const Uint32 *src =
                (const Uint32 *) ((const char *) drawableSurface->pixels +
                                  currY * drawableSurface->pitch);
            Uint32 *dst =
                (Uint32 *) (image->data + currY * image->bytes_per_line);
            if (plane_mask == (unsigned long) ~0) {
                memcpy(dst, src, width * sizeof(Uint32));
            } else {
                for (unsigned int currX = 0; currX < width; currX++) {
                    dst[currX] = (Uint32) (plane_mask & src[currX]);
                }
            }
        }
    } else {
        for (unsigned int currX = 0; currX < width; currX++) {
            for (unsigned int currY = 0; currY < height; currY++) {
                XPutPixel(image, (int) currX, (int) currY,
                          plane_mask & getPixel(drawableSurface, currX, currY));
            }
        }
    }
    SDL_FreeSurface(drawableSurface);
    return image;
}

XImage *XGetSubImage(Display *display,
                     Drawable drawable,
                     int x,
                     int y,
                     unsigned int width,
                     unsigned int height,
                     unsigned long plane_mask,
                     int format,
                     XImage *dest_image,
                     int dest_x,
                     int dest_y)
{
    XImage *image =
        XGetImage(display, drawable, x, y, width, height, plane_mask, format);
    if (!image) {
        return NULL;
    }
    if (!dest_image) {
        return image;
    }
    for (unsigned int currY = 0; currY < height; currY++) {
        for (unsigned int currX = 0; currX < width; currX++) {
            XPutPixel(dest_image, dest_x + (int) currX, dest_y + (int) currY,
                      XGetPixel(image, (int) currX, (int) currY));
        }
    }
    XDestroyImage(image);
    return dest_image;
}

Status _XInitImageFuncPtrs(XImage *image)
{
    image->f.put_pixel = XPutPixel;
    image->f.get_pixel = XGetPixel;
    image->f.create_image = XCreateImage;
    image->f.destroy_image = destroyImage;
    image->f.add_pixel = addPixel;
    image->f.sub_image = XSubImage;
    return 1;
}

Status XInitImage(XImage *image)
{
    // https://tronche.com/gui/x/xlib/graphics/XInitImage.html
    return _XInitImageFuncPtrs(image);
}
