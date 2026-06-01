#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/shape.h>
#include <stddef.h>

/* The X Shape extension reshapes windows from rectangles to arbitrary
 * regions. We do not back this for SDL-managed windows; the safe answer is
 * "unsupported" so probing clients (Tk, some GTK paths) fall back to plain
 * rectangular geometry rather than crashing. */

Bool XShapeQueryExtension(Display *dpy, int *event_basep, int *error_basep)
{
    (void) dpy;
    if (event_basep)
        *event_basep = 0;
    if (error_basep)
        *error_basep = 0;
    return False;
}

Status XShapeQueryVersion(Display *dpy,
                          int *major_versionp,
                          int *minor_versionp)
{
    (void) dpy;
    if (major_versionp)
        *major_versionp = 0;
    if (minor_versionp)
        *minor_versionp = 0;
    return 0;
}

void XShapeCombineRegion(Display *dpy,
                         Window dest,
                         int destKind,
                         int xOff,
                         int yOff,
                         Region r,
                         int op)
{
    (void) dpy;
    (void) dest;
    (void) destKind;
    (void) xOff;
    (void) yOff;
    (void) r;
    (void) op;
}

void XShapeCombineRectangles(Display *dpy,
                             XID dest,
                             int destKind,
                             int xOff,
                             int yOff,
                             XRectangle *rects,
                             int n_rects,
                             int op,
                             int ordering)
{
    (void) dpy;
    (void) dest;
    (void) destKind;
    (void) xOff;
    (void) yOff;
    (void) rects;
    (void) n_rects;
    (void) op;
    (void) ordering;
}

void XShapeCombineMask(Display *dpy,
                       XID dest,
                       int destKind,
                       int xOff,
                       int yOff,
                       Pixmap src,
                       int op)
{
    (void) dpy;
    (void) dest;
    (void) destKind;
    (void) xOff;
    (void) yOff;
    (void) src;
    (void) op;
}

void XShapeCombineShape(Display *dpy,
                        XID dest,
                        int destKind,
                        int xOff,
                        int yOff,
                        Pixmap src,
                        int srcKind,
                        int op)
{
    (void) dpy;
    (void) dest;
    (void) destKind;
    (void) xOff;
    (void) yOff;
    (void) src;
    (void) srcKind;
    (void) op;
}

void XShapeOffsetShape(Display *dpy, XID dest, int destKind, int xOff, int yOff)
{
    (void) dpy;
    (void) dest;
    (void) destKind;
    (void) xOff;
    (void) yOff;
}

Status XShapeQueryExtents(Display *dpy,
                          Window w,
                          Bool *bShaped,
                          int *xbs,
                          int *ybs,
                          unsigned int *wbs,
                          unsigned int *hbs,
                          Bool *cShaped,
                          int *xcs,
                          int *ycs,
                          unsigned int *wcs,
                          unsigned int *hcs)
{
    (void) dpy;
    (void) w;
    if (bShaped)
        *bShaped = False;
    if (cShaped)
        *cShaped = False;
    if (xbs)
        *xbs = 0;
    if (ybs)
        *ybs = 0;
    if (wbs)
        *wbs = 0;
    if (hbs)
        *hbs = 0;
    if (xcs)
        *xcs = 0;
    if (ycs)
        *ycs = 0;
    if (wcs)
        *wcs = 0;
    if (hcs)
        *hcs = 0;
    return 0;
}

void XShapeSelectInput(Display *dpy, Window window, unsigned long mask)
{
    (void) dpy;
    (void) window;
    (void) mask;
}

unsigned long XShapeInputSelected(Display *dpy, Window window)
{
    (void) dpy;
    (void) window;
    return 0;
}

XRectangle *XShapeGetRectangles(Display *dpy,
                                Window window,
                                int kind,
                                int *count,
                                int *ordering)
{
    (void) dpy;
    (void) window;
    (void) kind;
    if (count)
        *count = 0;
    if (ordering)
        *ordering = 0;
    return NULL;
}
