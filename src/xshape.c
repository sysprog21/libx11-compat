#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xregion.h>
#include <X11/extensions/shape.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "drawing.h"
#include "display.h"
#include "resource-types.h"

/* The X Shape extension reshapes windows from rectangles to arbitrary regions.
 * SDL-managed windows remain rectangular, so the compat layer stores masks
 * locally and applies them while drawing or querying shape state.
 */

Bool XShapeQueryExtension(Display *dpy, int *event_basep, int *error_basep)
{
    int opcode = 0;
    return XQueryExtension(dpy, SHAPENAME, &opcode, event_basep, error_basep);
}

static Bool shapeSlots(WindowStruct *window,
                       int kind,
                       SDL_Surface ***maskSlot,
                       int **offsetXSlot,
                       int **offsetYSlot)
{
    if (kind == ShapeBounding) {
        *maskSlot = &window->shapeBoundingMask;
        *offsetXSlot = &window->shapeBoundingOffsetX;
        *offsetYSlot = &window->shapeBoundingOffsetY;
        return True;
    }
    if (kind == ShapeClip) {
        *maskSlot = &window->shapeClipMask;
        *offsetXSlot = &window->shapeClipOffsetX;
        *offsetYSlot = &window->shapeClipOffsetY;
        return True;
    }
    return False;
}

static Bool isValidShapeOp(int op)
{
    return op == ShapeSet || op == ShapeUnion || op == ShapeIntersect ||
           op == ShapeSubtract || op == ShapeInvert;
}

static SDL_Surface *createShapeSurface(int w, int h)
{
    if (w < 0 || h < 0)
        return NULL;
    if (w == 0)
        w = 1;
    if (h == 0)
        h = 1;

    SDL_Surface *surface = SDL_CreateRGBSurface(
        0, w, h, SDL_SURFACE_DEPTH, DEFAULT_RED_MASK, DEFAULT_GREEN_MASK,
        DEFAULT_BLUE_MASK, DEFAULT_ALPHA_MASK);
    if (!surface)
        return NULL;
    Uint32 black = SDL_MapRGBA(XC_SURFACE_FORMAT(surface), 0, 0, 0, 255);
    SDL_FillRect(surface, NULL, black);
    return surface;
}

static Bool maskPixelActive(SDL_Surface *mask, int x, int y)
{
    if (!mask || x < 0 || y < 0 || x >= mask->w || y >= mask->h)
        return False;
    Uint32 pixel = getPixel(mask, (unsigned int) x, (unsigned int) y);
    Uint8 r = 0, g = 0, b = 0;
    SDL_GetRGB(pixel, XC_SURFACE_FORMAT(mask), &r, &g, &b);
    return r || g || b;
}

Bool shapeSurfaceContains(SDL_Surface *mask,
                          int offsetX,
                          int offsetY,
                          int64_t x,
                          int64_t y)
{
    int64_t mx = x - (int64_t) offsetX, my = y - (int64_t) offsetY;
    if (mx < 0 || my < 0 || mx > INT_MAX || my > INT_MAX)
        return False;
    return maskPixelActive(mask, (int) mx, (int) my);
}

static Bool defaultWindowContains(WindowStruct *window, int64_t x, int64_t y)
{
    return window && x >= 0 && y >= 0 && x < (int64_t) window->w &&
           y < (int64_t) window->h;
}

static Bool shapeRegionContains(SDL_Surface *mask,
                                int offsetX,
                                int offsetY,
                                WindowStruct *defaultWindow,
                                int64_t x,
                                int64_t y)
{
    if (mask)
        return shapeSurfaceContains(mask, offsetX, offsetY, x, y);
    return defaultWindowContains(defaultWindow, x, y);
}

static Bool sourceRegionContains(SDL_Surface *src,
                                 int xOff,
                                 int yOff,
                                 int64_t x,
                                 int64_t y)
{
    return src && shapeSurfaceContains(src, xOff, yOff, x, y);
}

static SDL_Surface *copyShapeSurface(SDL_Surface *src)
{
    if (!src)
        return NULL;
    SDL_Surface *copy = createShapeSurface(src->w, src->h);
    if (!copy)
        return NULL;
    Uint32 white = SDL_MapRGBA(XC_SURFACE_FORMAT(copy), 255, 255, 255, 255);
    for (int y = 0; y < src->h; y++) {
        for (int x = 0; x < src->w; x++) {
            if (maskPixelActive(src, x, y))
                putPixel(copy, (unsigned int) x, (unsigned int) y, white);
        }
    }
    return copy;
}

static Bool includeBounds(int64_t x,
                          int64_t y,
                          int64_t w,
                          int64_t h,
                          Bool *hasBounds,
                          int64_t *minX,
                          int64_t *minY,
                          int64_t *maxX,
                          int64_t *maxY)
{
    if (w <= 0 || h <= 0)
        return True;
    if (x < INT_MIN || y < INT_MIN || x > INT_MAX || y > INT_MAX ||
        w > INT_MAX || h > INT_MAX)
        return False;
    int64_t right = x + w - 1;
    int64_t bottom = y + h - 1;
    if (right > INT_MAX || bottom > INT_MAX)
        return False;
    if (!*hasBounds) {
        *minX = x;
        *minY = y;
        *maxX = right;
        *maxY = bottom;
        *hasBounds = True;
        return True;
    }
    if (x < *minX)
        *minX = x;
    if (y < *minY)
        *minY = y;
    if (right > *maxX)
        *maxX = right;
    if (bottom > *maxY)
        *maxY = bottom;
    return True;
}

static SDL_Surface *combineShapeSurfaces(WindowStruct *window,
                                         SDL_Surface *oldMask,
                                         int oldOffsetX,
                                         int oldOffsetY,
                                         SDL_Surface *srcMask,
                                         int srcOffsetX,
                                         int srcOffsetY,
                                         int op,
                                         int *outOffsetX,
                                         int *outOffsetY)
{
    if (op == ShapeSet) {
        if (!srcMask)
            return NULL;
        SDL_Surface *copy = copyShapeSurface(srcMask);
        if (copy) {
            *outOffsetX = srcOffsetX;
            *outOffsetY = srcOffsetY;
        }
        return copy;
    }

    Bool hasBounds = False;
    int64_t minX = 0, minY = 0, maxX = 0, maxY = 0;
    if (oldMask) {
        if (!includeBounds(oldOffsetX, oldOffsetY, oldMask->w, oldMask->h,
                           &hasBounds, &minX, &minY, &maxX, &maxY))
            return NULL;
    } else if (!includeBounds(0, 0, window->w, window->h, &hasBounds, &minX,
                              &minY, &maxX, &maxY)) {
        return NULL;
    }
    if (srcMask &&
        !includeBounds(srcOffsetX, srcOffsetY, srcMask->w, srcMask->h,
                       &hasBounds, &minX, &minY, &maxX, &maxY))
        return NULL;

    if (!hasBounds) {
        *outOffsetX = 0;
        *outOffsetY = 0;
        return createShapeSurface(0, 0);
    }
    int64_t outW64 = maxX - minX + 1;
    int64_t outH64 = maxY - minY + 1;
    if (minX < INT_MIN || minY < INT_MIN || minX > INT_MAX || minY > INT_MAX ||
        outW64 > INT_MAX || outH64 > INT_MAX)
        return NULL;

    SDL_Surface *out = createShapeSurface((int) outW64, (int) outH64);
    if (!out)
        return NULL;

    Uint32 white = SDL_MapRGBA(XC_SURFACE_FORMAT(out), 255, 255, 255, 255);
    /* A window is shaped iff a mask was installed, exactly like real X: a
     * combine that ends up admitting every window pixel still leaves the window
     * shaped (ShapeSet keeps such a mask too), so do not special-case full
     * coverage here.
     */
    for (int y = 0; y < out->h; y++) {
        int64_t wy = minY + y;
        for (int x = 0; x < out->w; x++) {
            int64_t wx = minX + x;
            Bool oldIn = shapeRegionContains(oldMask, oldOffsetX, oldOffsetY,
                                             window, wx, wy);
            Bool srcIn =
                sourceRegionContains(srcMask, srcOffsetX, srcOffsetY, wx, wy);
            Bool active = False;
            switch (op) {
            case ShapeUnion:
                active = oldIn || srcIn;
                break;
            case ShapeIntersect:
                active = oldIn && srcIn;
                break;
            case ShapeSubtract:
                active = oldIn && !srcIn;
                break;
            case ShapeInvert:
                active = srcIn && !oldIn;
                break;
            default:
                active = False;
                break;
            }
            if (active)
                putPixel(out, (unsigned int) x, (unsigned int) y, white);
        }
    }

    *outOffsetX = (int) minX;
    *outOffsetY = (int) minY;
    return out;
}

static Bool shapeActiveExtents(SDL_Surface *mask,
                               int offsetX,
                               int offsetY,
                               int *x,
                               int *y,
                               unsigned int *w,
                               unsigned int *h)
{
    Bool found = False;
    int64_t minX = 0, minY = 0, maxX = 0, maxY = 0;
    for (int yy = 0; yy < mask->h; yy++) {
        for (int xx = 0; xx < mask->w; xx++) {
            if (!maskPixelActive(mask, xx, yy))
                continue;
            int64_t wx = (int64_t) xx + (int64_t) offsetX;
            int64_t wy = (int64_t) yy + (int64_t) offsetY;
            if (!found) {
                minX = maxX = wx;
                minY = maxY = wy;
                found = True;
                continue;
            }
            if (wx < minX)
                minX = wx;
            if (wy < minY)
                minY = wy;
            if (wx > maxX)
                maxX = wx;
            if (wy > maxY)
                maxY = wy;
        }
    }
    if (!found) {
        if (x)
            *x = offsetX;
        if (y)
            *y = offsetY;
        if (w)
            *w = 0;
        if (h)
            *h = 0;
        return False;
    }
    if (x)
        *x = minX < INT_MIN ? INT_MIN : (minX > INT_MAX ? INT_MAX : (int) minX);
    if (y)
        *y = minY < INT_MIN ? INT_MIN : (minY > INT_MAX ? INT_MAX : (int) minY);
    if (w)
        *w = maxX - minX + 1 > UINT_MAX ? UINT_MAX
                                        : (unsigned int) (maxX - minX + 1);
    if (h)
        *h = maxY - minY + 1 > UINT_MAX ? UINT_MAX
                                        : (unsigned int) (maxY - minY + 1);
    return True;
}

static void repaintBoundingMask(Display *display, Window dest)
{
    (void) display;
    WindowStruct *window = GET_WINDOW_STRUCT(dest);
    SDL_Renderer *destRenderer = getWindowRenderer(dest);
    if (!destRenderer)
        return;
    SDL_SetRenderDrawBlendMode(destRenderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(destRenderer, 0, 0, 0, 255);
    unsigned int maxY =
        window->h > (unsigned int) INT_MAX ? (unsigned int) INT_MAX : window->h;
    unsigned int maxX =
        window->w > (unsigned int) INT_MAX ? (unsigned int) INT_MAX : window->w;
    for (unsigned int y = 0; y < maxY; y++) {
        for (unsigned int x = 0; x < maxX; x++) {
            if (shapeSurfaceContains(window->shapeBoundingMask,
                                     window->shapeBoundingOffsetX,
                                     window->shapeBoundingOffsetY, x, y))
                continue;
            SDL_RenderDrawPoint(destRenderer, (int) x, (int) y);
        }
    }
    presentDrawableIfVisible(dest);
}

Status XShapeQueryVersion(Display *dpy,
                          int *major_versionp,
                          int *minor_versionp)
{
    (void) dpy;
    if (major_versionp)
        *major_versionp = SHAPE_MAJOR_VERSION;
    if (minor_versionp)
        *minor_versionp = SHAPE_MINOR_VERSION;
    return 1;
}

/* Combine an already-rasterized source mask (srcSurface, at window-relative
 * offset srcOffsetX/Y) into the window's bounding or clip slot. srcSurface may
 * be NULL: with ShapeSet that clears the slot, with any other op it is the
 * empty source region. The caller retains ownership of srcSurface. This is the
 * shared tail of the rectangle-, region-, and mask-combine entry points.
 */
static void installShapeCombine(Display *dpy,
                                Window dest,
                                int destKind,
                                SDL_Surface *srcSurface,
                                int srcOffsetX,
                                int srcOffsetY,
                                int op)
{
    WindowStruct *window = GET_WINDOW_STRUCT(dest);
    SDL_Surface **maskSlot = NULL;
    int *offsetXSlot = NULL;
    int *offsetYSlot = NULL;
    if (!shapeSlots(window, destKind, &maskSlot, &offsetXSlot, &offsetYSlot))
        return;

    int newOffsetX = 0, newOffsetY = 0;
    SDL_Surface *newMask = combineShapeSurfaces(
        window, *maskSlot, *offsetXSlot, *offsetYSlot, srcSurface, srcOffsetX,
        srcOffsetY, op, &newOffsetX, &newOffsetY);
    /* NULL from a real source means the combine failed: keep the existing mask.
     * NULL from a NULL source under ShapeSet is the explicit clear, which drops
     * the installed mask and stops treating the window as shaped.
     */
    if (!newMask && !(!srcSurface && op == ShapeSet))
        return;

    if (*maskSlot)
        SDL_FreeSurface(*maskSlot);
    *maskSlot = newMask;
    *offsetXSlot = newMask ? newOffsetX : 0;
    *offsetYSlot = newMask ? newOffsetY : 0;

    if (destKind != ShapeBounding)
        return;
    invalidateVisibleRegionForTopLevel(dest);
    if (*maskSlot)
        repaintBoundingMask(dpy, dest);
}

/* Rasterize a rectangle list into a shape mask surface: pixels inside the union
 * of the rectangles are opaque, everything else transparent. The surface spans
 * the rectangles' bounding box; *localMinX/Y receive that box's origin in
 * rectangle-relative coordinates so the caller can bias it by xOff/yOff. An
 * empty (or all-degenerate) list yields a 1x1 transparent surface, which under
 * ShapeSet means "fully clipped", not "unshaped".
 *
 * Returns NULL only on allocation failure.
 */
static SDL_Surface *rasterizeShapeRects(const XRectangle *rects,
                                        int n_rects,
                                        int *localMinX,
                                        int *localMinY)
{
    Bool hasBounds = False;
    int minX = 0, minY = 0, maxX = 0, maxY = 0;
    for (int i = 0; i < n_rects; i++) {
        int rw = rects[i].width, rh = rects[i].height;
        if (rw <= 0 || rh <= 0)
            continue;
        int rx = rects[i].x, ry = rects[i].y;
        int right = rx + rw, bottom = ry + rh;
        if (!hasBounds) {
            minX = rx, minY = ry, maxX = right, maxY = bottom;
            hasBounds = True;
            continue;
        }
        if (rx < minX)
            minX = rx;
        if (ry < minY)
            minY = ry;
        if (right > maxX)
            maxX = right;
        if (bottom > maxY)
            maxY = bottom;
    }

    if (!hasBounds) {
        *localMinX = 0;
        *localMinY = 0;
        return createShapeSurface(0, 0);
    }

    SDL_Surface *surface = createShapeSurface(maxX - minX, maxY - minY);
    if (!surface)
        return NULL;
    Uint32 white = SDL_MapRGBA(XC_SURFACE_FORMAT(surface), 255, 255, 255, 255);
    for (int i = 0; i < n_rects; i++) {
        if (rects[i].width <= 0 || rects[i].height <= 0)
            continue;
        SDL_Rect r = {
            .x = rects[i].x - minX,
            .y = rects[i].y - minY,
            .w = rects[i].width,
            .h = rects[i].height,
        };
        SDL_FillRect(surface, &r, white);
    }
    *localMinX = minX;
    *localMinY = minY;
    return surface;
}

/* Bias a shape origin by a mask's bounding-box offset without signed overflow;
 * xOff/yOff are client-supplied, so a hostile value must not invoke UB.
 */
static Bool addShapeOffset(int base, int delta, int *out)
{
    int64_t v = (int64_t) base + (int64_t) delta;
    if (v < INT_MIN || v > INT_MAX)
        return False;
    *out = (int) v;
    return True;
}

void XShapeCombineRegion(Display *dpy,
                         Window dest,
                         int destKind,
                         int xOff,
                         int yOff,
                         Region r,
                         int op)
{
    if (!isValidShapeOp(op) ||
        (destKind != ShapeBounding && destKind != ShapeClip))
        return;
    if (dest == None || !IS_TYPE(dest, WINDOW) || !r)
        return;

    /* A Region is a YX-banded BOX list; convert to XRectangles and reuse the
     * rectangle rasterizer rather than writing a second one.
     */
    REGION *region = (REGION *) r;
    long n = region->numRects;
    if (n < 0 || n > INT_MAX || (size_t) n > SIZE_MAX / sizeof(XRectangle))
        return;
    XRectangle *rects = NULL;
    if (n > 0) {
        rects = malloc(sizeof(XRectangle) * (size_t) n);
        if (!rects)
            return;
        for (long i = 0; i < n; i++) {
            BOX *box = &region->rects[i];
            rects[i].x = box->x1;
            rects[i].y = box->y1;
            /* Canonical pixman boxes have x2 >= x1; clamp an inverted box to a
             * zero-size rect (filtered by the rasterizer) rather than wrapping
             * the unsigned width into a huge span.
             */
            rects[i].width =
                box->x2 > box->x1 ? (unsigned short) (box->x2 - box->x1) : 0;
            rects[i].height =
                box->y2 > box->y1 ? (unsigned short) (box->y2 - box->y1) : 0;
        }
    }

    int localMinX = 0, localMinY = 0;
    SDL_Surface *srcSurface =
        rasterizeShapeRects(rects, (int) n, &localMinX, &localMinY);
    free(rects);
    if (!srcSurface)
        return;
    int srcOffsetX = 0, srcOffsetY = 0;
    if (addShapeOffset(xOff, localMinX, &srcOffsetX) &&
        addShapeOffset(yOff, localMinY, &srcOffsetY))
        installShapeCombine(dpy, dest, destKind, srcSurface, srcOffsetX,
                            srcOffsetY, op);
    SDL_FreeSurface(srcSurface);
}

void XShapeCombineRectangles(Display *dpy,
                             XID dest,
                             int destKind,
                             int xOff,
                             int yOff,
                             XRectangle *rects,
                             int n_rects,
                             int op,
                             int ordering)
{
    /* The ordering hint (Unsorted/YSorted/YXSorted/YXBanded) only lets a real
     * server skip sorting; rasterizing by fill is order-independent, so it has
     * no effect here.
     */
    (void) ordering;
    if (!isValidShapeOp(op) ||
        (destKind != ShapeBounding && destKind != ShapeClip))
        return;
    if (dest == None || !IS_TYPE(dest, WINDOW))
        return;
    if (n_rects < 0 || (n_rects > 0 && !rects))
        return;

    int localMinX = 0, localMinY = 0;
    SDL_Surface *srcSurface =
        rasterizeShapeRects(rects, n_rects, &localMinX, &localMinY);
    if (!srcSurface)
        return;
    int srcOffsetX = 0, srcOffsetY = 0;
    if (addShapeOffset(xOff, localMinX, &srcOffsetX) &&
        addShapeOffset(yOff, localMinY, &srcOffsetY))
        installShapeCombine(dpy, dest, destKind, srcSurface, srcOffsetX,
                            srcOffsetY, op);
    SDL_FreeSurface(srcSurface);
}

void XShapeCombineMask(Display *dpy,
                       XID dest,
                       int destKind,
                       int xOff,
                       int yOff,
                       Pixmap src,
                       int op)
{
    if (!isValidShapeOp(op) ||
        (destKind != ShapeBounding && destKind != ShapeClip))
        return;
    if (dest == None || !IS_TYPE(dest, WINDOW))
        return;
    if (src != None && !IS_TYPE(src, PIXMAP))
        return;

    /* src == None is the empty source region: under ShapeSet it clears the
     * installed mask, under any other op it combines with nothing.
     */
    SDL_Surface *srcSurface = NULL;
    if (src != None) {
        PixmapStruct *pixmap = GET_PIXMAP_STRUCT(src);
        if (!pixmap)
            return;

        /* SDL_Rect uses signed int; reject pixmaps whose dimensions would
         * alias.
         */
        if (pixmap->width > (unsigned int) INT_MAX ||
            pixmap->height > (unsigned int) INT_MAX)
            return;

        SDL_Renderer *srcRenderer;
        GET_RENDERER(src, srcRenderer);
        if (!srcRenderer)
            return;
        SDL_Rect rect = {
            .x = 0,
            .y = 0,
            .w = (int) pixmap->width,
            .h = (int) pixmap->height,
        };
        srcSurface = getRenderSurfaceRect(srcRenderer, &rect);
        if (!srcSurface)
            return;
    }

    installShapeCombine(dpy, dest, destKind, srcSurface, xOff, yOff, op);
    SDL_FreeSurface(srcSurface);
}

void XShapeCombineShape(Display *dpy,
                        XID dest,
                        int destKind,
                        int xOff,
                        int yOff,
                        Window src,
                        int srcKind,
                        int op)
{
    if (!isValidShapeOp(op) ||
        (destKind != ShapeBounding && destKind != ShapeClip) ||
        (srcKind != ShapeBounding && srcKind != ShapeClip))
        return;
    if (dest == None || !IS_TYPE(dest, WINDOW) || src == None ||
        !IS_TYPE(src, WINDOW))
        return;

    WindowStruct *srcWindow = GET_WINDOW_STRUCT(src);
    SDL_Surface **srcMaskSlot = NULL;
    int *srcOffXSlot = NULL, *srcOffYSlot = NULL;
    if (!shapeSlots(srcWindow, srcKind, &srcMaskSlot, &srcOffXSlot,
                    &srcOffYSlot))
        return;

    /* The source is another window's installed shape. When that window is
     * unshaped its shape is its full rectangle, so synthesize an all-inside
     * mask of the window size (owned here and freed below). A shaped source
     * lends its live mask surface, read-only, so it is not freed here.
     *
     * The default bounding region is taken as (0,0,w,h), excluding the border,
     * to match combineShapeSurfaces' own default bounds, defaultWindowContains,
     * and XShapeQueryExtents. Real X includes the border in the bounding shape;
     * honoring that would be a layer-wide convention change, not a local one.
     */
    SDL_Surface *srcSurface = *srcMaskSlot;
    SDL_Surface *ownedSurface = NULL;
    int srcBaseX = *srcOffXSlot, srcBaseY = *srcOffYSlot;
    if (!srcSurface) {
        if (srcWindow->w > (unsigned int) INT_MAX ||
            srcWindow->h > (unsigned int) INT_MAX)
            return;
        ownedSurface =
            createShapeSurface((int) srcWindow->w, (int) srcWindow->h);
        if (!ownedSurface)
            return;
        SDL_FillRect(
            ownedSurface, NULL,
            SDL_MapRGBA(XC_SURFACE_FORMAT(ownedSurface), 255, 255, 255, 255));
        srcSurface = ownedSurface;
        srcBaseX = 0;
        srcBaseY = 0;
    }

    int srcOffsetX = 0, srcOffsetY = 0;
    if (addShapeOffset(xOff, srcBaseX, &srcOffsetX) &&
        addShapeOffset(yOff, srcBaseY, &srcOffsetY))
        installShapeCombine(dpy, dest, destKind, srcSurface, srcOffsetX,
                            srcOffsetY, op);
    if (ownedSurface)
        SDL_FreeSurface(ownedSurface);
}

void XShapeOffsetShape(Display *dpy, XID dest, int destKind, int xOff, int yOff)
{
    if (destKind != ShapeBounding && destKind != ShapeClip)
        return;
    if (dest == None || !IS_TYPE(dest, WINDOW))
        return;
    WindowStruct *window = GET_WINDOW_STRUCT(dest);
    SDL_Surface **maskSlot = NULL;
    int *offsetXSlot = NULL, *offsetYSlot = NULL;
    if (!shapeSlots(window, destKind, &maskSlot, &offsetXSlot, &offsetYSlot))
        return;
    /* Translating an absent shape is a no-op: an unshaped window stays
     * rectangular.
     */
    if (!*maskSlot)
        return;

    int newX = 0, newY = 0;
    if (!addShapeOffset(*offsetXSlot, xOff, &newX) ||
        !addShapeOffset(*offsetYSlot, yOff, &newY))
        return;
    *offsetXSlot = newX;
    *offsetYSlot = newY;

    if (destKind != ShapeBounding)
        return;
    invalidateVisibleRegionForTopLevel(dest);
    repaintBoundingMask(dpy, dest);
}

Status XShapeQueryExtents(Display *dpy,
                          Window w,
                          Bool *bShaped,
                          int *xbs,
                          int *ybs,
                          unsigned int *wbs,
                          unsigned int *hbs,
                          Bool *cShaped,
                          int *xcs,
                          int *ycs,
                          unsigned int *wcs,
                          unsigned int *hcs)
{
    (void) dpy;
    if (w == None || !IS_TYPE(w, WINDOW))
        return 0;
    WindowStruct *window = GET_WINDOW_STRUCT(w);
    SDL_Surface *bm = window->shapeBoundingMask;
    SDL_Surface *cm = window->shapeClipMask;
    int bx = 0, by = 0, cx = 0, cy = 0;
    unsigned int bw = window->w, bh = window->h, cw = window->w, ch = window->h;
    if (bm)
        shapeActiveExtents(bm, window->shapeBoundingOffsetX,
                           window->shapeBoundingOffsetY, &bx, &by, &bw, &bh);
    if (cm)
        shapeActiveExtents(cm, window->shapeClipOffsetX,
                           window->shapeClipOffsetY, &cx, &cy, &cw, &ch);
    if (bShaped)
        *bShaped = bm != NULL;
    if (cShaped)
        *cShaped = cm != NULL;
    if (xbs)
        *xbs = bx;
    if (ybs)
        *ybs = by;
    if (wbs)
        *wbs = bw;
    if (hbs)
        *hbs = bh;
    if (xcs)
        *xcs = cx;
    if (ycs)
        *ycs = cy;
    if (wcs)
        *wcs = cw;
    if (hcs)
        *hcs = ch;
    return 1;
}

void XShapeSelectInput(Display *dpy, Window window, unsigned long mask)
{
    (void) dpy;
    (void) window;
    (void) mask;
}

unsigned long XShapeInputSelected(Display *dpy, Window window)
{
    (void) dpy;
    (void) window;
    return 0;
}

/* Decompose an installed mask into window-relative rectangles, one horizontal
 * run of active pixels per scanline. Height-1 rows sorted top-to-bottom then
 * left-to-right are YXBanded, which is what X clients expect back.
 *
 * Returns a malloc'd array (freed by XFree) or NULL when the mask has no active
 * pixels.
 */
static XRectangle *maskToRectangles(SDL_Surface *mask,
                                    int offsetX,
                                    int offsetY,
                                    int *count)
{
    size_t capacity = 0;
    int n = 0;
    XRectangle *rects = NULL;
    for (int y = 0; y < mask->h; y++) {
        int x = 0;
        while (x < mask->w) {
            if (!maskPixelActive(mask, x, y)) {
                x++;
                continue;
            }
            int start = x;
            while (x < mask->w && maskPixelActive(mask, x, y))
                x++;
            /* XRectangle carries 16-bit coordinates, matching X's own rect
             * type. Skip a run whose window coordinates or width do not fit
             * rather than emit wrapped values; only an absurd offset or a run
             * wider than 64k can trip this, neither of which a real shape hits.
             */
            int64_t rx = (int64_t) start + offsetX;
            int64_t ry = (int64_t) y + offsetY;
            int runWidth = x - start;
            if (rx < SHRT_MIN || rx > SHRT_MAX || ry < SHRT_MIN ||
                ry > SHRT_MAX || runWidth > USHRT_MAX)
                continue;
            if ((size_t) n == capacity) {
                if (capacity > (SIZE_MAX / sizeof(XRectangle)) / 2) {
                    free(rects);
                    *count = 0;
                    return NULL;
                }
                size_t newCap = capacity ? capacity * 2 : 16;
                XRectangle *grown = realloc(rects, sizeof(XRectangle) * newCap);
                if (!grown) {
                    free(rects);
                    *count = 0;
                    return NULL;
                }
                rects = grown;
                capacity = newCap;
            }
            rects[n].x = (short) rx;
            rects[n].y = (short) ry;
            rects[n].width = (unsigned short) runWidth;
            rects[n].height = 1;
            n++;
        }
    }
    *count = n;
    return rects;
}

XRectangle *XShapeGetRectangles(Display *dpy,
                                Window window,
                                int kind,
                                int *count,
                                int *ordering)
{
    (void) dpy;
    if (count)
        *count = 0;
    if (ordering)
        *ordering = 0;
    if (window == None || !IS_TYPE(window, WINDOW))
        return NULL;
    if (kind != ShapeBounding && kind != ShapeClip)
        return NULL;

    WindowStruct *win = GET_WINDOW_STRUCT(window);
    SDL_Surface *mask =
        kind == ShapeBounding ? win->shapeBoundingMask : win->shapeClipMask;
    int offsetX = kind == ShapeBounding ? win->shapeBoundingOffsetX
                                        : win->shapeClipOffsetX;
    int offsetY = kind == ShapeBounding ? win->shapeBoundingOffsetY
                                        : win->shapeClipOffsetY;
    if (!mask) {
        if (win->w > USHRT_MAX || win->h > USHRT_MAX)
            return NULL;
        XRectangle *rects = malloc(sizeof(*rects));
        if (!rects)
            return NULL;
        rects[0] = (XRectangle) {0, 0, (unsigned short) win->w,
                                 (unsigned short) win->h};
        if (count)
            *count = 1;
        if (ordering)
            *ordering = YXBanded;
        return rects;
    }

    int n = 0;
    XRectangle *rects = maskToRectangles(mask, offsetX, offsetY, &n);
    if (!rects)
        return NULL;
    if (count)
        *count = n;
    if (ordering)
        *ordering = YXBanded;
    return rects;
}
