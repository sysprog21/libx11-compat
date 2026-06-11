/* Span rasterization for path fills
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef PATH_RASTERIZE_H
#define PATH_RASTERIZE_H

#include <stddef.h>
#include <stdint.h>
#include <X11/Xlib.h>

#include "edges.h"

typedef struct {
    int y;
    int xStart;
    int xEnd;
    uint8_t coverage;
} PathSpan;

typedef struct {
    PathSpan *spans;
    size_t count;
    size_t capacity;
} PathSpanList;

Bool pathRasterizeEdges(const PathEdgeList *edges,
                        int fillRule,
                        PathSpanList *out);
void pathFreeSpans(PathSpanList *spans);

#endif /* PATH_RASTERIZE_H */
