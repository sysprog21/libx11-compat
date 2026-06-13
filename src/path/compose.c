/* Span composition helpers
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */
#include "compose.h"

#include <limits.h>
#include <pixman.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "../colors.h"

static Uint32 colorToRgba8888(unsigned long color)
{
    return ((Uint32) color << 8) | ((Uint32) color >> 24);
}

static Bool spanFitsPixmanRect(PathSpan span, int width, int height)
{
    if (span.coverage != 255)
        return False;
    if (span.y < 0 || span.y >= height)
        return False;
    int xStart = span.xStart < 0 ? 0 : span.xStart;
    int xEnd = span.xEnd >= width ? width - 1 : span.xEnd;
    if (xEnd < xStart)
        return False;
    if (xStart > INT16_MAX || span.y > INT16_MAX)
        return False;
    if (xEnd - xStart + 1 > UINT16_MAX)
        return False;
    return True;
}

static Bool fillFullCoverageSpansWithPixman(Uint32 *buffer,
                                            int width,
                                            int height,
                                            const PathSpanList *spans,
                                            unsigned long color,
                                            Bool *usedPixman)
{
    *usedPixman = False;
    if (width > INT16_MAX || height > INT16_MAX)
        return True;

    size_t rectCount = 0;
    for (size_t i = 0; i < spans->count; i++) {
        if (spanFitsPixmanRect(spans->spans[i], width, height))
            rectCount++;
    }
    if (rectCount == 0)
        return True;
    if (rectCount > SIZE_MAX / sizeof(pixman_rectangle16_t))
        return False;
    pixman_rectangle16_t *rects =
        malloc(rectCount * sizeof(pixman_rectangle16_t));
    if (!rects)
        return False;

    size_t out = 0;
    for (size_t i = 0; i < spans->count; i++) {
        PathSpan span = spans->spans[i];
        if (!spanFitsPixmanRect(span, width, height))
            continue;
        int xStart = span.xStart < 0 ? 0 : span.xStart;
        int xEnd = span.xEnd >= width ? width - 1 : span.xEnd;
        rects[out++] = (pixman_rectangle16_t) {
            .x = (int16_t) xStart,
            .y = (int16_t) span.y,
            .width = (uint16_t) (xEnd - xStart + 1),
            .height = 1,
        };
    }

    pixman_image_t *image = pixman_image_create_bits(
        PIXMAN_r8g8b8a8, width, height, buffer, width * (int) sizeof(Uint32));
    if (!image) {
        free(rects);
        return False;
    }
    pixman_color_t pixmanColor = {
        .red = (uint16_t) GET_RED_FROM_COLOR(color) * 257u,
        .green = (uint16_t) GET_GREEN_FROM_COLOR(color) * 257u,
        .blue = (uint16_t) GET_BLUE_FROM_COLOR(color) * 257u,
        .alpha = (uint16_t) GET_ALPHA_FROM_COLOR(color) * 257u,
    };
    Bool ok = pixman_image_fill_rectangles(PIXMAN_OP_SRC, image, &pixmanColor,
                                           (int) rectCount, rects)
                  ? True
                  : False;
    pixman_image_unref(image);
    free(rects);
    *usedPixman = ok;
    return ok;
}

Bool pathComposeSpansToBuffer(Uint32 *buffer,
                              int width,
                              int height,
                              const PathSpanList *spans,
                              unsigned long color)
{
    if (!buffer || !spans || width < 0 || height < 0)
        return False;
    if (width > INT_MAX || height > INT_MAX)
        return False;
    if (height != 0 &&
        (size_t) width > SIZE_MAX / sizeof(Uint32) / (size_t) height) {
        return False;
    }

    /* Promote core X11 pixels (alpha byte == 0) to opaque before they reach the
     * pixman fill or the per-span blend loop; otherwise primitives that use
     * BlackPixel would write transparent destinations.
     */
    color = colorWithOpaqueDefault(color);

    Uint32 src = colorToRgba8888(color);
    Bool usedPixman = False;
    if (!fillFullCoverageSpansWithPixman(buffer, width, height, spans, color,
                                         &usedPixman)) {
        return False;
    }
    for (size_t i = 0; i < spans->count; i++) {
        PathSpan span = spans->spans[i];
        if (span.y < 0 || span.y >= height)
            continue;
        int xStart = span.xStart < 0 ? 0 : span.xStart;
        int xEnd = span.xEnd >= width ? width - 1 : span.xEnd;
        if (xEnd < xStart)
            continue;
        Uint32 *row = buffer + (size_t) span.y * (size_t) width;
        if (span.coverage == 255) {
            if (usedPixman && spanFitsPixmanRect(span, width, height))
                continue;
            for (int x = xStart; x <= xEnd; x++)
                row[x] = src;
        } else {
            Uint8 alpha = span.coverage;
            Uint8 invAlpha = 255 - alpha;
            Uint8 sr = GET_RED_FROM_COLOR(color);
            Uint8 sg = GET_GREEN_FROM_COLOR(color);
            Uint8 sb = GET_BLUE_FROM_COLOR(color);
            Uint8 sa = GET_ALPHA_FROM_COLOR(color);
            for (int x = xStart; x <= xEnd; x++) {
                Uint32 dst = row[x];
                Uint8 dr = (Uint8) ((dst >> 24) & 0xff);
                Uint8 dg = (Uint8) ((dst >> 16) & 0xff);
                Uint8 db = (Uint8) ((dst >> 8) & 0xff);
                Uint8 da = (Uint8) (dst & 0xff);
                Uint8 outR = (Uint8) ((sr * alpha + dr * invAlpha) / 255);
                Uint8 outG = (Uint8) ((sg * alpha + dg * invAlpha) / 255);
                Uint8 outB = (Uint8) ((sb * alpha + db * invAlpha) / 255);
                Uint8 outA = (Uint8) ((sa * alpha + da * invAlpha) / 255);
                row[x] = ((Uint32) outR << 24) | ((Uint32) outG << 16) |
                         ((Uint32) outB << 8) | outA;
            }
        }
    }
    return True;
}
