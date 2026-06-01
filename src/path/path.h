/* Path builder for libx11-compat
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#ifndef PATH_PATH_H
#define PATH_PATH_H

#include <stddef.h>
#include <X11/Xlib.h>

typedef enum {
    PATH_CMD_MOVE = 1,
    PATH_CMD_LINE,
    PATH_CMD_QUAD,
    PATH_CMD_CUBIC,
    PATH_CMD_CLOSE
} PathCommand;

typedef struct {
    unsigned char *commands;
    size_t commandCount;
    size_t commandCapacity;
    double *vertices;
    size_t vertexCount;
    size_t vertexCapacity;
} Path;

typedef struct {
    double x;
    double y;
} PathPoint;

Bool pathInit(Path *path);
void pathFree(Path *path);
Bool pathMoveTo(Path *path, double x, double y);
Bool pathLineTo(Path *path, double x, double y);
Bool pathQuadTo(Path *path, double x1, double y1, double x2, double y2);
Bool pathCubicTo(Path *path,
                 double x1,
                 double y1,
                 double x2,
                 double y2,
                 double x3,
                 double y3);
Bool pathClose(Path *path);

Bool pathFlatten(const Path *path,
                 double tolerance,
                 PathPoint **pointsReturn,
                 size_t *countReturn);
Bool pathAddArc(Path *path,
                double cx,
                double cy,
                double rx,
                double ry,
                double startRad,
                double sweepRad,
                int arcMode);
Bool pathStrokePolyline(Path *out,
                        const PathPoint *points,
                        size_t count,
                        double width,
                        int capStyle,
                        int joinStyle);
Bool pathPointIsBreak(PathPoint point);

#endif /* PATH_PATH_H */
