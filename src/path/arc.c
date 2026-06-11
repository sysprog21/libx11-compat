/* Arc-to-cubic path conversion
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#include "path.h"

#include <math.h>
#include <X11/Xlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static Bool appendArcSegment(Path *path,
                             double cx,
                             double cy,
                             double rx,
                             double ry,
                             double a0,
                             double a1)
{
    double delta = a1 - a0;
    double k = (4.0 / 3.0) * tan(delta / 4.0);
    double cos0 = cos(a0);
    double sin0 = sin(a0);
    double cos1 = cos(a1);
    double sin1 = sin(a1);

    double x1 = cx + rx * (cos0 - k * sin0);
    double y1 = cy - ry * (sin0 + k * cos0);
    double x2 = cx + rx * (cos1 + k * sin1);
    double y2 = cy - ry * (sin1 - k * cos1);
    double x3 = cx + rx * cos1;
    double y3 = cy - ry * sin1;
    return pathCubicTo(path, x1, y1, x2, y2, x3, y3);
}

Bool pathAddArc(Path *path,
                double cx,
                double cy,
                double rx,
                double ry,
                double startRad,
                double sweepRad,
                int arcMode)
{
    if (!path)
        return False;
    if (rx == 0.0 || ry == 0.0 || sweepRad == 0.0)
        return True;
    if (rx < 0.0)
        rx = -rx;
    if (ry < 0.0)
        ry = -ry;

    double full = 2.0 * M_PI;
    if (fabs(sweepRad) >= full)
        sweepRad = sweepRad < 0.0 ? -full : full;

    double startX = cx + rx * cos(startRad);
    double startY = cy - ry * sin(startRad);
    Bool fillArc = arcMode == ArcPieSlice || arcMode == ArcChord;
    if (fillArc && arcMode == ArcPieSlice) {
        if (!pathMoveTo(path, cx, cy))
            return False;
        if (!pathLineTo(path, startX, startY))
            return False;
    } else {
        if (!pathMoveTo(path, startX, startY))
            return False;
    }

    int segments = (int) ceil(fabs(sweepRad) / (M_PI / 2.0));
    if (segments < 1)
        segments = 1;
    double step = sweepRad / (double) segments;
    double a0 = startRad;
    for (int i = 0; i < segments; i++) {
        double a1 = i + 1 == segments ? startRad + sweepRad : a0 + step;
        if (!appendArcSegment(path, cx, cy, rx, ry, a0, a1))
            return False;
        a0 = a1;
    }
    if (fillArc)
        return pathClose(path);
    return True;
}
