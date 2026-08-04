#include <limits.h>
#include <stdint.h>
#include <X11/Xlib.h>

#include "errors.h"
#include "drawing.h"
#include "resource-types.h"
#include "window.h"
#include "display.h"
#include "gc.h"
#include "colors.h"
#include "image.h"

/* Cached scratch buffer and staging texture for XPutImage. x11perf issues
 * thousands of identically-sized XPutImage calls per benchmark; a per-call
 * malloc + SDL_CreateTexture + SDL_DestroyTexture chain dominates the wall
 * time, so XPutImage reuses both across calls and only grows (or recreates)
 * when the requested geometry or renderer changes.
 */
static struct {
    Uint32 *pixels;
    size_t pixelCapacity;
    SDL_Texture *texture;
    SDL_Renderer *textureRenderer;
    int textureWidth;
    int textureHeight;
} putImageScratch;

unsigned long x11compat_xputimage_direct_uploads = 0;

/* XPutImage from multiple threads would otherwise race on the scratch pixel
 * buffer's realloc and the staging texture's create/destroy. The mutex is
 * created lazily on first use and kept for the lifetime of the process: tearing
 * it down in freeImageStorage created a TOCTOU window where
 * ensurePutImageScratchLock could return a pointer that freeImageStorage
 * destroyed before the caller called SDL_LockMutex on it. The memory cost is
 * one SDL_mutex per process.
 */
static SDL_mutex *putImageScratchLock = NULL;
static SDL_SpinLock putImageScratchLockInitLock = 0;
static SDL_mutex *ensurePutImageScratchLock(void)
{
    SDL_AtomicLock(&putImageScratchLockInitLock);
    if (!putImageScratchLock)
        putImageScratchLock = SDL_CreateMutex();
    SDL_mutex *lock = putImageScratchLock;
    SDL_AtomicUnlock(&putImageScratchLockInitLock);
    return lock;
}

/* Return the acquired mutex so the caller pairs lock/unlock against the exact
 * pointer it locked. Reading putImageScratchLock unlocked at unlock time would
 * race against another thread initializing it (or, if SDL_CreateMutex returned
 * NULL on the first thread and succeeded on the second, would try to unlock a
 * mutex this thread never locked). A NULL return means "no lock was acquired";
 * passing NULL to unlockPutImageScratch makes the unlock a no-op.
 */
static SDL_mutex *lockPutImageScratch(void)
{
    SDL_mutex *lock = ensurePutImageScratchLock();
    if (lock)
        SDL_LockMutex(lock);
    return lock;
}
static void unlockPutImageScratch(SDL_mutex *lock)
{
    if (lock)
        SDL_UnlockMutex(lock);
}

static Uint32 *ensurePutImageScratchBuffer(size_t pixelsNeeded)
{
    /* Refuse anything that would overflow size_t when scaled to bytes. Caller
     * is also expected to validate width/height before this, but a second line
     * of defense is cheap. Caller must hold putImageScratchLock around the
     * buffer use.
     */
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

static int renderRgba8888Locked(Drawable drawable,
                                GC gc,
                                SDL_Renderer *renderer,
                                const void *pixels,
                                int pitch,
                                int dest_x,
                                int dest_y,
                                unsigned int width,
                                unsigned int height)
{
    SDL_Texture *texture =
        ensurePutImageStagingTexture(renderer, (int) width, (int) height);
    if (!texture) {
        LOG("SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }
    if (SDL_UpdateTexture(texture, NULL, pixels, pitch) < 0) {
        LOG("SDL_UpdateTexture failed in %s: %s\n", __func__, SDL_GetError());
        return -1;
    }
    SDL_Rect dst = {
        .x = dest_x,
        .y = dest_y,
        .w = width,
        .h = height,
    };
    ShapeGuard sg;
    shapeGuardBegin(&sg, drawable, renderer, &dst);
    int clipCount = getGcClipIterationCount(gc, drawable);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, drawable))
            continue;
        if (SDL_RenderCopy(renderer, texture, NULL, &dst) < 0) {
            clearRendererClip(renderer);
            shapeGuardEnd(&sg);
            LOG("SDL_RenderCopy failed: %s\n", SDL_GetError());
            return -1;
        }
    }
    clearRendererClip(renderer);
    Bool shapeOk = shapeGuardEnd(&sg);
    if (shapeOk)
        presentDrawableRectIfVisible(drawable, &dst);
    return 1;
}

/* One-shot reachability probe: set X11COMPAT_REPORT_XPUTIMAGE to have the
 * process print, at exit, how many XPutImage calls actually took the zero-copy
 * RGBA8888 direct upload. Standard clients build images through XCreateImage,
 * which forces canonical masks that never match the fast path, so a real
 * workload that reports 0 here is not banking the optimization.
 */
__attribute__((destructor)) static void reportDirectUploads(void)
{
    if (getenv("X11COMPAT_REPORT_XPUTIMAGE"))
        fprintf(stderr, "x11compat: XPutImage direct RGBA8888 uploads: %lu\n",
                x11compat_xputimage_direct_uploads);
}

void freeImageStorage(void)
{
    /* Release scratch payload but leave putImageScratchLock alive; destroying
     * it here is racy with concurrent callers holding a pointer returned by
     * ensurePutImageScratchLock().
     */
    SDL_mutex *lock = lockPutImageScratch();
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
    unlockPutImageScratch(lock);
}

/* Pack an X11 pixel into SDL2's RGBA8888 layout, which matches every drawable
 * render target in this layer so SDL_RenderCopy of the staging texture stays a
 * straight per-row copy instead of a converting blit. Promotes a zero (core-X11
 * opaque) alpha byte to 0xFF branch-free so core-X11 pixels render opaque
 * instead of vanishing into SDL2's alpha-aware blend, and does the whole
 * swizzle in Uint32 so the 32-bit XPutImage loops vectorize with narrow lanes
 * (NEON on arm64, SSE on x86) once this TU is built optimized.
 */
static inline Uint32 xColorToRgba8888(unsigned long color)
{
    Uint32 c = (Uint32) color;
    Uint32 alpha = (Uint32) 0xFF << ALPHA_SHIFT;
    c |= alpha & -(Uint32) ((c & alpha) == 0);
    return ((c >> RED_SHIFT & 0xFF) << 24) | ((c >> GREEN_SHIFT & 0xFF) << 16) |
           ((c >> BLUE_SHIFT & 0xFF) << 8) | (c >> ALPHA_SHIFT & 0xFF);
}

static unsigned long oneBitPixelFromRgba(Uint32 pixel)
{
    return (pixel & 0xFFFFFF00u) != 0 ? 1 : 0;
}

/* Inverse of xColorToRgba8888: the readback surfaces this file consumes are
 * RGBA8888 (0xRRGGBBAA), but a 32-bit ZPixmap's words are the compat's
 * canonical XColor (0xAARRGGBB via RED_SHIFT/etc), the same form XPutImage and
 * the Xft glyph blend expect. Convert on the way out of XGetImage so a client
 * that reads a colour back and either inspects it with GET_*_FROM_COLOR or
 * hands it straight back to XPutImage round-trips the pixel unchanged instead
 * of shifting every channel by a byte.
 */
static inline unsigned long rgba8888ToXColor(Uint32 rgba)
{
    return ((unsigned long) ((rgba >> 24) & 0xFF) << RED_SHIFT) |
           ((unsigned long) ((rgba >> 16) & 0xFF) << GREEN_SHIFT) |
           ((unsigned long) ((rgba >> 8) & 0xFF) << BLUE_SHIFT) |
           ((unsigned long) (rgba & 0xFF) << ALPHA_SHIFT);
}

void invalidatePutImageStagingTexture(SDL_Renderer *renderer)
{
    SDL_mutex *lock = lockPutImageScratch();
    if (putImageScratch.texture &&
        putImageScratch.textureRenderer == renderer) {
        SDL_DestroyTexture(putImageScratch.texture);
        putImageScratch.texture = NULL;
        putImageScratch.textureRenderer = NULL;
        putImageScratch.textureWidth = 0;
        putImageScratch.textureHeight = 0;
    }
    unlockPutImageScratch(lock);
}

SDL_Renderer *getPutImageStagingTextureRenderer(void)
{
    SDL_mutex *lock = lockPutImageScratch();
    SDL_Renderer *r = putImageScratch.textureRenderer;
    unlockPutImageScratch(lock);
    return r;
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

static int imageByteOrder(Display *display)
{
    if (display)
        return ImageByteOrder(display);
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

static Bool imageUsesCanonicalColorMasks(const XImage *image)
{
    return image->format == ZPixmap && image->bits_per_pixel == 32 &&
           image->depth >= 24;
}

static Bool imageRowsAreRgba8888(Display *display, const XImage *image)
{
    /* Bound depth at 32 so the depth == 32 (real alpha) versus depth < 32
     * (padding low byte) split downstream is exhaustive. A malformed depth
     * above 32 falls through to the general path instead of silently taking the
     * padding branch and having its low byte forced opaque.
     */
    return image->format == ZPixmap && image->bits_per_pixel == 32 &&
           image->depth >= 24 && image->depth <= 32 &&
           image->byte_order == imageByteOrder(display) &&
           image->red_mask == 0xFF000000ul &&
           image->green_mask == 0x00FF0000ul &&
           image->blue_mask == 0x0000FF00ul;
}

/* Base address of row y within a 32-bit ZPixmap's source rectangle. The direct
 * upload and both XPutImage swizzle loops share this one copy of the stride and
 * src-offset arithmetic rather than open-coding the overflow-prone offset three
 * times. The width and stride bounds checks in XPutImage keep the result inside
 * the caller buffer.
 */
static const char *zpixmap32Row(const XImage *image,
                                int src_x,
                                int src_y,
                                unsigned int y)
{
    return image->data + (size_t) image->bytes_per_line * ((size_t) src_y + y) +
           (size_t) src_x * sizeof(Uint32);
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

    /* Compute in uint64_t so width * bitsPerPixel (worst case ~2^32 * 32) and
     * the padding round-up cannot wrap. The final return is int, so reject
     * anything that would not fit.
     */
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
    /* Real Xlib only accepts bitmap_pad in {8, 16, 32}. Reject anything else
     * with BadValue so callers find bugs early instead of silently getting a
     * malformed scanline.
     */
    if (!isValidBitmapPad(bitmap_pad)) {
        handleError(0, display, None, 0, BadValue, 0);
        return NULL;
    }
    XImage *image = malloc(sizeof(XImage));
    if (!image) {
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
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
    if (imageUsesCanonicalColorMasks(image)) {
        /* This layer decodes every 32bpp ZPixmap word as the canonical XColor
         * layout (XPutImage's swizzle uses fixed RED_SHIFT/etc) and its only
         * visual advertises exactly these masks. Report canonical masks even
         * when a client hands in a Visual with different ones: honoring those
         * would make image->red_mask/green_mask/blue_mask disagree with how the
         * pixels are actually rendered and read back, not agree with it.
         */
        image->red_mask = 0xFFul << RED_SHIFT;
        image->green_mask = 0xFFul << GREEN_SHIFT;
        image->blue_mask = 0xFFul << BLUE_SHIFT;
    } else {
        image->red_mask = visual ? visual->red_mask : 0;
        image->green_mask = visual ? visual->green_mask : 0;
        image->blue_mask = visual ? visual->blue_mask : 0;
    }
    image->obdata = NULL;

    /* Real Xlib happily accepts width==0 or bits_per_pixel==0 and returns an
     * XImage whose bytes_per_line is zero. paddedBytesPerLine yields 0 in those
     * cases now, so just trust its result rather than aborting.
     */
    if (bytes_per_line == 0) {
        image->bytes_per_line =
            paddedBytesPerLine(width, image->bits_per_pixel, bitmap_pad);
    }

    XInitImage(image);
    return image;
}

static char *getImageDataPointer(XImage *image, unsigned int x, unsigned int y)
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
 * count at the unsigned-long bit width so `1UL << plane` cannot trigger UB on a
 * malformed image->depth.
 */
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
           (size_t) image->bytes_per_line * (size_t) y + x / 8;
}

static Bool imageCoordinatesValid(XImage *image, int x, int y)
{
    return image && image->data && x >= 0 && y >= 0 && x < image->width &&
           y < image->height;
}

/* Compute the data-buffer byte count for an internally-allocated XImage, with
 * overflow checks.
 *
 * Returns 0 on overflow or invalid input.
 *
 * For XYPixmap, the X protocol stores each bit-plane in its own slab of
 * (bytes_per_line * height) bytes, so the total is multiplied by depth. The
 * other formats use a single slab.
 */
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
    if (!image || !image->data)
        return 0;
    for (int y = 0; y < image->height; y++) {
        for (int x = 0; x < image->width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            if (!XPutPixel(image, x, y, pixel + (unsigned long) value))
                return 0;
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
    if (!image || !image->data)
        return NULL;
    if (x < 0 || y < 0 || image->width < 0 || image->height < 0 ||
        (unsigned int) x > (unsigned int) image->width ||
        (unsigned int) y > (unsigned int) image->height ||
        width > (unsigned int) image->width - (unsigned int) x ||
        height > (unsigned int) image->height - (unsigned int) y) {
        return NULL;
    }

    int bytesPerLine =
        paddedBytesPerLine(width, image->bits_per_pixel, image->bitmap_pad);

    /* paddedBytesPerLine returns 0 either for a legitimately empty image or
     * because the row would overflow int. The pixel loop below would still
     * iterate width*height times on a calloc(0, height) buffer, so reject the
     * overflow case explicitly.
     */
    if (bytesPerLine == 0 && width != 0)
        return NULL;
    size_t dataBytes = imageDataBufferSize(image->format, bytesPerLine, height,
                                           (unsigned int) image->depth);
    if (dataBytes == 0 && bytesPerLine != 0 && height != 0)
        return NULL;
    char *data = dataBytes ? calloc(1, dataBytes) : NULL;
    if (!data && dataBytes != 0)
        return NULL;
    XImage *subImage =
        XCreateImage(NULL, NULL, image->depth, image->format, image->xoffset,
                     data, width, height, image->bitmap_pad, bytesPerLine);
    if (!subImage) {
        free(data);
        return NULL;
    }

    /* XCreateImage above took a NULL visual, so it zeroed the channel masks. A
     * sub-image shares its parent's pixel format, and real Xlib carries the
     * parent's masks over, so copy them rather than leave a client inspecting
     * subImage->red_mask with 0.
     */
    subImage->red_mask = image->red_mask;
    subImage->green_mask = image->green_mask;
    subImage->blue_mask = image->blue_mask;

    for (unsigned int currY = 0; currY < height; currY++) {
        for (unsigned int currX = 0; currX < width; currX++) {
            XPutPixel(subImage, (int) currX, (int) currY,
                      XGetPixel(image, x + (int) currX, y + (int) currY));
        }
    }
    return subImage;
}

static int destroyImage(XImage *image)
{
    // https://tronche.com/gui/x/xlib/utilities/XDestroyImage.html
    if (image->data)
        free(image->data);
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
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(drawable, renderer);
    if (!renderer) {
        LOG("Failed to create renderer in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, drawable, 0, BadDrawable, 0);
        return -1;
    }

    if (!image) {
        handleError(0, display, drawable, 0, BadMatch, 0);
        return -1;
    }

    /* Reject geometries that would either overflow the scratch-buffer size or
     * exceed what SDL_Rect (signed int) and SDL_UpdateTexture's pitch parameter
     * (signed int) can address downstream. The width cap is INT_MAX /
     * sizeof(Uint32) so width * 4 fits in the pitch argument.
     */
    if (width > (unsigned int) (INT_MAX / sizeof(Uint32)) ||
        height > (unsigned int) INT_MAX ||
        (height != 0 && width > SIZE_MAX / sizeof(Uint32) / (size_t) height)) {
        handleError(0, display, drawable, 0, BadValue, 0);
        return -1;
    }

    /* Reject requests whose source rectangle does not lie wholly inside the
     * XImage. The old XGetPixel fallback silently returned 0 for out-of-bounds
     * pixels; the new direct fast paths read raw bytes from image->data and
     * would segfault. The width/height bound above guarantees the size_t
     * addition cannot wrap.
     */
    if (src_x < 0 || src_y < 0 || image->width < 0 || image->height < 0 ||
        (size_t) src_x + (size_t) width > (size_t) image->width ||
        (size_t) src_y + (size_t) height > (size_t) image->height) {
        handleError(0, display, drawable, 0, BadMatch, 0);
        return -1;
    }
    if (width > 0 && height > 0 && !image->data) {
        handleError(0, display, drawable, 0, BadMatch, 0);
        return -1;
    }

    /* Stride sanity for every format that dereferences image->data. A negative
     * or undersized bytes_per_line lets either the fast raw- byte paths or
     * XGetPixel (which still uses bytes_per_line for the row offset) step past
     * the caller's buffer. Caps:
     *   ZPixmap : ceil(maxX * bits_per_pixel / 8) bytes per row
     *   XYBitmap: ceil(maxX / 8) bytes per row
     * XYPixmap: ceil(maxX / 8) bytes per plane row
     */
    if (image->data && height > 0) {
        if (image->bytes_per_line < 0) {
            handleError(0, display, drawable, 0, BadMatch, 0);
            return -1;
        }
        size_t maxX = (size_t) src_x + (size_t) width;
        size_t minStride = 0;
        if (image->format == ZPixmap) {
            int bpp = image->bits_per_pixel;
            if (bpp <= 0 || bpp > 32 || maxX > (SIZE_MAX - 7) / (size_t) bpp) {
                handleError(0, display, drawable, 0, BadMatch, 0);
                return -1;
            }
            minStride = (maxX * (size_t) bpp + 7) / 8;
        } else if (image->format == XYBitmap || image->format == XYPixmap) {
            minStride = (maxX + 7) / 8;
        }
        if (minStride > 0 && (size_t) image->bytes_per_line < minStride) {
            handleError(0, display, drawable, 0, BadMatch, 0);
            return -1;
        }
    }

    GraphicContext *graphicContext =
        image->format == XYBitmap ? GET_GC(gc) : NULL;
    SDL_mutex *scratchLock = lockPutImageScratch();
    if (imageRowsAreRgba8888(display, image)) {
        if (image->depth == 32) {
            /* The low byte is a real alpha channel, so the caller rows are
             * already in the staging texture's layout: upload them straight
             * through with no per-pixel work.
             */
            int result = renderRgba8888Locked(
                drawable, gc, renderer, zpixmap32Row(image, src_x, src_y, 0),
                image->bytes_per_line, dest_x, dest_y, width, height);
            if (result == 1)
                x11compat_xputimage_direct_uploads++;
            unlockPutImageScratch(scratchLock);
            return result;
        }

        /* Depth below 32 leaves the low byte as padding, not alpha. Copying the
         * rows raw would upload a zero alpha and the image would vanish into
         * SDL's alpha-aware blend, so force each pixel opaque through the
         * scratch buffer, matching the alpha promotion xColorToRgba8888 applies
         * on the general path. The byte-order match in the predicate means the
         * memcpy word already carries the pixel in RGBA8888 order.
         */
        Uint32 *data =
            ensurePutImageScratchBuffer((size_t) width * (size_t) height);
        if (!data) {
            unlockPutImageScratch(scratchLock);
            handleOutOfMemory(0, display, 0, 0);
            return -1;
        }
        for (unsigned int y = 0; y < height; y++) {
            const char *srcRow = zpixmap32Row(image, src_x, src_y, y);
            Uint32 *dst = data + (size_t) y * width;
            for (unsigned int x = 0; x < width; x++) {
                Uint32 color;
                memcpy(&color, srcRow + x * sizeof(Uint32), sizeof(Uint32));
                dst[x] = color | 0x000000FFu;
            }
        }
        int result = renderRgba8888Locked(drawable, gc, renderer, data,
                                          (int) (width * sizeof(Uint32)),
                                          dest_x, dest_y, width, height);
        unlockPutImageScratch(scratchLock);
        return result;
    }

    /* Hold the scratch lock across pixel build + texture upload + RenderCopy so
     * a concurrent XPutImage cannot realloc the pixel buffer or destroy the
     * staging texture mid-flight.
     */
    Uint32 *data =
        ensurePutImageScratchBuffer((size_t) width * (size_t) height);
    if (!data) {
        unlockPutImageScratch(scratchLock);
        handleOutOfMemory(0, display, 0, 0);
        return -1;
    }

    if (!image->data) {
        if (image->format == XYBitmap && graphicContext) {
            Uint32 background = xColorToRgba8888(graphicContext->background);
            for (unsigned int y = 0; y < height; y++) {
                Uint32 *dst = data + y * width;
                for (unsigned int x = 0; x < width; x++)
                    dst[x] = background;
            }
        } else {
            memset(data, 0, sizeof(Uint32) * width * height);
        }
    } else if (image->format == ZPixmap && image->bits_per_pixel == 32) {
        /* Caller-supplied image->data carries no guaranteed type or alignment,
         * so read each 32-bit pixel through memcpy. A direct (Uint32 *) cast of
         * the char buffer is undefined under strict aliasing and would bite
         * once this file is compiled optimized; memcpy is the aliasing-safe
         * punt and the compiler lowers it to a plain load, so the loop still
         * vectorizes.
         */
        for (unsigned int y = 0; y < height; y++) {
            const char *srcRow = zpixmap32Row(image, src_x, src_y, y);
            Uint32 *dst = data + y * width;
            for (unsigned int x = 0; x < width; x++) {
                Uint32 color;
                memcpy(&color, srcRow + x * sizeof(Uint32), sizeof(Uint32));
                dst[x] = xColorToRgba8888(color);
            }
        }
    } else if (image->format == XYBitmap) {
        Uint32 foreground = xColorToRgba8888(graphicContext->foreground);
        Uint32 background = xColorToRgba8888(graphicContext->background);
        Bool msbFirst = image->bitmap_bit_order == MSBFirst;

        /* The 256-entry LUT below costs ~2us to build, only worth it when the
         * image is large enough to amortize over many bytes. For small inputs
         * the original per-pixel bit test wins.
         */
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
                    (size_t) image->bytes_per_line * ((size_t) src_y + y);
                Uint32 *dst = data + y * width;
                unsigned int x = 0;
                unsigned int srcBitX = (unsigned int) src_x;

                /* Head: pixels before the first aligned source byte. */
                while (x < width && (srcBitX & 7) != 0) {
                    unsigned char byte = srcRow[srcBitX >> 3];
                    dst[x++] = lut[byte][srcBitX & 7];
                    srcBitX++;
                }

                /* Aligned middle: one source byte at a time produces eight
                 * destination pixels per memcpy.
                 */
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
                        (size_t) image->bytes_per_line * ((size_t) src_y + y) +
                        ((size_t) src_x + x) / 8;
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
                data[y * width + x] = xColorToRgba8888(color);
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
                data[y * width + x] = xColorToRgba8888(color);
            }
        }
    }

    int result = renderRgba8888Locked(drawable, gc, renderer, data,
                                      (int) (width * sizeof(Uint32)), dest_x,
                                      dest_y, width, height);
    unlockPutImageScratch(scratchLock);
    return result;
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
         * SDL_Windows present independently, the SCREEN renderer's backing
         * surface is never updated with their pixels, and there is no single
         * root framebuffer to capture from. Returning NULL matches what Xlib
         * does when the request is rejected.
         */
        LOG("XGetImage on SCREEN_WINDOW is not supported by this shim.\n");
        return NULL;
    }

    if (IS_TYPE(drawable, WINDOW) && IS_INPUT_ONLY(drawable)) {
        handleError(0, display, drawable, 0, BadMatch, 0);
        return NULL;
    }

    /* SDL_Rect carries signed int, and the per-row pointer math below uses
     * width and height as positive offsets; reject any dimension that would
     * cast to a negative int before allocations are attempted.
     */
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

    /* paddedBytesPerLine returns 0 either for a legitimately empty image or
     * because the row size would overflow int. Either way, an extreme width
     * could make calloc(0, height) succeed even though the width*height pixel
     * loop below would walk off the buffer.
     */
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
    SDL_Rect srcRect = {x, y, (int) width, (int) height};
    SDL_Surface *drawableSurface;

    /* Pixmaps read through their lazy cache so repeated XGetImage on unchanged
     * contents does not stall on SDL_RenderReadPixels each call. Routing around
     * GET_RENDERER also keeps the read from re-marking the pixmap dirty.
     */
    if (IS_TYPE(drawable, PIXMAP)) {
        drawableSurface = getPixmapSurfaceRect(drawable, &srcRect);
    } else {
        SDL_Renderer *renderer = NULL;
        GET_RENDERER(drawable, renderer);
        if (!renderer) {
            LOG("Failed to create renderer in %s: %s\n", __func__,
                SDL_GetError());
            handleError(0, display, drawable, 0, BadDrawable, 0);
            XDestroyImage(image);
            return NULL;
        }
        drawableSurface = getRenderSurfaceRect(renderer, &srcRect);
    }
    if (!drawableSurface) {
        XDestroyImage(image);
        return NULL;
    }

    if (format == ZPixmap && image->bits_per_pixel == 32) {
        for (unsigned int currY = 0; currY < height; currY++) {
            /* drawableSurface->pixels is opaque SDL storage with no declared
             * type, so read each pixel through memcpy rather than a Uint32 *
             * cast, which would be undefined under strict aliasing once this
             * file is compiled optimized. image->data is our own allocation, so
             * the Uint32 store below sets its effective type and stays safe.
             */
            const char *srcRow = (const char *) drawableSurface->pixels +
                                 currY * drawableSurface->pitch;
            Uint32 *dst =
                (Uint32 *) (image->data + currY * image->bytes_per_line);
            if (plane_mask == (unsigned long) ~0) {
                for (unsigned int currX = 0; currX < width; currX++) {
                    Uint32 rgba;
                    memcpy(&rgba, srcRow + currX * sizeof(Uint32),
                           sizeof(Uint32));
                    dst[currX] = (Uint32) rgba8888ToXColor(rgba);
                }
            } else {
                for (unsigned int currX = 0; currX < width; currX++) {
                    Uint32 rgba;
                    memcpy(&rgba, srcRow + currX * sizeof(Uint32),
                           sizeof(Uint32));
                    dst[currX] = (Uint32) (plane_mask & rgba8888ToXColor(rgba));
                }
            }
        }
    } else {
        for (unsigned int currX = 0; currX < width; currX++) {
            for (unsigned int currY = 0; currY < height; currY++) {
                Uint32 sourcePixel = getPixel(drawableSurface, currX, currY);
                unsigned long imagePixel =
                    image->bits_per_pixel == 1
                        ? oneBitPixelFromRgba(sourcePixel)
                        : sourcePixel;
                XPutPixel(image, (int) currX, (int) currY,
                          plane_mask & imagePixel);
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
    if (!image)
        return NULL;
    if (!dest_image)
        return image;
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
