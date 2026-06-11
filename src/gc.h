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
     * clip region.
     */
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

#define GET_GC_FROM_XID(id) ((GraphicContext *) GET_XID_VALUE(id))

/* Debug-only NULL guard. Production keeps the bare deref so well-formed
 * callers pay nothing; misbehaving callers (or future regressions like
 * the round-6 XCopyArea NULL gc finding) abort with file:line.
 */
#ifdef DEBUG_LIBX11_COMPAT
#include <stdio.h>
#include <stdlib.h>
#define GET_GC(gc)                                                            \
    (__extension__({                                                          \
        GC _gg_gc = (gc);                                                     \
        if (!_gg_gc) {                                                        \
            fprintf(stderr, "%s:%d: GET_GC(NULL) in debug build\n", __FILE__, \
                    __LINE__);                                                \
            abort();                                                          \
        }                                                                     \
        GET_GC_FROM_XID(((struct _XGC *) _gg_gc)->gid);                       \
    }))
#else
#define GET_GC(gc) GET_GC_FROM_XID(((struct _XGC *) (gc))->gid)
#endif

#endif /* GC_H */
