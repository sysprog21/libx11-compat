/* Fixed-point edge storage for path fills
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef PATH_EDGES_H
#define PATH_EDGES_H

#include <stdint.h>
#include <stddef.h>
#include <X11/Xlib.h>

#include "path.h"

#define PATH_EDGE_FIXED_SHIFT 8
#define PATH_EDGE_FIXED_ONE (1 << PATH_EDGE_FIXED_SHIFT)

typedef struct {
    int64_t x0;
    int64_t y0;
    int64_t x1;
    int64_t y1;
    int winding;
} PathEdge;

typedef struct {
    PathEdge *edges;
    size_t count;
    size_t capacity;
    int64_t minY;
    int64_t maxY;
} PathEdgeList;

Bool pathBuildEdges(const PathPoint *points, size_t count, PathEdgeList *out);
void pathFreeEdges(PathEdgeList *edges);

#endif /* PATH_EDGES_H */
