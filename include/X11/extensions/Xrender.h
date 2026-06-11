#ifndef _XRENDER_H_
#define _XRENDER_H_

#include <X11/Xlib.h>

typedef unsigned long Picture;
typedef unsigned long GlyphSet;
typedef unsigned long Glyph;
typedef unsigned long PictFormat;
typedef int XFixed;

/* XFixed is 16.16 fixed-point; representable range is approximately
 * [-32768, +32768). A bare cast of an out-of-range double to int is
 * undefined in C, which upstream Xrender's macro form invites. Clamp
 * to the representable range so misbehaving clients cannot trigger UB
 * inside the compat layer.
 */
static inline XFixed _xCompatDoubleToFixed(double f)
{
    double scaled = f * 65536.0;
    /* NaN compares false to every numeric, so the clamp below would let
     * it fall through to a (XFixed) cast, which is also UB.
     */
    if (scaled != scaled)
        return 0;
    if (scaled >= 2147483647.0)
        return (XFixed) 0x7FFFFFFF;
    if (scaled <= -2147483648.0)
        return (XFixed) (-0x7FFFFFFF - 1);
    return (XFixed) scaled;
}
#define XDoubleToFixed(f) _xCompatDoubleToFixed((double) (f))

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
    XFixed matrix[3][3];
} XTransform;

typedef struct {
    int repeat;
    Picture alpha_map;
    int alpha_x_origin;
    int alpha_y_origin;
    int clip_x_origin;
    int clip_y_origin;
    Pixmap clip_mask;
    Bool graphics_exposures;
    int subwindow_mode;
    int poly_edge;
    int poly_mode;
    Atom dither;
    Bool component_alpha;
} XRenderPictureAttributes;

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
#define PictOpInReverse 6
#define PictOpOut 7
#define PictOpOutReverse 8
#define PictOpAtop 9
#define PictOpAtopReverse 10
#define PictOpXor 11
#define PictOpAdd 12
#define PictOpSaturate 13

#define PictStandardARGB32 0
#define PictStandardRGB24 1
#define PictStandardA8 2
#define PictStandardA4 3
#define PictStandardA1 4

#define CPRepeat (1L << 0)
#define CPAlphaMap (1L << 1)
#define CPAlphaXOrigin (1L << 2)
#define CPAlphaYOrigin (1L << 3)
#define CPClipXOrigin (1L << 4)
#define CPClipYOrigin (1L << 5)
#define CPClipMask (1L << 6)
#define CPGraphicsExposure (1L << 7)
#define CPSubwindowMode (1L << 8)
#define CPPolyEdge (1L << 9)
#define CPPolyMode (1L << 10)
#define CPDither (1L << 11)
#define CPComponentAlpha (1L << 12)

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
extern void XRenderSetPictureTransform(Display *dpy,
                                       Picture picture,
                                       XTransform *transform);
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
