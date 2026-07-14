#include <X11/Xft/Xft.h>
#include <X11/Xregion.h>
#include <X11/Xutil.h>
#include "sdl-ttf-compat.h"
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "colors.h"
#include "drawing.h"
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
        FcCharSet *c;
    } value;
} FcPatternEntry;

struct _FcCharSet {
    FcChar32 *chars;
    int length;
    int capacity;
    FcBool universal;
};

struct _FcObjectSet {
    char **objects;
    int length;
    int capacity;
};

static FcCharSet *copyCharSet(const FcCharSet *src)
{
    FcCharSet *dst = calloc(1, sizeof(FcCharSet));
    if (!dst)
        return NULL;
    if (!src) {
        dst->universal = FcTrue;
        return dst;
    }
    dst->universal = src->universal;
    if (src->length == 0)
        return dst;
    if (src->length < 0 || (size_t) src->length > SIZE_MAX / sizeof(FcChar32)) {
        free(dst);
        return NULL;
    }
    dst->chars = malloc(sizeof(FcChar32) * (size_t) src->length);
    if (!dst->chars) {
        free(dst);
        return NULL;
    }
    memcpy(dst->chars, src->chars, sizeof(FcChar32) * (size_t) src->length);
    dst->length = src->length;
    dst->capacity = src->length;
    return dst;
}

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

static FcPattern *makeFontPattern(const char *family,
                                  const char *style,
                                  FcBool monospace)
{
    FcPattern *pattern = FcPatternCreate();
    if (!pattern)
        return NULL;
    if (!FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *) family) ||
        !FcPatternAddString(pattern, FC_STYLE, (const FcChar8 *) style) ||
        !FcPatternAddString(pattern, FC_FULLNAME, (const FcChar8 *) family) ||
        !FcPatternAddBool(pattern, FC_SCALABLE, FcTrue) ||
        !FcPatternAddInteger(pattern, FC_SPACING,
                             monospace ? FC_MONO : FC_PROPORTIONAL)) {
        FcPatternDestroy(pattern);
        return NULL;
    }
    return pattern;
}

typedef struct {
    int x;
    int y;
    int width;
    int height;
} XftClipRect;

struct _XftDraw {
    Display *display;
    Drawable drawable;
    Visual *visual;
    Colormap colormap;
    XftClipRect *clipRects;
    int clipRectCount;
    Bool clipSet;
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
                              const void *value,
                              FcBool append)
{
    if (!pattern || !object || !reservePatternEntry(pattern))
        return FcFalse;
    int index = pattern->length;
    if (!append) {
        for (int i = 0; i < pattern->length; i++) {
            if (!strcmp(pattern->entries[i].object, object)) {
                index = i;
                break;
            }
        }
    }
    FcPatternEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.object = xftStrdup(object);
    if (!entry.object)
        return FcFalse;
    entry.type = type;
    switch (type) {
    case FcTypeString:
        entry.value.s = xftStrdup((const char *) value);
        if (!entry.value.s) {
            free(entry.object);
            return FcFalse;
        }
        break;
    case FcTypeInteger:
        entry.value.i = *(const int *) value;
        break;
    case FcTypeDouble:
        entry.value.d = *(const double *) value;
        break;
    case FcTypeBool:
        entry.value.b = *(const FcBool *) value;
        break;
    case FcTypeCharSet:
        entry.value.c = copyCharSet((const FcCharSet *) value);
        if (!entry.value.c) {
            free(entry.object);
            return FcFalse;
        }
        break;
    default:
        free(entry.object);
        return FcFalse;
    }
    for (int i = pattern->length; i > index; i--)
        pattern->entries[i] = pattern->entries[i - 1];
    pattern->entries[index] = entry;
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
        if (pattern->entries[i].type == FcTypeCharSet)
            FcCharSetDestroy(pattern->entries[i].value.c);
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
            if (pattern->entries[src].type == FcTypeCharSet)
                FcCharSetDestroy(pattern->entries[src].value.c);
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
            if (pattern->entries[i].type == FcTypeCharSet)
                FcCharSetDestroy(pattern->entries[i].value.c);
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
    case FcTypeCharSet:
        value->u.c = entry->value.c;
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
                           s ? s : (FcChar8 *) "", FcTrue);
}

FcBool FcPatternAddInteger(FcPattern *pattern, const char *object, int i)
{
    return addPatternEntry(pattern, object, FcTypeInteger, &i, FcTrue);
}

FcBool FcPatternAddDouble(FcPattern *pattern, const char *object, double d)
{
    return addPatternEntry(pattern, object, FcTypeDouble, &d, FcTrue);
}

FcBool FcPatternAddBool(FcPattern *pattern, const char *object, FcBool b)
{
    return addPatternEntry(pattern, object, FcTypeBool, &b, FcTrue);
}

FcBool FcPatternAddCharSet(FcPattern *pattern,
                           const char *object,
                           const FcCharSet *c)
{
    return addPatternEntry(pattern, object, FcTypeCharSet, c, FcTrue);
}

static FcBool addMatrixPatternEntry(FcPattern *pattern,
                                    const char *object,
                                    const FcMatrix *matrix,
                                    FcBool append)
{
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
    return addPatternEntry(pattern, object, FcTypeString, buf, append);
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
    return addMatrixPatternEntry(pattern, object, matrix, FcTrue);
}

FcBool FcPatternAdd(FcPattern *pattern,
                    const char *object,
                    FcValue value,
                    FcBool append)
{
    switch (value.type) {
    case FcTypeString:
        return addPatternEntry(pattern, object, FcTypeString,
                               value.u.s ? value.u.s : (FcChar8 *) "", append);
    case FcTypeInteger:
        return addPatternEntry(pattern, object, FcTypeInteger, &value.u.i,
                               append);
    case FcTypeDouble:
        return addPatternEntry(pattern, object, FcTypeDouble, &value.u.d,
                               append);
    case FcTypeBool:
        return addPatternEntry(pattern, object, FcTypeBool, &value.u.b, append);
    case FcTypeMatrix:
        return addMatrixPatternEntry(pattern, object, value.u.m, append);
    case FcTypeCharSet:
        return addPatternEntry(pattern, object, FcTypeCharSet, value.u.c,
                               append);
    default:
        return FcFalse;
    }
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
        case FcTypeCharSet:
            ok = addPatternEntry(dst, e->object, FcTypeCharSet, e->value.c,
                                 FcTrue);
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

FcCharSet *FcCharSetCreate(void)
{
    return calloc(1, sizeof(FcCharSet));
}

FcBool FcCharSetHasChar(const FcCharSet *charset, FcChar32 ucs4)
{
    if (!charset)
        return FcFalse;
    if (charset->universal)
        return FcTrue;
    for (int i = 0; i < charset->length; i++) {
        if (charset->chars[i] == ucs4)
            return FcTrue;
    }
    return FcFalse;
}

FcBool FcCharSetAddChar(FcCharSet *charset, FcChar32 ucs4)
{
    if (!charset)
        return FcFalse;
    if (FcCharSetHasChar(charset, ucs4))
        return FcTrue;
    if (charset->length == charset->capacity) {
        if (charset->capacity > INT_MAX / 2)
            return FcFalse;
        int capacity = charset->capacity ? charset->capacity * 2 : 8;
        if ((size_t) capacity > SIZE_MAX / sizeof(FcChar32))
            return FcFalse;
        FcChar32 *chars =
            realloc(charset->chars, sizeof(FcChar32) * (size_t) capacity);
        if (!chars)
            return FcFalse;
        charset->chars = chars;
        charset->capacity = capacity;
    }
    charset->chars[charset->length++] = ucs4;
    return FcTrue;
}

void FcCharSetDestroy(FcCharSet *charset)
{
    if (!charset)
        return;
    free(charset->chars);
    free(charset);
}

/* Deep copy. Real fontconfig ref-counts and returns the same object; our
 * FcCharSet is unshared, so callers (Tk stores one per face and destroys it
 * independently) get their own copy.
 */
FcCharSet *FcCharSetCopy(FcCharSet *src)
{
    /* Delegate to the shared deep-copy. The NULL guard stays here because
     * copyCharSet maps a NULL source to a universal set, whereas FcCharSetCopy
     * of NULL must stay NULL.
     */
    return src ? copyCharSet(src) : NULL;
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

FcResult FcPatternGetCharSet(const FcPattern *pattern,
                             const char *object,
                             int n,
                             FcCharSet **c)
{
    FcPatternEntry *entry = findPatternEntry(pattern, object, n);
    if (!entry)
        return FcResultNoMatch;
    if (entry->type != FcTypeCharSet)
        return FcResultTypeMismatch;
    if (c)
        *c = entry->value.c;
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
    if (FcPatternGetString(pattern, FC_STYLE, 0, NULL) == FcResultNoMatch)
        FcPatternAddString(pattern, FC_STYLE, (const FcChar8 *) "Regular");
}

FcPattern *FcFontMatch(FcConfig *config, FcPattern *pattern, FcResult *result)
{
    (void) config;
    if (!pattern) {
        if (result)
            *result = FcResultNoMatch;
        return NULL;
    }
    FcPattern *match = FcPatternDuplicate(pattern);
    if (!match) {
        if (result)
            *result = FcResultOutOfMemory;
        return NULL;
    }
    FcDefaultSubstitute(match);
    if (FcPatternGetString(match, FC_FULLNAME, 0, NULL) == FcResultNoMatch) {
        FcChar8 *family = NULL;
        if (FcPatternGetString(match, FC_FAMILY, 0, &family) != FcResultMatch)
            family = (FcChar8 *) "Sans";
        FcPatternAddString(match, FC_FULLNAME, family);
    }
    if (result)
        *result = FcResultMatch;
    return match;
}

FcObjectSet *FcObjectSetCreate(void)
{
    return calloc(1, sizeof(FcObjectSet));
}

FcBool FcObjectSetAdd(FcObjectSet *os, const char *object)
{
    if (!os || !object)
        return FcFalse;
    if (os->length == os->capacity) {
        if (os->capacity > INT_MAX / 2)
            return FcFalse;
        int capacity = os->capacity ? os->capacity * 2 : 8;
        if ((size_t) capacity > SIZE_MAX / sizeof(char *))
            return FcFalse;
        char **objects =
            realloc(os->objects, sizeof(char *) * (size_t) capacity);
        if (!objects)
            return FcFalse;
        os->objects = objects;
        os->capacity = capacity;
    }
    os->objects[os->length] = xftStrdup(object);
    if (!os->objects[os->length])
        return FcFalse;
    os->length++;
    return FcTrue;
}

void FcObjectSetDestroy(FcObjectSet *os)
{
    if (!os)
        return;
    for (int i = 0; i < os->length; i++)
        free(os->objects[i]);
    free(os->objects);
    free(os);
}

FcFontSet *FcFontList(FcConfig *config, FcPattern *pattern, FcObjectSet *os)
{
    (void) config;
    (void) os;
    int spacing = FC_PROPORTIONAL;
    Bool monoOnly = pattern &&
                    FcPatternGetInteger(pattern, FC_SPACING, 0, &spacing) ==
                        FcResultMatch &&
                    spacing >= FC_MONO;
    FcFontSet *set = calloc(1, sizeof(FcFontSet));
    if (!set)
        return NULL;
    set->sfont = monoOnly ? 1 : 3;
    set->fonts = calloc((size_t) set->sfont, sizeof(FcPattern *));
    if (!set->fonts) {
        free(set);
        return NULL;
    }
    set->fonts[set->nfont++] = makeFontPattern("Monospace", "Regular", FcTrue);
    if (!monoOnly) {
        set->fonts[set->nfont++] = makeFontPattern("Sans", "Regular", FcFalse);
        set->fonts[set->nfont++] = makeFontPattern("Serif", "Regular", FcFalse);
    }
    for (int i = 0; i < set->nfont; i++) {
        if (!set->fonts[i]) {
            FcFontSetDestroy(set);
            return NULL;
        }
    }
    return set;
}

FcFontSet *FcFontSort(FcConfig *config,
                      FcPattern *pattern,
                      FcBool trim,
                      FcCharSet **csp,
                      FcResult *result)
{
    (void) trim;
    if (csp)
        *csp = NULL;
    FcFontSet *set = FcFontList(config, pattern, NULL);
    if (result) {
        if (!set)
            *result = FcResultOutOfMemory;
        else if (set->nfont == 0)
            *result = FcResultNoMatch;
        else
            *result = FcResultMatch;
    }
    return set;
}

void FcFontSetDestroy(FcFontSet *set)
{
    if (!set)
        return;
    for (int i = 0; i < set->nfont; i++)
        FcPatternDestroy(set->fonts[i]);
    free(set->fonts);
    free(set);
}

/* Produce the pattern to actually render: the resolved font augmented with the
 * requested size and DPI, matching what real fontconfig returns so the caller
 * opens the font at the size it asked for. config is unused (no config db).
 */
FcPattern *FcFontRenderPrepare(FcConfig *config,
                               FcPattern *pat,
                               FcPattern *font)
{
    (void) config;
    if (!font)
        return NULL;
    FcPattern *result = FcPatternDuplicate(font);
    if (!result || !pat)
        return result;
    double d;
    int i;
    if (FcPatternGetDouble(pat, FC_PIXEL_SIZE, 0, &d) == FcResultMatch) {
        FcPatternDel(result, FC_PIXEL_SIZE);
        FcPatternAddDouble(result, FC_PIXEL_SIZE, d);
    }
    if (FcPatternGetDouble(pat, FC_SIZE, 0, &d) == FcResultMatch) {
        FcPatternDel(result, FC_SIZE);
        FcPatternAddDouble(result, FC_SIZE, d);
    } else if (FcPatternGetInteger(pat, FC_SIZE, 0, &i) == FcResultMatch) {
        FcPatternDel(result, FC_SIZE);
        FcPatternAddInteger(result, FC_SIZE, i);
    }
    if (FcPatternGetInteger(pat, FC_DPI, 0, &i) == FcResultMatch) {
        FcPatternDel(result, FC_DPI);
        FcPatternAddInteger(result, FC_DPI, i);
    }
    return result;
}

static double patternDoubleField(FcPattern *pattern, const char *field)
{
    double d;
    int i;
    if (FcPatternGetDouble(pattern, field, 0, &d) == FcResultMatch)
        return d;
    if (FcPatternGetInteger(pattern, field, 0, &i) == FcResultMatch)
        return (double) i;
    return -1.0;
}

/* Resolve a font's pixel size the way fontconfig does. An explicit pixelsize is
 * already in device pixels and wins outright. A bare size is in points and must
 * be scaled by the pattern DPI (pixels = points * dpi / 72); passing the point
 * value straight to SDL_ttf as pixels is what made every Xft client here render
 * a font a third too small. XftDefaultSubstitute stamps FC_DPI from the screen
 * resolution, so a client that runs the normal match path lands on the same
 * pixel size a native X server would. Fall back to 96 DPI, the value the compat
 * screen advertises, when no DPI is present.
 */
static int patternSize(FcPattern *pattern)
{
    double pixel = patternDoubleField(pattern, FC_PIXEL_SIZE);
    if (pixel > 0 && pixel <= 256) {
        int px = (int) (pixel + 0.5);
        return px < 1 ? 1 : px;
    }
    double point = patternDoubleField(pattern, FC_SIZE);
    if (!(point > 0 && point <= 256))
        point = 12;
    double dpi = patternDoubleField(pattern, FC_DPI);
    if (!(dpi > 0 && dpi <= 2000))
        dpi = 96.0;
    int px = (int) (point * dpi / 72.0 + 0.5);
    if (px < 1)
        px = 1;
    if (px > 256)
        px = 256;
    return px;
}

static const FcCharSet *patternCharSet(FcPattern *pattern)
{
    FcValue value;
    if (FcPatternGet(pattern, FC_CHARSET, 0, &value) != FcResultMatch ||
        value.type != FcTypeCharSet)
        return NULL;
    return value.u.c;
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

static FcBool ttfProvidesCodepoint(TTF_Font *font, FcChar32 codepoint)
{
    if (!font)
        return FcFalse;
    return xc_TTF_GlyphIsProvidedUcs4(font, codepoint) ? FcTrue : FcFalse;
}

static FcBool ttfProvidesCharSet(TTF_Font *font, const FcCharSet *charset)
{
    if (!charset || charset->universal || charset->length <= 0)
        return FcTrue;
    for (int i = 0; i < charset->length; i++) {
        if (!ttfProvidesCodepoint(font, charset->chars[i]))
            return FcFalse;
    }
    return FcTrue;
}

static TTF_Font *openFallbackForCharSet(const char *family,
                                        int size,
                                        const FcCharSet *charset)
{
    if (!charset || charset->universal || charset->length <= 0)
        return compatFontOpenFamilyFallback(family, size);
    for (int i = 0; i < charset->length; i++) {
        TTF_Font *font = compatFontOpenFamilyFallbackForChar(family, size,
                                                             charset->chars[i]);
        if (ttfProvidesCharSet(font, charset))
            return font;
        if (font)
            TTF_CloseFont(font);
    }
    /* No single host font covers the whole requested charset (for example a
     * Latin + CJK mix on a box without a CJK font). Real Xft never refuses the
     * pattern here; it returns a base font and resolves the rest per glyph at
     * draw time. Mirror that and hand back the best-effort family fallback
     * instead of NULL so XftFontOpenPattern stays non-NULL.
     */
    return compatFontOpenFamilyFallback(family, size);
}

static TTF_Font *openFontFromPattern(FcPattern *pattern)
{
    int size = patternSize(pattern);
    const FcCharSet *requestedCharset = patternCharSet(pattern);
    /* FC_FILE wins because the caller named an exact file. Anything else routes
     * through the shared font-family fallback chain in src/font.c so xft and
     * the core XLoadQueryFont path agree on which TTF backs "helvetica" /
     * "sans" / "times" / monospace.
     */
    const char *file = patternFile(pattern);
    if (file) {
        TTF_Font *font = TTF_OpenFont(file, size);
        if (font && ttfProvidesCharSet(font, requestedCharset))
            return font;
        if (font)
            TTF_CloseFont(font);
    }
    return openFallbackForCharSet(patternFamily(pattern), size,
                                  requestedCharset);
}

static FcCharSet *charSetForTtf(TTF_Font *ttf, const FcCharSet *requested)
{
    FcCharSet *charset = FcCharSetCreate();
    if (!charset)
        return NULL;
    if (requested && !requested->universal) {
        for (int i = 0; i < requested->length; i++) {
            if (!ttfProvidesCodepoint(ttf, requested->chars[i]))
                continue;
            if (!FcCharSetAddChar(charset, requested->chars[i])) {
                FcCharSetDestroy(charset);
                return NULL;
            }
        }
        return charset;
    }

    for (FcChar32 c = 0x20; c <= 0x2ff; c++) {
        if (!ttfProvidesCodepoint(ttf, c))
            continue;
        if (!FcCharSetAddChar(charset, c)) {
            FcCharSetDestroy(charset);
            return NULL;
        }
    }
    for (FcChar32 c = 0x370; c <= 0x3ff; c++) {
        if (!ttfProvidesCodepoint(ttf, c))
            continue;
        if (!FcCharSetAddChar(charset, c)) {
            FcCharSetDestroy(charset);
            return NULL;
        }
    }
    return charset;
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
    /* Real Xft seeds FC_DPI from the screen resolution before matching so the
     * point-to-pixel conversion in patternSize uses the same DPI a native
     * server would. Only add it when the client has not pinned one already.
     */
    if (dpy && pattern && screen >= 0 && screen < ScreenCount(dpy) &&
        FcPatternGetDouble(pattern, FC_DPI, 0, NULL) == FcResultNoMatch &&
        FcPatternGetInteger(pattern, FC_DPI, 0, NULL) == FcResultNoMatch) {
        int hmm = DisplayHeightMM(dpy, screen);
        int hpx = DisplayHeight(dpy, screen);
        if (hmm > 0 && hpx > 0) {
            double dpi = (double) hpx * 25.4 / (double) hmm;
            if (dpi > 0 && dpi <= 2000)
                FcPatternAddDouble(pattern, FC_DPI, dpi);
        }
    }
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
    FcValue charsetValue;
    const FcCharSet *requestedCharset = NULL;
    if (FcPatternGet(pattern, FC_CHARSET, 0, &charsetValue) == FcResultMatch &&
        charsetValue.type == FcTypeCharSet)
        requestedCharset = charsetValue.u.c;
    font->public.charset = charSetForTtf(ttf, requestedCharset);
    if (!font->public.charset) {
        TTF_CloseFont(ttf);
        free(font);
        return NULL;
    }
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

/* Parse an X Logical Font Description into a pattern (family plus pixel size).
 * XLFD fields are dash-separated: -foundry-family-weight-slant-setwidth-
 * addstyle-pixelsize-pointsize-... A non-XLFD string is taken as a family name.
 * ignore_scalable / complete tune real fontconfig heuristics we do not model.
 */
FcPattern *XftXlfdParse(const char *xlfd_orig,
                        FcBool ignore_scalable,
                        FcBool complete)
{
    (void) ignore_scalable;
    (void) complete;
    FcPattern *pattern = FcPatternCreate();
    if (!pattern)
        return NULL;
    if (!xlfd_orig || xlfd_orig[0] != '-') {
        FcPatternAddString(
            pattern, FC_FAMILY,
            (const FcChar8 *) (xlfd_orig && xlfd_orig[0] ? xlfd_orig : "Sans"));
        return pattern;
    }
    char *copy = xftStrdup(xlfd_orig);
    if (!copy) {
        FcPatternDestroy(pattern);
        return NULL;
    }
    char *tokens[14] = {0};
    int nt = 0;
    tokens[nt++] = copy; /* leading empty field before the first dash */
    for (char *q = copy; *q && nt < 14; q++) {
        if (*q == '-') {
            *q = '\0';
            tokens[nt++] = q + 1;
        }
    }
    const char *family =
        (nt > 2 && tokens[2][0] && strcmp(tokens[2], "*")) ? tokens[2] : "Sans";
    FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *) family);
    if (nt > 7 && tokens[7][0]) {
        long px = strtol(tokens[7], NULL, 10);
        if (px >= 1 && px <= 256)
            FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double) px);
    }
    free(copy);
    return pattern;
}

/* Enumerate the font families this backend can render. The variadic pattern and
 * object-set selectors real XftListFonts takes are ignored; callers use this to
 * populate a family list and get the shim's Monospace / Sans / Serif set.
 */
FcFontSet *XftListFonts(Display *dpy, int screen, ...)
{
    (void) dpy;
    (void) screen;
    return FcFontList(NULL, NULL, NULL);
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
    FcCharSetDestroy(font->charset);
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
    if (draw)
        free(draw->clipRects);
    free(draw);
}

void XftDrawChange(XftDraw *draw, Drawable drawable)
{
    if (draw)
        draw->drawable = drawable;
}

static int xftClampToInt(long long value)
{
    if (value < INT_MIN)
        return INT_MIN;
    if (value > INT_MAX)
        return INT_MAX;
    return (int) value;
}

Bool XftDrawSetClipRectangles(XftDraw *draw,
                              int x_origin,
                              int y_origin,
                              const XRectangle *rects,
                              int n)
{
    if (!draw || n < 0)
        return False;
    if (n == 0) {
        free(draw->clipRects);
        draw->clipRects = NULL;
        draw->clipRectCount = 0;
        draw->clipSet = True;
        return True;
    }
    if (!rects || (size_t) n > SIZE_MAX / sizeof(XftClipRect))
        return False;
    /* Build the replacement before touching the live clip so a failed
     * allocation or invalid input leaves the existing clip intact instead of
     * silently dropping it (the API has no way to report a partial failure).
     */
    XftClipRect *newRects = malloc(sizeof(XftClipRect) * (size_t) n);
    if (!newRects)
        return False;
    for (int i = 0; i < n; i++) {
        newRects[i].x = xftClampToInt((long long) rects[i].x + x_origin);
        newRects[i].y = xftClampToInt((long long) rects[i].y + y_origin);
        newRects[i].width = rects[i].width;
        newRects[i].height = rects[i].height;
    }
    free(draw->clipRects);
    draw->clipRects = newRects;
    draw->clipRectCount = n;
    draw->clipSet = True;
    return True;
}

Bool XftDrawSetClip(XftDraw *draw, Region region)
{
    if (!draw)
        return False;
    if (!region) {
        free(draw->clipRects);
        draw->clipRects = NULL;
        draw->clipRectCount = 0;
        draw->clipSet = False;
        return True;
    }
    if (region->numRects <= 0)
        return XftDrawSetClipRectangles(draw, 0, 0, NULL, 0);
    if (region->numRects > INT_MAX ||
        (size_t) region->numRects > SIZE_MAX / sizeof(XftClipRect))
        return False;
    XftClipRect *newRects =
        malloc(sizeof(XftClipRect) * (size_t) region->numRects);
    if (!newRects)
        return False;
    for (long i = 0; i < region->numRects; i++) {
        BOX *box = &region->rects[i];
        newRects[i].x = box->x1;
        newRects[i].y = box->y1;
        newRects[i].width = box->x2 - box->x1;
        newRects[i].height = box->y2 - box->y1;
    }
    free(draw->clipRects);
    draw->clipRects = newRects;
    draw->clipRectCount = (int) region->numRects;
    draw->clipSet = True;
    return True;
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
    if ((size_t) chars > SIZE_MAX / sizeof(FcChar16))
        return NULL;
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
    /* Clip the source/destination rect against the drawable's bounds so a
     * string that overflows the drawable - common at edges and when clients
     * pass y past the baseline - paints only its visible portion instead of
     * stretching the glyph over the whole target. Intersect the glyph rect with
     * [0, dw) x [0, dh) and adjust the source offset into the glyphs surface to
     * match.
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
    /* Composite the glyph on the GPU instead of reading the destination back
     * for a CPU blend. The previous XGetImage/blendPixel/XPutImage path forced
     * a full-pixmap SDL_RenderReadPixels per glyph (XPutImage re-dirties the
     * readback cache), which dominated typing latency. Uploading the blended
     * glyph as a BLEND texture and letting SDL_RenderCopy alpha-composite it
     * keeps the whole draw in renderer space.
     */
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(draw->drawable, renderer);
    if (!renderer) {
        SDL_FreeSurface(glyphs);
        return;
    }
    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, glyphs);
    SDL_FreeSurface(glyphs);
    if (!texture)
        return;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    /* TTF_RenderUTF8_Blended already bakes the colour's alpha into the surface
     * pixels (per-glyph coverage * colour alpha) on the SDL_ttf versions this
     * links against, so the texture alpha comes solely from those pixels; an
     * extra SDL_SetTextureAlphaMod(sdlColor.a) would multiply the colour alpha
     * a second time and render translucent text too faintly. The core-font path
     * (font.c) copies the blended texture the same way without an alpha mod.
     */
#if defined(LIBX11_COMPAT_SDL3) || SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_SetTextureScaleMode(texture, XC_SCALEMODE_LINEAR);
#endif
    SDL_Rect srcRect = {srcX, srcY, rectW, rectH};
    SDL_Rect destRect = {destX, destY, rectW, rectH};
    ShapeGuard sg;
    shapeGuardBegin(&sg, draw->drawable, renderer, &destRect);
    /* Clip to the drawable's sibling visible region the same way the core-font
     * path does (font.c), so Xft text into a child obscured by a higher-stacked
     * sibling cannot paint over that sibling. getWindowRenderer resolves every
     * child to its top-level's shared backing texture, so without this the copy
     * would only honour the base clip and the Xft clip rects. Passing gc==NULL
     * makes these helpers iterate the visible region alone (gc-clip count 1),
     * and setGcClipForIteration installs the base clip intersected with each
     * visible-region rect; a fully occluded window reports 0 iterations.
     */
    int clipCount = getGcClipIterationCount(NULL, draw->drawable);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, NULL, clip, draw->drawable))
            continue;
        /* No clip set means draw unclipped; a clip that was set but holds no
         * rects is an empty region, so nothing is visible and the glyph is
         * skipped. Only clipRectCount alone cannot tell these apart.
         */
        if (!draw->clipSet) {
            SDL_RenderCopy(renderer, texture, &srcRect, &destRect);
            continue;
        }
        if (draw->clipRectCount <= 0)
            continue;
        /* Xft clip rects share the drawable/viewport space of the base+sibling
         * clip just installed, so intersect each one with that clip (read back
         * from the renderer) and copy per rect.
         */
        SDL_bool baseClipEnabled = SDL_RenderIsClipEnabled(renderer);
        SDL_Rect baseClip;
        if (baseClipEnabled)
            SDL_RenderGetClipRect(renderer, &baseClip);
        for (int i = 0; i < draw->clipRectCount; i++) {
            SDL_Rect xftClip = {draw->clipRects[i].x, draw->clipRects[i].y,
                                draw->clipRects[i].width,
                                draw->clipRects[i].height};
            if (baseClipEnabled &&
                !SDL_IntersectRect(&xftClip, &baseClip, &xftClip))
                continue;
            SDL_RenderSetClipRect(renderer, &xftClip);
            SDL_RenderCopy(renderer, texture, &srcRect, &destRect);
        }
    }
    clearRendererClip(renderer);
    /* If the post-draw shape composite failed, masked-out pixels may still be
     * on the renderer; skip the present so the next draw recomposes from a
     * fresh baseline rather than flashing stale output (mirrors image.c).
     */
    Bool shapeOk = shapeGuardEnd(&sg);
    SDL_DestroyTexture(texture);
    if (shapeOk)
        presentDrawableRectIfVisible(draw->drawable, &destRect);
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
    /* The synthesized charset only seeds a few default ranges, so it is not a
     * reliable authority on what the rasterizer can draw. A universal charset
     * still short-circuits to FcTrue; otherwise defer to TTF_GlyphIsProvided,
     * which reflects the actual font.
     */
    if (font->charset && font->charset->universal)
        return FcTrue;
    CompatXftFont *compat = (CompatXftFont *) font;
    if (!compat->ttf)
        return FcFalse;
    return xc_TTF_GlyphIsProvidedUcs4(compat->ttf, ucs4) ? FcTrue : FcFalse;
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

/* Draw glyphs that each carry their own font and position. Tk uses this to lay
 * out a line that mixes fonts (fallback glyphs). Render one at a time through
 * the single-font path; the runs Tk passes are short.
 */
void XftDrawGlyphFontSpec(XftDraw *draw,
                          const XftColor *color,
                          const XftGlyphFontSpec *glyphs,
                          int len)
{
    if (!draw || !color || !glyphs)
        return;
    for (int i = 0; i < len; i++) {
        FT_UInt glyph = glyphs[i].glyph;
        XftDrawGlyphs(draw, color, glyphs[i].font, glyphs[i].x, glyphs[i].y,
                      &glyph, 1);
    }
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
    if (!draw->clipSet) {
        XFillRectangle(draw->display, draw->drawable, gc, x, y, width, height);
        XFreeGC(draw->display, gc);
        return;
    }
    /* Clip the fill against each clip rectangle. Opaque fills tolerate the
     * overlap that intersecting regions would produce. Edges are computed in
     * long long so caller-controlled dimensions cannot signed-overflow.
     */
    long long rectLeft = x;
    long long rectTop = y;
    long long rectRight = (long long) x + width;
    long long rectBottom = (long long) y + height;
    for (int i = 0; i < draw->clipRectCount; i++) {
        const XftClipRect *clip = &draw->clipRects[i];
        long long left = clip->x > rectLeft ? clip->x : rectLeft;
        long long top = clip->y > rectTop ? clip->y : rectTop;
        long long right = (long long) clip->x + clip->width;
        if (right > rectRight)
            right = rectRight;
        long long bottom = (long long) clip->y + clip->height;
        if (bottom > rectBottom)
            bottom = rectBottom;
        if (right > left && bottom > top)
            XFillRectangle(draw->display, draw->drawable, gc, (int) left,
                           (int) top, (unsigned int) (right - left),
                           (unsigned int) (bottom - top));
    }
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

int FcUtf8ToUcs4(const FcChar8 *src, FcChar32 *dst, int len)
{
    return XftUtf8ToUcs4(src, dst, len);
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
