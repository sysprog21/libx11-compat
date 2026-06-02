/* Stroke outline construction for libx11-compat
 *
 * Builds a single continuous outline polygon per polyline contour. The
 * outline traverses the left offset edges forward, around the end cap,
 * the right offset edges backward, and around the start cap. Joins on
 * the OUTER side of each bend get proper miter / bevel / round geometry;
 * the inner side is connected with a plain line and WindingRule fill
 * cleans up the resulting self-overlap.
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#include "path.h"

#include <math.h>
#include <stdlib.h>
#include <X11/X.h>
#include <X11/Xlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* X11 default per the protocol spec. Clients can't override it through
 * Xlib so a fixed value matches every conforming server. */
#define STROKE_MITER_LIMIT 10.0

/* Round joins/caps tessellation step in radians. Roughly 11.25 degrees
 * per segment gives smooth strokes for the widths libx11-compat
 * clients actually use without ballooning the outline vertex count. */
#define STROKE_ARC_STEP (M_PI / 16.0)

static Bool appendArc(Path *out,
                      double cx,
                      double cy,
                      double radius,
                      double startAngle,
                      double sweep)
{
    int segments = (int) ceil(fabs(sweep) / STROKE_ARC_STEP);
    if (segments < 1)
        segments = 1;
    for (int k = 1; k <= segments; k++) {
        double t = (double) k / (double) segments;
        double a = startAngle + sweep * t;
        if (!pathLineTo(out, cx + cos(a) * radius, cy + sin(a) * radius))
            return False;
    }
    return True;
}

/* End of contour cap. We are positioned at p + halfWidth*(nx,ny) (left
 * edge end) and need to traverse around to p - halfWidth*(nx,ny) (right
 * edge end). (ux,uy) is the segment's unit direction so the cap can bulge
 * outward. */
static Bool appendCap(Path *out,
                      double px,
                      double py,
                      double ux,
                      double uy,
                      double nx,
                      double ny,
                      double halfWidth,
                      int capStyle)
{
    if (capStyle == CapRound) {
        /* Semicircle from +n through +u to -n. The cap_left point is at
         * angle atan2(ny,nx); we sweep by -PI (clockwise) so the curve
         * passes through the +u direction. */
        double a0 = atan2(ny, nx);
        return appendArc(out, px, py, halfWidth, a0, -M_PI);
    }
    if (capStyle == CapProjecting) {
        double ex = ux * halfWidth;
        double ey = uy * halfWidth;
        if (!pathLineTo(out, px + nx * halfWidth + ex,
                        py + ny * halfWidth + ey))
            return False;
        if (!pathLineTo(out, px - nx * halfWidth + ex,
                        py - ny * halfWidth + ey))
            return False;
        return pathLineTo(out, px - nx * halfWidth, py - ny * halfWidth);
    }
    /* CapButt and CapNotLast both render as a straight crossing. */
    return pathLineTo(out, px - nx * halfWidth, py - ny * halfWidth);
}

static Bool appendDegenerateStroke(Path *out,
                                   double px,
                                   double py,
                                   double halfWidth,
                                   int capStyle)
{
    if (capStyle == CapRound) {
        if (!pathMoveTo(out, px + halfWidth, py))
            return False;
        if (!appendArc(out, px, py, halfWidth, 0.0, 2.0 * M_PI))
            return False;
        return pathClose(out);
    }

    if (capStyle == CapProjecting) {
        if (!pathMoveTo(out, px - halfWidth, py - halfWidth))
            return False;
        if (!pathLineTo(out, px + halfWidth, py - halfWidth))
            return False;
        if (!pathLineTo(out, px + halfWidth, py + halfWidth))
            return False;
        if (!pathLineTo(out, px - halfWidth, py + halfWidth))
            return False;
        return pathClose(out);
    }

    return True;
}

/* Emit join geometry from edgeFrom to edgeTo. The vertex is at (vx, vy);
 * (uFromX,uFromY) is the incoming edge's tangent into the vertex,
 * (uToX,uToY) is the outgoing edge's tangent leaving the vertex.
 * outerSide is True when this side of the bend is the outer one (i.e.
 * the side with a visible gap that needs filling). For the inner side
 * we simply line to edgeTo. */
static Bool appendJoin(Path *out,
                       double vx,
                       double vy,
                       double edgeFromX,
                       double edgeFromY,
                       double edgeToX,
                       double edgeToY,
                       double uFromX,
                       double uFromY,
                       double uToX,
                       double uToY,
                       double halfWidth,
                       int joinStyle,
                       Bool outerSide)
{
    if (!outerSide)
        return pathLineTo(out, edgeToX, edgeToY);

    if (joinStyle == JoinMiter) {
        /* Solve edgeFrom + t*uFrom = edgeTo - s*uTo for the miter point. */
        double det = uFromX * uToY - uFromY * uToX;
        if (fabs(det) >= 1e-12) {
            double dx = edgeToX - edgeFromX;
            double dy = edgeToY - edgeFromY;
            double t = (dx * uToY - dy * uToX) / det;
            double mx = edgeFromX + t * uFromX;
            double my = edgeFromY + t * uFromY;
            double miterDx = mx - vx;
            double miterDy = my - vy;
            double miterLen = hypot(miterDx, miterDy);
            if (miterLen <= halfWidth * STROKE_MITER_LIMIT) {
                if (!pathLineTo(out, mx, my))
                    return False;
                return pathLineTo(out, edgeToX, edgeToY);
            }
        }
        /* Parallel edges or miter exceeds the limit: bevel fallback. */
        return pathLineTo(out, edgeToX, edgeToY);
    }

    if (joinStyle == JoinRound) {
        double a0 = atan2(edgeFromY - vy, edgeFromX - vx);
        double a1 = atan2(edgeToY - vy, edgeToX - vx);
        double sweep = a1 - a0;
        /* Sweep on the outer side: pick the direction that matches the
         * cross product of edge-from-to-edge-to (computed from V). */
        double fx = edgeFromX - vx;
        double fy = edgeFromY - vy;
        double tx = edgeToX - vx;
        double ty = edgeToY - vy;
        double cross = fx * ty - fy * tx;
        if (cross > 0) {
            while (sweep < 0)
                sweep += 2.0 * M_PI;
        } else {
            while (sweep > 0)
                sweep -= 2.0 * M_PI;
        }
        return appendArc(out, vx, vy, halfWidth, a0, sweep);
    }

    /* JoinBevel and any unknown style: straight line. */
    return pathLineTo(out, edgeToX, edgeToY);
}

Bool pathStrokePolyline(Path *out,
                        const PathPoint *points,
                        size_t count,
                        double width,
                        int capStyle,
                        int joinStyle)
{
    if (!out || !points || count < 2 || width <= 1.0)
        return False;
    double halfWidth = width / 2.0;

    /* Collapse zero-length segments by dedup-copy. A zero-length segment
     * has no defined direction, which would corrupt the join math; the
     * easiest cure is to drop those points entirely. */
    PathPoint *pts = malloc(sizeof(PathPoint) * count);
    if (!pts)
        return False;
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        if (n > 0 && pts[n - 1].x == points[i].x && pts[n - 1].y == points[i].y)
            continue;
        pts[n++] = points[i];
    }
    if (n < 2) {
        Bool ok = appendDegenerateStroke(out, pts[0].x, pts[0].y, halfWidth,
                                         capStyle);
        free(pts);
        return ok;
    }

    size_t numSegments = n - 1;
    /* Per-segment unit tangent (ux, uy) and left normal (nx, ny). */
    typedef struct {
        double ux, uy;
        double nx, ny;
    } SegmentFrame;
    SegmentFrame *seg = malloc(sizeof(SegmentFrame) * numSegments);
    if (!seg) {
        free(pts);
        return False;
    }
    for (size_t i = 0; i < numSegments; i++) {
        double dx = pts[i + 1].x - pts[i].x;
        double dy = pts[i + 1].y - pts[i].y;
        double len = hypot(dx, dy);
        /* len > 0 here because dedup ran above. */
        seg[i].ux = dx / len;
        seg[i].uy = dy / len;
        seg[i].nx = -seg[i].uy;
        seg[i].ny = seg[i].ux;
    }

    Bool ok = True;
    /* A contour with coincident endpoints is closed: emit joins at every
     * vertex including the closure, no caps. Two bridge segments
     * (LSTART(0)<->RSTART(0)) traversed in opposite directions cancel
     * under the WindingRule fill, so a single self-intersecting outline
     * is enough. Requires n>=4 to have at least 3 unique vertices. */
    Bool closed =
        n >= 4 && pts[0].x == pts[n - 1].x && pts[0].y == pts[n - 1].y;

    double lstartX = pts[0].x + halfWidth * seg[0].nx;
    double lstartY = pts[0].y + halfWidth * seg[0].ny;
    if (!pathMoveTo(out, lstartX, lstartY)) {
        ok = False;
        goto done;
    }

    /* Forward walk along the left offset edges. For a closed contour the
     * last iteration also emits a join (the closure) instead of falling
     * through to an end cap. */
    for (size_t i = 0; i < numSegments && ok; i++) {
        double lendX = pts[i + 1].x + halfWidth * seg[i].nx;
        double lendY = pts[i + 1].y + halfWidth * seg[i].ny;
        if (!pathLineTo(out, lendX, lendY)) {
            ok = False;
            break;
        }
        if (i + 1 < numSegments || closed) {
            size_t nextSeg = (i + 1) % numSegments;
            size_t vIdx = (i + 1 < numSegments) ? (i + 1) : 0;
            double nextLstartX = pts[vIdx].x + halfWidth * seg[nextSeg].nx;
            double nextLstartY = pts[vIdx].y + halfWidth * seg[nextSeg].ny;
            double cross =
                seg[i].ux * seg[nextSeg].uy - seg[i].uy * seg[nextSeg].ux;
            /* Left side is outer when the polyline turns right (cross < 0). */
            Bool outer = cross < 0;
            ok = appendJoin(out, pts[vIdx].x, pts[vIdx].y, lendX, lendY,
                            nextLstartX, nextLstartY, seg[i].ux, seg[i].uy,
                            seg[nextSeg].ux, seg[nextSeg].uy, halfWidth,
                            joinStyle, outer);
        }
    }

    if (!closed && ok) {
        size_t last = numSegments - 1;
        ok = appendCap(out, pts[n - 1].x, pts[n - 1].y, seg[last].ux,
                       seg[last].uy, seg[last].nx, seg[last].ny, halfWidth,
                       capStyle);
    } else if (closed && ok) {
        /* Bridge from the forward walk's final position (LSTART(0)) to
         * the backward walk's start (RSTART(0)). The pathClose at the
         * end traces the same bridge in reverse so winding cancels. */
        ok = pathLineTo(out, pts[0].x - halfWidth * seg[0].nx,
                        pts[0].y - halfWidth * seg[0].ny);
        /* Emit the reverse closure join: from RSTART(0) to REND(num-1)
         * at vertex pts[0]. Walking backward the incoming tangent is
         * -seg[0].u and the outgoing tangent is -seg[last].u. */
        if (ok) {
            size_t last = numSegments - 1;
            double prevRendX = pts[0].x - halfWidth * seg[last].nx;
            double prevRendY = pts[0].y - halfWidth * seg[last].ny;
            double cross = seg[last].ux * seg[0].uy - seg[last].uy * seg[0].ux;
            Bool outer = cross > 0;
            ok = appendJoin(out, pts[0].x, pts[0].y,
                            pts[0].x - halfWidth * seg[0].nx,
                            pts[0].y - halfWidth * seg[0].ny, prevRendX,
                            prevRendY, -seg[0].ux, -seg[0].uy, -seg[last].ux,
                            -seg[last].uy, halfWidth, joinStyle, outer);
        }
    }

    /* Backward walk along the right offset edges. Going backward, the
     * incoming tangent is -u[i] and the outgoing tangent is -u[i-1]. */
    if (closed) {
        /* Closure join was emitted above and left us at REND(num-1).
         * Walk i = num-1 down to 1: lineTo RSTART(i) then join at pts[i]
         * to REND(i-1). The final lineTo RSTART(0) closes the bridge so
         * pathClose retraces the forward bridge in reverse. */
        for (size_t i = numSegments - 1; i > 0 && ok; i--) {
            double rstartX = pts[i].x - halfWidth * seg[i].nx;
            double rstartY = pts[i].y - halfWidth * seg[i].ny;
            if (!pathLineTo(out, rstartX, rstartY)) {
                ok = False;
                break;
            }
            double prevRendX = pts[i].x - halfWidth * seg[i - 1].nx;
            double prevRendY = pts[i].y - halfWidth * seg[i - 1].ny;
            double cross =
                seg[i - 1].ux * seg[i].uy - seg[i - 1].uy * seg[i].ux;
            Bool outer = cross > 0;
            ok =
                appendJoin(out, pts[i].x, pts[i].y, rstartX, rstartY, prevRendX,
                           prevRendY, -seg[i].ux, -seg[i].uy, -seg[i - 1].ux,
                           -seg[i - 1].uy, halfWidth, joinStyle, outer);
        }
        if (ok)
            ok = pathLineTo(out, pts[0].x - halfWidth * seg[0].nx,
                            pts[0].y - halfWidth * seg[0].ny);
    } else {
        for (size_t i = numSegments; i-- > 0 && ok;) {
            double rstartX = pts[i].x - halfWidth * seg[i].nx;
            double rstartY = pts[i].y - halfWidth * seg[i].ny;
            if (!pathLineTo(out, rstartX, rstartY)) {
                ok = False;
                break;
            }
            if (i > 0) {
                double prevRendX = pts[i].x - halfWidth * seg[i - 1].nx;
                double prevRendY = pts[i].y - halfWidth * seg[i - 1].ny;
                double cross =
                    seg[i - 1].ux * seg[i].uy - seg[i - 1].uy * seg[i].ux;
                Bool outer = cross > 0;
                ok = appendJoin(out, pts[i].x, pts[i].y, rstartX, rstartY,
                                prevRendX, prevRendY, -seg[i].ux, -seg[i].uy,
                                -seg[i - 1].ux, -seg[i - 1].uy, halfWidth,
                                joinStyle, outer);
            }
        }
        if (ok) {
            /* Start cap walks from RSTART(0) to LSTART(0) along the
             * reversed first-segment direction. */
            ok = appendCap(out, pts[0].x, pts[0].y, -seg[0].ux, -seg[0].uy,
                           -seg[0].nx, -seg[0].ny, halfWidth, capStyle);
        }
    }

    if (ok)
        ok = pathClose(out);

done:
    free(seg);
    free(pts);
    return ok;
}
