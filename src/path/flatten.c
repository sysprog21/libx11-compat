/* Adaptive path flattening
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#include "path.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PATH_FLATTEN_MAX_DEPTH 32
#define PATH_FLATTEN_SEGMENT_POINT_CAP 65536

typedef struct {
    PathPoint *points;
    size_t count;
    size_t capacity;
} PointList;

typedef struct {
    PathPoint p0;
    PathPoint p1;
    PathPoint p2;
    PathPoint p3;
    int depth;
} CubicWork;

typedef struct {
    PathPoint p0;
    PathPoint p1;
    PathPoint p2;
    int depth;
} QuadWork;

static double pointDistanceToLine(PathPoint p, PathPoint a, PathPoint b)
{
    double dx = b.x - a.x;
    double dy = b.y - a.y;
    double len = hypot(dx, dy);
    if (len == 0.0)
        return hypot(p.x - a.x, p.y - a.y);
    return fabs(dy * p.x - dx * p.y + b.x * a.y - b.y * a.x) / len;
}

static PathPoint midpoint(PathPoint a, PathPoint b)
{
    PathPoint out = {(a.x + b.x) * 0.5, (a.y + b.y) * 0.5};
    return out;
}

static Bool appendPointInternal(PointList *list, PathPoint point, Bool dedup)
{
    if (dedup && list->count > 0) {
        PathPoint last = list->points[list->count - 1];
        if (last.x == point.x && last.y == point.y)
            return True;
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2 : 32;
        if (capacity <= list->capacity ||
            capacity > SIZE_MAX / sizeof(PathPoint)) {
            return False;
        }
        PathPoint *grown = realloc(list->points, capacity * sizeof(PathPoint));
        if (!grown)
            return False;
        list->points = grown;
        list->capacity = capacity;
    }
    list->points[list->count++] = point;
    return True;
}

static Bool appendPoint(PointList *list, PathPoint point)
{
    return appendPointInternal(list, point, True);
}

static Bool appendLine(PointList *list, PathPoint from, PathPoint to)
{
    if (!appendPoint(list, from))
        return False;
    if (from.x == to.x && from.y == to.y)
        return appendPointInternal(list, to, False);
    return appendPoint(list, to);
}

static Bool appendContourBreak(PointList *list)
{
    PathPoint point = {NAN, NAN};
    return appendPoint(list, point);
}

Bool pathPointIsBreak(PathPoint point)
{
    return isnan(point.x) || isnan(point.y);
}

static Bool flattenQuad(PointList *list,
                        PathPoint p0,
                        PathPoint p1,
                        PathPoint p2,
                        double tolerance)
{
    QuadWork stack[PATH_FLATTEN_MAX_DEPTH + 1];
    int stackCount = 1;
    size_t emitted = 0;
    stack[0] = (QuadWork) {p0, p1, p2, 0};
    while (stackCount > 0) {
        QuadWork work = stack[--stackCount];
        double flatness = pointDistanceToLine(work.p1, work.p0, work.p2);
        if (flatness <= tolerance) {
            if (++emitted > PATH_FLATTEN_SEGMENT_POINT_CAP)
                return False;
            if (!appendLine(list, work.p0, work.p2))
                return False;
            continue;
        }
        if (work.depth >= PATH_FLATTEN_MAX_DEPTH)
            return False;
        PathPoint p01 = midpoint(work.p0, work.p1);
        PathPoint p12 = midpoint(work.p1, work.p2);
        PathPoint p012 = midpoint(p01, p12);
        if (stackCount + 2 > (int) (sizeof(stack) / sizeof(stack[0]))) {
            return False;
        }
        stack[stackCount++] = (QuadWork) {p012, p12, work.p2, work.depth + 1};
        stack[stackCount++] = (QuadWork) {work.p0, p01, p012, work.depth + 1};
    }
    return True;
}

static Bool flattenCubic(PointList *list,
                         PathPoint p0,
                         PathPoint p1,
                         PathPoint p2,
                         PathPoint p3,
                         double tolerance)
{
    CubicWork stack[PATH_FLATTEN_MAX_DEPTH + 1];
    int stackCount = 1;
    size_t emitted = 0;
    stack[0] = (CubicWork) {p0, p1, p2, p3, 0};
    while (stackCount > 0) {
        CubicWork work = stack[--stackCount];
        double flatness1 = pointDistanceToLine(work.p1, work.p0, work.p3);
        double flatness2 = pointDistanceToLine(work.p2, work.p0, work.p3);
        if (flatness1 <= tolerance && flatness2 <= tolerance) {
            if (++emitted > PATH_FLATTEN_SEGMENT_POINT_CAP)
                return False;
            if (!appendLine(list, work.p0, work.p3))
                return False;
            continue;
        }
        if (work.depth >= PATH_FLATTEN_MAX_DEPTH)
            return False;
        PathPoint p01 = midpoint(work.p0, work.p1);
        PathPoint p12 = midpoint(work.p1, work.p2);
        PathPoint p23 = midpoint(work.p2, work.p3);
        PathPoint p012 = midpoint(p01, p12);
        PathPoint p123 = midpoint(p12, p23);
        PathPoint p0123 = midpoint(p012, p123);
        if (stackCount + 2 > (int) (sizeof(stack) / sizeof(stack[0]))) {
            return False;
        }
        stack[stackCount++] =
            (CubicWork) {p0123, p123, p23, work.p3, work.depth + 1};
        stack[stackCount++] =
            (CubicWork) {work.p0, p01, p012, p0123, work.depth + 1};
    }
    return True;
}

Bool pathFlatten(const Path *path,
                 double tolerance,
                 PathPoint **pointsReturn,
                 size_t *countReturn)
{
    if (!path || !pointsReturn || !countReturn)
        return False;
    *pointsReturn = NULL;
    *countReturn = 0;
    if (tolerance <= 0.0)
        tolerance = 0.25;

    PointList list;
    memset(&list, 0, sizeof(list));
    size_t vertex = 0;
    PathPoint current = {0.0, 0.0};
    PathPoint subpathStart = {0.0, 0.0};
    Bool hasCurrent = False;
    Bool ok = True;

    for (size_t i = 0; ok && i < path->commandCount; i++) {
        switch ((PathCommand) path->commands[i]) {
        case PATH_CMD_MOVE:
            if (vertex + 2 > path->vertexCount) {
                ok = False;
                break;
            }
            if (hasCurrent)
                ok = appendContourBreak(&list);
            if (!ok)
                break;
            current.x = path->vertices[vertex++];
            current.y = path->vertices[vertex++];
            subpathStart = current;
            hasCurrent = True;
            ok = appendPoint(&list, current);
            break;
        case PATH_CMD_LINE: {
            if (!hasCurrent || vertex + 2 > path->vertexCount) {
                ok = False;
                break;
            }
            PathPoint to = {path->vertices[vertex], path->vertices[vertex + 1]};
            vertex += 2;
            ok = appendLine(&list, current, to);
            current = to;
            break;
        }
        case PATH_CMD_QUAD: {
            if (!hasCurrent || vertex + 4 > path->vertexCount) {
                ok = False;
                break;
            }
            PathPoint p1 = {path->vertices[vertex], path->vertices[vertex + 1]};
            PathPoint p2 = {path->vertices[vertex + 2],
                            path->vertices[vertex + 3]};
            vertex += 4;
            ok = flattenQuad(&list, current, p1, p2, tolerance);
            current = p2;
            break;
        }
        case PATH_CMD_CUBIC: {
            if (!hasCurrent || vertex + 6 > path->vertexCount) {
                ok = False;
                break;
            }
            PathPoint p1 = {path->vertices[vertex], path->vertices[vertex + 1]};
            PathPoint p2 = {path->vertices[vertex + 2],
                            path->vertices[vertex + 3]};
            PathPoint p3 = {path->vertices[vertex + 4],
                            path->vertices[vertex + 5]};
            vertex += 6;
            ok = flattenCubic(&list, current, p1, p2, p3, tolerance);
            current = p3;
            break;
        }
        case PATH_CMD_CLOSE:
            if (!hasCurrent) {
                ok = False;
                break;
            }
            ok = appendLine(&list, current, subpathStart);
            current = subpathStart;
            break;
        default:
            ok = False;
            break;
        }
    }
    if (!ok) {
        free(list.points);
        return False;
    }
    *pointsReturn = list.points;
    *countReturn = list.count;
    return True;
}
