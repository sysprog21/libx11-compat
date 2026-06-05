#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <ctype.h>
#include "colors.h"
#include "std-colors.h"
#include "errors.h"
#include "display.h"
#include "resource-types.h"
#include "util.h"

typedef struct {
    int visualClass;
    Bool defaultColormap;
} ColormapStruct;

Colormap GREY_SCALE_COLORMAP = None;
Colormap REAL_COLOR_COLORMAP = None;

static void setColorFromPixel(XColor *color, unsigned long pixel);

static Colormap createColormapResource(Display *display,
                                       int visualClass,
                                       Bool defaultColormap)
{
    XID colormap = ALLOC_XID();
    if (colormap == None) {
        if (display) {
            handleOutOfMemory(0, display, 0, 0);
        }
        return None;
    }
    ColormapStruct *colormapStruct = malloc(sizeof(ColormapStruct));
    if (!colormapStruct) {
        FREE_XID(colormap);
        if (display) {
            handleOutOfMemory(0, display, 0, 0);
        }
        return None;
    }
    colormapStruct->visualClass = visualClass;
    colormapStruct->defaultColormap = defaultColormap;
    SET_XID_TYPE(colormap, COLORMAP);
    SET_XID_VALUE(colormap, colormapStruct);
    return colormap;
}

Bool initColorStorage(void)
{
    GREY_SCALE_COLORMAP = createColormapResource(NULL, StaticGray, True);
    REAL_COLOR_COLORMAP = createColormapResource(NULL, TrueColor, True);
    return GREY_SCALE_COLORMAP != None && REAL_COLOR_COLORMAP != None;
}

void freeColorStorage(void)
{
    if (GREY_SCALE_COLORMAP != None) {
        free(GET_XID_VALUE(GREY_SCALE_COLORMAP));
        FREE_XID(GREY_SCALE_COLORMAP);
        GREY_SCALE_COLORMAP = None;
    }
    if (REAL_COLOR_COLORMAP != None) {
        free(GET_XID_VALUE(REAL_COLOR_COLORMAP));
        FREE_XID(REAL_COLOR_COLORMAP);
        REAL_COLOR_COLORMAP = None;
    }
}

SDL_Color uLongToColor(SDL_PixelFormat *pixelFormat, unsigned long color)
{
    SDL_Color res;
    SDL_GetRGBA(color, pixelFormat, &res.r, &res.g, &res.b, &res.a);
    return res;
}

SDL_Color uLongToColorFromVisual(Visual *visual, unsigned long color)
{
    SDL_Color res;
    res.r = (visual->red_mask & color) >> 24;
    res.g = (visual->green_mask & color) >> 16;
    res.b = (visual->blue_mask & color) >> 8;
    res.a =
        (~(visual->red_mask | visual->green_mask | visual->blue_mask)) & color;
    return res;
}

int XFreeColormap(Display *display, Colormap colormap)
{
    // https://tronche.com/gui/x/xlib/color/XFreeColormap.html
    SET_X_SERVER_REQUEST(display, X_FreeColormap);
    TYPE_CHECK(colormap, COLORMAP, display, 0);
    ColormapStruct *colormapStruct = GET_XID_VALUE(colormap);
    if (!colormapStruct->defaultColormap) {
        free(colormapStruct);
        FREE_XID(colormap);
    }
    return 1;
}

Colormap XCreateColormap(Display *display,
                         Window window,
                         Visual *visual,
                         int allocate)
{
    // https://tronche.com/gui/x/xlib/color/XCreateColormap.html
    SET_X_SERVER_REQUEST(display, X_CreateColormap);
    // depth is assumed to be sizeof(SDL_Color)
    int visualClass = visual->CLASS_ATTRIBUTE;
    switch (visualClass) {
    case StaticGray:
    case StaticColor:
    case TrueColor:
        if (allocate != AllocNone) {
            fprintf(stderr,
                    "Bad parameter: Got StaticGray, StaticColor or TrueColor "
                    "but allocate is not AllocNone in XCreateColormap!\n");
            handleError(0, display, None, 0, BadMatch, 0);
            return None;
        }
        break;
    case GrayScale:
    case PseudoColor:
    case DirectColor:
        break;
    default:
        LOG("Bad parameter: got an unknown visual class in XCreateColormap: "
            "%d\n",
            visualClass);
        handleError(0, display, None, 0, BadMatch, 0);
        return None;
    }
    return createColormapResource(display, visualClass, False);
}

int XQueryColors(Display *display,
                 Colormap colormap,
                 XColor *defs_in_out,
                 int ncolors)
{
    // https://tronche.com/gui/x/xlib/color/XQueryColors.html
    SET_X_SERVER_REQUEST(display, X_QueryColors);
    TYPE_CHECK(colormap, COLORMAP, display, 0);
    for (int i = 0; i < ncolors; ++i)
        setColorFromPixel(&defs_in_out[i], defs_in_out[i].pixel);
    return 1;
}

static void setColorFromPixel(XColor *color, unsigned long pixel)
{
    color->pixel = pixel;
    color->red = (unsigned short) (GET_RED_FROM_COLOR(pixel) * 257);
    color->green = (unsigned short) (GET_GREEN_FROM_COLOR(pixel) * 257);
    color->blue = (unsigned short) (GET_BLUE_FROM_COLOR(pixel) * 257);
    color->flags = DoRed | DoGreen | DoBlue;
}

static void setColorFromRgbaPixel(XColor *color, unsigned long pixel)
{
    /* std-colors.h stores generated rgb.txt values as RRGGBBAA. */
    unsigned long red = (pixel >> 24) & 0xFFul;
    unsigned long green = (pixel >> 16) & 0xFFul;
    unsigned long blue = (pixel >> 8) & 0xFFul;
    unsigned long alpha = pixel & 0xFFul;
    setColorFromPixel(color, (alpha << ALPHA_SHIFT) | (red << RED_SHIFT) |
                                 (green << GREEN_SHIFT) | (blue << BLUE_SHIFT));
}

static Bool colorNameMatches(const char *requested, const char *standard)
{
    while (*requested != '\0' && *standard != '\0') {
        if (*standard == ' ' && *requested != ' ') {
            standard++;
            continue;
        }
        if (tolower((unsigned char) *requested) != *standard) {
            return False;
        }
        requested++;
        standard++;
    }
    while (*standard == ' ') {
        standard++;
    }
    return *requested == '\0' && *standard == '\0';
}

static int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static Bool parseHexComponent(const char *text,
                              int digits,
                              unsigned short *value)
{
    unsigned int component = 0;
    for (int i = 0; i < digits; i++) {
        int nibble = hexValue(text[i]);
        if (nibble < 0) {
            return False;
        }
        component = (component << 4) | (unsigned int) nibble;
    }

    switch (digits) {
    case 1:
        *value = (unsigned short) (component << 12);
        return True;
    case 2:
        *value = (unsigned short) (component << 8);
        return True;
    case 3:
        *value = (unsigned short) (component << 4);
        return True;
    case 4:
        *value = (unsigned short) component;
        return True;
    default:
        return False;
    }
}

static Bool parseHexColor(const char *spec, XColor *color)
{
    size_t length = strlen(spec);
    if (spec[0] != '#' ||
        (length != 4 && length != 7 && length != 10 && length != 13)) {
        return False;
    }

    int digits = (int) ((length - 1) / 3);
    unsigned short red, green, blue;
    if (!parseHexComponent(spec + 1, digits, &red) ||
        !parseHexComponent(spec + 1 + digits, digits, &green) ||
        !parseHexComponent(spec + 1 + 2 * digits, digits, &blue)) {
        return False;
    }
    color->red = red;
    color->green = green;
    color->blue = blue;
    color->pixel = ((unsigned long) (red >> 8) << RED_SHIFT) |
                   ((unsigned long) (green >> 8) << GREEN_SHIFT) |
                   ((unsigned long) (blue >> 8) << BLUE_SHIFT) |
                   (0xFFul << ALPHA_SHIFT);
    color->flags = DoRed | DoGreen | DoBlue;
    return True;
}

Status XParseColor(Display *display,
                   Colormap colormap,
                   _Xconst char *spec,
                   XColor *exact_def_return)
{
    // https://tronche.com/gui/x/xlib/color/XParseColor.html
    SET_X_SERVER_REQUEST(display, X_LookupColor);
    (void) colormap;
    TYPE_CHECK(colormap, COLORMAP, display, 0);
    if (!spec || !exact_def_return) {
        return 0;
    }
    if (parseHexColor(spec, exact_def_return)) {
        return 1;
    }

    for (size_t i = 0; i < NUM_STANDARD_COLORS; i++) {
        if (colorNameMatches(spec, STANDARD_COLORS[i].name)) {
            setColorFromRgbaPixel(exact_def_return,
                                  STANDARD_COLORS[i].pixelValue);
            return 1;
        }
    }
    return 0;
}

Status XLookupColor(Display *display,
                    Colormap colormap,
                    _Xconst char *color_name,
                    XColor *exact_def_return,
                    XColor *screen_def_return)
{
    // https://tronche.com/gui/x/xlib/color/XLookupColor.html
    SET_X_SERVER_REQUEST(display, X_LookupColor);
    if (!XParseColor(display, colormap, color_name, exact_def_return)) {
        return 0;
    }
    *screen_def_return = *exact_def_return;
    return 1;
}

Status XAllocNamedColor(Display *display,
                        Colormap colormap,
                        _Xconst char *color_name,
                        XColor *screen_def_return,
                        XColor *exact_def_return)
{
    // https://tronche.com/gui/x/xlib/color/XAllocNamedColor.html
    SET_X_SERVER_REQUEST(display, X_AllocNamedColor);
    return XLookupColor(display, colormap, color_name, screen_def_return,
                        exact_def_return);
}

int XFreeColors(Display *display,
                Colormap colormap,
                unsigned long pixels[],
                int npixels,
                unsigned long planes)
{
    // https://tronche.com/gui/x/xlib/color/XFreeColors.html
    SET_X_SERVER_REQUEST(display, X_FreeColors);
    TYPE_CHECK(colormap, COLORMAP, display, 0);
    /* Direct-color visual: pixels are not allocated entries, so nothing to
     * release. */
    (void) pixels;
    (void) npixels;
    (void) planes;
    return 1;
}

Status XAllocColor(Display *display, Colormap colormap, XColor *screen_in_out)
{
    // https://tronche.com/gui/x/xlib/color/XAllocColor.html
    SET_X_SERVER_REQUEST(display, X_AllocColor);
    TYPE_CHECK(colormap, COLORMAP, display, 0);
    if (!screen_in_out)
        return 0;
    /* Spec: on success, .pixel is filled with the allocated index and the
     * .red/.green/.blue fields with the actually rendered values (which may
     * differ from the requested ones on indexed visuals). Our visual is
     * direct-color, so the requested RGB round-trips exactly. Pack into the
     * shim's ARGB pixel format (A=24, R=16, G=8, B=0; see src/colors.h). */
    unsigned long r = (screen_in_out->red >> 8) & 0xFFul;
    unsigned long g = (screen_in_out->green >> 8) & 0xFFul;
    unsigned long b = (screen_in_out->blue >> 8) & 0xFFul;
    screen_in_out->pixel = (r << RED_SHIFT) | (g << GREEN_SHIFT) |
                           (b << BLUE_SHIFT) | (0xFFul << ALPHA_SHIFT);
    return 1;
}
