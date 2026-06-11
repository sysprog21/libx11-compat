/* Fixed-point edge builder for path fills
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#include "edges.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static Bool doubleToFixed(double value, int64_t *out)
{
    double scaled = value * (double) PATH_EDGE_FIXED_ONE;
    if (scaled < (double) INT32_MIN || scaled > (double) INT32_MAX)
        return False;
    *out = (int64_t) llround(scaled);
    return True;
}

static Bool reserveEdge(PathEdgeList *list)
{
    if (list->count < list->capacity)
        return True;
    size_t capacity = list->capacity ? list->capacity * 2 : 32;
    if (capacity <= list->capacity || capacity > SIZE_MAX / sizeof(PathEdge))
        return False;
    PathEdge *grown = realloc(list->edges, capacity * sizeof(PathEdge));
    if (!grown)
        return False;
    list->edges = grown;
    list->capacity = capacity;
    return True;
}

static Bool appendEdge(PathEdgeList *list, PathPoint a, PathPoint b)
{
    if (a.y == b.y)
        return True;
    if (!reserveEdge(list))
        return False;
    PathEdge edge;
    if (!doubleToFixed(a.x, &edge.x0) || !doubleToFixed(a.y, &edge.y0) ||
        !doubleToFixed(b.x, &edge.x1) || !doubleToFixed(b.y, &edge.y1)) {
        return False;
    }
    edge.winding = b.y > a.y ? 1 : -1;
    list->edges[list->count++] = edge;
    int64_t edgeMinY = edge.y0 < edge.y1 ? edge.y0 : edge.y1;
    int64_t edgeMaxY = edge.y0 > edge.y1 ? edge.y0 : edge.y1;
    if (list->count == 1 || edgeMinY < list->minY)
        list->minY = edgeMinY;
    if (list->count == 1 || edgeMaxY > list->maxY)
        list->maxY = edgeMaxY;
    return True;
}

Bool pathBuildEdges(const PathPoint *points, size_t count, PathEdgeList *out)
{
    if (!out)
        return False;
    memset(out, 0, sizeof(*out));
    if (!points)
        return True;
    for (size_t i = 0; i + 1 < count; i++) {
        if (pathPointIsBreak(points[i]) || pathPointIsBreak(points[i + 1]))
            continue;
        if (!appendEdge(out, points[i], points[i + 1])) {
            pathFreeEdges(out);
            return False;
        }
    }
    return True;
}

void pathFreeEdges(PathEdgeList *edges)
{
    if (!edges)
        return;
    free(edges->edges);
    memset(edges, 0, sizeof(*edges));
}
