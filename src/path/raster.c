/* Scanline path rasterization for libx11-compat
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#include "raster.h"

#include <math.h>
#include <pixman.h>
#include <stdlib.h>

#include "../drawing.h"
#include "../gc.h"
#include "edges.h"
#include "rasterize.h"

static Bool buildPixmanRegionFromSpans(const PathSpanList *spans,
                                       pixman_region16_t *region)
{
    pixman_region_init(region);
    for (size_t i = 0; i < spans->count; i++) {
        PathSpan span = spans->spans[i];
        if (span.coverage == 0 || span.xEnd < span.xStart)
            continue;
        if (!pixman_region_union_rect(
                region, region, span.xStart, span.y,
                (unsigned int) (span.xEnd - span.xStart + 1), 1)) {
            pixman_region_fini(region);
            return False;
        }
    }
    return True;
}

/* Color and fillRule are explicit so the wide-stroke path can force
 * WindingRule (its outline self-overlaps at joins) and dashed strokes can
 * fill each sub-path with foreground or background without mutating the
 * live GC. */
static Bool rasterFillPathInternal(SDL_Renderer *renderer,
                                   GC gc,
                                   const Path *path,
                                   int fillRule,
                                   unsigned long color,
                                   Bool coalesceWithPixman)
{
    if (!renderer || !gc || !path)
        return False;
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->fillStyle != FillSolid || gContext->function != GXcopy ||
        (gContext->planeMask != AllPlanes &&
         gContext->planeMask != 0xFFFFFFFFu)) {
        return False;
    }

    PathPoint *points = NULL;
    size_t pointCount = 0;
    PathEdgeList edges;
    PathSpanList spans;
    pixman_region16_t spanRegion;
    Bool spanRegionInitialized = False;
    Bool ok = False;
    edges.edges = NULL;
    spans.spans = NULL;
    if (!pathFlatten(path, 0.25, &points, &pointCount))
        goto cleanup;
    if (pointCount < 3) {
        ok = True;
        goto cleanup;
    }
    if (!pathBuildEdges(points, pointCount, &edges))
        goto cleanup;
    if (edges.count == 0) {
        ok = True;
        goto cleanup;
    }
    if (!pathRasterizeEdges(&edges, fillRule, &spans))
        goto cleanup;
    if (coalesceWithPixman) {
        if (!buildPixmanRegionFromSpans(&spans, &spanRegion))
            goto cleanup;
        spanRegionInitialized = True;
    }

    applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE, color);
    int clipCount = getGcClipIterationCount(gc);
    if (spanRegionInitialized) {
        int rectCount = 0;
        pixman_box16_t *rects =
            pixman_region_rectangles(&spanRegion, &rectCount);
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip))
                continue;
            for (int i = 0; i < rectCount; i++) {
                SDL_Rect rect = {
                    .x = rects[i].x1,
                    .y = rects[i].y1,
                    .w = rects[i].x2 - rects[i].x1,
                    .h = rects[i].y2 - rects[i].y1,
                };
                SDL_RenderFillRect(renderer, &rect);
            }
        }
    } else {
        for (int clip = 0; clip < clipCount; clip++) {
            if (!setGcClipForIteration(renderer, gc, clip))
                continue;
            for (size_t i = 0; i < spans.count; i++) {
                PathSpan span = spans.spans[i];
                if (span.xEnd < span.xStart)
                    continue;
                SDL_RenderDrawLine(renderer, span.xStart, span.y, span.xEnd,
                                   span.y);
            }
        }
    }
    clearRendererClip(renderer);
    ok = True;

cleanup:
    if (spanRegionInitialized)
        pixman_region_fini(&spanRegion);
    pathFreeSpans(&spans);
    pathFreeEdges(&edges);
    free(points);
    return ok;
}

Bool rasterFillPathOnRendererWithOptions(SDL_Renderer *renderer,
                                         GC gc,
                                         const Path *path,
                                         Bool coalesceWithPixman)
{
    if (!renderer || !gc || !path)
        return False;
    GraphicContext *gContext = GET_GC(gc);
    return rasterFillPathInternal(renderer, gc, path, gContext->fillRule,
                                  gContext->foreground, coalesceWithPixman);
}

Bool rasterFillPathOnRenderer(SDL_Renderer *renderer, GC gc, const Path *path)
{
    return rasterFillPathOnRendererWithOptions(renderer, gc, path, True);
}

static Bool rasterStrokeSolidPath(SDL_Renderer *renderer,
                                  GC gc,
                                  const Path *path,
                                  unsigned long color,
                                  int lineWidth,
                                  int capStyle,
                                  int joinStyle)
{
    if (!renderer || !gc || !path)
        return False;

    PathPoint *points = NULL;
    size_t pointCount = 0;
    Bool ok = False;
    if (!pathFlatten(path, 0.25, &points, &pointCount))
        goto cleanup;
    if (pointCount < 2) {
        ok = True;
        goto cleanup;
    }

    if (lineWidth > 1) {
        Path outline;
        if (!pathInit(&outline))
            goto cleanup;
        size_t start = 0;
        ok = True;
        for (size_t i = 0; i <= pointCount; i++) {
            if (i < pointCount && !pathPointIsBreak(points[i]))
                continue;
            if (i > start + 1) {
                ok =
                    pathStrokePolyline(&outline, &points[start], i - start,
                                       (double) lineWidth, capStyle, joinStyle);
                if (!ok)
                    break;
            }
            start = i + 1;
        }
        if (ok) {
            /* Force WindingRule: see rasterFillPathInternal. */
            ok = rasterFillPathInternal(renderer, gc, &outline, WindingRule,
                                        color, False);
        }
        pathFree(&outline);
        goto cleanup;
    }

    applySdlDrawState(renderer, gc, SDL_BLENDMODE_BLEND, color);
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        for (size_t i = 0; i + 1 < pointCount; i++) {
            if (pathPointIsBreak(points[i]) || pathPointIsBreak(points[i + 1]))
                continue;
            SDL_RenderDrawLine(
                renderer, (int) lround(points[i].x), (int) lround(points[i].y),
                (int) lround(points[i + 1].x), (int) lround(points[i + 1].y));
        }
    }
    clearRendererClip(renderer);
    ok = True;

cleanup:
    free(points);
    return ok;
}

Bool rasterStrokePathOnRenderer(SDL_Renderer *renderer, GC gc, const Path *path)
{
    if (!renderer || !gc || !path)
        return False;
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->function != GXcopy)
        return False;

    /* Non-dashed, OR dashed-with-empty-dash-list (treat as solid since
     * there is no pattern to apply). The dash loop would spin forever
     * if numDashes is 0 because dashLeft can never refill. */
    Bool dashed = gContext->lineStyle == LineOnOffDash ||
                  gContext->lineStyle == LineDoubleDash;
    if (!dashed || gContext->numDashes == 0) {
        if (!dashed && gContext->lineStyle != LineSolid)
            return False;
        return rasterStrokeSolidPath(renderer, gc, path, gContext->foreground,
                                     gContext->lineWidth, gContext->capStyle,
                                     gContext->joinStyle);
    }

    /* Split the flattened polyline into on/off sub-paths by dash phase,
     * then stroke each with the appropriate color via the helper above. */
    PathPoint *points = NULL;
    size_t pointCount = 0;
    Bool ok = False;
    if (!pathFlatten(path, 0.25, &points, &pointCount))
        goto cleanup;
    if (pointCount < 2) {
        ok = True;
        goto cleanup;
    }

    Path onPath;
    Path offPath;
    if (!pathInit(&onPath))
        goto cleanup;
    if (!pathInit(&offPath)) {
        pathFree(&onPath);
        goto cleanup;
    }
    size_t dashIndex = 0;
    double dashLeft =
        gContext->numDashes > 0 ? (unsigned char) gContext->dashes[0] : 1.0;
    Bool dashOn = True;
    /* Reduce dashOffset mod the pattern period before consuming it.
     * Unsigned negation dodges -INT_MIN UB; modulo caps the consume loop
     * iteration count. X11 doubles the period for odd-length lists. */
    unsigned int patternTotal = 0;
    for (size_t k = 0; k < gContext->numDashes; k++)
        patternTotal += (unsigned char) gContext->dashes[k];
    if (gContext->numDashes > 0 && (gContext->numDashes & 1u))
        patternTotal *= 2u;
    unsigned int dashOffset = 0;
    if (patternTotal > 0) {
        int rawOffset = gContext->dashOffset;
        unsigned int absOffset = rawOffset >= 0 ? (unsigned int) rawOffset
                                                : -(unsigned int) rawOffset;
        absOffset %= patternTotal;
        if (rawOffset < 0 && absOffset > 0)
            absOffset = patternTotal - absOffset;
        dashOffset = absOffset;
    }
    while (gContext->numDashes > 0 && dashOffset > 0) {
        double consume = fmin((double) dashOffset, dashLeft);
        dashLeft -= consume;
        dashOffset -= (unsigned int) consume;
        if (dashLeft <= 0.0) {
            dashIndex = (dashIndex + 1) % gContext->numDashes;
            dashLeft = (unsigned char) gContext->dashes[dashIndex];
            dashOn = !dashOn;
        }
    }
    ok = True;
    /* Dash state lives outside the contour loop: X11 carries phase
     * across MoveTo boundaries instead of restarting per subpath. */
    int dashedLineStyle = gContext->lineStyle;
    for (size_t i = 0; ok && i + 1 < pointCount; i++) {
        if (pathPointIsBreak(points[i]) || pathPointIsBreak(points[i + 1]))
            continue;
        PathPoint a = points[i];
        PathPoint b = points[i + 1];
        double dx = b.x - a.x;
        double dy = b.y - a.y;
        double len = hypot(dx, dy);
        if (len == 0.0) {
            Path *target = dashOn ? &onPath : &offPath;
            if (dashOn || dashedLineStyle == LineDoubleDash)
                ok = pathMoveTo(target, a.x, a.y) &&
                     pathLineTo(target, b.x, b.y);
            continue;
        }
        double pos = 0.0;
        while (ok && pos < len) {
            double run = fmin(dashLeft, len - pos);
            double t0 = pos / len;
            double t1 = (pos + run) / len;
            PathPoint p0 = {a.x + dx * t0, a.y + dy * t0};
            PathPoint p1 = {a.x + dx * t1, a.y + dy * t1};
            Path *target = dashOn ? &onPath : &offPath;
            if (dashOn || dashedLineStyle == LineDoubleDash)
                ok = pathMoveTo(target, p0.x, p0.y) &&
                     pathLineTo(target, p1.x, p1.y);
            pos += run;
            dashLeft -= run;
            if (dashLeft <= 0.0 && gContext->numDashes > 0) {
                dashIndex = (dashIndex + 1) % gContext->numDashes;
                dashLeft = (unsigned char) gContext->dashes[dashIndex];
                dashOn = !dashOn;
            }
        }
    }
    if (ok)
        ok = rasterStrokeSolidPath(renderer, gc, &onPath, gContext->foreground,
                                   gContext->lineWidth, gContext->capStyle,
                                   gContext->joinStyle);
    if (ok && dashedLineStyle == LineDoubleDash)
        ok = rasterStrokeSolidPath(renderer, gc, &offPath, gContext->background,
                                   gContext->lineWidth, gContext->capStyle,
                                   gContext->joinStyle);
    pathFree(&offPath);
    pathFree(&onPath);

cleanup:
    free(points);
    return ok;
}
