#include <X11/Xft/Xft.h>
#include <X11/Xutil.h>
#include <SDL2/SDL_ttf.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "colors.h"
#include "font.h"
#include "util.h"

#ifndef FC_MATCH_PATTERN
#define FC_MATCH_PATTERN 0
#endif

typedef struct {
    char *object;
    FcType type;
    union {
        char *s;
        int i;
        double d;
        FcBool b;
    } value;
} FcPatternEntry;

struct _FcPattern {
    FcPatternEntry *entries;
    int length;
    int capacity;
    /* Real fontconfig reference-counts patterns; many Xft clients (Motif, older
     * GTK) destroy the pattern they pass to XftFontOpenPattern as soon as the
     * call returns and rely on the font's internal pattern surviving via the
     * refcount. Track our own refcount so destroying a caller-side reference
     * does not crash the font.
     */
    int refcount;
};

struct _XftDraw {
    Display *display;
    Drawable drawable;
    Visual *visual;
    Colormap colormap;
};

typedef struct {
    XftFont public;
    TTF_Font *ttf;
    FcBool patternReferenced;
} CompatXftFont;

static char *xftStrdup(const char *s)
{
    if (!s)
        return NULL;
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy)
        memcpy(copy, s, len);
    return copy;
}

static FcPatternEntry *findPatternEntry(const FcPattern *pattern,
                                        const char *object,
                                        int n)
{
    if (!pattern || !object || n < 0)
        return NULL;
    int seen = 0;
    for (int i = 0; i < pattern->length; i++) {
        if (strcmp(pattern->entries[i].object, object))
            continue;
        if (seen == n)
            return &pattern->entries[i];
        seen++;
    }
    return NULL;
}

static FcBool reservePatternEntry(FcPattern *pattern)
{
    if (pattern->length < pattern->capacity)
        return FcTrue;
    int newCapacity = pattern->capacity ? pattern->capacity * 2 : 8;
    if (newCapacity <= pattern->capacity)
        return FcFalse;
    /* Guard newCapacity * sizeof(FcPatternEntry) against size_t wrap before
     * realloc - relevant on 32-bit hosts where a very large caller-built
     * pattern could overflow the multiplication.
     */
    if ((size_t) newCapacity > SIZE_MAX / sizeof(FcPatternEntry))
        return FcFalse;
    FcPatternEntry *entries = realloc(
        pattern->entries, sizeof(FcPatternEntry) * (size_t) newCapacity);
    if (!entries)
        return FcFalse;
    pattern->entries = entries;
    pattern->capacity = newCapacity;
    return FcTrue;
}

static FcBool addPatternEntry(FcPattern *pattern,
                              const char *object,
                              FcType type,
                              const void *value)
{
    if (!pattern || !object || !reservePatternEntry(pattern))
        return FcFalse;
    FcPatternEntry *entry = &pattern->entries[pattern->length];
    memset(entry, 0, sizeof(*entry));
    entry->object = xftStrdup(object);
    if (!entry->object)
        return FcFalse;
    entry->type = type;
    switch (type) {
    case FcTypeString:
        entry->value.s = xftStrdup((const char *) value);
        if (!entry->value.s) {
            free(entry->object);
            return FcFalse;
        }
        break;
    case FcTypeInteger:
        entry->value.i = *(const int *) value;
        break;
    case FcTypeDouble:
        entry->value.d = *(const double *) value;
        break;
    case FcTypeBool:
        entry->value.b = *(const FcBool *) value;
        break;
    default:
        free(entry->object);
        return FcFalse;
    }
    pattern->length++;
    return FcTrue;
}

FcPattern *FcPatternCreate(void)
{
    FcPattern *pattern = calloc(1, sizeof(FcPattern));
    if (pattern)
        pattern->refcount = 1;
    return pattern;
}

void FcPatternReference(FcPattern *pattern)
{
    if (pattern && pattern->refcount > 0)
        pattern->refcount++;
}

void FcPatternDestroy(FcPattern *pattern)
{
    if (!pattern)
        return;
    if (pattern->refcount > 1) {
        pattern->refcount--;
        return;
    }
    for (int i = 0; i < pattern->length; i++) {
        free(pattern->entries[i].object);
        if (pattern->entries[i].type == FcTypeString)
            free(pattern->entries[i].value.s);
    }
    free(pattern->entries);
    free(pattern);
}

FcBool FcPatternDel(FcPattern *pattern, const char *object)
{
    if (!pattern || !object)
        return FcFalse;
    FcBool found = FcFalse;
    int dst = 0;
    for (int src = 0; src < pattern->length; src++) {
        if (!strcmp(pattern->entries[src].object, object)) {
            free(pattern->entries[src].object);
            if (pattern->entries[src].type == FcTypeString)
                free(pattern->entries[src].value.s);
            found = FcTrue;
            continue;
        }
        if (dst != src)
            pattern->entries[dst] = pattern->entries[src];
        dst++;
    }
    pattern->length = dst;
    return found;
}

/* Encode a single codepoint as UTF-8.
 *
 * Returns the number of bytes written into dest (1..4) or 0 if the codepoint is
 * out of range. Mirrors fontconfig's FcUcs4ToUtf8 contract; xfig's w_canvas.c
 * uses this to encode the in-progress IM commit string.
 */
int FcUcs4ToUtf8(FcChar32 ucs4, FcChar8 dest[FC_UTF8_MAX_LEN])
{
    if (!dest)
        return 0;
    if (ucs4 < 0x80) {
        dest[0] = (FcChar8) ucs4;
        return 1;
    }
    if (ucs4 < 0x800) {
        dest[0] = (FcChar8) (0xc0 | (ucs4 >> 6));
        dest[1] = (FcChar8) (0x80 | (ucs4 & 0x3f));
        return 2;
    }
    if (ucs4 < 0x10000) {
        dest[0] = (FcChar8) (0xe0 | (ucs4 >> 12));
        dest[1] = (FcChar8) (0x80 | ((ucs4 >> 6) & 0x3f));
        dest[2] = (FcChar8) (0x80 | (ucs4 & 0x3f));
        return 3;
    }
    if (ucs4 < 0x110000) {
        dest[0] = (FcChar8) (0xf0 | (ucs4 >> 18));
        dest[1] = (FcChar8) (0x80 | ((ucs4 >> 12) & 0x3f));
        dest[2] = (FcChar8) (0x80 | ((ucs4 >> 6) & 0x3f));
        dest[3] = (FcChar8) (0x80 | (ucs4 & 0x3f));
        return 4;
    }
    return 0;
}

/* Drop only the n-th value bound to "object". FcPatternDel wipes them all;
 * FcPatternRemove is the surgical version Xfig uses to clear a single antialias
 * / hintstyle / rgba slot before the font configures its final substitution
 * chain.
 */
FcBool FcPatternRemove(FcPattern *pattern, const char *object, int n)
{
    if (!pattern || !object || n < 0)
        return FcFalse;
    int seen = 0;
    for (int i = 0; i < pattern->length; i++) {
        if (strcmp(pattern->entries[i].object, object))
            continue;
        if (seen == n) {
            free(pattern->entries[i].object);
            if (pattern->entries[i].type == FcTypeString)
                free(pattern->entries[i].value.s);
            for (int j = i; j + 1 < pattern->length; j++)
                pattern->entries[j] = pattern->entries[j + 1];
            pattern->length--;
            return FcTrue;
        }
        seen++;
    }
    return FcFalse;
}

FcResult FcPatternGet(const FcPattern *pattern,
                      const char *object,
                      int n,
                      FcValue *value)
{
    FcPatternEntry *entry = findPatternEntry(pattern, object, n);
    if (!entry)
        return FcResultNoMatch;
    if (!value)
        return FcResultMatch;
    value->type = entry->type;
    switch (entry->type) {
    case FcTypeString:
        value->u.s = (const FcChar8 *) entry->value.s;
        break;
    case FcTypeInteger:
        value->u.i = entry->value.i;
        break;
    case FcTypeDouble:
        value->u.d = entry->value.d;
        break;
    case FcTypeBool:
        value->u.b = entry->value.b;
        break;
    default:
        return FcResultTypeMismatch;
    }
    return FcResultMatch;
}

FcBool FcPatternAddString(FcPattern *pattern,
                          const char *object,
                          const FcChar8 *s)
{
    return addPatternEntry(pattern, object, FcTypeString,
                           s ? s : (FcChar8 *) "");
}

FcBool FcPatternAddInteger(FcPattern *pattern, const char *object, int i)
{
    return addPatternEntry(pattern, object, FcTypeInteger, &i);
}

FcBool FcPatternAddDouble(FcPattern *pattern, const char *object, double d)
{
    return addPatternEntry(pattern, object, FcTypeDouble, &d);
}

FcBool FcPatternAddBool(FcPattern *pattern, const char *object, FcBool b)
{
    return addPatternEntry(pattern, object, FcTypeBool, &b);
}

FcBool FcPatternAddMatrix(FcPattern *pattern,
                          const char *object,
                          const FcMatrix *matrix)
{
    /* Stash matrices as a raw byte blob behind the FcTypeString slot: Xfig only
     * uses XftPatternAddMatrix to forward a rotation / shear hint into the font
     * path and then forgets about it; the compat layer rasterizes via SDL_ttf
     * which has no transform surface, so the value never needs to round-trip
     * cleanly through FcPatternGet. Reject NULL matrices loudly so a miswired
     * caller does not stash an uninitialized scratch entry.
     */
    if (!pattern || !object || !matrix)
        return FcFalse;

    char buf[sizeof(FcMatrix) * 2 + 1];
    static const char hex[] = "0123456789abcdef";
    const unsigned char *src = (const unsigned char *) matrix;
    for (size_t i = 0; i < sizeof(FcMatrix); i++) {
        buf[i * 2] = hex[(src[i] >> 4) & 0x0f];
        buf[i * 2 + 1] = hex[src[i] & 0x0f];
    }
    buf[sizeof(FcMatrix) * 2] = '\0';
    return addPatternEntry(pattern, object, FcTypeString, buf);
}

/* Deep copy a pattern. Each entry's heap-owned string is duplicated so
 * destroying the original does not free the copy's payload.
 *
 * Returns a fresh pattern with refcount=1.
 */
FcPattern *FcPatternDuplicate(const FcPattern *src)
{
    if (!src)
        return NULL;
    FcPattern *dst = FcPatternCreate();
    if (!dst)
        return NULL;
    for (int i = 0; i < src->length; i++) {
        const FcPatternEntry *e = &src->entries[i];
        FcBool ok = FcFalse;
        switch (e->type) {
        case FcTypeString:
            ok = FcPatternAddString(dst, e->object,
                                    (const FcChar8 *) e->value.s);
            break;
        case FcTypeInteger:
            ok = FcPatternAddInteger(dst, e->object, e->value.i);
            break;
        case FcTypeDouble:
            ok = FcPatternAddDouble(dst, e->object, e->value.d);
            break;
        case FcTypeBool:
            ok = FcPatternAddBool(dst, e->object, e->value.b);
            break;
        default:
            ok = FcFalse;
            break;
        }
        if (!ok) {
            FcPatternDestroy(dst);
            return NULL;
        }
    }
    return dst;
}

FcResult FcPatternGetString(const FcPattern *pattern,
                            const char *object,
                            int n,
                            FcChar8 **s)
{
    FcPatternEntry *entry = findPatternEntry(pattern, object, n);
    if (!entry)
        return FcResultNoMatch;
    if (entry->type != FcTypeString)
        return FcResultTypeMismatch;
    if (s)
        *s = (FcChar8 *) entry->value.s;
    return FcResultMatch;
}

FcResult FcPatternGetInteger(const FcPattern *pattern,
                             const char *object,
                             int n,
                             int *i)
{
    FcPatternEntry *entry = findPatternEntry(pattern, object, n);
    if (!entry)
        return FcResultNoMatch;
    if (entry->type != FcTypeInteger)
        return FcResultTypeMismatch;
    if (i)
        *i = entry->value.i;
    return FcResultMatch;
}

FcResult FcPatternGetDouble(const FcPattern *pattern,
                            const char *object,
                            int n,
                            double *d)
{
    FcPatternEntry *entry = findPatternEntry(pattern, object, n);
    if (!entry)
        return FcResultNoMatch;
    if (entry->type == FcTypeInteger) {
        if (d)
            *d = (double) entry->value.i;
        return FcResultMatch;
    }
    if (entry->type != FcTypeDouble)
        return FcResultTypeMismatch;
    if (d)
        *d = entry->value.d;
    return FcResultMatch;
}

FcResult FcPatternGetBool(const FcPattern *pattern,
                          const char *object,
                          int n,
                          FcBool *b)
{
    FcPatternEntry *entry = findPatternEntry(pattern, object, n);
    if (!entry)
        return FcResultNoMatch;
    if (entry->type != FcTypeBool)
        return FcResultTypeMismatch;
    if (b)
        *b = entry->value.b;
    return FcResultMatch;
}

static int parseTrailingSize(const char *name)
{
    if (!name)
        return 12;
    const char *end = name + strlen(name);
    const char *p = end;
    while (p > name && isdigit((unsigned char) p[-1]))
        p--;
    if (p == end)
        return 12;
    long size = strtol(p, NULL, 10);
    if (size < 1 || size > 256)
        return 12;
    return (int) size;
}

FcPattern *FcNameParse(const FcChar8 *name)
{
    FcPattern *pattern = FcPatternCreate();
    if (!pattern)
        return NULL;
    const char *text = name ? (const char *) name : "Sans-12";
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *) text);
    FcPatternAddInteger(pattern, FC_SIZE, parseTrailingSize(text));
    return pattern;
}

FcBool FcConfigSubstitute(FcConfig *config, FcPattern *pattern, int kind)
{
    (void) config;
    (void) pattern;
    (void) kind;
    return FcTrue;
}

void FcDefaultSubstitute(FcPattern *pattern)
{
    if (!pattern)
        return;
    if (FcPatternGetInteger(pattern, FC_SIZE, 0, NULL) == FcResultNoMatch)
        FcPatternAddInteger(pattern, FC_SIZE, 12);
}

static int patternSize(FcPattern *pattern)
{
    int i = 0;
    double d = 0;
    if (FcPatternGetInteger(pattern, FC_PIXEL_SIZE, 0, &i) == FcResultMatch ||
        FcPatternGetInteger(pattern, FC_SIZE, 0, &i) == FcResultMatch) {
        if (i > 0 && i <= 256)
            return i;
    }
    if (FcPatternGetDouble(pattern, FC_SIZE, 0, &d) == FcResultMatch) {
        if (d > 0 && d <= 256)
            return (int) (d + 0.5);
    }
    return 12;
}

static const char *patternFile(FcPattern *pattern)
{
    FcChar8 *file = NULL;
    if (FcPatternGetString(pattern, FC_FILE, 0, &file) == FcResultMatch)
        return (const char *) file;
    return NULL;
}

static const char *patternFamily(FcPattern *pattern)
{
    FcChar8 *family = NULL;
    if (FcPatternGetString(pattern, FC_FAMILY, 0, &family) == FcResultMatch)
        return (const char *) family;
    return NULL;
}

static TTF_Font *openFontFromPattern(FcPattern *pattern)
{
    int size = patternSize(pattern);
    /* FC_FILE wins because the caller named an exact file. Anything else routes
     * through the shared font-family fallback chain in src/font.c so xft and
     * the core XLoadQueryFont path agree on which TTF backs "helvetica" /
     * "sans" / "times" / monospace.
     */
    const char *file = patternFile(pattern);
    if (file) {
        TTF_Font *font = TTF_OpenFont(file, size);
        if (font)
            return font;
    }
    return compatFontOpenFamilyFallback(patternFamily(pattern), size);
}

Bool XftInit(const char *config)
{
    (void) config;
    return TTF_WasInit() || TTF_Init() == 0;
}

int XftGetVersion(void)
{
    return 20399;
}

Bool XftDefaultHasRender(Display *dpy)
{
    (void) dpy;
    return True;
}

void XftDefaultSubstitute(Display *dpy, int screen, FcPattern *pattern)
{
    (void) dpy;
    (void) screen;
    FcDefaultSubstitute(pattern);
}

FcPattern *XftNameParse(const char *name)
{
    return FcNameParse((const FcChar8 *) name);
}

FcPattern *XftFontMatch(Display *dpy,
                        int screen,
                        FcPattern *pattern,
                        FcResult *result)
{
    (void) dpy;
    (void) screen;
    if (!pattern) {
        if (result)
            *result = FcResultNoMatch;
        return NULL;
    }
    /* Real Xft returns a fresh pattern owned by the caller. Keep the match
     * independent so later edits to the original pattern cannot change it.
     */
    FcPattern *match = FcPatternDuplicate(pattern);
    if (!match) {
        if (result)
            *result = FcResultOutOfMemory;
        return NULL;
    }
    if (result)
        *result = FcResultMatch;
    return match;
}

XftFont *XftFontOpenPattern(Display *dpy, FcPattern *pattern)
{
    (void) dpy;
    if (!XftInit(NULL) || !pattern)
        return NULL;
    TTF_Font *ttf = openFontFromPattern(pattern);
    if (!ttf)
        return NULL;
    CompatXftFont *font = calloc(1, sizeof(*font));
    if (!font) {
        TTF_CloseFont(ttf);
        return NULL;
    }
    font->ttf = ttf;
    font->public.ascent = TTF_FontAscent(ttf);
    font->public.descent = -TTF_FontDescent(ttf);
    font->public.height = font->public.ascent + font->public.descent;
    font->public.max_advance_width = 0;
    TTF_GlyphMetrics(ttf, 'W', NULL, NULL, NULL, NULL,
                     &font->public.max_advance_width);
    /* Take a protective reference for clients that destroy the pattern after
     * this call. XftFontClose releases this extra reference as well as the
     * ownership reference transferred to XftFontOpenPattern.
     */
    FcPatternReference(pattern);
    font->patternReferenced = FcTrue;
    font->public.pattern = pattern;
    return &font->public;
}

XftFont *XftFontOpenName(Display *dpy, int screen, const char *name)
{
    (void) screen;
    FcPattern *pattern = FcNameParse((const FcChar8 *) name);
    if (!pattern)
        return NULL;
    XftFont *font = XftFontOpenPattern(dpy, pattern);
    FcPatternDestroy(pattern);
    return font;
}

XftFont *XftFontOpenXlfd(Display *dpy, int screen, const char *xlfd)
{
    return XftFontOpenName(dpy, screen, xlfd);
}

XftFont *XftFontOpen(Display *dpy, int screen, ...)
{
    FcPattern *pattern = FcPatternCreate();
    if (!pattern)
        return NULL;
    va_list ap;
    va_start(ap, screen);
    const char *object;
    while ((object = va_arg(ap, const char *))) {
        int type = va_arg(ap, int);
        if (type == FcTypeString) {
            const char *s = va_arg(ap, const char *);
            FcPatternAddString(pattern, object, (const FcChar8 *) s);
        } else if (type == FcTypeInteger) {
            int i = va_arg(ap, int);
            FcPatternAddInteger(pattern, object, i);
        } else if (type == FcTypeDouble) {
            double d = va_arg(ap, double);
            FcPatternAddDouble(pattern, object, d);
        } else if (type == FcTypeBool) {
            FcBool b = va_arg(ap, int);
            FcPatternAddBool(pattern, object, b);
        } else {
            (void) va_arg(ap, void *);
        }
    }
    va_end(ap);
    XftFont *font = XftFontOpenPattern(dpy, pattern);
    FcPatternDestroy(pattern);
    return font;
}

void XftFontClose(Display *dpy, XftFont *font)
{
    (void) dpy;
    if (!font)
        return;
    CompatXftFont *compat = (CompatXftFont *) font;
    TTF_CloseFont(compat->ttf);
    if (compat->patternReferenced && font->pattern &&
        font->pattern->refcount > 1)
        FcPatternDestroy(font->pattern);
    FcPatternDestroy(font->pattern);
    free(compat);
}

XftDraw *XftDrawCreate(Display *dpy,
                       Drawable drawable,
                       Visual *visual,
                       Colormap colormap)
{
    XftDraw *draw = calloc(1, sizeof(*draw));
    if (!draw)
        return NULL;
    draw->display = dpy;
    draw->drawable = drawable;
    draw->visual = visual;
    draw->colormap = colormap;
    return draw;
}

void XftDrawDestroy(XftDraw *draw)
{
    free(draw);
}

void XftDrawChange(XftDraw *draw, Drawable drawable)
{
    if (draw)
        draw->drawable = drawable;
}

Display *XftDrawDisplay(XftDraw *draw)
{
    return draw ? draw->display : NULL;
}

Drawable XftDrawDrawable(XftDraw *draw)
{
    return draw ? draw->drawable : None;
}

Colormap XftDrawColormap(XftDraw *draw)
{
    return draw ? draw->colormap : None;
}

Visual *XftDrawVisual(XftDraw *draw)
{
    return draw ? draw->visual : NULL;
}

Bool XftColorAllocValue(Display *dpy,
                        Visual *visual,
                        Colormap cmap,
                        const XRenderColor *color,
                        XftColor *result)
{
    (void) dpy;
    (void) visual;
    (void) cmap;
    if (!color || !result)
        return False;
    result->color = *color;
    result->pixel = ((unsigned long) (color->alpha >> 8) << ALPHA_SHIFT) |
                    ((unsigned long) (color->red >> 8) << RED_SHIFT) |
                    ((unsigned long) (color->green >> 8) << GREEN_SHIFT) |
                    ((unsigned long) (color->blue >> 8) << BLUE_SHIFT);
    return True;
}

Bool XftColorAllocName(Display *dpy,
                       Visual *visual,
                       Colormap cmap,
                       const char *name,
                       XftColor *result)
{
    XColor exact;
    XColor screen;
    if (!result || !XAllocNamedColor(dpy, cmap, name, &screen, &exact))
        return False;
    XRenderColor color = {
        .red = exact.red,
        .green = exact.green,
        .blue = exact.blue,
        .alpha = 0xffff,
    };
    return XftColorAllocValue(dpy, visual, cmap, &color, result);
}

void XftColorFree(Display *dpy, Visual *visual, Colormap cmap, XftColor *color)
{
    (void) dpy;
    (void) visual;
    (void) cmap;
    (void) color;
}

static char *copyUtf8(const FcChar8 *string, int len)
{
    if (!string || len < 0)
        return NULL;
    char *copy = malloc((size_t) len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, string, (size_t) len);
    copy[len] = '\0';
    return copy;
}

static int appendUtf8(char *out, int offset, uint32_t codepoint)
{
    if (codepoint <= 0x7f) {
        out[offset++] = (char) codepoint;
    } else if (codepoint <= 0x7ff) {
        out[offset++] = (char) (0xc0 | (codepoint >> 6));
        out[offset++] = (char) (0x80 | (codepoint & 0x3f));
    } else if (codepoint <= 0xffff) {
        out[offset++] = (char) (0xe0 | (codepoint >> 12));
        out[offset++] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        out[offset++] = (char) (0x80 | (codepoint & 0x3f));
    } else {
        out[offset++] = (char) (0xf0 | (codepoint >> 18));
        out[offset++] = (char) (0x80 | ((codepoint >> 12) & 0x3f));
        out[offset++] = (char) (0x80 | ((codepoint >> 6) & 0x3f));
        out[offset++] = (char) (0x80 | (codepoint & 0x3f));
    }
    return offset;
}

static char *utf16ToUtf8(const FcChar16 *string, int len)
{
    if (!string || len < 0 || len > INT_MAX / 4)
        return NULL;
    char *out = malloc((size_t) len * 4 + 1);
    if (!out)
        return NULL;
    int offset = 0;
    for (int i = 0; i < len; i++)
        offset = appendUtf8(out, offset, string[i]);
    out[offset] = '\0';
    return out;
}

static char *latin1ToUtf8(const FcChar8 *string, int len)
{
    if (!string || len < 0 || len > INT_MAX / 2)
        return NULL;
    char *out = malloc((size_t) len * 2 + 1);
    if (!out)
        return NULL;
    int offset = 0;
    for (int i = 0; i < len; i++)
        offset = appendUtf8(out, offset, string[i]);
    out[offset] = '\0';
    return out;
}

/* Encode a run of UCS-4 codepoints (or any unsigned-int glyph indices the
 * caller has already mapped 1:1 to codepoints, see XftCharIndex) to a fresh
 * UTF-8 string. Shared by the FcChar32 / FT_UInt entry points since both source
 * types are unsigned int.
 */
static char *codepointsToUtf8(const unsigned int *codepoints, int len)
{
    if (!codepoints || len < 0 || len > INT_MAX / 4)
        return NULL;
    char *out = malloc((size_t) len * 4 + 1);
    if (!out)
        return NULL;
    int offset = 0;
    for (int i = 0; i < len; i++)
        offset = appendUtf8(out, offset, codepoints[i]);
    out[offset] = '\0';
    return out;
}

static char *utf16BytesToUtf8(const FcChar8 *string, FcEndian endian, int len)
{
    if (!string || len < 0)
        return NULL;
    int chars = len / 2;
    FcChar16 *tmp = malloc(sizeof(FcChar16) * (size_t) chars);
    if (!tmp)
        return NULL;
    for (int i = 0; i < chars; i++) {
        unsigned int a = string[i * 2];
        unsigned int b = string[i * 2 + 1];
        tmp[i] = endian == FcEndianBig ? (FcChar16) ((a << 8) | b)
                                       : (FcChar16) ((b << 8) | a);
    }
    char *out = utf16ToUtf8(tmp, chars);
    free(tmp);
    return out;
}

static SDL_Color sdlColorFromXft(const XftColor *color)
{
    SDL_Color sdl = {
        .r = color ? (Uint8) (color->color.red >> 8) : 0,
        .g = color ? (Uint8) (color->color.green >> 8) : 0,
        .b = color ? (Uint8) (color->color.blue >> 8) : 0,
        .a = color ? (Uint8) (color->color.alpha >> 8) : 255,
    };
    return sdl;
}

static unsigned long blendPixel(unsigned long dst,
                                SDL_PixelFormat *format,
                                Uint32 srcPixel,
                                Uint8 colorAlpha)
{
    Uint8 sr, sg, sb, sa;
    SDL_GetRGBA(srcPixel, format, &sr, &sg, &sb, &sa);
    sa = (Uint8) (((unsigned int) sa * colorAlpha) / 255u);
    if (sa == 0)
        return dst;
    unsigned int dr = GET_RED_FROM_COLOR(dst);
    unsigned int dg = GET_GREEN_FROM_COLOR(dst);
    unsigned int db = GET_BLUE_FROM_COLOR(dst);
    unsigned int inv = 255u - sa;
    unsigned int r = (sr * sa + dr * inv) / 255u;
    unsigned int g = (sg * sa + dg * inv) / 255u;
    unsigned int b = (sb * sa + db * inv) / 255u;
    return ((unsigned long) 0xffu << ALPHA_SHIFT) |
           ((unsigned long) r << RED_SHIFT) |
           ((unsigned long) g << GREEN_SHIFT) |
           ((unsigned long) b << BLUE_SHIFT);
}

static void drawUtf8String(XftDraw *draw,
                           const XftColor *color,
                           XftFont *font,
                           int x,
                           int y,
                           const char *text)
{
    if (!draw || !font || !text)
        return;
    CompatXftFont *compat = (CompatXftFont *) font;
    SDL_Color sdlColor = sdlColorFromXft(color);
    SDL_Surface *glyphs = TTF_RenderUTF8_Blended(compat->ttf, text, sdlColor);
    if (!glyphs)
        return;
    int top = y - font->ascent;
    if (glyphs->w <= 0 || glyphs->h <= 0) {
        SDL_FreeSurface(glyphs);
        return;
    }
    /* Clip the source/destination rect against the drawable's bounds. XGetImage
     * rejects any read that touches off-drawable pixels with a BadMatch, which
     * would silently drop the entire string when the caller's text overflows
     * the drawable - common at edges and when clients pass y past the baseline.
     * Intersect the glyph rect with [0, dw) x [0, dh) and adjust the source
     * offset into the glyphs surface so partial draws still paint the visible
     * portion.
     */
    Window rootRet = None;
    int dx = 0;
    int dy = 0;
    unsigned int dw = 0;
    unsigned int dh = 0;
    unsigned int borderRet = 0;
    unsigned int depthRet = 0;
    if (!XGetGeometry(draw->display, draw->drawable, &rootRet, &dx, &dy, &dw,
                      &dh, &borderRet, &depthRet) ||
        dw == 0 || dh == 0) {
        SDL_FreeSurface(glyphs);
        return;
    }
    int destX = x;
    int destY = top;
    int srcX = 0;
    int srcY = 0;
    int rectW = glyphs->w;
    int rectH = glyphs->h;
    if (destX < 0) {
        srcX = -destX;
        rectW -= srcX;
        destX = 0;
    }
    if (destY < 0) {
        srcY = -destY;
        rectH -= srcY;
        destY = 0;
    }
    if ((unsigned) destX >= dw || (unsigned) destY >= dh || rectW <= 0 ||
        rectH <= 0) {
        SDL_FreeSurface(glyphs);
        return;
    }
    if ((unsigned) (destX + rectW) > dw)
        rectW = (int) dw - destX;
    if ((unsigned) (destY + rectH) > dh)
        rectH = (int) dh - destY;
    if (rectW <= 0 || rectH <= 0) {
        SDL_FreeSurface(glyphs);
        return;
    }
    XImage *image = XGetImage(draw->display, draw->drawable, destX, destY,
                              (unsigned int) rectW, (unsigned int) rectH,
                              AllPlanes, ZPixmap);
    if (!image) {
        SDL_FreeSurface(glyphs);
        return;
    }
    if (SDL_LockSurface(glyphs) != 0) {
        XDestroyImage(image);
        SDL_FreeSurface(glyphs);
        return;
    }
    for (int yy = 0; yy < rectH; yy++) {
        for (int xx = 0; xx < rectW; xx++) {
            Uint32 *row = (Uint32 *) ((char *) glyphs->pixels +
                                      (yy + srcY) * glyphs->pitch);
            unsigned long dst = XGetPixel(image, xx, yy);
            unsigned long blended =
                blendPixel(dst, glyphs->format, row[xx + srcX], sdlColor.a);
            XPutPixel(image, xx, yy, blended);
        }
    }
    SDL_UnlockSurface(glyphs);
    GC gc = XCreateGC(draw->display, draw->drawable, 0, NULL);
    if (gc) {
        XPutImage(draw->display, draw->drawable, gc, image, 0, 0, destX, destY,
                  (unsigned int) rectW, (unsigned int) rectH);
        XFreeGC(draw->display, gc);
    }
    XDestroyImage(image);
    SDL_FreeSurface(glyphs);
}

void XftDrawStringUtf8(XftDraw *draw,
                       const XftColor *color,
                       XftFont *font,
                       int x,
                       int y,
                       const FcChar8 *string,
                       int len)
{
    char *text = copyUtf8(string, len);
    drawUtf8String(draw, color, font, x, y, text);
    free(text);
}

void XftDrawString8(XftDraw *draw,
                    const XftColor *color,
                    XftFont *font,
                    int x,
                    int y,
                    const FcChar8 *string,
                    int len)
{
    char *text = latin1ToUtf8(string, len);
    drawUtf8String(draw, color, font, x, y, text);
    free(text);
}

void XftDrawString16(XftDraw *draw,
                     const XftColor *color,
                     XftFont *font,
                     int x,
                     int y,
                     const FcChar16 *string,
                     int len)
{
    char *text = utf16ToUtf8(string, len);
    drawUtf8String(draw, color, font, x, y, text);
    free(text);
}

void XftDrawString32(XftDraw *draw,
                     const XftColor *color,
                     XftFont *font,
                     int x,
                     int y,
                     const FcChar32 *string,
                     int len)
{
    char *text = codepointsToUtf8(string, len);
    drawUtf8String(draw, color, font, x, y, text);
    free(text);
}

void XftDrawStringUtf16(XftDraw *draw,
                        const XftColor *color,
                        XftFont *font,
                        int x,
                        int y,
                        const FcChar8 *string,
                        FcEndian endian,
                        int len)
{
    char *text = utf16BytesToUtf8(string, endian, len);
    drawUtf8String(draw, color, font, x, y, text);
    free(text);
}

static void textExtentsUtf8(XftFont *font,
                            const char *text,
                            XGlyphInfo *extents)
{
    if (!extents)
        return;
    memset(extents, 0, sizeof(*extents));
    if (!font || !text)
        return;
    CompatXftFont *compat = (CompatXftFont *) font;
    int w = 0;
    int h = 0;
    if (TTF_SizeUTF8(compat->ttf, text, &w, &h) != 0)
        return;
    /* XGlyphInfo packs width/height as unsigned short and x/y/xOff/yOff as
     * signed short. SDL_ttf returns int and silently widens; raw casting wraps
     * long strings (advances flip negative) and a client doing layout math sees
     * junk. Clamp to the representable range; downstream consumers see "as wide
     * as it gets" instead of a negative advance.
     */
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    extents->width = w > 0xffff ? (unsigned short) 0xffff : (unsigned short) w;
    extents->height = h > 0xffff ? (unsigned short) 0xffff : (unsigned short) h;
    extents->xOff = w > 0x7fff ? (short) 0x7fff : (short) w;
    extents->yOff = 0;
}

void XftTextExtentsUtf8(Display *dpy,
                        XftFont *font,
                        const FcChar8 *string,
                        int len,
                        XGlyphInfo *extents)
{
    (void) dpy;
    char *text = copyUtf8(string, len);
    textExtentsUtf8(font, text, extents);
    free(text);
}

void XftTextExtents8(Display *dpy,
                     XftFont *font,
                     const FcChar8 *string,
                     int len,
                     XGlyphInfo *extents)
{
    (void) dpy;
    char *text = latin1ToUtf8(string, len);
    textExtentsUtf8(font, text, extents);
    free(text);
}

void XftTextExtents16(Display *dpy,
                      XftFont *font,
                      const FcChar16 *string,
                      int len,
                      XGlyphInfo *extents)
{
    char *text = utf16ToUtf8(string, len);
    textExtentsUtf8(font, text, extents);
    free(text);
}

void XftTextExtents32(Display *dpy,
                      XftFont *font,
                      const FcChar32 *string,
                      int len,
                      XGlyphInfo *extents)
{
    char *text = codepointsToUtf8(string, len);
    textExtentsUtf8(font, text, extents);
    free(text);
}

void XftTextExtentsUtf16(Display *dpy,
                         XftFont *font,
                         const FcChar8 *string,
                         FcEndian endian,
                         int len,
                         XGlyphInfo *extents)
{
    char *text = utf16BytesToUtf8(string, endian, len);
    textExtentsUtf8(font, text, extents);
    free(text);
}

/* Re-open a font through the same pattern so the returned XftFont is an
 * independently-owned instance with its own refcount on the underlying pattern.
 * Real Xft returns the SAME pointer (it reference-counts XftFont itself); we
 * settle for opening a fresh TTF backing because we don't track XftFont
 * refcounts.
 */
XftFont *XftFontCopy(Display *dpy, XftFont *font)
{
    if (!font || !font->pattern)
        return NULL;
    FcPatternReference(font->pattern);
    XftFont *copy = XftFontOpenPattern(dpy, font->pattern);
    if (!copy)
        FcPatternDestroy(font->pattern);
    return copy;
}

/* SDL_ttf has no public glyph-index surface; XftCharIndex normally returns a
 * per-font integer the caller passes to XftDrawGlyphs / XftGlyphExtents.
 * Returning the codepoint as-is is a degenerate but valid mapping: our
 * implementation of XftDrawGlyphs / XftGlyphExtents accepts the same int and
 * rebuilds a UTF-8 buffer for the SDL_ttf rasterizer.
 */
FT_UInt XftCharIndex(Display *dpy, XftFont *font, FcChar32 ucs4)
{
    (void) dpy;
    (void) font;
    return (FT_UInt) ucs4;
}

FcBool XftCharExists(Display *dpy, XftFont *font, FcChar32 ucs4)
{
    (void) dpy;
    if (!font)
        return FcFalse;
    CompatXftFont *compat = (CompatXftFont *) font;
    if (!compat->ttf)
        return FcFalse;
    /* SDL_ttf only ships a 16-bit glyph probe in the version pinned by this
     * repo, so any codepoint beyond the BMP gets a best-effort answer.
     * Returning FcTrue keeps the AA path active; the actual glyph render uses
     * UTF-8 round-trip which the rasterizer can still handle.
     */
    if (ucs4 > 0xffff)
        return FcTrue;
    return TTF_GlyphIsProvided(compat->ttf, (Uint16) ucs4) ? FcTrue : FcFalse;
}

void XftGlyphExtents(Display *dpy,
                     XftFont *font,
                     const FT_UInt *glyphs,
                     int nglyphs,
                     XGlyphInfo *extents)
{
    (void) dpy;
    char *text = codepointsToUtf8(glyphs, nglyphs);
    textExtentsUtf8(font, text, extents);
    free(text);
}

void XftDrawGlyphs(XftDraw *draw,
                   const XftColor *color,
                   XftFont *font,
                   int x,
                   int y,
                   const FT_UInt *glyphs,
                   int nglyphs)
{
    char *text = codepointsToUtf8(glyphs, nglyphs);
    drawUtf8String(draw, color, font, x, y, text);
    free(text);
}

/* Fill an axis-aligned rectangle on `draw`'s drawable using the caller's
 * XftColor. Maps to XFillRectangle through a one-shot GC so the call always
 * routes through the libx11-compat surface (the X Render Composite path is not
 * available in this shim).
 */
void XftDrawRect(XftDraw *draw,
                 const XftColor *color,
                 int x,
                 int y,
                 unsigned int width,
                 unsigned int height)
{
    if (!draw || !color || width == 0 || height == 0)
        return;
    GC gc = XCreateGC(draw->display, draw->drawable, 0, NULL);
    if (!gc)
        return;
    XSetForeground(draw->display, gc, color->pixel);
    XFillRectangle(draw->display, draw->drawable, gc, x, y, width, height);
    XFreeGC(draw->display, gc);
}

/* Decode the next UTF-8 codepoint in `src`.
 *
 * Returns the number of bytes consumed, or 0 on a malformed lead byte. Mirrors
 * fontconfig's FcUtf8ToUcs4 shape; Xft re-exports the same surface so clients
 * (Xfig, gtk2) link either name.
 */
int XftUtf8ToUcs4(const FcChar8 *src, FcChar32 *dst, int len)
{
    if (!src || !dst || len <= 0)
        return 0;
    unsigned char c = src[0];
    if (c < 0x80) {
        *dst = c;
        return 1;
    }
    int extra;
    FcChar32 cp;
    FcChar32 minCp;
    if ((c & 0xe0) == 0xc0) {
        extra = 1;
        cp = c & 0x1f;
        minCp = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        extra = 2;
        cp = c & 0x0f;
        minCp = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
        extra = 3;
        cp = c & 0x07;
        minCp = 0x10000;
    } else {
        return 0;
    }
    if (len < 1 + extra)
        return 0;
    for (int i = 0; i < extra; i++) {
        unsigned char cont = src[1 + i];
        if ((cont & 0xc0) != 0x80)
            return 0;
        cp = (cp << 6) | (cont & 0x3f);
    }
    if (cp < minCp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0;
    *dst = cp;
    return 1 + extra;
}

/* Walk `string` counting UTF-8 codepoints; record the count in *nchar and the
 * minimum UCS bytes-per-character needed to represent the widest codepoint in
 * *wchar. Per fontconfig's FcUtf8Len contract this is 1 for pure ASCII /
 * Latin-1, 2 for the rest of the BMP, and 4 for supplementary planes - never
 * the UTF-8 encoded byte width itself. Clients (Xfig's d_text.c) size
 * wide-buffer allocations off this value so reporting "3 bytes" for a 3-byte
 * UTF-8 sequence that represents a BMP codepoint would over-allocate and could
 * mis-align downstream.
 * Returns FcTrue on success, FcFalse on a malformed sequence.
 */
FcBool XftUtf8Len(const FcChar8 *string, int len, int *nchar, int *wchar)
{
    int countOut = 0;
    FcChar32 maxCp = 0;
    if (!string || len < 0) {
        if (nchar)
            *nchar = 0;
        if (wchar)
            *wchar = 0;
        return FcFalse;
    }
    int i = 0;
    FcBool ok = FcTrue;
    while (i < len) {
        FcChar32 cp = 0;
        int step = XftUtf8ToUcs4(string + i, &cp, len - i);
        if (step <= 0) {
            ok = FcFalse;
            break;
        }
        i += step;
        countOut++;
        if (cp > maxCp)
            maxCp = cp;
    }
    if (nchar)
        *nchar = countOut;
    if (wchar)
        *wchar = maxCp < 0x100 ? 1 : (maxCp < 0x10000 ? 2 : 4);
    return ok;
}
