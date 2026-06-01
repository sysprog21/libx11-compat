#ifndef GC_H
#define GC_H

#include "X11/Xlibint.h"
#include "resource-types.h"

typedef struct _GraphicContext {
    int lineWidth;
    unsigned long foreground;
    unsigned long background;
    int fillStyle;
    Pixmap stipple;
    Font font;
    int function;
    unsigned long planeMask;
    int lineStyle;
    int capStyle;
    int joinStyle;
    int fillRule;
    Pixmap tile;
    int tileStipOriginX;
    int tileStipOriginY;
    int subWindowMode;
    int graphicsExposures;
    int clipOriginX;
    int clipOriginY;
    Pixmap clipMask;
    /* Stored clip rectangles from XSetClipRectangles / XSetRegion.
     * clipRectanglesSet distinguishes an empty clip region from no rectangle
     * clip region. */
    Bool clipRectanglesSet;
    XRectangle *clipRects;
    int clipRectCount;
    int dashOffset;
    char *dashes;  // If numDashes is uneven, this has to be treated as
                   // concatenated with itself.
    size_t numDashes;
    int arcMode;
    /* Bumped on every mutation. Lets the draw path skip re-applying
     * unchanged SDL state by caching the (gc, generation) pair per
     * drawable / renderer. */
    unsigned long generation;
} GraphicContext;

#define GC_BUMP_GENERATION(gc) ((gc)->generation++)

#define GET_GC(gc) GET_GC_FROM_XID(((struct _XGC *) (gc))->gid)
#define GET_GC_FROM_XID(id) ((GraphicContext *) GET_XID_VALUE(id))

#endif /* GC_H */
