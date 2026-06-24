#ifndef X11_XFT_XFT_H
#define X11_XFT_XFT_H

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>
#include <fontconfig/fontconfig.h>

typedef unsigned int FT_UInt;
/* FcEndian is a fontconfig type. When the host fontconfig header is
 * picked up alongside this shim (Xfig pkg-configs the host fontconfig
 * even when our libXft-compat resolves the Fc* / Xft* surface) the
 * upstream definition wins. Gate ours on the upstream header guard
 * macro so the duplicate enum / typedef does not collide.
 */
#ifndef _FCFREETYPE_H_
#ifndef _FONTCONFIG_H_
typedef enum { FcEndianBig, FcEndianLittle } FcEndian;
#endif
#endif

typedef FcChar8 XftChar8;
typedef FcChar16 XftChar16;
typedef FcChar32 XftChar32;
typedef FcResult XftResult;

#define XftTypeVoid FcTypeVoid
#define XftTypeInteger FcTypeInteger
#define XftTypeDouble FcTypeDouble
#define XftTypeString FcTypeString
#define XftTypeBool FcTypeBool
#define XftTypeMatrix FcTypeMatrix
#define XftTypeCharSet FcTypeCharSet
#define XftTypeFTFace FcTypeFTFace
#define XftTypeLangSet FcTypeLangSet

#define XFT_MAJOR 2
#define XFT_MINOR 3
#define XFT_REVISION 99
#define XFT_VERSION ((XFT_MAJOR * 10000) + (XFT_MINOR * 100) + XFT_REVISION)
#define XftVersion XFT_VERSION

/* Real libXft mostly re-exports fontconfig property names but also
 * defines a handful of its own. Aliasing keeps client code that uses
 * either spelling source-compatible against this shim.
 */
#define XFT_FAMILY FC_FAMILY
#define XFT_STYLE FC_STYLE
#define XFT_SLANT FC_SLANT
#define XFT_WEIGHT FC_WEIGHT
#define XFT_SIZE FC_SIZE
#define XFT_PIXEL_SIZE FC_PIXEL_SIZE
#define XFT_FILE FC_FILE
#define XFT_INDEX FC_INDEX
#define XFT_FOUNDRY FC_FOUNDRY
#define XFT_ANTIALIAS FC_ANTIALIAS
#define XFT_HINTING FC_HINTING
#define XFT_HINT_STYLE FC_HINT_STYLE
#define XFT_AUTOHINT FC_AUTOHINT
#define XFT_OUTLINE FC_OUTLINE
#define XFT_SCALABLE FC_SCALABLE
#define XFT_RGBA FC_RGBA
#define XFT_MINSPACE FC_MINSPACE
#define XFT_DPI FC_DPI
#define XFT_RENDER FC_RENDER
#define XFT_ENCODING "encoding"
#define XFT_CHARSET FC_CHARSET
#define XFT_CHAR_WIDTH "charwidth"
#define XFT_CHAR_HEIGHT "charheight"
#define XFT_MATRIX "matrix"
#define XFT_CORE "core"

typedef struct _XftFont {
    int ascent;
    int descent;
    int height;
    int max_advance_width;
    FcCharSet *charset;
    FcPattern *pattern;
} XftFont;

typedef struct _XftDraw XftDraw;

typedef struct _XftColor {
    unsigned long pixel;
    XRenderColor color;
} XftColor;

typedef struct _XftGlyphSpec {
    FT_UInt glyph;
    short x;
    short y;
} XftGlyphSpec;

Bool XftInit(const char *config);
int XftGetVersion(void);
Bool XftDefaultHasRender(Display *dpy);
void XftDefaultSubstitute(Display *dpy, int screen, FcPattern *pattern);
FcPattern *XftNameParse(const char *name);
FcPattern *XftFontMatch(Display *dpy,
                        int screen,
                        FcPattern *pattern,
                        FcResult *result);
XftFont *XftFontOpen(Display *dpy, int screen, ...);
XftFont *XftFontOpenName(Display *dpy, int screen, const char *name);
XftFont *XftFontOpenPattern(Display *dpy, FcPattern *pattern);
XftFont *XftFontOpenXlfd(Display *dpy, int screen, const char *xlfd);
void XftFontClose(Display *dpy, XftFont *font);
XftDraw *XftDrawCreate(Display *dpy,
                       Drawable drawable,
                       Visual *visual,
                       Colormap colormap);
void XftDrawDestroy(XftDraw *draw);
void XftDrawChange(XftDraw *draw, Drawable drawable);
Bool XftDrawSetClipRectangles(XftDraw *draw,
                              int x_origin,
                              int y_origin,
                              const XRectangle *rects,
                              int n);
Display *XftDrawDisplay(XftDraw *draw);
Drawable XftDrawDrawable(XftDraw *draw);
Colormap XftDrawColormap(XftDraw *draw);
Visual *XftDrawVisual(XftDraw *draw);
Bool XftColorAllocValue(Display *dpy,
                        Visual *visual,
                        Colormap cmap,
                        const XRenderColor *color,
                        XftColor *result);
Bool XftColorAllocName(Display *dpy,
                       Visual *visual,
                       Colormap cmap,
                       const char *name,
                       XftColor *result);
void XftColorFree(Display *dpy, Visual *visual, Colormap cmap, XftColor *color);
void XftDrawString8(XftDraw *draw,
                    const XftColor *color,
                    XftFont *font,
                    int x,
                    int y,
                    const FcChar8 *string,
                    int len);
void XftDrawString16(XftDraw *draw,
                     const XftColor *color,
                     XftFont *font,
                     int x,
                     int y,
                     const FcChar16 *string,
                     int len);
void XftDrawString32(XftDraw *draw,
                     const XftColor *color,
                     XftFont *font,
                     int x,
                     int y,
                     const FcChar32 *string,
                     int len);
void XftDrawStringUtf8(XftDraw *draw,
                       const XftColor *color,
                       XftFont *font,
                       int x,
                       int y,
                       const FcChar8 *string,
                       int len);
void XftDrawStringUtf16(XftDraw *draw,
                        const XftColor *color,
                        XftFont *font,
                        int x,
                        int y,
                        const FcChar8 *string,
                        FcEndian endian,
                        int len);
void XftTextExtents8(Display *dpy,
                     XftFont *font,
                     const FcChar8 *string,
                     int len,
                     XGlyphInfo *extents);
void XftTextExtents16(Display *dpy,
                      XftFont *font,
                      const FcChar16 *string,
                      int len,
                      XGlyphInfo *extents);
void XftTextExtents32(Display *dpy,
                      XftFont *font,
                      const FcChar32 *string,
                      int len,
                      XGlyphInfo *extents);
void XftTextExtentsUtf8(Display *dpy,
                        XftFont *font,
                        const FcChar8 *string,
                        int len,
                        XGlyphInfo *extents);
void XftTextExtentsUtf16(Display *dpy,
                         XftFont *font,
                         const FcChar8 *string,
                         FcEndian endian,
                         int len,
                         XGlyphInfo *extents);

FcBool XftUtf8Len(const FcChar8 *string, int len, int *nchar, int *wchar);
int XftUtf8ToUcs4(const FcChar8 *src, FcChar32 *dst, int len);

XftFont *XftFontCopy(Display *dpy, XftFont *font);
FcBool XftCharExists(Display *dpy, XftFont *font, FcChar32 ucs4);
FT_UInt XftCharIndex(Display *dpy, XftFont *font, FcChar32 ucs4);
void XftGlyphExtents(Display *dpy,
                     XftFont *font,
                     const FT_UInt *glyphs,
                     int nglyphs,
                     XGlyphInfo *extents);
void XftDrawGlyphs(XftDraw *draw,
                   const XftColor *color,
                   XftFont *font,
                   int x,
                   int y,
                   const FT_UInt *glyphs,
                   int nglyphs);
void XftDrawRect(XftDraw *draw,
                 const XftColor *color,
                 int x,
                 int y,
                 unsigned int width,
                 unsigned int height);

/* Legacy Xft 1.x pattern surface. Real libXft typedefs XftPattern /
 * XftMatrix to FcPattern / FcMatrix and aliases each XftPattern*
 * mutator to the matching FcPattern* mutator; Xfig and a couple of
 * older Athena toolkits use the prefixed names. Keeping these as
 * macros (not separate symbols) keeps the link contract focused on
 * the Fc* surface our libXft-compat actually exports.
 */
typedef FcPattern XftPattern;
typedef FcMatrix XftMatrix;
#define XftPatternCreate FcPatternCreate
#define XftPatternDestroy FcPatternDestroy
#define XftPatternDuplicate FcPatternDuplicate
#define XftPatternReference FcPatternReference
#define XftPatternDel FcPatternDel
#define XftPatternRemove FcPatternRemove
#define XftPatternAddInteger FcPatternAddInteger
#define XftPatternAddDouble FcPatternAddDouble
#define XftPatternAddString FcPatternAddString
#define XftPatternAddBool FcPatternAddBool
#define XftPatternAddMatrix FcPatternAddMatrix
#define XftPatternGet FcPatternGet
#define XftPatternGetInteger FcPatternGetInteger
#define XftPatternGetDouble FcPatternGetDouble
#define XftPatternGetString FcPatternGetString
#define XftPatternGetBool FcPatternGetBool

#endif /* X11_XFT_XFT_H */
