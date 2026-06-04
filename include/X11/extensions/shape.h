#ifndef _SHAPE_H_
#define _SHAPE_H_

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#define SHAPENAME "SHAPE"

#define SHAPE_MAJOR_VERSION 1
#define SHAPE_MINOR_VERSION 1

#define ShapeSet 0
#define ShapeUnion 1
#define ShapeIntersect 2
#define ShapeSubtract 3
#define ShapeInvert 4

#define ShapeBounding 0
#define ShapeClip 1
#define ShapeInput 2

#define ShapeNotifyMask (1L << 0)
#define ShapeNotify 0

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    int kind;
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    Time time;
    Bool shaped;
} XShapeEvent;

extern Bool XShapeQueryExtension(Display *dpy,
                                 int *event_basep,
                                 int *error_basep);
extern Status XShapeQueryVersion(Display *dpy,
                                 int *major_versionp,
                                 int *minor_versionp);
extern void XShapeCombineRegion(Display *dpy,
                                Window dest,
                                int destKind,
                                int xOff,
                                int yOff,
                                Region r,
                                int op);
extern void XShapeCombineRectangles(Display *dpy,
                                    XID dest,
                                    int destKind,
                                    int xOff,
                                    int yOff,
                                    XRectangle *rects,
                                    int n_rects,
                                    int op,
                                    int ordering);
extern void XShapeCombineMask(Display *dpy,
                              XID dest,
                              int destKind,
                              int xOff,
                              int yOff,
                              Pixmap src,
                              int op);
extern void XShapeCombineShape(Display *dpy,
                               XID dest,
                               int destKind,
                               int xOff,
                               int yOff,
                               Pixmap src,
                               int srcKind,
                               int op);
extern void XShapeOffsetShape(Display *dpy,
                              XID dest,
                              int destKind,
                              int xOff,
                              int yOff);
extern Status XShapeQueryExtents(Display *dpy,
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
                                 unsigned int *hcs);
extern void XShapeSelectInput(Display *dpy, Window window, unsigned long mask);
extern unsigned long XShapeInputSelected(Display *dpy, Window window);
extern XRectangle *XShapeGetRectangles(Display *dpy,
                                       Window window,
                                       int kind,
                                       int *count,
                                       int *ordering);

#endif /* _SHAPE_H_ */
