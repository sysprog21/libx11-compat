#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

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
    Uint32 black = SDL_MapRGBA(surface->format, 0, 0, 0, 255);
    SDL_FillRect(surface, NULL, black);
    return surface;
}

static Bool maskPixelActive(SDL_Surface *mask, int x, int y)
{
    if (!mask || x < 0 || y < 0 || x >= mask->w || y >= mask->h)
        return False;
    Uint32 pixel = getPixel(mask, (unsigned int) x, (unsigned int) y);
    Uint8 r = 0, g = 0, b = 0;
    SDL_GetRGB(pixel, mask->format, &r, &g, &b);
    return r || g || b;
}

static Bool shapeSurfaceContains(SDL_Surface *mask,
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
    Uint32 white = SDL_MapRGBA(copy->format, 255, 255, 255, 255);
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
                                         int *outOffsetY,
                                         Bool *outNoop)
{
    if (outNoop)
        *outNoop = False;
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

    Uint32 white = SDL_MapRGBA(out->format, 255, 255, 255, 255);
    /* Track whether the produced mask actually excludes any window pixel. A
     * combine that ends up admitting every window pixel within the mask bbox
     * AND whose bbox covers the whole window is a no-op. The mask in that case
     * is functionally "no mask installed" and should not flip the window into
     * shaped state.
     */
    Bool excludesAny = False;
    for (int y = 0; y < out->h; y++) {
        int64_t wy = minY + y;
        Bool yInWindow = wy >= 0 && wy < (int64_t) window->h;
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
            else if (yInWindow && wx >= 0 && wx < (int64_t) window->w)
                excludesAny = True;
        }
    }
    Bool coversWindow = minX <= 0 && minY <= 0 &&
                        maxX >= (int64_t) window->w - 1 &&
                        maxY >= (int64_t) window->h - 1;
    if (!excludesAny && coversWindow) {
        /* The combine produced a mask that admits every window pixel,
         * functionally equivalent to no mask installed. Drop the surface and
         * flag the no-op so the caller can clear any existing mask without
         * leaving the window flagged as shaped.
         */
        SDL_FreeSurface(out);
        if (outNoop)
            *outNoop = True;
        return NULL;
    }

    *outOffsetX = (int) minX, *outOffsetY = (int) minY;
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

void XShapeCombineRegion(Display *dpy,
                         Window dest,
                         int destKind,
                         int xOff,
                         int yOff,
                         Region r,
                         int op)
{
    (void) dpy;
    (void) dest;
    (void) destKind;
    (void) xOff;
    (void) yOff;
    (void) r;
    (void) op;
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
    (void) dpy;
    (void) dest;
    (void) destKind;
    (void) xOff;
    (void) yOff;
    (void) rects;
    (void) n_rects;
    (void) op;
    (void) ordering;
}

void XShapeCombineMask(Display *dpy,
                       XID dest,
                       int destKind,
                       int xOff,
                       int yOff,
                       Pixmap src,
                       int op)
{
    (void) dpy;
    if (!isValidShapeOp(op) ||
        (destKind != ShapeBounding && destKind != ShapeClip))
        return;
    if (dest == None || !IS_TYPE(dest, WINDOW))
        return;
    WindowStruct *window = GET_WINDOW_STRUCT(dest);
    SDL_Surface **maskSlot = NULL;
    int *offsetXSlot = NULL;
    int *offsetYSlot = NULL;
    if (!shapeSlots(window, destKind, &maskSlot, &offsetXSlot, &offsetYSlot))
        return;

    /* src == None clears an installed mask for ShapeSet. For other combine
     * operations it is an empty source region.
     */
    if (src == None && op == ShapeSet) {
        if (*maskSlot) {
            SDL_FreeSurface(*maskSlot);
            *maskSlot = NULL;
        }
        *offsetXSlot = 0;
        *offsetYSlot = 0;
        return;
    }
    if (src != None && !IS_TYPE(src, PIXMAP))
        return;

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

    int newOffsetX = 0, newOffsetY = 0;
    Bool combineNoop = False;
    SDL_Surface *newMask = combineShapeSurfaces(
        window, *maskSlot, *offsetXSlot, *offsetYSlot, srcSurface, xOff, yOff,
        op, &newOffsetX, &newOffsetY, &combineNoop);
    SDL_FreeSurface(srcSurface);
    /* NULL with combineNoop == False means the combine failed; keep the
     * existing mask. NULL with combineNoop == True (or the explicit
     * ShapeSet/None clear) means the result admits every window pixel, so drop
     * any installed mask and stop treating the window as shaped.
     */
    if (!newMask && !combineNoop && !(op == ShapeSet && src == None))
        return;

    if (*maskSlot)
        SDL_FreeSurface(*maskSlot);
    *maskSlot = newMask;
    *offsetXSlot = newMask ? newOffsetX : 0;
    *offsetYSlot = newMask ? newOffsetY : 0;

    if (destKind != ShapeBounding)
        return;
    if (*maskSlot)
        repaintBoundingMask(dpy, dest);
}

void XShapeCombineShape(Display *dpy,
                        XID dest,
                        int destKind,
                        int xOff,
                        int yOff,
                        Pixmap src,
                        int srcKind,
                        int op)
{
    (void) dpy;
    (void) dest;
    (void) destKind;
    (void) xOff;
    (void) yOff;
    (void) src;
    (void) srcKind;
    (void) op;
}

void XShapeOffsetShape(Display *dpy, XID dest, int destKind, int xOff, int yOff)
{
    (void) dpy;
    (void) dest;
    (void) destKind;
    (void) xOff;
    (void) yOff;
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

XRectangle *XShapeGetRectangles(Display *dpy,
                                Window window,
                                int kind,
                                int *count,
                                int *ordering)
{
    (void) dpy;
    (void) window;
    (void) kind;
    if (count)
        *count = 0;
    if (ordering)
        *ordering = 0;
    return NULL;
}
