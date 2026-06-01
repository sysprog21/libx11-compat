#include <X11/extensions/Xrender.h>
#include "display.h"
#include "errors.h"

Bool XRenderQueryExtension(Display *dpy,
                           int *event_base_return,
                           int *error_base_return)
{
    (void) dpy;
    if (event_base_return)
        *event_base_return = 0;
    if (error_base_return)
        *error_base_return = 0;
    return False;
}

Status XRenderQueryVersion(Display *dpy,
                           int *major_version_return,
                           int *minor_version_return)
{
    (void) dpy;
    if (major_version_return)
        *major_version_return = 0;
    if (minor_version_return)
        *minor_version_return = 0;
    return 0;
}

XRenderPictFormat *XRenderFindStandardFormat(Display *dpy, int format)
{
    (void) dpy;
    (void) format;
    return NULL;
}

XRenderPictFormat *XRenderFindVisualFormat(Display *dpy, _Xconst Visual *visual)
{
    (void) dpy;
    (void) visual;
    return NULL;
}

Picture XRenderCreatePicture(Display *dpy,
                             Drawable drawable,
                             _Xconst XRenderPictFormat *format,
                             unsigned long valuemask,
                             _Xconst void *attributes)
{
    (void) format;
    (void) valuemask;
    (void) attributes;
    handleError(0, dpy, drawable, 0, BadDrawable, 0);
    return None;
}

void XRenderFreePicture(Display *dpy, Picture picture)
{
    (void) dpy;
    (void) picture;
}

void XRenderComposite(Display *dpy,
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
                      unsigned int height)
{
    (void) dpy;
    (void) op;
    (void) src;
    (void) mask;
    (void) dst;
    (void) src_x;
    (void) src_y;
    (void) mask_x;
    (void) mask_y;
    (void) dst_x;
    (void) dst_y;
    (void) width;
    (void) height;
}

void XRenderFillRectangle(Display *dpy,
                          int op,
                          Picture dst,
                          _Xconst XRenderColor *color,
                          int x,
                          int y,
                          unsigned int width,
                          unsigned int height)
{
    (void) dpy;
    (void) op;
    (void) dst;
    (void) color;
    (void) x;
    (void) y;
    (void) width;
    (void) height;
}

void XRenderFillRectangles(Display *dpy,
                           int op,
                           Picture dst,
                           _Xconst XRenderColor *color,
                           _Xconst XRectangle *rectangles,
                           int n_rects)
{
    (void) dpy;
    (void) op;
    (void) dst;
    (void) color;
    (void) rectangles;
    (void) n_rects;
}

GlyphSet XRenderCreateGlyphSet(Display *dpy, _Xconst XRenderPictFormat *format)
{
    (void) dpy;
    (void) format;
    return None;
}

void XRenderAddGlyphs(Display *dpy,
                      GlyphSet glyphset,
                      _Xconst Glyph *gids,
                      _Xconst void *glyphs,
                      int nglyphs,
                      _Xconst char *images,
                      int nbyte_images)
{
    (void) dpy;
    (void) glyphset;
    (void) gids;
    (void) glyphs;
    (void) nglyphs;
    (void) images;
    (void) nbyte_images;
}

void XRenderCompositeText8(Display *dpy,
                           int op,
                           Picture src,
                           Picture dst,
                           _Xconst XRenderPictFormat *mask_format,
                           int src_x,
                           int src_y,
                           int dst_x,
                           int dst_y,
                           _Xconst XGlyphElt8 *elts,
                           int n_elts)
{
    (void) dpy;
    (void) op;
    (void) src;
    (void) dst;
    (void) mask_format;
    (void) src_x;
    (void) src_y;
    (void) dst_x;
    (void) dst_y;
    (void) elts;
    (void) n_elts;
}

void XRenderCompositeText16(Display *dpy,
                            int op,
                            Picture src,
                            Picture dst,
                            _Xconst XRenderPictFormat *mask_format,
                            int src_x,
                            int src_y,
                            int dst_x,
                            int dst_y,
                            _Xconst XGlyphElt16 *elts,
                            int n_elts)
{
    (void) dpy;
    (void) op;
    (void) src;
    (void) dst;
    (void) mask_format;
    (void) src_x;
    (void) src_y;
    (void) dst_x;
    (void) dst_y;
    (void) elts;
    (void) n_elts;
}

void XRenderCompositeText32(Display *dpy,
                            int op,
                            Picture src,
                            Picture dst,
                            _Xconst XRenderPictFormat *mask_format,
                            int src_x,
                            int src_y,
                            int dst_x,
                            int dst_y,
                            _Xconst XGlyphElt32 *elts,
                            int n_elts)
{
    (void) dpy;
    (void) op;
    (void) src;
    (void) dst;
    (void) mask_format;
    (void) src_x;
    (void) src_y;
    (void) dst_x;
    (void) dst_y;
    (void) elts;
    (void) n_elts;
}
