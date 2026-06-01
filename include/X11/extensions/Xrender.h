#ifndef _XRENDER_H_
#define _XRENDER_H_

#include <X11/Xlib.h>

typedef unsigned long Picture;
typedef unsigned long GlyphSet;
typedef unsigned long Glyph;
typedef unsigned long PictFormat;

typedef struct {
    short red;
    short redMask;
    short green;
    short greenMask;
    short blue;
    short blueMask;
    short alpha;
    short alphaMask;
} XRenderDirectFormat;

typedef struct {
    PictFormat id;
    int type;
    int depth;
    XRenderDirectFormat direct;
    Colormap colormap;
} XRenderPictFormat;

typedef struct {
    unsigned short red;
    unsigned short green;
    unsigned short blue;
    unsigned short alpha;
} XRenderColor;

typedef struct {
    GlyphSet glyphset;
    _Xconst char *chars;
    int nchars;
    int xOff;
    int yOff;
} XGlyphElt8;

typedef struct {
    GlyphSet glyphset;
    _Xconst unsigned short *chars;
    int nchars;
    int xOff;
    int yOff;
} XGlyphElt16;

typedef struct {
    GlyphSet glyphset;
    _Xconst unsigned int *chars;
    int nchars;
    int xOff;
    int yOff;
} XGlyphElt32;

#define PictOpClear 0
#define PictOpSrc 1
#define PictOpDst 2
#define PictOpOver 3
#define PictOpOverReverse 4
#define PictOpIn 5
#define PictOpOut 6
#define PictOpAtop 9
#define PictOpXor 11
#define PictOpAdd 12

#define PictStandardARGB32 0
#define PictStandardRGB24 1
#define PictStandardA8 2
#define PictStandardA4 3
#define PictStandardA1 4

extern Bool XRenderQueryExtension(Display *dpy,
                                  int *event_base_return,
                                  int *error_base_return);
extern Status XRenderQueryVersion(Display *dpy,
                                  int *major_version_return,
                                  int *minor_version_return);
extern XRenderPictFormat *XRenderFindStandardFormat(Display *dpy, int format);
extern XRenderPictFormat *XRenderFindVisualFormat(Display *dpy,
                                                  _Xconst Visual *visual);
extern Picture XRenderCreatePicture(Display *dpy,
                                    Drawable drawable,
                                    _Xconst XRenderPictFormat *format,
                                    unsigned long valuemask,
                                    _Xconst void *attributes);
extern void XRenderFreePicture(Display *dpy, Picture picture);
extern void XRenderComposite(Display *dpy,
                             int op,
                             Picture src,
                             Picture mask,
                             Picture dst,
                             int src_x,
                             int src_y,
                             int mask_x,
                             int mask_y,
                             int dst_x,
                             int dst_y,
                             unsigned int width,
                             unsigned int height);
extern void XRenderFillRectangle(Display *dpy,
                                 int op,
                                 Picture dst,
                                 _Xconst XRenderColor *color,
                                 int x,
                                 int y,
                                 unsigned int width,
                                 unsigned int height);
extern void XRenderFillRectangles(Display *dpy,
                                  int op,
                                  Picture dst,
                                  _Xconst XRenderColor *color,
                                  _Xconst XRectangle *rectangles,
                                  int n_rects);
extern GlyphSet XRenderCreateGlyphSet(Display *dpy,
                                      _Xconst XRenderPictFormat *format);
extern void XRenderAddGlyphs(Display *dpy,
                             GlyphSet glyphset,
                             _Xconst Glyph *gids,
                             _Xconst void *glyphs,
                             int nglyphs,
                             _Xconst char *images,
                             int nbyte_images);
extern void XRenderCompositeText8(Display *dpy,
                                  int op,
                                  Picture src,
                                  Picture dst,
                                  _Xconst XRenderPictFormat *mask_format,
                                  int src_x,
                                  int src_y,
                                  int dst_x,
                                  int dst_y,
                                  _Xconst XGlyphElt8 *elts,
                                  int n_elts);
extern void XRenderCompositeText16(Display *dpy,
                                   int op,
                                   Picture src,
                                   Picture dst,
                                   _Xconst XRenderPictFormat *mask_format,
                                   int src_x,
                                   int src_y,
                                   int dst_x,
                                   int dst_y,
                                   _Xconst XGlyphElt16 *elts,
                                   int n_elts);
extern void XRenderCompositeText32(Display *dpy,
                                   int op,
                                   Picture src,
                                   Picture dst,
                                   _Xconst XRenderPictFormat *mask_format,
                                   int src_x,
                                   int src_y,
                                   int dst_x,
                                   int dst_y,
                                   _Xconst XGlyphElt32 *elts,
                                   int n_elts);

#endif /* _XRENDER_H_ */
