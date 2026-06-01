#include "X11/Xlib.h"
#include "X11/Xutil.h"
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <X11/Xregion.h>
#include <pixman.h>
#include "drawing.h"
#include "resource-types.h"

typedef struct pixman_region16 *pRegion;
#define GET_REGION(pixmanRegion) ((Region) (void *) pixmanRegion)
#define GET_P_REGION(region) ((pRegion) (void *) region)

typedef struct {
    double x;
    int winding;
} PolygonCrossing;

static int compareCrossing(const void *left, const void *right)
{
    const PolygonCrossing *a = left;
    const PolygonCrossing *b = right;
    if (a->x < b->x)
        return -1;
    if (a->x > b->x)
        return 1;
    return 0;
}

int XDestroyRegion(Region region)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XDestroyRegion.html
    pixman_region_fini(GET_P_REGION(region));
    free(GET_P_REGION(region));
    return 1;
}

Region XCreateRegion()
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XCreateRegion.html
    pRegion region = malloc(sizeof(struct pixman_region16));
    if (!region) {
        LOG("Out of memory: Could not allocate Region structure in "
            "XCreateRegion!\n");
        return NULL;
    }
    pixman_region_init(region);
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
    pixman_box16_t *extends = pixman_region_extents(GET_P_REGION(region));
    rect_return->width = (unsigned short) abs(extends->x2 - extends->x1);
    rect_return->y = extends->y1;
    rect_return->height = (unsigned short) abs(extends->y2 - extends->y1);
    rect_return->x = extends->x1;
    return 1;
}

int XIntersectRegion(Region sra, Region srb, Region dr_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XIntersectRegion.html
    return pixman_region_intersect(GET_P_REGION(dr_return), GET_P_REGION(sra),
                                   GET_P_REGION(srb))
               ? 1
               : 0;
}

int XUnionRegion(Region sra, Region srb, Region dr_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XUnionRegion.html
    return pixman_region_union(GET_P_REGION(dr_return), GET_P_REGION(sra),
                               GET_P_REGION(srb))
               ? 1
               : 0;
}

int XSubtractRegion(Region sra, Region srb, Region dr_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XSubtractRegion.html
    return pixman_region_subtract(GET_P_REGION(dr_return), GET_P_REGION(sra),
                                  GET_P_REGION(srb))
               ? 1
               : 0;
}


int XXorRegion(Region sra, Region srb, Region dr_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XXorRegion.html
    struct pixman_region16 aMinusB;
    struct pixman_region16 bMinusA;
    pixman_region_init(&aMinusB);
    pixman_region_init(&bMinusA);
    Bool ok = pixman_region_subtract(&aMinusB, GET_P_REGION(sra),
                                     GET_P_REGION(srb)) &&
              pixman_region_subtract(&bMinusA, GET_P_REGION(srb),
                                     GET_P_REGION(sra)) &&
              pixman_region_union(GET_P_REGION(dr_return), &aMinusB, &bMinusA);
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
    return 1;
}

Bool XPointInRegion(Region region, int x, int y)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XPointInRegion.html
    return pixman_region_contains_point(GET_P_REGION(region), x, y, NULL) != 0;
}

int XShrinkRegion(Region region, int dx, int dy)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XShrinkRegion.html
    pRegion pixmanRegion = GET_P_REGION(region);
    int rectCount = 0;
    pixman_box16_t *rects = pixman_region_rectangles(pixmanRegion, &rectCount);
    struct pixman_region16 result;
    pixman_region_init(&result);

    for (int i = 0; i < rectCount; i++) {
        int x1 = rects[i].x1 + dx;
        int y1 = rects[i].y1 + dy;
        int x2 = rects[i].x2 - dx;
        int y2 = rects[i].y2 - dy;
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }
        if (!pixman_region_union_rect(&result, &result, x1, y1,
                                      (unsigned int) (x2 - x1),
                                      (unsigned int) (y2 - y1))) {
            pixman_region_fini(&result);
            return 0;
        }
    }

    Bool ok = pixman_region_copy(pixmanRegion, &result);
    pixman_region_fini(&result);
    return ok ? 1 : 0;
}

Region XPolygonRegion(XPoint *points, int count, int fill_rule)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XPolygonRegion.html
    Region region = XCreateRegion();
    if (!region || !points || count < 3) {
        return region;
    }

    int minY = points[0].y;
    int maxY = points[0].y;
    for (int i = 1; i < count; i++) {
        if (points[i].y < minY)
            minY = points[i].y;
        if (points[i].y > maxY)
            maxY = points[i].y;
    }

    PolygonCrossing *crossings =
        malloc(sizeof(PolygonCrossing) * (size_t) count);
    if (!crossings) {
        XDestroyRegion(region);
        return NULL;
    }

    for (int y = minY; y < maxY; y++) {
        double scanY = (double) y + 0.5;
        int crossingCount = 0;
        for (int i = 0; i < count; i++) {
            XPoint p1 = points[i];
            XPoint p2 = points[(i + 1) % count];
            if (p1.y == p2.y) {
                continue;
            }
            int ymin = p1.y < p2.y ? p1.y : p2.y;
            int ymax = p1.y > p2.y ? p1.y : p2.y;
            if (scanY < ymin || scanY >= ymax) {
                continue;
            }
            double t = (scanY - p1.y) / (double) (p2.y - p1.y);
            crossings[crossingCount].x = p1.x + t * (double) (p2.x - p1.x);
            crossings[crossingCount].winding = p2.y > p1.y ? 1 : -1;
            crossingCount++;
        }
        qsort(crossings, (size_t) crossingCount, sizeof(PolygonCrossing),
              compareCrossing);

        if (fill_rule == WindingRule) {
            int winding = 0;
            int startX = 0;
            for (int i = 0; i < crossingCount; i++) {
                int oldWinding = winding;
                winding += crossings[i].winding;
                if (oldWinding == 0 && winding != 0) {
                    startX = (int) ceil(crossings[i].x);
                } else if (oldWinding != 0 && winding == 0) {
                    int endX = (int) ceil(crossings[i].x);
                    if (endX > startX) {
                        if (!pixman_region_union_rect(
                                GET_P_REGION(region), GET_P_REGION(region),
                                startX, y, (unsigned int) (endX - startX), 1)) {
                            free(crossings);
                            XDestroyRegion(region);
                            return NULL;
                        }
                    }
                }
            }
        } else {
            for (int i = 0; i + 1 < crossingCount; i += 2) {
                int startX = (int) ceil(crossings[i].x);
                int endX = (int) ceil(crossings[i + 1].x);
                if (endX > startX) {
                    if (!pixman_region_union_rect(
                            GET_P_REGION(region), GET_P_REGION(region), startX,
                            y, (unsigned int) (endX - startX), 1)) {
                        free(crossings);
                        XDestroyRegion(region);
                        return NULL;
                    }
                }
            }
        }
    }

    free(crossings);
    return region;
}

int XUnionRectWithRegion(XRectangle *rectangle,
                         Region src_region,
                         Region dest_region_return)
{
    // https://tronche.com/gui/x/xlib/utilities/regions/XUnionRectWithRegion.html
    return pixman_region_union_rect(
               GET_P_REGION(dest_region_return), GET_P_REGION(src_region),
               rectangle->x, rectangle->y, rectangle->width, rectangle->height)
               ? 1
               : 0;
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
