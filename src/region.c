#include "X11/Xlib.h"
#include "X11/Xutil.h"
#include <stdint.h>
#include <stdlib.h>
#include <X11/Xregion.h>
#include <pixman.h>
#include "resource-types.h"
#include "util.h"
#include "path/edges.h"
#include "path/rasterize.h"

typedef struct XCompatRegion {
    long size;
    long numRects;
    BOX *rects;
    BOX extents;
    pixman_region16_t pixman;
} XCompatRegion;

#define GET_REGION(region) ((Region) (void *) region)
#define GET_COMPAT_REGION(region) ((XCompatRegion *) (void *) region)
#define GET_P_REGION(region) (&GET_COMPAT_REGION(region)->pixman)

static void setBoxFromPixman(BOX *dst, const pixman_box16_t *src)
{
    dst->x1 = src->x1;
    dst->x2 = src->x2;
    dst->y1 = src->y1;
    dst->y2 = src->y2;
}

static Bool syncRegionPublicFields(Region region)
{
    if (!region)
        return False;
    XCompatRegion *compat = GET_COMPAT_REGION(region);
    int n = 0;
    pixman_box16_t *boxes = pixman_region_rectangles(&compat->pixman, &n);
    if (n <= 0) {
        compat->numRects = 0;
        compat->extents.x1 = 0;
        compat->extents.x2 = 0;
        compat->extents.y1 = 0;
        compat->extents.y2 = 0;
        return True;
    }
    if (compat->size < n) {
        BOX *newRects = realloc(compat->rects, sizeof(BOX) * (size_t) n);
        if (!newRects)
            return False;
        compat->rects = newRects;
        compat->size = n;
    }
    for (int i = 0; i < n; i++)
        setBoxFromPixman(&compat->rects[i], &boxes[i]);
    compat->numRects = n;
    setBoxFromPixman(&compat->extents, pixman_region_extents(&compat->pixman));
    return True;
}

int XDestroyRegion(Region region)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XDestroyRegion.html
    XCompatRegion *compat = GET_COMPAT_REGION(region);
    pixman_region_fini(&compat->pixman);
    free(compat->rects);
    free(compat);
    return 1;
}

Region XCreateRegion()
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XCreateRegion.html
    XCompatRegion *region = calloc(1, sizeof(*region));
    if (!region) {
        LOG("Out of memory: Could not allocate Region structure in "
            "XCreateRegion!\n");
        return NULL;
    }
    pixman_region_init(&region->pixman);
    syncRegionPublicFields(GET_REGION(region));
    return GET_REGION(region);
}

Bool XEmptyRegion(Region region)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XEmptyRegion.html
    return !pixman_region_not_empty(GET_P_REGION(region));
}

int XRectInRegion(Region region,
                  int x,
                  int y,
                  unsigned int width,
                  unsigned int height)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XRectInRegion.html
    pixman_box16_t box = {
        .x1 = (int16_t) x,
        .y1 = (int16_t) y,
        .x2 = (int16_t) (x + width),
        .y2 = (int16_t) (y + height),
    };
    switch (pixman_region_contains_rectangle(GET_P_REGION(region), &box)) {
    case PIXMAN_REGION_OUT:
        return RectangleOut;
    case PIXMAN_REGION_IN:
        return RectangleIn;
    default:
        return RectanglePart;
    }
}

int XClipBox(Region region, XRectangle *rect_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XClipBox.html
    /* pixman_region_extents always returns x1<=x2 and y1<=y2. */
    pixman_box16_t *extents = pixman_region_extents(GET_P_REGION(region));
    rect_return->x = extents->x1;
    rect_return->y = extents->y1;
    rect_return->width = (unsigned short) (extents->x2 - extents->x1);
    rect_return->height = (unsigned short) (extents->y2 - extents->y1);
    return 1;
}

int XIntersectRegion(Region sra, Region srb, Region dr_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XIntersectRegion.html
    Bool ok = pixman_region_intersect(GET_P_REGION(dr_return),
                                      GET_P_REGION(sra), GET_P_REGION(srb)) &&
              syncRegionPublicFields(dr_return);
    return ok ? 1 : 0;
}

int XUnionRegion(Region sra, Region srb, Region dr_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XUnionRegion.html
    Bool ok = pixman_region_union(GET_P_REGION(dr_return), GET_P_REGION(sra),
                                  GET_P_REGION(srb)) &&
              syncRegionPublicFields(dr_return);
    return ok ? 1 : 0;
}

int XSubtractRegion(Region sra, Region srb, Region dr_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XSubtractRegion.html
    Bool ok = pixman_region_subtract(GET_P_REGION(dr_return), GET_P_REGION(sra),
                                     GET_P_REGION(srb)) &&
              syncRegionPublicFields(dr_return);
    return ok ? 1 : 0;
}

int XXorRegion(Region sra, Region srb, Region dr_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XXorRegion.html
    struct pixman_region16 aMinusB;
    struct pixman_region16 bMinusA;
    pixman_region_init(&aMinusB);
    pixman_region_init(&bMinusA);
    Bool ok =
        pixman_region_subtract(&aMinusB, GET_P_REGION(sra),
                               GET_P_REGION(srb)) &&
        pixman_region_subtract(&bMinusA, GET_P_REGION(srb),
                               GET_P_REGION(sra)) &&
        pixman_region_union(GET_P_REGION(dr_return), &aMinusB, &bMinusA) &&
        syncRegionPublicFields(dr_return);
    pixman_region_fini(&aMinusB);
    pixman_region_fini(&bMinusA);
    return ok ? 1 : 0;
}

int XEqualRegion(Region r1, Region r2)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XEqualRegion.html
    return pixman_region_equal(GET_P_REGION(r1), GET_P_REGION(r2)) ? 1 : 0;
}

int XOffsetRegion(Region region, int dx, int dy)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XOffsetRegion.html
    pixman_region_translate(GET_P_REGION(region), dx, dy);
    return syncRegionPublicFields(region) ? 1 : 0;
}

Bool XPointInRegion(Region region, int x, int y)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XPointInRegion.html
    return pixman_region_contains_point(GET_P_REGION(region), x, y, NULL) != 0;
}

/* One-axis erosion: replace "target" with target ∩ translate(target, -1) ∩
 * translate(target, +1), repeated `n` times. Box erosion is separable so doing
 * this horizontally then vertically gives the same answer as intersecting all
 * 2D offsets, but in O(n) work per axis instead of O(n^2).
 */
static Bool erodeAxis(struct pixman_region16 *target,
                      int n,
                      int dxStep,
                      int dyStep)
{
    if (n <= 0)
        return True;
    for (int step = 0; step < n; step++) {
        struct pixman_region16 left;
        struct pixman_region16 right;
        struct pixman_region16 tmp;
        pixman_region_init(&left);
        pixman_region_init(&right);
        pixman_region_init(&tmp);
        if (!pixman_region_copy(&left, target) ||
            !pixman_region_copy(&right, target)) {
            pixman_region_fini(&left);
            pixman_region_fini(&right);
            pixman_region_fini(&tmp);
            return False;
        }
        pixman_region_translate(&left, -dxStep, -dyStep);
        pixman_region_translate(&right, dxStep, dyStep);
        if (!pixman_region_intersect(&tmp, target, &left) ||
            !pixman_region_intersect(target, &tmp, &right)) {
            pixman_region_fini(&left);
            pixman_region_fini(&right);
            pixman_region_fini(&tmp);
            return False;
        }
        pixman_region_fini(&left);
        pixman_region_fini(&right);
        pixman_region_fini(&tmp);
    }
    return True;
}

/* One-axis dilation: per-rectangle expansion + union is exact for axis-aligned
 * dilation because pixman canonicalizes overlapping rects. Used for negative
 * dx/dy (XShrinkRegion treats negative as "make larger").
 */
static Bool dilateAxis(struct pixman_region16 *target, int dx, int dy)
{
    int rectCount = 0;
    pixman_box16_t *rects = pixman_region_rectangles(target, &rectCount);
    struct pixman_region16 grown;
    pixman_region_init(&grown);
    for (int i = 0; i < rectCount; i++) {
        int x1 = rects[i].x1 - dx;
        int y1 = rects[i].y1 - dy;
        int x2 = rects[i].x2 + dx;
        int y2 = rects[i].y2 + dy;
        if (x2 <= x1 || y2 <= y1)
            continue;
        if (!pixman_region_union_rect(&grown, &grown, x1, y1,
                                      (unsigned int) (x2 - x1),
                                      (unsigned int) (y2 - y1))) {
            pixman_region_fini(&grown);
            return False;
        }
    }
    Bool ok = pixman_region_copy(target, &grown);
    pixman_region_fini(&grown);
    return ok;
}

int XShrinkRegion(Region region, int dx, int dy)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XShrinkRegion.html
    /* Per-rectangle shrinkage carves notches out of non-convex regions
     * (U-shapes shard at the inner joins between bands). Proper Minkowski
     * erosion on the region as a whole keeps connected components connected;
     * the positive (shrink) case applies that erosion separably along each
     * axis.
     *
     * Negative dx/dy means expand. Per-rectangle grow + union is exact for
     * axis-aligned dilation, so reuse the simpler path there.
     */
    pixman_region16_t *pixmanRegion = GET_P_REGION(region);
    Bool ok = True;
    if (dx > 0 && ok)
        ok = erodeAxis(pixmanRegion, dx, 1, 0);
    if (dy > 0 && ok)
        ok = erodeAxis(pixmanRegion, dy, 0, 1);
    if (dx < 0 && ok)
        ok = dilateAxis(pixmanRegion, -dx, 0);
    if (dy < 0 && ok)
        ok = dilateAxis(pixmanRegion, 0, -dy);
    if (!ok)
        return 0;
    return syncRegionPublicFields(region) ? 1 : 0;
}

Region XPolygonRegion(XPoint *points, int count, int fill_rule)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XPolygonRegion.html
    Region region = XCreateRegion();
    if (!region || !points || count < 3)
        return region;
    /* +1 trailing closing point. Compute in size_t to avoid signed overflow
     * when count is near INT_MAX, then bound by allocator-safe size.
     */
    if ((size_t) count > SIZE_MAX / sizeof(PathPoint) - 1) {
        XDestroyRegion(region);
        return NULL;
    }
    PathPoint *pathPoints = malloc(sizeof(PathPoint) * ((size_t) count + 1));
    if (!pathPoints) {
        XDestroyRegion(region);
        return NULL;
    }
    for (int i = 0; i < count; i++) {
        pathPoints[i].x = points[i].x;
        pathPoints[i].y = points[i].y;
    }
    pathPoints[count] = pathPoints[0];

    PathEdgeList edges = {.edges = NULL};
    PathSpanList spans = {.spans = NULL};
    Bool ok = pathBuildEdges(pathPoints, (size_t) count + 1, &edges) &&
              pathRasterizeEdges(&edges, fill_rule, &spans);
    for (size_t i = 0; ok && i < spans.count; i++) {
        PathSpan span = spans.spans[i];
        ok = pixman_region_union_rect(
            GET_P_REGION(region), GET_P_REGION(region), span.xStart, span.y,
            (unsigned int) (span.xEnd - span.xStart + 1), 1);
    }

    pathFreeSpans(&spans);
    pathFreeEdges(&edges);
    free(pathPoints);
    if (!ok) {
        XDestroyRegion(region);
        return NULL;
    }
    if (!syncRegionPublicFields(region)) {
        XDestroyRegion(region);
        return NULL;
    }
    return region;
}

int XUnionRectWithRegion(XRectangle *rectangle,
                         Region src_region,
                         Region dest_region_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XUnionRectWithRegion.html
    Bool ok = pixman_region_union_rect(GET_P_REGION(dest_region_return),
                                       GET_P_REGION(src_region), rectangle->x,
                                       rectangle->y, rectangle->width,
                                       rectangle->height) &&
              syncRegionPublicFields(dest_region_return);
    return ok ? 1 : 0;
}

int XSetRegion(Display *display, GC gc, Region region)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XSetRegion.html
    if (!region || !gc)
        return 0;
    int n = 0;
    pixman_box16_t *boxes = pixman_region_rectangles(GET_P_REGION(region), &n);
    if (n <= 0)
        return XSetClipRectangles(display, gc, 0, 0, NULL, 0, Unsorted);
    XRectangle *rects = malloc(sizeof(XRectangle) * (size_t) n);
    if (!rects)
        return 0;
    for (int i = 0; i < n; i++) {
        rects[i].x = boxes[i].x1;
        rects[i].y = boxes[i].y1;
        rects[i].width = (unsigned short) (boxes[i].x2 - boxes[i].x1);
        rects[i].height = (unsigned short) (boxes[i].y2 - boxes[i].y1);
    }
    int rc = XSetClipRectangles(display, gc, 0, 0, rects, n, YXBanded);
    free(rects);
    return rc;
}
