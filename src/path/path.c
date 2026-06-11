/* Path builder storage
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#include "path.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static Bool reserveCommands(Path *path, size_t extra)
{
    if (extra > SIZE_MAX - path->commandCount)
        return False;
    size_t needed = path->commandCount + extra;
    if (needed <= path->commandCapacity)
        return True;
    size_t capacity = path->commandCapacity ? path->commandCapacity : 8;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return False;
        capacity *= 2;
    }
    unsigned char *grown = realloc(path->commands, capacity);
    if (!grown)
        return False;
    path->commands = grown;
    path->commandCapacity = capacity;
    return True;
}

static Bool reserveVertices(Path *path, size_t extraDoubles)
{
    if (extraDoubles > SIZE_MAX - path->vertexCount)
        return False;
    size_t needed = path->vertexCount + extraDoubles;
    if (needed <= path->vertexCapacity)
        return True;
    size_t capacity = path->vertexCapacity ? path->vertexCapacity : 16;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2)
            return False;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(double))
        return False;
    double *grown = realloc(path->vertices, capacity * sizeof(double));
    if (!grown)
        return False;
    path->vertices = grown;
    path->vertexCapacity = capacity;
    return True;
}

static Bool appendCommand(Path *path, PathCommand command)
{
    if (!reserveCommands(path, 1))
        return False;
    path->commands[path->commandCount++] = (unsigned char) command;
    return True;
}

static Bool appendVertex(Path *path, double x, double y)
{
    if (!reserveVertices(path, 2))
        return False;
    path->vertices[path->vertexCount++] = x;
    path->vertices[path->vertexCount++] = y;
    return True;
}

Bool pathInit(Path *path)
{
    if (!path)
        return False;
    memset(path, 0, sizeof(*path));
    return True;
}

void pathFree(Path *path)
{
    if (!path)
        return;
    free(path->commands);
    free(path->vertices);
    memset(path, 0, sizeof(*path));
}

Bool pathMoveTo(Path *path, double x, double y)
{
    if (!appendCommand(path, PATH_CMD_MOVE))
        return False;
    if (!appendVertex(path, x, y)) {
        path->commandCount--;
        return False;
    }
    return True;
}

Bool pathLineTo(Path *path, double x, double y)
{
    if (!appendCommand(path, PATH_CMD_LINE))
        return False;
    if (!appendVertex(path, x, y)) {
        path->commandCount--;
        return False;
    }
    return True;
}

Bool pathQuadTo(Path *path, double x1, double y1, double x2, double y2)
{
    if (!appendCommand(path, PATH_CMD_QUAD))
        return False;
    if (!reserveVertices(path, 4)) {
        path->commandCount--;
        return False;
    }
    appendVertex(path, x1, y1);
    appendVertex(path, x2, y2);
    return True;
}

Bool pathCubicTo(Path *path,
                 double x1,
                 double y1,
                 double x2,
                 double y2,
                 double x3,
                 double y3)
{
    if (!appendCommand(path, PATH_CMD_CUBIC))
        return False;
    if (!reserveVertices(path, 6)) {
        path->commandCount--;
        return False;
    }
    appendVertex(path, x1, y1);
    appendVertex(path, x2, y2);
    appendVertex(path, x3, y3);
    return True;
}

Bool pathClose(Path *path)
{
    return appendCommand(path, PATH_CMD_CLOSE);
}
