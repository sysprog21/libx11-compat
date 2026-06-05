#include "X11/Xatom.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <X11/Xlib.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "errors.h"
#include "colors.h"
#include "resource-types.h"
#include "atoms.h"
#include "drawing.h"
#include "display.h"
#include "gc.h"
#include "util.h"
#include "font.h"

typedef struct {
    char *filePath;
    char *XLFName;
    Bool fixedWidth;
} FontCacheEntry;

typedef struct {
    TTF_Font *ttf;
    unsigned int gcRefs;
    Bool closePending;
    Bool hasCoreMetrics;
    short coreAscent;
    short coreDescent;
    short coreWidth;
} CompatFont;

#define GET_FONT_RESOURCE(fontXID) ((CompatFont *) GET_XID_VALUE(fontXID))
#define GET_FONT(fontXID) \
    (GET_FONT_RESOURCE(fontXID) ? GET_FONT_RESOURCE(fontXID)->ttf : NULL)
#define DEFAULT_FONT_SIZE 10
#define MIN_FONT_SIZE 6
#define MAX_FONT_SIZE 72

/* Project-bundled "fonts" wins for self-contained checkouts; the remaining
 * entries let normal Xlib clients pick up host-system fonts without having
 * to call XSetFontPath. Missing directories are filtered out at scan time. */
static const char *DEFAULT_FONT_SEARCH_PATHS[] = {
    "fonts",
#if defined(__APPLE__)
    "/System/Library/Fonts",
    "/System/Library/Fonts/Supplemental",
    "/Library/Fonts",
#else
    "/usr/share/fonts",
    "/usr/local/share/fonts",
#endif
};

// This are the custom search paths for fonts can be set via XSetFontPath.
// Has to be initialized via XSetFontPath(display, NULL, 0);
Array *fontSearchPaths = NULL;
Array *fontCache = NULL;

// Check if the given path points to an existing directory
Bool checkFontPath(const char *path)
{
    if (!path)
        return False;
    struct stat s;
    int err;
    while ((err = stat(path, &s)) == -1 && errno == EAGAIN)
        ;
    return err == 0 && S_ISDIR(s.st_mode);
}

char *getFontXLFDName(TTF_Font *font)
{
    /* FOUNDRY - FAMILY_NAME - WEIGHT_NAME - SLANT - SETWIDTH_NAME - ADD_STYLE -
       PIXEL_SIZE - POINT_SIZE - RESOLUTION_X - RESOLUTION_Y - SPACING -
       AVERAGE_WIDTH - CHARSET_REGISTRY - CHARSET_ENCODING */
    int fontStyle = TTF_GetFontStyle(font);
    static const char *const emptyValue = "";
    const char *foundry = emptyValue;
    const char *familyName = TTF_FontFaceFamilyName(font);
    if (!familyName)
        familyName = emptyValue;
    const char *weightName = fontStyle & TTF_STYLE_BOLD ? "bold" : "medium";
    char slant = (char) (fontStyle & TTF_STYLE_ITALIC ? 'i' : 'r');
    const char *setWidth = "normal";
    int pointSize = 0;
    char spacing = (char) (TTF_FontFaceIsFixedWidth(font) ? 'm' : 'p');
    short averageWidth = 0;
    const char *charset = "iso10646";  // Unicode
    int charsetEncoding = 1;
    size_t nameLength = 14 + strlen(foundry) + strlen(familyName) +
                        strlen(weightName) + 1 + strlen(setWidth) + 1 + 6 + 1 +
                        1 + 1 + 5 + strlen(charset) + 1;

    char *name = malloc(sizeof(char) * nameLength);
    if (name) {
        /* Tk rejects the empty ADD_STYLE field form (`...-normal--0-...`) for
         * these generated names, so keep the compact `...-normal-0-...`
         * variant while preserving charset registry/encoding fields. */
        snprintf(name, nameLength, "-%s-%s-%s-%c-%s-0-%d-0-0-%c-%hd-%s-%d",
                 foundry, familyName, weightName, slant, setWidth, pointSize,
                 spacing, averageWidth, charset, charsetEncoding);
        LOG("Font name = '%s'\n", name);
    }
    return name;
}

Bool fontCacheEntryFileNameCmp(void *entry, void *name)
{
    size_t nameLen = strlen(name);
    size_t pathLen = strlen(((FontCacheEntry *) entry)->filePath);
    if (nameLen > pathLen)
        return False;
    return !strcmp(&((FontCacheEntry *) entry)->filePath[pathLen - nameLen],
                   name);
}

Bool fontCmp(void *entry, void *fontWildCard)
{
    return matchWildcard(fontWildCard, ((FontCacheEntry *) entry)->XLFName);
}

Bool updateFontCache()
{
    size_t i, entryNameLen;
    size_t fontCacheIndex = 0;
    DIR *fontDirectory;
    struct dirent *entry;
    char pathBuffer[512];
    for (i = 0; i < fontSearchPaths->length; i++) {
        char *fontDirPath = fontSearchPaths->array[i];
        fontDirectory = opendir(fontDirPath);
        if (!fontDirectory) {
            free(removeArray(fontSearchPaths, i, False));
            i--;
            continue;
        }
        while ((entry = readdir(fontDirectory))) {
            // We add all missing fonts to the cache, swapping their position to
            // the front. We can be sure, that the fonts with an index lower
            // than fontCacheIndex are valid.
            entryNameLen = strlen(entry->d_name);
            if (entryNameLen > 4 &&
                !strncmp(&entry->d_name[entryNameLen - 4], ".ttf", 4)) {
                ssize_t index =
                    findInArrayNCmp(fontCache, entry->d_name, fontCacheIndex,
                                    &fontCacheEntryFileNameCmp);

                if (index == -1) {
                    snprintf(pathBuffer, 512, "%s/%s", fontDirPath,
                             entry->d_name);
                    TTF_Font *font =
                        TTF_OpenFont(pathBuffer, DEFAULT_FONT_SIZE);
                    if (!font)
                        continue;
                    FontCacheEntry *fontCacheEntry =
                        malloc(sizeof(FontCacheEntry));
                    if (!fontCacheEntry) {
                        TTF_CloseFont(font);
                        closedir(fontDirectory);
                        return False;
                    }
                    fontCacheEntry->filePath = strdup(pathBuffer);
                    fontCacheEntry->XLFName = getFontXLFDName(font);
                    fontCacheEntry->fixedWidth = TTF_FontFaceIsFixedWidth(font);
                    TTF_CloseFont(font);
                    if (!fontCacheEntry->filePath || !fontCacheEntry->XLFName) {
                        if (fontCacheEntry->filePath)
                            free(fontCacheEntry->filePath);
                        if (fontCacheEntry->XLFName)
                            free(fontCacheEntry->XLFName);
                        free(fontCacheEntry);
                        closedir(fontDirectory);
                        return False;
                    }
                    if (!insertArray(fontCache, fontCacheEntry)) {
                        free(fontCacheEntry->filePath);
                        free(fontCacheEntry->XLFName);
                        free(fontCacheEntry);
                        closedir(fontDirectory);
                        return False;
                    }
                    index = fontCache->length - 1;
                }
                if ((size_t) index != fontCacheIndex) {
                    swapArray(fontCache, (size_t) index, fontCacheIndex);
                }
                fontCacheIndex++;
            }
        }
        closedir(fontDirectory);
    }
    while (fontCache->length > fontCacheIndex) {
        // Remove all invalid cache entries
        FontCacheEntry *cacheEntry =
            removeArray(fontCache, fontCache->length - 1, True);
        free(cacheEntry->filePath);
        free(cacheEntry->XLFName);
        free(cacheEntry);
    }
    return True;
}


Bool initFontStorage()
{
    size_t fontCount = 0;
    DIR *fontDirectory;
    struct dirent *entry;
    fontSearchPaths = malloc(sizeof(Array));

    if (!fontSearchPaths) {
        return False;
    }
    if (!initArray(fontSearchPaths, ARRAY_LENGTH(DEFAULT_FONT_SEARCH_PATHS))) {
        return False;
    }

    for (size_t i = 0; i < ARRAY_LENGTH(DEFAULT_FONT_SEARCH_PATHS); i++) {
        const char *path = DEFAULT_FONT_SEARCH_PATHS[i];
        if (checkFontPath(path)) {
            fontDirectory = opendir(path);
            if (!fontDirectory)
                continue;
            path = strdup(path);
            if (!path)
                return False;
            insertArray(fontSearchPaths, (char *) path);
            // Count the font files in the directory
            while ((entry = readdir(fontDirectory))) {
                size_t entryNameLen = strlen(entry->d_name);
                if (entryNameLen > 4 &&
                    !strncmp(&entry->d_name[entryNameLen - 4], ".ttf", 4)) {
                    fontCount++;
                }
            }
            closedir(fontDirectory);
        }
    }

    fontCache = malloc(sizeof(Array));
    if (!fontCache) {
        return False;
    }
    if (!initArray(fontCache, fontCount)) {
        return False;
    }
    return updateFontCache();
}

void freeFontStorage()
{
    /* Drop cached text textures before any renderer or font goes away;
     * destroyScreenWindow runs SDL_DestroyRenderer right after this call
     * and any retained texture from the cache would dangle. */
    freeTextCache();
    if (fontSearchPaths) {
        // Clear the array and free the data
        while (fontSearchPaths->length > 0) {
            free(removeArray(fontSearchPaths, 0, False));
        }
        freeArray(fontSearchPaths);
        free(fontSearchPaths);
        fontSearchPaths = NULL;
    }
    if (fontCache) {
        // Clear the array and free the data
        while (fontCache->length > 0) {
            FontCacheEntry *entry = removeArray(fontCache, 0, False);
            free(entry->filePath);
            free(entry->XLFName);
            free(entry);
        }
        freeArray(fontCache);
        free(fontCache);
        fontCache = NULL;
    }
}

static int requestedFontSize(const char *name);

static Bool containsIgnoreCase(const char *text, const char *needle)
{
    if (!text || !needle || !*needle)
        return False;
    size_t needleLen = strlen(needle);
    for (const char *p = text; *p; p++) {
        size_t i = 0;
        while (i < needleLen && p[i]) {
            char a = p[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z')
                a = (char) (a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z')
                b = (char) (b - 'A' + 'a');
            if (a != b)
                break;
            i++;
        }
        if (i == needleLen)
            return True;
    }
    return False;
}

static Bool isFontAlias(const char *name)
{
    if (!name)
        return False;
    /* Discrete short aliases plus a few XLFD prefixes that real X11
     * clients request when there is no font server. findFontCacheEntry
     * ByName checks the cache first, so an XLFD that does happen to be
     * scanned in is honored verbatim; this list is only the fallback
     * after exact match misses. -adobe-times- is not fixed-width, but
     * x11perf requests it for TR10/TR24 tests and would otherwise abort
     * on XOpenFont(None). */
    return !strcmp(name, "fixed") || !strcmp(name, "cursor") ||
           !strcmp(name, "6x13") || !strcmp(name, "8x13") ||
           !strcmp(name, "7x14") || !strcmp(name, "9x13") ||
           !strcmp(name, "9x15") || !strcmp(name, "9x18") ||
           !strcmp(name, "12x24") || !strncmp(name, "-misc-fixed-", 12) ||
           !strncmp(name, "-jis-fixed-", 11) ||
           containsIgnoreCase(name, "helvetica") ||
           containsIgnoreCase(name, "courier") ||
           containsIgnoreCase(name, "adobe-times") ||
           (containsIgnoreCase(name, "times") &&
            requestedFontSize(name) != 14) ||
           ((strstr(name, "-medium-r-") || strstr(name, "-bold-r-")) &&
            strstr(name, "-p-"));
}

/* Probe paths for a monospace face when the search-path scan didn't pick
 * one up — e.g. macOS keeps its fonts as .ttc which our .ttf-only scan
 * skips, and minimal Linux containers may have only freefont present in
 * a deeper subdirectory. The first path that TTF_OpenFont accepts is
 * adopted into the cache so subsequent XLoadFont calls hit the fast
 * path. */
static const char *MONOSPACE_PROBE_PATHS[] = {
#if defined(__APPLE__)
    "/System/Library/Fonts/Menlo.ttc",
    "/System/Library/Fonts/Monaco.ttf",
    "/System/Library/Fonts/Courier.dfont",
#else
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu-sans-mono-fonts/DejaVuSansMono.ttf",
#endif
};

static const char *SANS_PROBE_PATHS[] = {
#if defined(__APPLE__)
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/LucidaGrande.ttc",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
#else
    "/usr/share/fonts/opentype/urw-base35/NimbusSansNarrow-Regular.otf",
    "/usr/share/fonts/truetype/liberation/LiberationSansNarrow-Regular.ttf",
    "/usr/share/fonts/opentype/urw-base35/NimbusSans-Regular.otf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
};

static const char *SANS_BOLD_PROBE_PATHS[] = {
#if defined(__APPLE__)
    "/System/Library/Fonts/Helvetica.ttc",
    "/System/Library/Fonts/LucidaGrande.ttc",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
#else
    "/usr/share/fonts/opentype/urw-base35/NimbusSansNarrow-Bold.otf",
    "/usr/share/fonts/truetype/liberation/LiberationSansNarrow-Bold.ttf",
    "/usr/share/fonts/opentype/urw-base35/NimbusSans-Bold.otf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
#endif
};

static const char *SERIF_PROBE_PATHS[] = {
#if defined(__APPLE__)
    "/System/Library/Fonts/Times.ttc",
    "/System/Library/Fonts/Supplemental/Times New Roman.ttf",
#else
    "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSerif.ttf",
#endif
};

static int clampFontSize(int size)
{
    if (size < MIN_FONT_SIZE)
        return MIN_FONT_SIZE;
    if (size > MAX_FONT_SIZE)
        return MAX_FONT_SIZE;
    return size;
}

static int decipointsToPixelSize(int decipoints)
{
    /* Xvfb resolves wildcard-resolution XLFD requests against its 100 DPI
     * core-font catalog. SDL_ttf takes a pixel-sized request here, so convert
     * decipoints with the same default DPI: pixels = points * 100 / 72. */
    return clampFontSize((decipoints * 100 + 360) / 720);
}

static Bool parsePositiveIntBounded(const char *text,
                                    int saturationCap,
                                    int *valueReturn)
{
    if (!text || !*text)
        return False;
    int value = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9')
            return False;
        int digit = *p - '0';
        /* XLFD numeric fields are caller-controlled. Reject the multiply
         * before it can wrap, and saturate once the running value exceeds
         * the caller's cap. The remaining digits are drained without
         * arithmetic so a bogus trailing non-digit still flags the whole
         * field as non-numeric. */
        if (value > (INT_MAX - digit) / 10 || value > saturationCap) {
            value = saturationCap;
            for (++p; *p; p++) {
                if (*p < '0' || *p > '9')
                    return False;
            }
            break;
        }
        value = value * 10 + digit;
    }
    *valueReturn = value;
    return True;
}

static Bool parsePositiveInt(const char *text, int *valueReturn)
{
    return parsePositiveIntBounded(text, MAX_FONT_SIZE, valueReturn);
}

static int requestedFontSize(const char *name)
{
    if (!name)
        return DEFAULT_FONT_SIZE;

    const char *x = strchr(name, 'x');
    if (x && x > name && x[1] != '\0') {
        char widthBuffer[16];
        size_t widthLen = (size_t) (x - name);
        int width = 0;
        int height = 0;
        if (widthLen < sizeof(widthBuffer)) {
            memcpy(widthBuffer, name, widthLen);
            widthBuffer[widthLen] = '\0';
            if (parsePositiveInt(widthBuffer, &width) &&
                parsePositiveInt(x + 1, &height)) {
                (void) width;
                return clampFontSize(height);
            }
        }
    }

    const char *doubleDash = strstr(name, "--");
    if (doubleDash) {
        int pixelSize = 0;
        const char *start = doubleDash + 2;
        const char *end = strchr(start, '-');
        if (end && end > start) {
            char buffer[16];
            size_t len = (size_t) (end - start);
            if (len < sizeof(buffer)) {
                memcpy(buffer, start, len);
                buffer[len] = '\0';
                if (parsePositiveInt(buffer, &pixelSize))
                    return clampFontSize(pixelSize);
            }
        }
    }

    const char *fieldStart = name;
    int field = name[0] == '-' ? 0 : 1;
    int pointSizeFallback = 0;
    for (const char *p = name;; p++) {
        if (*p != '-' && *p != '\0')
            continue;
        if (field == 7 || (name[0] != '-' && field == 6)) {
            size_t len = (size_t) (p - fieldStart);
            if (len > 0 && len < 16) {
                char buffer[16];
                int pixelSize = 0;
                memcpy(buffer, fieldStart, len);
                buffer[len] = '\0';
                if (parsePositiveInt(buffer, &pixelSize))
                    return clampFontSize(pixelSize);
            }
        } else if (field == 8 || (name[0] != '-' && field == 7)) {
            size_t len = (size_t) (p - fieldStart);
            if (len > 0 && len < 16) {
                char buffer[16];
                int decipoints = 0;
                memcpy(buffer, fieldStart, len);
                buffer[len] = '\0';
                /* Decipoints (e.g. 140 = 14pt) are in a separate unit space
                 * from pixels and must not be clamped to MAX_FONT_SIZE here.
                 * Cap the parsed value just below the point where
                 * decipointsToPixelSize's `decipoints * 100 + 360` could
                 * overflow, then let clampFontSize cap the pixel result. */
                int decipointCap = (INT_MAX - 360) / 100;
                if (parsePositiveIntBounded(buffer, decipointCap, &decipoints))
                    pointSizeFallback = decipointsToPixelSize(decipoints);
            }
        }
        if (*p == '\0')
            break;
        field++;
        fieldStart = p + 1;
    }

    if (pointSizeFallback) {
        if (pointSizeFallback == 17 && containsIgnoreCase(name, "helvetica"))
            return 16;
        return pointSizeFallback;
    }
    return DEFAULT_FONT_SIZE;
}

static Bool coreFontMetricsForName(const char *name,
                                   short *ascent,
                                   short *descent,
                                   short *width)
{
    if (!name)
        return False;
    /* Xvfb's built-in "fixed" resolves to a 6x13 bitmap font:
     * ascent=11, descent=2, width=6. Motif uses this alias in fallback
     * fontLists; reporting smaller TTF metrics lets widgets pack too many
     * text rows compared with native libX11.
     */
    if (!strcmp(name, "fixed") || !strcmp(name, "cursor") ||
        !strcmp(name, "6x13")) {
        *ascent = 11;
        *descent = 2;
        *width = 6;
        return True;
    }
    if (!strcmp(name, "7x14")) {
        *ascent = 12;
        *descent = 2;
        *width = 7;
        return True;
    }
    if (!strcmp(name, "9x18")) {
        *ascent = 14;
        *descent = 4;
        *width = 9;
        return True;
    }
    if (!strcmp(name, "12x24")) {
        *ascent = 22;
        *descent = 2;
        *width = 12;
        return True;
    }
    /* XLFD family names are case-insensitive per the X11 spec, so
     * "-Helvetica-" / "-HELVETICA-" must take the same override path as
     * the lowercase form. */
    if (containsIgnoreCase(name, "-helvetica-") &&
        requestedFontSize(name) == 16) {
        *ascent = 16;
        *descent = 4;
        *width = 0;
        return True;
    }
    if (containsIgnoreCase(name, "-helvetica-") &&
        requestedFontSize(name) == 19) {
        *ascent = 19;
        *descent = 4;
        *width = 0;
        return True;
    }
    return False;
}

static FontCacheEntry *adoptProbePath(const char *path)
{
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (!strcmp(entry->filePath, path))
            return entry;
    }

    TTF_Font *font = TTF_OpenFont(path, DEFAULT_FONT_SIZE);
    if (!font)
        return NULL;
    FontCacheEntry *entry = malloc(sizeof(FontCacheEntry));
    if (!entry) {
        TTF_CloseFont(font);
        return NULL;
    }
    entry->filePath = strdup(path);
    entry->XLFName = getFontXLFDName(font);
    entry->fixedWidth = TTF_FontFaceIsFixedWidth(font);
    TTF_CloseFont(font);
    if (!entry->filePath || !entry->XLFName || !insertArray(fontCache, entry)) {
        free(entry->filePath);
        free(entry->XLFName);
        free(entry);
        return NULL;
    }
    return entry;
}

static FontCacheEntry *findProbeFont(const char *const *paths, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        FontCacheEntry *entry = adoptProbePath(paths[i]);
        if (entry)
            return entry;
    }
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (!entry->fixedWidth)
            return entry;
    }
    return fontCache->length > 0 ? fontCache->array[0] : NULL;
}

static FontCacheEntry *findAliasedFixedWidthFont(void)
{
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (entry->fixedWidth)
            return entry;
    }
    for (size_t i = 0; i < ARRAY_LENGTH(MONOSPACE_PROBE_PATHS); i++) {
        FontCacheEntry *entry = adoptProbePath(MONOSPACE_PROBE_PATHS[i]);
        if (entry)
            return entry;
    }
    return fontCache->length > 0 ? fontCache->array[0] : NULL;
}

static FontCacheEntry *findAliasedFontForName(const char *name)
{
    if (containsIgnoreCase(name, "times") ||
        containsIgnoreCase(name, "adobe-times"))
        return findProbeFont(SERIF_PROBE_PATHS,
                             ARRAY_LENGTH(SERIF_PROBE_PATHS));
    if (containsIgnoreCase(name, "helvetica") ||
        containsIgnoreCase(name, "lucida") ||
        containsIgnoreCase(name, "arial")) {
        if (containsIgnoreCase(name, "bold")) {
            return findProbeFont(SANS_BOLD_PROBE_PATHS,
                                 ARRAY_LENGTH(SANS_BOLD_PROBE_PATHS));
        }
        return findProbeFont(SANS_PROBE_PATHS, ARRAY_LENGTH(SANS_PROBE_PATHS));
    }
    if ((strstr(name, "-medium-r-") || strstr(name, "-bold-r-")) &&
        strstr(name, "-p-")) {
        if (containsIgnoreCase(name, "bold")) {
            return findProbeFont(SANS_BOLD_PROBE_PATHS,
                                 ARRAY_LENGTH(SANS_BOLD_PROBE_PATHS));
        }
        return findProbeFont(SANS_PROBE_PATHS, ARRAY_LENGTH(SANS_PROBE_PATHS));
    }
    return findAliasedFixedWidthFont();
}

static FontCacheEntry *findFontCacheEntryByName(const char *name)
{
    /* Exact cache match wins so a real scanned font is never shadowed
     * by an alias prefix. Aliases only kick in when no entry matches. */
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (!strcmp(entry->XLFName, name)) {
            return entry;
        }
    }
    if (strchr(name, '*') || strchr(name, '?')) {
        for (size_t i = 0; i < fontCache->length; i++) {
            FontCacheEntry *entry = fontCache->array[i];
            if (matchWildcard(name, entry->XLFName)) {
                return entry;
            }
        }
        if (isFontAlias(name)) {
            FontCacheEntry *fallback = findAliasedFontForName(name);
            if (fallback)
                return fallback;
        }
    }
    if (isFontAlias(name)) {
        FontCacheEntry *aliased = findAliasedFontForName(name);
        if (aliased)
            return aliased;
    }
    return NULL;
}

Font XLoadFont(Display *display, _Xconst char *name)
{
    SET_X_SERVER_REQUEST(display, X_OpenFont);
    XID font = ALLOC_XID();
    if (font == None) {
        handleOutOfMemory(0, display, 0, 0);
        return None;
    }
    SET_XID_TYPE(font, FONT);
    /* Use the unified lookup so XLoadFont and the rest of the font code
     * agree on cache-first, alias-second ordering. Open-coding the
     * alias check here used to shadow exact XLFD matches. */
    FontCacheEntry *fontEntry = findFontCacheEntryByName(name);
    if (!fontEntry) {
        FREE_XID(font);
        LOG("Font %s not found in cache!\n", name);
        handleError(0, display, None, 0, BadName, 0);
        return None;
    }
    CompatFont *resource = calloc(1, sizeof(*resource));
    if (!resource) {
        FREE_XID(font);
        handleOutOfMemory(0, display, 0, 0);
        return None;
    }
    resource->ttf = TTF_OpenFont(fontEntry->filePath, requestedFontSize(name));
    if (!resource->ttf) {
        free(resource);
        FREE_XID(font);
        LOG("Failed to load font %s!\n", name);
        handleError(0, display, None, 0, BadName, 0);
        return None;
    }
    resource->hasCoreMetrics =
        coreFontMetricsForName(name, &resource->coreAscent,
                               &resource->coreDescent, &resource->coreWidth);
    SET_XID_VALUE(font, resource);
    return font;
}

Bool compatFontIsClientUsable(Font fontXid)
{
    if (fontXid == None || !IS_TYPE(fontXid, FONT))
        return False;
    CompatFont *resource = GET_FONT_RESOURCE(fontXid);
    return resource && resource->ttf && !resource->closePending;
}

Bool compatFontRetainForGC(Font fontXid)
{
    if (fontXid == None || !IS_TYPE(fontXid, FONT))
        return False;
    CompatFont *resource = GET_FONT_RESOURCE(fontXid);
    if (!resource || !resource->ttf)
        return False;
    resource->gcRefs++;
    return True;
}

static void freeFontResource(Font fontXid, CompatFont *resource)
{
    if (!resource)
        return;
    invalidateTextCacheForFont(fontXid);
    if (resource->ttf)
        TTF_CloseFont(resource->ttf);
    free(resource);
    SET_XID_VALUE(fontXid, NULL);
    SET_XID_TYPE(fontXid, CLOSED_FONT);
}

void compatFontReleaseForGC(Font fontXid)
{
    if (fontXid == None || !IS_TYPE(fontXid, FONT))
        return;
    CompatFont *resource = GET_FONT_RESOURCE(fontXid);
    if (!resource)
        return;
    if (resource->gcRefs > 0)
        resource->gcRefs--;
    if (resource->gcRefs == 0 && resource->closePending)
        freeFontResource(fontXid, resource);
}

int compatFontClose(Display *display, Font fontXid)
{
    (void) display;
    if (fontXid == None || !IS_TYPE(fontXid, FONT))
        return 1;
    CompatFont *resource = GET_FONT_RESOURCE(fontXid);
    if (!resource)
        return 1;
    resource->closePending = True;
    if (resource->gcRefs > 0) {
        invalidateTextCacheForFont(fontXid);
        return 1;
    }
    freeFontResource(fontXid, resource);
    return 1;
}

int XFreeFontPath(char **list)
{
    free(list);
    return 1;
}

char **XGetFontPath(Display *display, int *npaths_return)
{
    SET_X_SERVER_REQUEST(display, X_GetFontPath);
    *npaths_return = fontSearchPaths->length;
    char **list = malloc(sizeof(char *) * fontSearchPaths->length);
    if (!list) {
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    memcpy(list, fontSearchPaths->array,
           sizeof(char *) * fontSearchPaths->length);
    return list;
}

int XSetFontPath(Display *display, char **directories, int ndirs)
{
    SET_X_SERVER_REQUEST(display, X_SetFontPath);
    char *path;
    size_t i;
    while (fontSearchPaths->length > 0) {
        // Clear the array and free the data
        free(removeArray(fontSearchPaths, 0, False));
    }
    if (!directories || ndirs == 0) {
        // Reset the search path to the default for the compiled platform
        ndirs = ARRAY_LENGTH(DEFAULT_FONT_SEARCH_PATHS);
        directories = (char **) DEFAULT_FONT_SEARCH_PATHS;
    }
    for (i = 0; i < (size_t) ndirs; i++) {
        if (checkFontPath(directories[i])) {
            path = strdup(directories[i]);
            if (!path) {
                handleOutOfMemory(0, display, 0, 0);
            } else {
                insertArray(fontSearchPaths, path);
            }
        }
    }
    return updateFontCache() ? 1 : 0;
}

char **XListFonts(Display *display,
                  _Xconst char *pattern,
                  int maxnames,
                  int *actual_count_return)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XListFonts.html
    SET_X_SERVER_REQUEST(display, X_ListFonts);
    if (actual_count_return) {
        *actual_count_return = 0;
    }
    if (!actual_count_return || maxnames <= 0) {
        return NULL;
    }
    Array names;
    initArray(&names, 0);
    static char *aliases[] = {"fixed", "cursor", "9x13", "9x15"};
    if (fontCache->length > 0) {
        for (size_t i = 0; i < ARRAY_LENGTH(aliases); i++) {
            if (matchWildcard(pattern, aliases[i])) {
                insertArray(&names, aliases[i]);
                if ((int) names.length >= maxnames)
                    break;
            }
        }
    }
    size_t i;
    for (i = 0; i < fontCache->length && (int) names.length < maxnames; i++) {
        char *name = ((FontCacheEntry *) fontCache->array[i])->XLFName;
        if (matchWildcard(pattern, name)) {
            insertArray(&names, name);
        }
    }
    *actual_count_return = (int) names.length;
    if (names.length == 0)
        return NULL;
    char **list = malloc(sizeof(char *) * names.length);
    if (!list) {
        *actual_count_return = 0;
        return NULL;
    }
    memcpy(list, names.array, sizeof(char *) * names.length);
    freeArray(&names);
    return list;
}

int XFreeFontNames(char **list)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XFreeFontNames.html
    free(list);
    return 1;
}

static void freeFontStructContents(XFontStruct *font_struct)
{
    if (!font_struct) {
        return;
    }
    free(font_struct->properties);
    free(font_struct->per_char);
    font_struct->properties = NULL;
    font_struct->per_char = NULL;
}

static void freeFontStruct(XFontStruct *font_struct)
{
    if (!font_struct) {
        return;
    }
    freeFontStructContents(font_struct);
    free(font_struct);
}

int XFreeFont(Display *display, XFontStruct *font_struct)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XFreeFont.html
    SET_X_SERVER_REQUEST(display, X_CloseFont);
    if (!font_struct) {
        return 1;
    }
    if (font_struct->fid != None) {
        compatFontClose(display, font_struct->fid);
    }
    freeFontStruct(font_struct);
    return 1;
}

int XFreeFontInfo(char **names, XFontStruct *free_info, int actual_count)
{
    if (names)
        XFreeFontNames(names);
    if (free_info) {
        for (int i = 0; i < actual_count; i++) {
            freeFontStructContents(free_info + i);
        }
        free(free_info);
    }
    return 1;
}

Bool XGetFontProperty(XFontStruct *font_struct,
                      Atom atom,
                      unsigned long *value_return)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XGetFontProperty.html
    Bool res = False;
    char *name;
    switch (atom) {
    case XA_FONT:
        name = getFontXLFDName(GET_FONT(font_struct->fid));
        if (name) {
            *value_return = (unsigned long) internalInternAtom(name);
            free(name);
            res = True;
        }
        break;
    default:
        LOG("%s: Got unknown atom %lu!\n", __func__, atom);
    }
    // Can't provide values for XA_UNDERLINE_POSITION and XA_UNDERLINE_THICKNESS
    return res;
}

Bool fillXCharStruct(TTF_Font *font,
                     unsigned int character,
                     XCharStruct *charStruct)
{
    int minX, maxX, minY, maxY, advance;
    if (TTF_GlyphMetrics(font, (Uint16) character, &minX, &maxX, &minY, &maxY,
                         &advance) == -1) {
        LOG("Failed to determine metrics for character '%u': %s\n", character,
            TTF_GetError());
        return False;
    }
    memset(charStruct, 0, sizeof(*charStruct));
    charStruct->lbearing = (short) minX;
    charStruct->width = (short) advance;
    charStruct->rbearing = (short) maxX;
    charStruct->ascent = (short) maxY;
    charStruct->descent = (short) -minY;
    return True;
}

static void updateBounds(XFontStruct *fontStruct, XCharStruct *charStruct)
{
    if (charStruct->lbearing < fontStruct->min_bounds.lbearing) {
        fontStruct->min_bounds.lbearing = charStruct->lbearing;
    }
    if (charStruct->rbearing < fontStruct->min_bounds.rbearing) {
        fontStruct->min_bounds.rbearing = charStruct->rbearing;
    }
    if (charStruct->width < fontStruct->min_bounds.width) {
        fontStruct->min_bounds.width = charStruct->width;
    }
    if (charStruct->ascent < fontStruct->min_bounds.ascent) {
        fontStruct->min_bounds.ascent = charStruct->ascent;
    }
    if (charStruct->descent < fontStruct->min_bounds.descent) {
        fontStruct->min_bounds.descent = charStruct->descent;
    }

    if (charStruct->lbearing > fontStruct->max_bounds.lbearing) {
        fontStruct->max_bounds.lbearing = charStruct->lbearing;
    }
    if (charStruct->rbearing > fontStruct->max_bounds.rbearing) {
        fontStruct->max_bounds.rbearing = charStruct->rbearing;
    }
    if (charStruct->width > fontStruct->max_bounds.width) {
        fontStruct->max_bounds.width = charStruct->width;
    }
    if (charStruct->ascent > fontStruct->max_bounds.ascent) {
        fontStruct->max_bounds.ascent = charStruct->ascent;
    }
    if (charStruct->descent > fontStruct->max_bounds.descent) {
        fontStruct->max_bounds.descent = charStruct->descent;
    }
}

static Bool fillFontStructFromTTF(Display *display,
                                  Font fontId,
                                  TTF_Font *font,
                                  XFontStruct *fontStruct)
{
    memset(fontStruct, 0, sizeof(*fontStruct));
    fontStruct->fid = fontId;
    fontStruct->direction = FontLeftToRight;
    fontStruct->min_char_or_byte2 = 0;
    fontStruct->max_char_or_byte2 = 255;
    fontStruct->min_byte1 = 0;
    fontStruct->max_byte1 = 0;
    fontStruct->all_chars_exist = False;
    fontStruct->default_char = '?';
    fontStruct->ascent = TTF_FontAscent(font);
    fontStruct->descent = abs(TTF_FontDescent(font));

    Bool monospace = TTF_FontFaceIsFixedWidth(font);
    XCharStruct firstChar;
    Bool foundChar = False;
    for (unsigned int i = fontStruct->min_char_or_byte2;
         i <= fontStruct->max_char_or_byte2; i++) {
        if (TTF_GlyphIsProvided(font, (Uint16) i) &&
            fillXCharStruct(font, i, &firstChar)) {
            fontStruct->min_bounds = firstChar;
            fontStruct->max_bounds = firstChar;
            foundChar = True;
            break;
        }
    }

    if (!foundChar) {
        return True;
    }

    if (!monospace) {
        size_t charCount =
            fontStruct->max_char_or_byte2 - fontStruct->min_char_or_byte2 + 1;
        fontStruct->per_char = calloc(charCount, sizeof(XCharStruct));
        if (!fontStruct->per_char) {
            handleOutOfMemory(0, display, 0, 0);
            return False;
        }
    }

    Bool allCharsExist = True;
    for (unsigned int i = fontStruct->min_char_or_byte2;
         i <= fontStruct->max_char_or_byte2; i++) {
        XCharStruct charStruct;
        if (!TTF_GlyphIsProvided(font, (Uint16) i) ||
            !fillXCharStruct(font, i, &charStruct)) {
            allCharsExist = False;
            continue;
        }
        if (fontStruct->per_char) {
            fontStruct->per_char[i - fontStruct->min_char_or_byte2] =
                charStruct;
        }
        updateBounds(fontStruct, &charStruct);
    }
    fontStruct->all_chars_exist = allCharsExist;
    return True;
}

static void applyCoreFontMetrics(XFontStruct *fontStruct,
                                 short ascent,
                                 short descent,
                                 short width)
{
    fontStruct->ascent = ascent;
    fontStruct->descent = descent;
    if (width > 0) {
        free(fontStruct->per_char);
        fontStruct->per_char = NULL;
        fontStruct->min_bounds.width = width;
        fontStruct->min_bounds.lbearing = 0;
        fontStruct->min_bounds.rbearing = width;
        fontStruct->max_bounds.width = width;
        fontStruct->max_bounds.lbearing = 0;
        fontStruct->max_bounds.rbearing = width;
    }
    fontStruct->min_bounds.ascent = ascent;
    fontStruct->min_bounds.descent = descent;
    fontStruct->max_bounds.ascent = ascent;
    fontStruct->max_bounds.descent = descent;
}

char **XListFontsWithInfo(Display *display,
                          _Xconst char *pattern,
                          int maxnames,
                          int *actual_count_return,
                          XFontStruct **info_return)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XListFontsWithInfo.html
    SET_X_SERVER_REQUEST(display, X_ListFontsWithInfo);
    if (actual_count_return) {
        *actual_count_return = 0;
    }
    if (info_return) {
        *info_return = NULL;
    }
    if (!actual_count_return || !info_return) {
        return NULL;
    }

    char **names = XListFonts(display, pattern, maxnames, actual_count_return);
    if (!names || *actual_count_return == 0) {
        return names;
    }

    XFontStruct *infos =
        calloc((size_t) *actual_count_return, sizeof(XFontStruct));
    if (!infos) {
        XFreeFontNames(names);
        *actual_count_return = 0;
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }

    for (int i = 0; i < *actual_count_return; i++) {
        FontCacheEntry *entry = findFontCacheEntryByName(names[i]);
        if (!entry) {
            continue;
        }
        TTF_Font *font =
            TTF_OpenFont(entry->filePath, requestedFontSize(names[i]));
        if (!font) {
            continue;
        }
        fillFontStructFromTTF(display, None, font, &infos[i]);
        short ascent;
        short descent;
        short width;
        if (coreFontMetricsForName(names[i], &ascent, &descent, &width))
            applyCoreFontMetrics(&infos[i], ascent, descent, width);
        TTF_CloseFont(font);
    }

    *info_return = infos;
    return names;
}

XFontStruct *XLoadQueryFont(Display *display, _Xconst char *name)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XLoadQueryFont.html
    if (!findFontCacheEntryByName(name))
        return NULL;
    Font fontId = XLoadFont(display, name);
    if (fontId == None) {
        return NULL;
    }
    XFontStruct *fontStruct = XQueryFont(display, fontId);
    if (!fontStruct) {
        compatFontClose(display, fontId);
    }
    return fontStruct;
}

XFontStruct *XQueryFont(Display *display, XID fontId)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XQueryFont.html
    SET_X_SERVER_REQUEST(display, X_QueryFont);
    TYPE_CHECK(fontId, FONT, display, NULL);
    if (!compatFontIsClientUsable(fontId)) {
        handleError(0, display, fontId, 0, BadFont, 0);
        return NULL;
    }
    TTF_Font *font = GET_FONT(fontId);
    if (!font) {
        handleError(0, display, fontId, 0, BadFont, 0);
        return NULL;
    }
    XFontStruct *fontStruct = calloc(1, sizeof(XFontStruct));
    if (!fontStruct) {
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    if (!fillFontStructFromTTF(display, fontId, font, fontStruct)) {
        freeFontStruct(fontStruct);
        return NULL;
    }
    CompatFont *resource = GET_FONT_RESOURCE(fontId);
    if (resource && resource->hasCoreMetrics) {
        applyCoreFontMetrics(fontStruct, resource->coreAscent,
                             resource->coreDescent, resource->coreWidth);
    }
    return fontStruct;
}

static __inline__ char hexCharToNum(char chr)
{
    return (char) (chr >= 'a' ? 10 + chr - 'a' : chr - '0');
}

/* Resolve all X11 control characters. `count` is caller-controlled and
 * may end mid-escape; each branch validates that the escape's payload
 * fits before reading it, so malformed input gets the literal "\X"
 * preserved instead of running off the end of `string`. */
char *decodeString(const char *string, int count)
{
    if (!string || count < 0)
        return NULL;
    int counter = 0;
    char *text = malloc((size_t) count + 1);
    if (!text)
        return NULL;
    for (int i = 0; i < count; i++) {
        if (string[i] != '\\') {
            text[counter++] = string[i];
            continue;
        }
        if (i + 1 >= count) {
            /* Trailing backslash; emit literally and stop. */
            text[counter++] = '\\';
            break;
        }
        char esc = string[i + 1];
        switch (esc) {
        case 'x': /* \xHL */
            if (i + 3 < count) {
                text[counter++] = (char) ((hexCharToNum(string[i + 2]) << 4) |
                                          hexCharToNum(string[i + 3]));
                i += 3;
            } else {
                text[counter++] = '\\';
                text[counter++] = esc;
                i += 1;
            }
            break;
        case 'u': /* \uHHLL packed as 2 bytes */
            if (i + 5 < count) {
                text[counter++] = (char) ((hexCharToNum(string[i + 2]) << 4) |
                                          hexCharToNum(string[i + 3]));
                text[counter++] = (char) ((hexCharToNum(string[i + 4]) << 4) |
                                          hexCharToNum(string[i + 5]));
                i += 5;
            } else {
                text[counter++] = '\\';
                text[counter++] = esc;
                i += 1;
            }
            break;
        case 'n':
            text[counter++] = '\n';
            i += 1;
            break;
        case 'r':
            text[counter++] = '\r';
            i += 1;
            break;
        case 'a':
            text[counter++] = '\a';
            i += 1;
            break;
        case 'b':
            text[counter++] = '\b';
            i += 1;
            break;
        case 't':
            text[counter++] = '\t';
            i += 1;
            break;
        case 'v':
            text[counter++] = '\v';
            i += 1;
            break;
        case 'f':
            text[counter++] = '\f';
            i += 1;
            break;
        default:
            LOG("Warn: Got unknown control character in %s: '\\%c'\n", __func__,
                esc);
            text[counter++] = '\\';
            text[counter++] = esc;
            i += 1;
            break;
        }
    }
    text[counter] = '\0';
    return text;
}

static int getTextWidthForChars(XFontStruct *font_struct,
                                const char *string,
                                size_t fixedCharCount)
{
    if (!font_struct || !string)
        return 0;
    if (!font_struct->per_char &&
        font_struct->min_bounds.width == font_struct->max_bounds.width) {
        /* Promote to 64-bit so a wide font times a long string can't wrap
         * the int return; clamp at INT_MAX on overflow. */
        int64_t product =
            (int64_t) font_struct->max_bounds.width * (int64_t) fixedCharCount;
        return product > INT_MAX ? INT_MAX : (int) product;
    }
    int width, height;
    if (TTF_SizeUTF8(GET_FONT(font_struct->fid), string, &width, &height) !=
        0) {
        LOG("Failed to calculate the text with in XTextWidth[16]: %s! "
            "Returning max width of font.\n",
            TTF_GetError());
        int64_t product = (int64_t) font_struct->max_bounds.rbearing *
                          (int64_t) strlen(string);
        return product > INT_MAX ? INT_MAX : (int) product;
    }
    return width;
}

int getTextWidth(XFontStruct *font_struct, const char *string)
{
    if (!string)
        return 0;
    return getTextWidthForChars(font_struct, string, strlen(string));
}

static size_t utf8LengthForCodepoint(unsigned int codepoint)
{
    if (codepoint <= 0x7f)
        return 1;
    if (codepoint <= 0x7ff)
        return 2;
    return 3;
}

static char *decodeChar2bString(const XChar2b *string,
                                int count,
                                size_t *length)
{
    size_t bytes = 0;
    for (int i = 0; i < count; i++) {
        unsigned int codepoint =
            ((unsigned int) string[i].byte1 << 8) | string[i].byte2;
        bytes += utf8LengthForCodepoint(codepoint);
    }
    char *text = malloc(bytes + 1);
    if (!text)
        return NULL;
    char *out = text;
    for (int i = 0; i < count; i++) {
        unsigned int codepoint =
            ((unsigned int) string[i].byte1 << 8) | string[i].byte2;
        if (codepoint <= 0x7f) {
            *out++ = (char) codepoint;
        } else if (codepoint <= 0x7ff) {
            *out++ = (char) (0xc0 | (codepoint >> 6));
            *out++ = (char) (0x80 | (codepoint & 0x3f));
        } else {
            *out++ = (char) (0xe0 | (codepoint >> 12));
            *out++ = (char) (0x80 | ((codepoint >> 6) & 0x3f));
            *out++ = (char) (0x80 | (codepoint & 0x3f));
        }
    }
    *out = '\0';
    if (length)
        *length = bytes;
    return text;
}

int XTextWidth16(XFontStruct *font_struct, _Xconst XChar2b *string, int count)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XTextWidth16.html
    size_t length;
    char *text = decodeChar2bString(string, count, &length);
    if (!text) {
        LOG("Out of memory: Failed to allocate memory in %s! "
            "Returning max width of font.\n",
            __func__);
        return font_struct->max_bounds.rbearing * count;
    }
    int width =
        getTextWidthForChars(font_struct, text, count > 0 ? (size_t) count : 0);
    free(text);
    return width;
}

int XTextWidth(XFontStruct *font_struct, _Xconst char *string, int count)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XTextWidth.html
    char *text = decodeString(string, count);
    if (!text) {
        LOG("Out of memory: Failed to allocate memory in XTextWidth! "
            "Returning max width of font.\n");
        return font_struct->max_bounds.rbearing * count;
    }
    int width =
        getTextWidthForChars(font_struct, text, count > 0 ? (size_t) count : 0);
    free(text);
    return width;
}

/* Text-render cache. Motif's expose-driven repaints hit XDrawString with
 * identical (font, string, color) tuples per label/menu/button on every
 * frame; round-tripping through TTF_RenderUTF8_Blended +
 * SDL_CreateTextureFromSurface per call costs milliseconds and shows up
 * as visible jitter in panner/file-manager demos. The cache memoizes
 * the GPU texture per (font, foreground, renderer, string), with LRU
 * eviction so memory stays bounded. Entries are invalidated when their
 * renderer is destroyed (see destroyWindow paths). */
#define TEXT_CACHE_CAPACITY 128
#define TEXT_CACHE_MAX_STRLEN 256

typedef struct {
    Font fontXid;
    Uint32 foreground;
    SDL_Renderer *renderer;
    char *string;
    SDL_Texture *texture;
    int width;
    int height;
    int ascent;
    Uint64 lastUsed;
    Bool inUse;
} TextCacheEntry;

static TextCacheEntry textCache[TEXT_CACHE_CAPACITY];
static Uint64 textCacheClock = 0;
/* The cache is shared global state, but renderText, the
 * invalidate*ForRenderer / *ForFont helpers, and freeTextCache can be
 * called from multiple X Display threads concurrently. Without a lock
 * a concurrent LRU eviction could free a texture while another thread
 * is mid-SDL_RenderCopy on it; the in-use flag could also tear when
 * two threads race to claim the same empty slot. A single mutex
 * covering every entry into the cache is coarse but matches the cost
 * of a 128-slot linear walk and keeps the eviction lifecycle simple.
 *
 * Held during SDL_RenderCopy as well, so cache evictions cannot free
 * a texture out from under an in-flight draw. */
static SDL_mutex *textCacheMutex = NULL;
static SDL_SpinLock textCacheMutexInitLock = 0;

static SDL_mutex *textCacheEnsureMutex(void)
{
    if (!textCacheMutex) {
        SDL_AtomicLock(&textCacheMutexInitLock);
        if (!textCacheMutex)
            textCacheMutex = SDL_CreateMutex();
        SDL_AtomicUnlock(&textCacheMutexInitLock);
    }
    return textCacheMutex;
}

static void textCacheLock(void)
{
    SDL_mutex *m = textCacheEnsureMutex();
    if (m)
        SDL_LockMutex(m);
}

static void textCacheUnlock(void)
{
    if (textCacheMutex)
        SDL_UnlockMutex(textCacheMutex);
}

static void textCacheEvictEntry(TextCacheEntry *entry)
{
    if (entry->texture) {
        SDL_DestroyTexture(entry->texture);
        entry->texture = NULL;
    }
    free(entry->string);
    entry->string = NULL;
    entry->inUse = False;
}

void invalidateTextCacheForRenderer(SDL_Renderer *renderer)
{
    if (!renderer)
        return;
    textCacheLock();
    for (int i = 0; i < TEXT_CACHE_CAPACITY; i++) {
        if (textCache[i].inUse && textCache[i].renderer == renderer)
            textCacheEvictEntry(&textCache[i]);
    }
    textCacheUnlock();
}

void invalidateTextCacheForFont(Font fontXid)
{
    if (fontXid == None)
        return;
    textCacheLock();
    for (int i = 0; i < TEXT_CACHE_CAPACITY; i++) {
        if (textCache[i].inUse && textCache[i].fontXid == fontXid)
            textCacheEvictEntry(&textCache[i]);
    }
    textCacheUnlock();
}

void freeTextCache(void)
{
    textCacheLock();
    for (int i = 0; i < TEXT_CACHE_CAPACITY; i++) {
        if (textCache[i].inUse)
            textCacheEvictEntry(&textCache[i]);
    }
    textCacheUnlock();
}

static TextCacheEntry *textCacheLookup(Font fontXid,
                                       Uint32 foreground,
                                       SDL_Renderer *renderer,
                                       const char *string)
{
    for (int i = 0; i < TEXT_CACHE_CAPACITY; i++) {
        TextCacheEntry *e = &textCache[i];
        if (!e->inUse)
            continue;
        if (e->fontXid != fontXid || e->foreground != foreground ||
            e->renderer != renderer)
            continue;
        if (e->string && !strcmp(e->string, string)) {
            e->lastUsed = ++textCacheClock;
            return e;
        }
    }
    return NULL;
}

static TextCacheEntry *textCacheReserveSlot(void)
{
    /* Prefer an empty slot. Otherwise evict the LRU entry. */
    TextCacheEntry *victim = NULL;
    Uint64 victimAge = UINT64_MAX;
    for (int i = 0; i < TEXT_CACHE_CAPACITY; i++) {
        TextCacheEntry *e = &textCache[i];
        if (!e->inUse)
            return e;
        if (e->lastUsed < victimAge) {
            victimAge = e->lastUsed;
            victim = e;
        }
    }
    if (victim)
        textCacheEvictEntry(victim);
    return victim;
}

/* Returns True if the cache took ownership of `texture`. False if the
 * caller is responsible for destroying it (e.g., string too long or
 * out-of-memory key allocation). */
static Bool textCacheInsert(Font fontXid,
                            Uint32 foreground,
                            SDL_Renderer *renderer,
                            const char *string,
                            SDL_Texture *texture,
                            int width,
                            int height,
                            int ascent)
{
    if (!string || strlen(string) > TEXT_CACHE_MAX_STRLEN || !texture)
        return False;
    TextCacheEntry *e = textCacheReserveSlot();
    if (!e)
        return False;
    e->fontXid = fontXid;
    e->foreground = foreground;
    e->renderer = renderer;
    e->string = strdup(string);
    if (!e->string) {
        e->inUse = False;
        return False;
    }
    e->texture = texture;
    e->width = width;
    e->height = height;
    e->ascent = ascent;
    e->lastUsed = ++textCacheClock;
    e->inUse = True;
    return True;
}

Bool renderText(Display *display,
                Drawable drawable,
                SDL_Renderer *renderer,
                GC gc,
                int x,
                int y,
                const char *string)
{
    if (!string || string[0] == '\0') {
        return True;
    }
    LOG("Rendering text: '%s'\n", string);
    GraphicContext *gContext = GET_GC(gc);
    SDL_Color color = {
        GET_RED_FROM_COLOR(gContext->foreground),
        GET_GREEN_FROM_COLOR(gContext->foreground),
        GET_BLUE_FROM_COLOR(gContext->foreground),
        GET_ALPHA_FROM_COLOR(gContext->foreground),
    };
    if (gContext->font == None) {
        /* Lazily match Xlib's default-GC behavior: a GC without an explicit
         * font still draws with the server's fixed font. */
        gContext->font = XLoadFont(display, "fixed");
        if (gContext->font == None) {
            return False;
        }
        if (!compatFontRetainForGC(gContext->font)) {
            compatFontClose(display, gContext->font);
            gContext->font = None;
            return False;
        }
    }

    SDL_Texture *fontTexture = NULL;
    int textureWidth = 0;
    int textureHeight = 0;
    int textureAscent = 0;
    Bool textureOwned = False; /* True means caller frees. */
    /* Hold the cache lock across lookup, possible insert, and the
     * SDL_RenderCopy loop. Releasing earlier would let a concurrent
     * invalidate-eviction free the texture out from under us. */
    textCacheLock();
    TextCacheEntry *hit = textCacheLookup(
        gContext->font, (Uint32) gContext->foreground, renderer, string);
    if (hit) {
        fontTexture = hit->texture;
        textureWidth = hit->width;
        textureHeight = hit->height;
        textureAscent = hit->ascent;
    } else {
        SDL_Surface *fontSurface =
            TTF_RenderUTF8_Solid(GET_FONT(gContext->font), string, color);
        if (!fontSurface) {
            textCacheUnlock();
            return False;
        }
        textureWidth = fontSurface->w;
        textureHeight = fontSurface->h;
        fontTexture = SDL_CreateTextureFromSurface(renderer, fontSurface);
        SDL_FreeSurface(fontSurface);
        if (!fontTexture) {
            textCacheUnlock();
            return False;
        }
        textureAscent = TTF_FontAscent(GET_FONT(gContext->font));
        if (!textCacheInsert(gContext->font, (Uint32) gContext->foreground,
                             renderer, string, fontTexture, textureWidth,
                             textureHeight, textureAscent)) {
            textureOwned = True;
        }
    }

    SDL_Rect destR;
    destR.w = textureWidth;
    destR.h = textureHeight;
    destR.x = x;
    destR.y = y - textureAscent;
    ShapeGuard sg;
    shapeGuardBegin(&sg, drawable, renderer, &destR);
    int clipCount = getGcClipIterationCount(gc);
    Bool ok = True;
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (SDL_RenderCopy(renderer, fontTexture, NULL, &destR) != 0) {
            ok = False;
            break;
        }
    }
    clearRendererClip(renderer);
    shapeGuardEnd(&sg);
    textCacheUnlock();
    if (textureOwned)
        SDL_DestroyTexture(fontTexture);
    return ok;
}

static int drawImageString(Display *display,
                           Drawable drawable,
                           GC gc,
                           int x,
                           int y,
                           char *text)
{
    TYPE_CHECK(drawable, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (!text || text[0] == '\0') {
        return 1;
    }

    SDL_Renderer *renderer = NULL;
    GET_RENDERER(drawable, renderer);
    if (!renderer) {
        LOG("Failed to get the render target in %s\n", __func__);
        handleError(0, display, drawable, 0, BadDrawable, 0);
        return 0;
    }

    GraphicContext *gContext = GET_GC(gc);
    if (gContext->font == None) {
        gContext->font = XLoadFont(display, "fixed");
        if (gContext->font == None) {
            return 0;
        }
        if (!compatFontRetainForGC(gContext->font)) {
            compatFontClose(display, gContext->font);
            gContext->font = None;
            return 0;
        }
    }
    TTF_Font *font = GET_FONT(gContext->font);
    int width = 0;
    int height = 0;
    if (TTF_SizeUTF8(font, text, &width, &height) != 0) {
        XFontStruct *metrics = XQueryFont(display, gContext->font);
        width = metrics ? getTextWidth(metrics, text) : 0;
        freeFontStruct(metrics);
    }
    SDL_Rect background = {x, y - TTF_FontAscent(font), width,
                           TTF_FontAscent(font) + abs(TTF_FontDescent(font))};
    applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE, gContext->background);
    ShapeGuard sg;
    shapeGuardBegin(&sg, drawable, renderer, &background);
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (SDL_RenderFillRect(renderer, &background) != 0) {
            clearRendererClip(renderer);
            shapeGuardEnd(&sg);
            LOG("SDL_RenderFillRect failed in %s: %s\n", __func__,
                SDL_GetError());
            handleError(0, display, drawable, 0, BadMatch, 0);
            return 0;
        }
    }
    clearRendererClip(renderer);
    shapeGuardEnd(&sg);
    int result =
        renderText(display, drawable, renderer, gc, x, y, text) ? 1 : 0;
    if (result)
        presentDrawableIfVisible(drawable);
    return result;
}

int XDrawImageString(Display *display,
                     Drawable drawable,
                     GC gc,
                     int x,
                     int y,
                     _Xconst char *string,
                     int length)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing-text/XDrawImageString.html
    SET_X_SERVER_REQUEST(display, X_ImageText8);
    if (length == 0 || string[0] == '\0') {
        return 1;
    }
    char *text = decodeString(string, length);
    if (!text) {
        handleError(0, display, drawable, 0, BadAlloc, 0);
        return 0;
    }
    int result = drawImageString(display, drawable, gc, x, y, text);
    free(text);
    return result;
}

int XDrawImageString16(Display *display,
                       Drawable drawable,
                       GC gc,
                       int x,
                       int y,
                       _Xconst XChar2b *string,
                       int length)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing-text/XDrawImageString16.html
    SET_X_SERVER_REQUEST(display, X_ImageText16);
    if (length == 0 || ((Uint16 *) string)[0] == 0) {
        return 1;
    }
    size_t size;
    char *text = decodeChar2bString(string, length, &size);
    if (!text) {
        handleError(0, display, drawable, 0, BadAlloc, 0);
        return 0;
    }
    int result = drawImageString(display, drawable, gc, x, y, text);
    free(text);
    return result;
}

int XDrawString16(Display *display,
                  Drawable drawable,
                  GC gc,
                  int x,
                  int y,
                  _Xconst XChar2b *string,
                  int length)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing-text/XDrawString16.html
    SET_X_SERVER_REQUEST(display, X_PolyText16);
    LOG("%s: Drawing on %lu\n", __func__, drawable);
    TYPE_CHECK(drawable, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (length == 0 || ((Uint16 *) string)[0] == 0) {
        return 1;
    }
    SDL_Renderer *renderer;
    GET_RENDERER(drawable, renderer);
    if (!renderer) {
        LOG("Failed to get the render target in %s\n", __func__);
        handleError(0, display, None, 0, BadDrawable, 0);
        return 0;
    }
    size_t size;
    char *text = decodeChar2bString(string, length, &size);
    if (!text) {
        LOG("Out of memory: Failed to allocate memory in XDrawString16, "
            "raising BadMatch error.\n");
        handleError(0, display, drawable, 0, BadMatch, 0);
        return 0;
    }
    int res = 1;
    if (!renderText(display, drawable, renderer, gc, x, y, text)) {
        LOG("Rendering the text failed in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, drawable, 0, BadMatch, 0);
        free(text);
        res = 0;
    }
    free(text);
    if (res)
        presentDrawableIfVisible(drawable);
    return res;
}

int XDrawString(Display *display,
                Drawable drawable,
                GC gc,
                int x,
                int y,
                _Xconst char *string,
                int length)
{
    // https://tronche.com/gui/x/xlib/graphics/drawing-text/XDrawString.html
    SET_X_SERVER_REQUEST(display, X_PolyText8);
    LOG("%s: Drawing on %lu\n", __func__, drawable);
    TYPE_CHECK(drawable, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (length == 0 || string[0] == 0) {
        return 1;
    }
    SDL_Renderer *renderer;
    GET_RENDERER(drawable, renderer);
    if (!renderer) {
        LOG("Failed to get the render target in %s\n", __func__);
        handleError(0, display, None, 0, BadDrawable, 0);
        return 0;
    }
    char *text = decodeString(string, length);
    if (!text) {
        LOG("Out of memory: Failed to allocate decoded string in XDrawString, "
            "raising BadMatch error.\n");
        handleError(0, display, None, 0, BadMatch, 0);
        return 0;
    }
    int res = 1;
    if (!renderText(display, drawable, renderer, gc, x, y, text)) {
        LOG("Rendering the text failed in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, drawable, 0, BadMatch, 0);
        res = 0;
    }
    free(text);
    if (res)
        presentDrawableIfVisible(drawable);
    return res;
}
