#include <X11/Xlib.h>
#include <stdint.h>
#include <stdlib.h>
#include "gc.h"
#include "display.h"
#include "drawing.h"
#include "colors.h"
#include "font.h"

int XFreeGC(Display *display, GC gc)
{
    SET_X_SERVER_REQUEST(display, X_FreeGC);
    invalidateGcStateCache(gc);
    GraphicContext *gContext = GET_GC(gc);
    if (gContext->dashes)
        free(gContext->dashes);
    if (gContext->clipRects) {
        free(gContext->clipRects);
        gContext->clipRects = NULL;
        gContext->clipRectCount = 0;
    }
    compatFontReleaseForGC(gContext->font);
    gContext->clipRectanglesSet = False;
    free(gContext);
    XExtData *extData = gc->ext_data;
    while (extData) {
        XExtData *data = extData;
        extData = data->next;
        free(data);
    }
    FREE_XID(gc->gid);
    free(gc);
    return 1;
}

GC XCreateGC(Display *display,
             Drawable d,
             unsigned long valuemask,
             XGCValues *values)
{
    // https://tronche.com/gui/x/xlib/GC/XCreateGC.html
    SET_X_SERVER_REQUEST(display, X_CreateGC);
    TYPE_CHECK(d, DRAWABLE, display, NULL);
    if (IS_TYPE(d, WINDOW) && IS_INPUT_ONLY(d)) {
        handleError(0, display, d, 0, BadMatch, 0);
        return NULL;
    }
    XID contextId = ALLOC_XID();
    if (contextId == None) {
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    GraphicContext *gc = malloc(sizeof(GraphicContext));
    if (!gc) {
        FREE_XID(contextId);
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    GC graphicContextStruct = malloc(sizeof(struct _XGC));
    if (!graphicContextStruct) {
        FREE_XID(contextId);
        free(gc);
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    graphicContextStruct->ext_data = NULL;
    graphicContextStruct->gid = contextId;
    SET_XID_TYPE(contextId, GRAPHICS_CONTEXT);
    SET_XID_VALUE(contextId, gc);
    /* Initialize every field to safe defaults before any fallible allocation.
     * XFreeGC walks gc->clipRects via free() and gc->font via
     * compatFontReleaseForGC; if the dashes malloc below fails the cleanup path
     * runs with whatever garbage malloc(3) left behind unless those fields are
     * zeroed first.
     */
    gc->dashes = NULL;
    gc->numDashes = 0;
    gc->function = GXcopy;
    gc->planeMask = 0xFFFFFFFF;
    /* Per X11 spec the defaults are pixel indices 0 (foreground) and 1
     * (background). This shim treats pixel values as direct ARGB (see
     * src/colors.h), so default to opaque black/white instead. Otherwise
     * alpha=0 makes blended text rendering (TTF_RenderUTF8_Blended) invisible.
     */
    gc->foreground = 0xFF000000;
    gc->background = 0xFFFFFFFF;
    gc->lineWidth = 0;
    gc->lineStyle = LineSolid;
    gc->capStyle = CapButt;
    gc->joinStyle = JoinMiter;
    gc->fillStyle = FillSolid;
    gc->fillRule = EvenOddRule;
    gc->arcMode = ArcPieSlice;
    gc->tile = None;
    gc->stipple = None;
    gc->tileStipOriginX = 0;
    gc->tileStipOriginY = 0;
    gc->font = None;
    gc->subWindowMode = ClipByChildren;
    gc->graphicsExposures = True;
    gc->clipOriginX = 0;
    gc->clipOriginY = 0;
    gc->clipMask = None;
    gc->clipRectanglesSet = False;
    gc->clipRects = NULL;
    gc->clipRectCount = 0;
    gc->dashOffset = 0;
    gc->generation = 0;

    gc->dashes = malloc(sizeof(char) * 2);
    if (!gc->dashes) {
        XFreeGC(display, graphicContextStruct);
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    gc->numDashes = 2;
    gc->dashes[0] = 4;
    gc->dashes[1] = 4;

    if (!XChangeGC(display, graphicContextStruct, valuemask, values)) {
        XFreeGC(display, graphicContextStruct);
        return NULL;
    }
    return graphicContextStruct;
}

GContext XGContextFromGC(GC gc)
{
    // https://tronche.com/gui/x/xlib/GC/XGContextFromGC.html
    return gc->gid;
}

Bool setDashes(Display *display,
               GraphicContext *gc,
               const char dashes[],
               size_t numDashes,
               Bool verifyValues)
{
    if (verifyValues) {
        size_t i;
        for (i = 0; i < numDashes; i++) {
            if (dashes[i] == 0) {
                handleError(0, display, None, 0, BadValue, 0);
                return False;
            }
        }
    }
    if (gc->numDashes != numDashes) {
        free(gc->dashes);
        gc->numDashes = 0;
        gc->dashes = malloc(sizeof(char) * numDashes);
        if (!gc->dashes) {
            handleOutOfMemory(0, display, 0, 0);
            return False;
        }
        gc->numDashes = numDashes;
    }
    memcpy(gc->dashes, dashes, sizeof(char) * numDashes);
    return True;
}

int XChangeGC(Display *display,
              GC gc,
              unsigned long valuemask,
              XGCValues *values)
{
    // https://tronche.com/gui/x/xlib/GC/XChangeGC.html
    SET_X_SERVER_REQUEST(display, X_ChangeGC);
    if (valuemask != 0 && !values) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    GraphicContext *graphicContext = GET_GC(gc);
    if (HAS_VALUE(valuemask, GCFunction))
        graphicContext->function = values->function;
    if (HAS_VALUE(valuemask, GCPlaneMask))
        graphicContext->planeMask = values->plane_mask;
    if (HAS_VALUE(valuemask, GCForeground))
        XSetForeground(display, gc, values->foreground);
    if (HAS_VALUE(valuemask, GCBackground))
        graphicContext->background = values->background;
    if (HAS_VALUE(valuemask, GCLineWidth))
        graphicContext->lineWidth = values->line_width;
    if (HAS_VALUE(valuemask, GCLineStyle))
        graphicContext->lineStyle = values->line_style;
    if (HAS_VALUE(valuemask, GCCapStyle))
        graphicContext->capStyle = values->cap_style;
    if (HAS_VALUE(valuemask, GCJoinStyle))
        graphicContext->joinStyle = values->join_style;
    if (HAS_VALUE(valuemask, GCFillStyle))
        graphicContext->fillStyle = values->fill_style;
    if (HAS_VALUE(valuemask, GCFillRule))
        graphicContext->fillRule = values->fill_rule;
    if (HAS_VALUE(valuemask, GCTile)) {
        if (!XSetTile(display, gc, values->tile))
            return 0;
    }
    if (HAS_VALUE(valuemask, GCStipple)) {
        if (!XSetStipple(display, gc, values->stipple))
            return 0;
    }
    if (HAS_VALUE(valuemask, GCTileStipXOrigin))
        graphicContext->tileStipOriginX = values->ts_x_origin;
    if (HAS_VALUE(valuemask, GCTileStipYOrigin))
        graphicContext->tileStipOriginY = values->ts_y_origin;
    if (HAS_VALUE(valuemask, GCFont)) {
        if (!XSetFont(display, gc, values->font))
            return 0;
    }
    if (HAS_VALUE(valuemask, GCSubwindowMode))
        graphicContext->subWindowMode = values->subwindow_mode;
    if (HAS_VALUE(valuemask, GCGraphicsExposures))
        graphicContext->graphicsExposures = values->graphics_exposures;
    if (HAS_VALUE(valuemask, GCClipXOrigin))
        graphicContext->clipOriginX = values->clip_x_origin;
    if (HAS_VALUE(valuemask, GCClipYOrigin))
        graphicContext->clipOriginY = values->clip_y_origin;
    if (HAS_VALUE(valuemask, GCClipMask)) {
        if (!XSetClipMask(display, gc, values->clip_mask))
            return 0;
    }
    if (HAS_VALUE(valuemask, GCDashOffset))
        graphicContext->dashOffset = values->dash_offset;
    if (HAS_VALUE(valuemask, GCDashList)) {
        const char value[] = {values->dashes, values->dashes};
        if (!setDashes(display, graphicContext, value, 2, True))
            return 0;
    }
    if (HAS_VALUE(valuemask, GCArcMode))
        graphicContext->arcMode = values->arc_mode;
    if (valuemask != 0)
        GC_BUMP_GENERATION(graphicContext);
    return 1;
}

int XCopyGC(Display *display, GC src, unsigned long valuemask, GC dest)
{
    // https://tronche.com/gui/x/xlib/GC/XCopyGC.html
    SET_X_SERVER_REQUEST(display, X_CopyGC);
    XGCValues gcValues;
    if (!XGetGCValues(display, src, valuemask, &gcValues))
        return 0;
    GraphicContext *srcGraphicContext = GET_GC(src);
    gcValues.clip_mask = srcGraphicContext->clipMask;
    if (!XChangeGC(display, dest, valuemask, &gcValues))
        return 0;
    return setDashes(display, GET_GC(dest), srcGraphicContext->dashes,
                     srcGraphicContext->numDashes, False)
               ? 1
               : 0;
}

Status XGetGCValues(Display *display,
                    GC gc,
                    unsigned long valuemask,
                    XGCValues *values_return)
{
    // https://tronche.com/gui/x/xlib/GC/XGetGCValues.html
    GraphicContext *graphicContext = GET_GC(gc);
    /* A GC id can resolve to NULL (freed or stale, for example a GC retired
     * when the client closed and reopened its display).
     *
     * Return a zero Status rather than faulting inside Xt's GC-cache Matches
     * probe; Xt reads the result and treats the GC as non-matching. This is
     * deliberately quieter than raising a protocol error, which would spam
     * every cache probe of a stale GC.
     */
    if (!graphicContext)
        return 0;
    if (HAS_VALUE(valuemask, GCFunction))
        values_return->function = graphicContext->function;
    if (HAS_VALUE(valuemask, GCPlaneMask))
        values_return->plane_mask = graphicContext->planeMask;
    if (HAS_VALUE(valuemask, GCForeground))
        values_return->foreground = graphicContext->foreground;
    if (HAS_VALUE(valuemask, GCBackground))
        values_return->background = graphicContext->background;
    if (HAS_VALUE(valuemask, GCLineWidth))
        values_return->line_width = graphicContext->lineWidth;
    if (HAS_VALUE(valuemask, GCLineStyle))
        values_return->line_style = graphicContext->lineStyle;
    if (HAS_VALUE(valuemask, GCCapStyle))
        values_return->cap_style = graphicContext->capStyle;
    if (HAS_VALUE(valuemask, GCJoinStyle))
        values_return->join_style = graphicContext->joinStyle;
    if (HAS_VALUE(valuemask, GCFillStyle))
        values_return->fill_style = graphicContext->fillStyle;
    if (HAS_VALUE(valuemask, GCFillRule))
        values_return->fill_rule = graphicContext->fillRule;
    if (HAS_VALUE(valuemask, GCTile))
        values_return->tile = graphicContext->tile;
    if (HAS_VALUE(valuemask, GCStipple))
        values_return->stipple = graphicContext->stipple;
    if (HAS_VALUE(valuemask, GCTileStipXOrigin))
        values_return->ts_x_origin = graphicContext->tileStipOriginX;
    if (HAS_VALUE(valuemask, GCTileStipYOrigin))
        values_return->ts_y_origin = graphicContext->tileStipOriginY;
    if (HAS_VALUE(valuemask, GCFont))
        values_return->font = graphicContext->font;
    if (HAS_VALUE(valuemask, GCSubwindowMode))
        values_return->subwindow_mode = graphicContext->subWindowMode;
    if (HAS_VALUE(valuemask, GCGraphicsExposures))
        values_return->graphics_exposures = graphicContext->graphicsExposures;
    if (HAS_VALUE(valuemask, GCClipXOrigin))
        values_return->clip_x_origin = graphicContext->clipOriginX;
    if (HAS_VALUE(valuemask, GCClipYOrigin))
        values_return->clip_y_origin = graphicContext->clipOriginY;
    if (HAS_VALUE(valuemask, GCClipMask))
        values_return->clip_mask = graphicContext->clipMask;
    if (HAS_VALUE(valuemask, GCDashOffset))
        values_return->dash_offset = graphicContext->dashOffset;
    if (HAS_VALUE(valuemask, GCDashList)) {
        /* XGCValues.dashes is the default uniform on-off length for the dash
         * pattern; XGetDashes returns the full array. If no dashes have been
         * programmed, real Xlib reports 4 (the X protocol default). Anything
         * but a non-zero byte trips setDashes's BadValue guard, so leaving this
         * field uninitialized would crash XCopyGC the first time it asks for
         * the full GC value set.
         */
        if (graphicContext->dashes && graphicContext->numDashes > 0 &&
            graphicContext->dashes[0])
            values_return->dashes = graphicContext->dashes[0];
        else
            values_return->dashes = 4;
    }
    if (HAS_VALUE(valuemask, GCArcMode))
        values_return->arc_mode = graphicContext->arcMode;
    return 1;
}

int XSetTSOrigin(Display *display, GC gc, int ts_x_origin, int ts_y_origin)
{
    // https://tronche.com/gui/x/xlib/GC/convenience-functions/XSetTSOrigin.html
    (void) display;
    GET_GC(gc)->tileStipOriginX = ts_x_origin;
    GET_GC(gc)->tileStipOriginY = ts_y_origin;
    return 1;
}

int XSetClipOrigin(Display *display,
                   GC gc,
                   int clip_x_origin,
                   int clip_y_origin)
{
    // https://tronche.com/gui/x/xlib/GC/convenience-functions/XSetClipOrigin.html
    (void) display;
    GraphicContext *g = GET_GC(gc);
    g->clipOriginX = clip_x_origin;
    g->clipOriginY = clip_y_origin;
    GC_BUMP_GENERATION(g);
    return 1;
}

int XSetDashes(Display *display,
               GC gc,
               int dash_offset,
               _Xconst char dash_list[],
               int n)
{
    // https://tronche.com/gui/x/xlib/GC/convenience-functions/XSetDashes.html
    if (n < 1) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }
    GraphicContext *graphicContext = GET_GC(gc);
    if (!setDashes(display, graphicContext, dash_list, (size_t) n, True))
        return 0;
    graphicContext->dashOffset = dash_offset;
    return 1;
}

int XSetClipMask(Display *display, GC gc, Pixmap pixmap)
{
    // http://www.net.uom.gr/Books/Manuals/xlib/GC/convenience-functions/XSetClipMask.html
    if (pixmap != None)
        TYPE_CHECK(pixmap, PIXMAP, display, 0);
    GraphicContext *graphicContext = GET_GC(gc);
    SET_X_SERVER_REQUEST(display, X_ChangeGC);
    graphicContext->clipMask = pixmap;
    if (graphicContext->clipRects) {
        free(graphicContext->clipRects);
        graphicContext->clipRects = NULL;
        graphicContext->clipRectCount = 0;
    }
    graphicContext->clipRectanglesSet = False;
    GC_BUMP_GENERATION(graphicContext);
    return 1;
}

int XSetTile(Display *display, GC gc, Pixmap tile)
{
    if (tile != None)
        TYPE_CHECK(tile, PIXMAP, display, 0);
    SET_X_SERVER_REQUEST(display, X_ChangeGC);
    GET_GC(gc)->tile = tile;
    return 1;
}

int XSetStipple(Display *display, GC gc, Pixmap stipple)
{
    if (stipple != None)
        TYPE_CHECK(stipple, PIXMAP, display, 0);
    SET_X_SERVER_REQUEST(display, X_ChangeGC);
    GET_GC(gc)->stipple = stipple;
    return 1;
}

int XSetForeground(Display *display, GC gc, unsigned long foreground)
{
    // https://linux.die.net/man/3/xsetforeground
    (void) display;
    GraphicContext *g = GET_GC(gc);
    g->foreground = foreground;
    GC_BUMP_GENERATION(g);
    return 1;
}

int XSetBackground(Display *dpy, GC gc, unsigned long background)
{
    (void) dpy;
    GraphicContext *g = GET_GC(gc);
    g->background = background;
    GC_BUMP_GENERATION(g);
    return 1;
}

int XSetState(Display *display,
              GC gc,
              unsigned long foreground,
              unsigned long background,
              int function,
              unsigned long plane_mask)
{
    (void) display;
    GraphicContext *graphicContext = GET_GC(gc);
    graphicContext->foreground = foreground;
    graphicContext->background = background;
    graphicContext->function = function;
    graphicContext->planeMask = plane_mask;
    GC_BUMP_GENERATION(graphicContext);
    return 1;
}

int XSetPlaneMask(Display *dpy, GC gc, unsigned long planemask)
{
    (void) dpy;
    GraphicContext *g = GET_GC(gc);
    g->planeMask = planemask;
    GC_BUMP_GENERATION(g);
    return 1;
}

int XSetFont(Display *display, GC gc, Font font)
{
    /* http://www.net.uom.gr/Books/Manuals/xlib/GC/convenience-functions/XSetFont.html
     *
     * Real Xlib silently accepts font == None: it just clears the GC's font
     * slot so the server falls back to its default. Athena and Xt clients
     * exercise this through XCopyGC with a full mask against a GC that was
     * never given an explicit font (XGetGCValues returns the stored None);
     * rejecting that with BadFont broke xfig and any widget set that copies a
     * default GC. Treat None as a release.
     */
    GraphicContext *g = GET_GC(gc);
    if (font == None) {
        if (g->font != None) {
            compatFontReleaseForGC(g->font);
            g->font = None;
            GC_BUMP_GENERATION(g);
        }
        return 1;
    }
    if (font == (Font) ~0UL || font == (Font) 0xffffffffUL ||
        (uintptr_t) font < 4096) {
        handleError(0, display, font, 0, BadFont, 0);
        return 0;
    }
    TYPE_CHECK(font, FONT, display, 0);
    if (!compatFontIsClientUsable(font)) {
        handleError(0, display, font, 0, BadFont, 0);
        return 0;
    }
    if (g->font == font)
        return 1;
    if (!compatFontRetainForGC(font)) {
        handleError(0, display, font, 0, BadFont, 0);
        return 0;
    }
    compatFontReleaseForGC(g->font);
    g->font = font;
    GC_BUMP_GENERATION(g);
    return 1;
}

int XSetGraphicsExposures(Display *display, GC gc, Bool graphics_exposures)
{
    (void) display;
    GraphicContext *g = GET_GC(gc);
    g->graphicsExposures = graphics_exposures;
    GC_BUMP_GENERATION(g);
    return 1;
}

int XSetSubwindowMode(Display *display, GC gc, int subwindow_mode)
{
    (void) display;
    GraphicContext *g = GET_GC(gc);
    g->subWindowMode = subwindow_mode;
    GC_BUMP_GENERATION(g);
    return 1;
}

int XSetLineAttributes(Display *display,
                       GC gc,
                       unsigned int linewidth,
                       int linestyle,
                       int capstyle,
                       int joinstyle)
{
    (void) display;
    GraphicContext *g = GET_GC(gc);
    g->lineWidth = linewidth;
    g->lineStyle = linestyle;
    g->capStyle = capstyle;
    g->joinStyle = joinstyle;
    GC_BUMP_GENERATION(g);
    return 1;
}

int XSetFunction(Display *display, GC gc, int function)
{
    (void) display;
    GraphicContext *g = GET_GC(gc);
    g->function = function;
    GC_BUMP_GENERATION(g);
    return 1;
}
