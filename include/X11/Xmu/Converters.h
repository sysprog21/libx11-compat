#ifndef LIBX11_COMPAT_XMU_CONVERTERS_H
#define LIBX11_COMPAT_XMU_CONVERTERS_H

#include <X11/Intrinsic.h>
#include <X11/Xfuncproto.h>

typedef int XtGravity;
typedef enum { XtJustifyLeft, XtJustifyCenter, XtJustifyRight } XtJustify;
typedef enum { XtorientHorizontal, XtorientVertical } XtOrientation;

#define XtNbackingStore "backingStore"
#define XtCBackingStore "BackingStore"
#define XtRBackingStore "BackingStore"
#define XtEnotUseful "notUseful"
#define XtEwhenMapped "whenMapped"
#define XtEalways "always"
#define XtEdefault "default"
#define XtRColorCursor "ColorCursor"
#define XtNpointerColor "pointerColor"
#define XtNpointerColorBackground "pointerColorBackground"
#ifndef XtRGravity
#define XtRGravity "Gravity"
#endif
#define XtRLong "Long"
#ifndef XtRJustify
#define XtRJustify "Justify"
#endif
#define XtEForget "forget"
#define XtENorthWest "northwest"
#define XtENorth "north"
#define XtENorthEast "northeast"
#define XtEWest "west"
#define XtECenter "center"
#define XtEEast "east"
#define XtESouthWest "southwest"
#define XtESouth "south"
#define XtESouthEast "southeast"
#define XtEStatic "static"
#define XtEUnmap "unmap"
#define XtEleft "left"
#define XtEcenter "center"
#define XtEright "right"
#define XtEtop "top"
#define XtEbottom "bottom"

_XFUNCPROTOBEGIN
void XmuCvtFunctionToCallback(XrmValue *args,
                              Cardinal *num_args,
                              XrmValuePtr fromVal,
                              XrmValuePtr toVal);
void XmuCvtStringToBackingStore(XrmValue *args,
                                Cardinal *num_args,
                                XrmValuePtr fromVal,
                                XrmValuePtr toVal);
Boolean XmuCvtBackingStoreToString(Display *dpy,
                                   XrmValue *args,
                                   Cardinal *num_args,
                                   XrmValuePtr fromVal,
                                   XrmValuePtr toVal,
                                   XtPointer *converter_data);
void XmuCvtStringToCursor(XrmValue *args,
                          Cardinal *num_args,
                          XrmValuePtr fromVal,
                          XrmValuePtr toVal);
Boolean XmuCvtStringToColorCursor(Display *dpy,
                                  XrmValue *args,
                                  Cardinal *num_args,
                                  XrmValuePtr fromVal,
                                  XrmValuePtr toVal,
                                  XtPointer *converter_data);
void XmuCvtStringToGravity(XrmValue *args,
                           Cardinal *num_args,
                           XrmValuePtr fromVal,
                           XrmValuePtr toVal);
Boolean XmuCvtGravityToString(Display *dpy,
                              XrmValue *args,
                              Cardinal *num_args,
                              XrmValuePtr fromVal,
                              XrmValuePtr toVal,
                              XtPointer *converter_data);
void XmuCvtStringToJustify(XrmValue *args,
                           Cardinal *num_args,
                           XrmValuePtr fromVal,
                           XrmValuePtr toVal);
Boolean XmuCvtJustifyToString(Display *dpy,
                              XrmValue *args,
                              Cardinal *num_args,
                              XrmValuePtr fromVal,
                              XrmValuePtr toVal,
                              XtPointer *converter_data);
void XmuCvtStringToLong(XrmValue *args,
                        Cardinal *num_args,
                        XrmValuePtr fromVal,
                        XrmValuePtr toVal);
Boolean XmuCvtLongToString(Display *dpy,
                           XrmValue *args,
                           Cardinal *num_args,
                           XrmValuePtr fromVal,
                           XrmValuePtr toVal,
                           XtPointer *converter_data);
void XmuCvtStringToOrientation(XrmValue *args,
                               Cardinal *num_args,
                               XrmValuePtr fromVal,
                               XrmValuePtr toVal);
Boolean XmuCvtOrientationToString(Display *dpy,
                                  XrmValue *args,
                                  Cardinal *num_args,
                                  XrmValuePtr fromVal,
                                  XrmValuePtr toVal,
                                  XtPointer *converter_data);
void XmuCvtStringToBitmap(XrmValue *args,
                          Cardinal *num_args,
                          XrmValuePtr fromVal,
                          XrmValuePtr toVal);
_XFUNCPROTOEND

#endif
