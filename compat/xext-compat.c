#include <dlfcn.h>

#include <X11/extensions/shape.h>

#define RESOLVE(name) ((name##Fn) dlsym(RTLD_NEXT, #name))

typedef Bool (*XShapeQueryExtensionFn)(Display *, int *, int *);
typedef Status (*XShapeQueryVersionFn)(Display *, int *, int *);
typedef void (
    *XShapeCombineRegionFn)(Display *, Window, int, int, int, Region, int);
typedef void (*XShapeCombineRectanglesFn)(Display *,
                                          XID,
                                          int,
                                          int,
                                          int,
                                          XRectangle *,
                                          int,
                                          int,
                                          int);
typedef void (*XShapeCombineMaskFn)(Display *, XID, int, int, int, Pixmap, int);
typedef void (
    *XShapeCombineShapeFn)(Display *, XID, int, int, int, Pixmap, int, int);
typedef void (*XShapeOffsetShapeFn)(Display *, XID, int, int, int);
typedef Status (*XShapeQueryExtentsFn)(Display *,
                                       Window,
                                       Bool *,
                                       int *,
                                       int *,
                                       unsigned int *,
                                       unsigned int *,
                                       Bool *,
                                       int *,
                                       int *,
                                       unsigned int *,
                                       unsigned int *);
typedef void (*XShapeSelectInputFn)(Display *, Window, unsigned long);
typedef unsigned long (*XShapeInputSelectedFn)(Display *, Window);
typedef XRectangle *(
    *XShapeGetRectanglesFn)(Display *, Window, int, int *, int *);

Bool XShapeQueryExtension(Display *dpy, int *event_basep, int *error_basep)
{
    XShapeQueryExtensionFn fn = RESOLVE(XShapeQueryExtension);
    return fn ? fn(dpy, event_basep, error_basep) : False;
}

Status XShapeQueryVersion(Display *dpy,
                          int *major_versionp,
                          int *minor_versionp)
{
    XShapeQueryVersionFn fn = RESOLVE(XShapeQueryVersion);
    return fn ? fn(dpy, major_versionp, minor_versionp) : 0;
}

void XShapeCombineRegion(Display *dpy,
                         Window dest,
                         int destKind,
                         int xOff,
                         int yOff,
                         Region r,
                         int op)
{
    XShapeCombineRegionFn fn = RESOLVE(XShapeCombineRegion);
    if (fn)
        fn(dpy, dest, destKind, xOff, yOff, r, op);
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
    XShapeCombineRectanglesFn fn = RESOLVE(XShapeCombineRectangles);
    if (fn)
        fn(dpy, dest, destKind, xOff, yOff, rects, n_rects, op, ordering);
}

void XShapeCombineMask(Display *dpy,
                       XID dest,
                       int destKind,
                       int xOff,
                       int yOff,
                       Pixmap src,
                       int op)
{
    XShapeCombineMaskFn fn = RESOLVE(XShapeCombineMask);
    if (fn)
        fn(dpy, dest, destKind, xOff, yOff, src, op);
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
    XShapeCombineShapeFn fn = RESOLVE(XShapeCombineShape);
    if (fn)
        fn(dpy, dest, destKind, xOff, yOff, src, srcKind, op);
}

void XShapeOffsetShape(Display *dpy, XID dest, int destKind, int xOff, int yOff)
{
    XShapeOffsetShapeFn fn = RESOLVE(XShapeOffsetShape);
    if (fn)
        fn(dpy, dest, destKind, xOff, yOff);
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
    XShapeQueryExtentsFn fn = RESOLVE(XShapeQueryExtents);
    return fn ? fn(dpy, w, bShaped, xbs, ybs, wbs, hbs, cShaped, xcs, ycs, wcs,
                   hcs)
              : 0;
}

void XShapeSelectInput(Display *dpy, Window window, unsigned long mask)
{
    XShapeSelectInputFn fn = RESOLVE(XShapeSelectInput);
    if (fn)
        fn(dpy, window, mask);
}

unsigned long XShapeInputSelected(Display *dpy, Window window)
{
    XShapeInputSelectedFn fn = RESOLVE(XShapeInputSelected);
    return fn ? fn(dpy, window) : 0;
}

XRectangle *XShapeGetRectangles(Display *dpy,
                                Window window,
                                int kind,
                                int *count,
                                int *ordering)
{
    XShapeGetRectanglesFn fn = RESOLVE(XShapeGetRectangles);
    return fn ? fn(dpy, window, kind, count, ordering) : NULL;
}
