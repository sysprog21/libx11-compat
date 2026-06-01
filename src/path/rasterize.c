/* Active-edge span rasterization for libx11-compat
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#include "rasterize.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    double x;
    int winding;
} Crossing;

static int compareCrossing(const void *left, const void *right)
{
    const Crossing *a = left;
    const Crossing *b = right;
    return (a->x > b->x) - (a->x < b->x);
}

static Bool appendSpan(PathSpanList *list, int y, int xStart, int xEnd)
{
    if (xEnd < xStart)
        return True;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 64;
        if (capacity <= list->capacity ||
            capacity > SIZE_MAX / sizeof(PathSpan)) {
            return False;
        }
        PathSpan *grown = realloc(list->spans, capacity * sizeof(PathSpan));
        if (!grown)
            return False;
        list->spans = grown;
        list->capacity = capacity;
    }
    list->spans[list->count++] = (PathSpan) {
        .y = y,
        .xStart = xStart,
        .xEnd = xEnd,
        .coverage = 255,
    };
    return True;
}

static Bool emitWindingSpans(PathSpanList *out,
                             Crossing *crossings,
                             size_t count,
                             int y)
{
    int winding = 0;
    int xStart = 0;
    for (size_t i = 0; i < count; i++) {
        int oldWinding = winding;
        winding += crossings[i].winding;
        if (oldWinding == 0 && winding != 0) {
            xStart = (int) ceil(crossings[i].x);
        } else if (oldWinding != 0 && winding == 0) {
            if (!appendSpan(out, y, xStart, (int) ceil(crossings[i].x) - 1))
                return False;
        }
    }
    return True;
}

static Bool emitEvenOddSpans(PathSpanList *out,
                             Crossing *crossings,
                             size_t count,
                             int y)
{
    for (size_t i = 0; i + 1 < count; i += 2) {
        if (!appendSpan(out, y, (int) ceil(crossings[i].x),
                        (int) ceil(crossings[i + 1].x) - 1)) {
            return False;
        }
    }
    return True;
}

Bool pathRasterizeEdges(const PathEdgeList *edges,
                        int fillRule,
                        PathSpanList *out)
{
    if (!edges || !out)
        return False;
    memset(out, 0, sizeof(*out));
    if (edges->count == 0)
        return True;

    Crossing *crossings = malloc(edges->count * sizeof(Crossing));
    if (!crossings)
        return False;

    Bool ok = True;
    int minY = (int) floor((double) edges->minY / (double) PATH_EDGE_FIXED_ONE);
    int maxY = (int) ceil((double) edges->maxY / (double) PATH_EDGE_FIXED_ONE);
    for (int y = minY; ok && y <= maxY; y++) {
        double scanY = y + 0.5;
        size_t crossingCount = 0;
        for (size_t i = 0; i < edges->count; i++) {
            PathEdge edge = edges->edges[i];
            double ax = (double) edge.x0 / (double) PATH_EDGE_FIXED_ONE;
            double ay = (double) edge.y0 / (double) PATH_EDGE_FIXED_ONE;
            double bx = (double) edge.x1 / (double) PATH_EDGE_FIXED_ONE;
            double by = (double) edge.y1 / (double) PATH_EDGE_FIXED_ONE;
            double edgeMinY = ay < by ? ay : by;
            double edgeMaxY = ay > by ? ay : by;
            if (scanY < edgeMinY || scanY >= edgeMaxY)
                continue;
            crossings[crossingCount].x =
                ax + (scanY - ay) * (bx - ax) / (by - ay);
            crossings[crossingCount].winding = edge.winding;
            crossingCount++;
        }
        if (crossingCount < 2)
            continue;
        qsort(crossings, crossingCount, sizeof(Crossing), compareCrossing);
        ok = fillRule == WindingRule
                 ? emitWindingSpans(out, crossings, crossingCount, y)
                 : emitEvenOddSpans(out, crossings, crossingCount, y);
    }

    free(crossings);
    if (!ok)
        pathFreeSpans(out);
    return ok;
}

void pathFreeSpans(PathSpanList *spans)
{
    if (!spans)
        return;
    free(spans->spans);
    memset(spans, 0, sizeof(*spans));
}
