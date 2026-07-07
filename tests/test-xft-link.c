#include <X11/Xft/Xft.h>
#include <X11/Xlib.h>
#define XUTIL_DEFINE_FUNCTIONS
#include <X11/Xutil.h>
#include <stdio.h>
#include <string.h>

/* XGetImage's 32-bit fast path memcpys SDL_PIXELFORMAT_RGBA8888 bytes straight
 * into XImage->data (see src/image.c). The Uint32 pixel value therefore carries
 * R in the high byte, G next, then B, then A - the SDL RGBA8888 mask layout -
 * regardless of host endianness. A correct Xft draw of an opaque black string
 * against a white pixmap should yield pixels whose R, G, B bytes are all low
 * and roughly equal; a channel-swap regression in blendPixel /
 * XftColorAllocValue routes the black through xColorToRgba8888 with the wrong
 * shifts and ends up storing blue (the smoking gun the multi-model review
 * flagged).
 */
static int has_neutral_dark_pixel(XImage *image)
{
    if (!image)
        return 0;
    unsigned long darkestPixel = 0xffffffffUL;
    unsigned darkestMax = 0xff;
    for (int y = 0; y < image->height; y++) {
        for (int x = 0; x < image->width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            /* XGetImage returns the compat's canonical XColor (0xAARRGGBB). */
            unsigned r = (unsigned) ((pixel >> 16) & 0xffu);
            unsigned g = (unsigned) ((pixel >> 8) & 0xffu);
            unsigned b = (unsigned) (pixel & 0xffu);
            unsigned maxC = r > g ? r : g;
            if (b > maxC)
                maxC = b;
            unsigned minC = r < g ? r : g;
            if (b < minC)
                minC = b;
            if (maxC < darkestMax) {
                darkestMax = maxC;
                darkestPixel = pixel;
            }
            if (maxC < 0x80 && maxC - minC <= 0x10)
                return 1;
        }
    }
    fprintf(stderr, "darkest pixel observed: 0x%08lx (max=%u)\n", darkestPixel,
            darkestMax);
    return 0;
}

static int has_dark_pixel_in_rect(XImage *image, int x0, int y0, int w, int h)
{
    if (!image)
        return 0;
    for (int y = y0; y < y0 + h && y < image->height; y++) {
        for (int x = x0; x < x0 + w && x < image->width; x++) {
            unsigned long pixel = XGetPixel(image, x, y);
            /* XGetImage returns the compat's canonical XColor (0xAARRGGBB). */
            unsigned r = (unsigned) ((pixel >> 16) & 0xffu);
            unsigned g = (unsigned) ((pixel >> 8) & 0xffu);
            unsigned b = (unsigned) (pixel & 0xffu);
            if (r < 0x80 && g < 0x80 && b < 0x80)
                return 1;
        }
    }
    return 0;
}

/* Drive the XftFontMatch -> caller-destroys-match -> XftFontClose sequence
 * Motif uses, and verify the match stays independent from later edits to the
 * source pattern.
 */
static int exercise_match_refcount(Display *display, int screen)
{
    FcPattern *pattern = XftNameParse("Sans-12");
    FcPattern *match = NULL;
    XftFont *font = NULL;
    int result = 0;

    if (!pattern) {
        fprintf(stderr, "XftNameParse failed\n");
        goto cleanup;
    }
    if (!FcPatternAddString(pattern, "compat-test",
                            (const FcChar8 *) "original")) {
        fprintf(stderr, "FcPatternAddString failed\n");
        goto cleanup;
    }
    FcResult matchResult = FcResultMatch;
    match = XftFontMatch(display, screen, pattern, &matchResult);
    if (!match || matchResult != FcResultMatch) {
        fprintf(stderr, "XftFontMatch failed (result=%d)\n", matchResult);
        goto cleanup;
    }
    if (match == pattern) {
        fprintf(stderr, "XftFontMatch returned aliased pattern\n");
        goto cleanup;
    }
    FcPatternDel(pattern, "compat-test");
    if (!FcPatternAddString(pattern, "compat-test",
                            (const FcChar8 *) "changed")) {
        fprintf(stderr, "FcPatternAddString changed value failed\n");
        goto cleanup;
    }
    FcChar8 *matchedValue = NULL;
    if (FcPatternGetString(match, "compat-test", 0, &matchedValue) !=
            FcResultMatch ||
        strcmp((const char *) matchedValue, "original")) {
        fprintf(stderr, "XftFontMatch result changed with source pattern\n");
        goto cleanup;
    }
    font = XftFontOpenPattern(display, match);
    if (!font) {
        fprintf(stderr, "XftFontOpenPattern(match) failed\n");
        goto cleanup;
    }
    /* Both the caller-side match reference and the caller-side pattern
     * reference are destroyed before the font is closed. If refcounting
     * regressed, XftFontClose would free already-freed memory.
     */
    FcPatternDestroy(match);
    match = NULL;
    FcPatternDestroy(pattern);
    pattern = NULL;
    XftFontClose(display, font);
    font = NULL;
    result = 1;

cleanup:
    if (font)
        XftFontClose(display, font);
    if (match)
        FcPatternDestroy(match);
    if (pattern)
        FcPatternDestroy(pattern);
    return result;
}

static int exercise_font_copy_refcount(Display *display, int screen)
{
    XftFont *font = XftFontOpenName(display, screen, "Sans-12");
    if (!font) {
        fprintf(stderr, "XftFontOpenName for copy test failed\n");
        return 0;
    }
    XftFont *copy = XftFontCopy(display, font);
    if (!copy) {
        fprintf(stderr, "XftFontCopy failed\n");
        XftFontClose(display, font);
        return 0;
    }
    XftFontClose(display, font);
    XftFontClose(display, copy);

    font = XftFontOpenName(display, screen, "Sans-12");
    if (!font) {
        fprintf(stderr, "XftFontOpenName for reverse copy test failed\n");
        return 0;
    }
    copy = XftFontCopy(display, font);
    if (!copy) {
        fprintf(stderr, "reverse XftFontCopy failed\n");
        XftFontClose(display, font);
        return 0;
    }
    XftFontClose(display, copy);
    XftFontClose(display, font);
    return 1;
}

static int exercise_pattern_prepend(void)
{
    FcPattern *pattern = FcPatternCreate();
    FcValue value;
    FcChar8 *family = NULL;
    int result = 0;

    if (!pattern) {
        fprintf(stderr, "FcPatternCreate failed\n");
        return 0;
    }
    value.type = FcTypeString;
    value.u.s = (const FcChar8 *) "first";
    if (!FcPatternAdd(pattern, FC_FAMILY, value, FcTrue)) {
        fprintf(stderr, "FcPatternAdd append failed\n");
        goto cleanup;
    }
    value.u.s = (const FcChar8 *) "override";
    if (!FcPatternAdd(pattern, FC_FAMILY, value, FcFalse)) {
        fprintf(stderr, "FcPatternAdd prepend failed\n");
        goto cleanup;
    }
    if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) != FcResultMatch ||
        strcmp((const char *) family, "override")) {
        fprintf(stderr, "FcPatternAdd prepend did not become value 0\n");
        goto cleanup;
    }
    if (FcPatternGetString(pattern, FC_FAMILY, 1, &family) != FcResultMatch ||
        strcmp((const char *) family, "first")) {
        fprintf(stderr, "FcPatternAdd prepend displaced old value\n");
        goto cleanup;
    }
    result = 1;

cleanup:
    FcPatternDestroy(pattern);
    return result;
}

/* Detect whether the host can render a CJK glyph at all, by asking the
 * charset-aware fallback for a font that covers just 0x4e00. Minimal CI images
 * and the bundled font set carry no CJK font, so the mixed-charset test below
 * only enforces CJK coverage where it is actually achievable.
 */
static int host_provides_cjk(Display *display)
{
    FcPattern *pattern = FcPatternCreate();
    FcCharSet *charset = FcCharSetCreate();
    XftFont *font = NULL;
    int provides = 0;

    if (!pattern || !charset)
        goto cleanup;
    if (!FcCharSetAddChar(charset, 0x4e00))
        goto cleanup;
    if (!FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *) "Sans") ||
        !FcPatternAddInteger(pattern, FC_SIZE, 12) ||
        !FcPatternAddCharSet(pattern, FC_CHARSET, charset))
        goto cleanup;
    font = XftFontOpenPattern(display, pattern);
    /* XftFontOpenPattern only takes ownership of the pattern on success; on
     * failure the caller still owns it, so clear the local handle only once the
     * font opened to avoid leaking the pattern down the cleanup path.
     */
    if (font)
        pattern = NULL;
    provides = font && XftCharExists(display, font, 0x4e00);

cleanup:
    if (font)
        XftFontClose(display, font);
    if (pattern)
        FcPatternDestroy(pattern);
    FcCharSetDestroy(charset);
    return provides;
}

static int exercise_mixed_charset(Display *display)
{
    FcPattern *pattern = FcPatternCreate();
    FcCharSet *charset = FcCharSetCreate();
    XftFont *font = NULL;
    int result = 0;

    if (!pattern || !charset) {
        fprintf(stderr, "mixed charset setup failed\n");
        goto cleanup;
    }
    if (!FcCharSetAddChar(charset, 'A') || !FcCharSetAddChar(charset, 0x4e00)) {
        fprintf(stderr, "mixed charset add failed\n");
        goto cleanup;
    }
    if (!FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *) "Sans") ||
        !FcPatternAddInteger(pattern, FC_SIZE, 12) ||
        !FcPatternAddCharSet(pattern, FC_CHARSET, charset)) {
        fprintf(stderr, "mixed charset pattern failed\n");
        goto cleanup;
    }
    font = XftFontOpenPattern(display, pattern);
    if (!font) {
        fprintf(stderr, "mixed charset font open failed\n");
        goto cleanup;
    }
    /* Pattern ownership transferred to the font only now that it opened. */
    pattern = NULL;
    if (!XftCharExists(display, font, 'A')) {
        fprintf(stderr, "mixed charset font missed ASCII glyph\n");
        goto cleanup;
    }
    /* When the host actually has a CJK font, the charset-aware fallback must
     * have selected one that covers the requested Han glyph. Where no CJK font
     * exists, XftFontOpenPattern still returns a usable base font and the glyph
     * resolves per draw call, so do not fail the link smoke test.
     */
    if (host_provides_cjk(display) && !XftCharExists(display, font, 0x4e00)) {
        fprintf(stderr,
                "mixed charset fallback dropped a renderable CJK glyph\n");
        goto cleanup;
    }
    result = 1;

cleanup:
    if (font)
        XftFontClose(display, font);
    if (pattern)
        FcPatternDestroy(pattern);
    FcCharSetDestroy(charset);
    return result;
}

static int exercise_font_sort(void)
{
    FcPattern *pattern = FcPatternCreate();
    FcFontSet *set = NULL;
    FcResult sortResult = FcResultNoMatch;
    int result = 0;

    if (!pattern)
        return 0;
    if (!FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *) "Monospace"))
        goto cleanup;
    set = FcFontSort(NULL, pattern, FcTrue, NULL, &sortResult);
    if (!set || sortResult != FcResultMatch || set->nfont < 1) {
        fprintf(stderr, "FcFontSort failed\n");
        goto cleanup;
    }
    result = 1;

cleanup:
    /* Real fontconfig's FcFontSetDestroy dereferences its argument, so guard
     * the NULL the FcPatternAddString / FcFontSort failure paths leave behind.
     */
    if (set)
        FcFontSetDestroy(set);
    FcPatternDestroy(pattern);
    return result;
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "XOpenDisplay failed\n");
        return 1;
    }
    int screen = DefaultScreen(display);
    if (!exercise_pattern_prepend()) {
        XCloseDisplay(display);
        return 1;
    }
    if (!exercise_mixed_charset(display)) {
        XCloseDisplay(display);
        return 1;
    }
    if (!exercise_font_sort()) {
        XCloseDisplay(display);
        return 1;
    }
    if (!exercise_match_refcount(display, screen)) {
        XCloseDisplay(display);
        return 1;
    }
    if (!exercise_font_copy_refcount(display, screen)) {
        XCloseDisplay(display);
        return 1;
    }
    Window root = RootWindow(display, screen);
    Pixmap pixmap = XCreatePixmap(display, root, 256, 32,
                                  (unsigned int) DefaultDepth(display, screen));
    if (pixmap == None) {
        fprintf(stderr, "XCreatePixmap failed\n");
        XCloseDisplay(display);
        return 1;
    }
    GC clearGc = XCreateGC(display, pixmap, 0, NULL);
    XSetForeground(display, clearGc, WhitePixel(display, screen));
    XFillRectangle(display, pixmap, clearGc, 0, 0, 256, 32);
    XFreeGC(display, clearGc);

    XftFont *font = XftFontOpenName(display, screen, "Sans-12");
    if (!font) {
        fprintf(stderr, "XftFontOpenName failed\n");
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }
    if (XftCharExists(display, font, 0x10ffff)) {
        fprintf(stderr, "XftCharExists accepted unsupported non-BMP glyph\n");
        XftFontClose(display, font);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }
    XftFont *varargFont =
        XftFontOpen(display, screen, XFT_FAMILY, XftTypeString, "Sans",
                    XFT_SIZE, XftTypeDouble, 12.0, XFT_WEIGHT, XftTypeInteger,
                    FC_WEIGHT_REGULAR, NULL);
    if (!varargFont) {
        fprintf(stderr, "XftFontOpen with XftType tags failed\n");
        XftFontClose(display, font);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }
    XGlyphInfo extents8;
    XGlyphInfo extentsUtf8;
    const FcChar8 latin1EAcute[] = {0xe9};
    const FcChar8 utf8EAcute[] = {0xc3, 0xa9};
    XftTextExtents8(display, varargFont, latin1EAcute, 1, &extents8);
    XftTextExtentsUtf8(display, varargFont, utf8EAcute, 2, &extentsUtf8);
    if (extents8.xOff != extentsUtf8.xOff ||
        extents8.width != extentsUtf8.width) {
        fprintf(stderr, "XftTextExtents8 did not decode Latin-1 input\n");
        XftFontClose(display, varargFont);
        XftFontClose(display, font);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }
    XRenderColor black = {0, 0, 0, 0xffff};
    XftColor color;
    if (!XftColorAllocValue(display, DefaultVisual(display, screen),
                            DefaultColormap(display, screen), &black, &color)) {
        fprintf(stderr, "XftColorAllocValue failed\n");
        XftFontClose(display, varargFont);
        XftFontClose(display, font);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }
    XftDraw *draw =
        XftDrawCreate(display, pixmap, DefaultVisual(display, screen),
                      DefaultColormap(display, screen));
    if (!draw) {
        fprintf(stderr, "XftDrawCreate failed\n");
        XftFontClose(display, varargFont);
        XftFontClose(display, font);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }
    const FcChar8 text[] = "Hello";
    XftDrawStringUtf8(draw, &color, font, 4, 20, text,
                      (int) strlen((const char *) text));
    XRectangle clip = {4, 0, 24, 32};
    if (!XftDrawSetClipRectangles(draw, 0, 0, &clip, 1)) {
        fprintf(stderr, "XftDrawSetClipRectangles failed\n");
        XftDrawDestroy(draw);
        XftColorFree(display, DefaultVisual(display, screen),
                     DefaultColormap(display, screen), &color);
        XftFontClose(display, varargFont);
        XftFontClose(display, font);
        XFreePixmap(display, pixmap);
        XCloseDisplay(display);
        return 1;
    }
    XftDrawStringUtf8(draw, &color, font, 80, 20, text,
                      (int) strlen((const char *) text));
    XImage *image =
        XGetImage(display, pixmap, 0, 0, 256, 32, AllPlanes, ZPixmap);
    int ok = has_neutral_dark_pixel(image) &&
             !has_dark_pixel_in_rect(image, 80, 0, 80, 32);
    if (image)
        XDestroyImage(image);
    XftDrawDestroy(draw);
    XftColorFree(display, DefaultVisual(display, screen),
                 DefaultColormap(display, screen), &color);
    XftFontClose(display, varargFont);
    XftFontClose(display, font);
    XFreePixmap(display, pixmap);
    XCloseDisplay(display);
    if (!ok) {
        fprintf(stderr,
                "Xft draw produced no neutral dark pixels - black text "
                "rendered in the wrong color channel\n");
        return 1;
    }
    return 0;
}
