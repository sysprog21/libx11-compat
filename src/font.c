#include "X11/Xatom.h"
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <strings.h>
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
#include "events.h"
#include "gc.h"
#include "util.h"
#include "font.h"

typedef struct {
    char *filePath;
    char *XLFName;
    Bool fixedWidth;
    Bool asciiMetrics;
} FontCacheEntry;

typedef struct {
    TTF_Font *ttf;
    unsigned int gcRefs;
    Bool closePending;
    Bool hasCoreMetrics;
    short coreAscent;
    short coreDescent;
    short coreWidth;
    Bool useFixedBitmap;
} CompatFont;

#define GET_FONT_RESOURCE(fontXID) ((CompatFont *) GET_XID_VALUE(fontXID))
#define GET_FONT(fontXID) \
    (GET_FONT_RESOURCE(fontXID) ? GET_FONT_RESOURCE(fontXID)->ttf : NULL)
#define DEFAULT_FONT_SIZE 10
#define MIN_FONT_SIZE 6
#define MAX_FONT_SIZE 72
#define ASCII_RENDER_PROBE_TEXT "Aa0 "
#define FIXED_BITMAP_WIDTH 6
#define FIXED_BITMAP_HEIGHT 13
/* Match the core metrics reported for Xvfb's "fixed" alias. The bundled 6x13
 * bitmap has 13 rows, so drawing it in an 11/2 box keeps every glyph pixel
 * inside the row extent clients use when clearing scrolled text.
 */
#define FIXED_BITMAP_ASCENT 11
#define FIXED_BITMAP_DESCENT 2
#define FIXED_BITMAP_CHAR_COUNT 128
/* Minimum decoded glyphs before the loader declares the BDF usable. ASCII
 * printable is 0x20..0x7E (95 glyphs); requiring 64 keeps a partially trimmed
 * file out of the renderer while still tolerating glyphs that are intentionally
 * blank (e.g. space).
 */
#define FIXED_BITMAP_MIN_GLYPHS 64

typedef struct {
    Bool loaded;
    Bool available;
    unsigned char rows[FIXED_BITMAP_CHAR_COUNT][FIXED_BITMAP_HEIGHT];
} FixedBitmapFont;

static FixedBitmapFont fixedBitmapFont;
static SDL_SpinLock fixedBitmapFontLock;

static void finishTextDamage(Display *display,
                             Drawable drawable,
                             const SDL_Rect *damage)
{
    if (damage && IS_TYPE(drawable, WINDOW))
        postExposeEventsForMappedChildren(display, drawable, damage, 1);
    presentDrawableRectIfVisible(drawable, damage);
}

/* Project-bundled "fonts" wins for self-contained checkouts; the remaining
 * entries let normal Xlib clients pick up host-system fonts without having to
 * call XSetFontPath. Missing directories are filtered out at scan time.
 */
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

// This are the custom search paths for fonts can be set via XSetFontPath. Has
// to be initialized via XSetFontPath(display, NULL, 0);
Array *fontSearchPaths = NULL;
Array *fontCache = NULL;

/* coreFontMetricsForName needs to consult loadFixedBitmapFont() (defined
 * further down) to decide whether to advertise the bitmap-font metrics.
 */
static Bool loadFixedBitmapFont(void);
static Array *fontAliasNames = NULL;

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
     * PIXEL_SIZE - POINT_SIZE - RESOLUTION_X - RESOLUTION_Y - SPACING -
     * AVERAGE_WIDTH - CHARSET_REGISTRY - CHARSET_ENCODING
     */
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
        /* Tk rejects the empty ADD_STYLE field form ("...-normal--0-...") for
         * these generated names, so keep the compact `...-normal-0-...` variant
         * while preserving charset registry/encoding fields.
         */
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

static Bool isSupportedFontFileName(const char *name)
{
    if (!name)
        return False;
    size_t len = strlen(name);
    static const char *const extensions[] = {".ttf", ".ttc", ".otf", ".dfont",
                                             ".bdf"};
    for (size_t i = 0; i < ARRAY_LENGTH(extensions); i++) {
        size_t extLen = strlen(extensions[i]);
        if (len <= extLen)
            continue;
        if (!strcasecmp(&name[len - extLen], extensions[i]))
            return True;
    }
    return False;
}

static Bool fontHasPositiveAdvanceForChar(TTF_Font *font, Uint16 ch)
{
    int minX;
    int maxX;
    int minY;
    int maxY;
    int advance;
    return TTF_GlyphIsProvided(font, ch) &&
           TTF_GlyphMetrics(font, ch, &minX, &maxX, &minY, &maxY, &advance) !=
               -1 &&
           advance > 0;
}

static Bool fontHasAsciiTextMetrics(TTF_Font *font)
{
    if (!fontHasPositiveAdvanceForChar(font, (Uint16) ' ') ||
        !fontHasPositiveAdvanceForChar(font, (Uint16) '0') ||
        !fontHasPositiveAdvanceForChar(font, (Uint16) 'A') ||
        !fontHasPositiveAdvanceForChar(font, (Uint16) 'a')) {
        return False;
    }

    SDL_Color black = {0, 0, 0, 255};
    SDL_Surface *surface =
        TTF_RenderUTF8_Solid(font, ASCII_RENDER_PROBE_TEXT, black);
    if (!surface)
        return False;
    Bool renderable = surface->w > 0 && surface->h > 0;
    SDL_FreeSurface(surface);
    return renderable;
}

Bool updateFontCache()
{
    size_t i;
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
            if (isSupportedFontFileName(entry->d_name)) {
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
                    fontCacheEntry->asciiMetrics =
                        fontHasAsciiTextMetrics(font);
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
                if ((size_t) index != fontCacheIndex)
                    swapArray(fontCache, (size_t) index, fontCacheIndex);
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

    if (!fontSearchPaths)
        return False;
    if (!initArray(fontSearchPaths, ARRAY_LENGTH(DEFAULT_FONT_SEARCH_PATHS)))
        return False;

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
                if (isSupportedFontFileName(entry->d_name))
                    fontCount++;
            }
            closedir(fontDirectory);
        }
    }

    fontCache = malloc(sizeof(Array));
    if (!fontCache)
        return False;
    if (!initArray(fontCache, fontCount))
        return False;
    fontAliasNames = malloc(sizeof(Array));
    if (!fontAliasNames) {
        freeArray(fontCache);
        free(fontCache);
        fontCache = NULL;
        return False;
    }
    if (!initArray(fontAliasNames, 0)) {
        free(fontAliasNames);
        fontAliasNames = NULL;
        freeArray(fontCache);
        free(fontCache);
        fontCache = NULL;
        return False;
    }
    return updateFontCache();
}

void freeFontStorage()
{
    /* Drop cached text textures before any renderer or font goes away;
     * destroyScreenWindow runs SDL_DestroyRenderer right after this call and
     * any retained texture from the cache would dangle.
     */
    freeTextCache();
    if (fontSearchPaths) {
        // Clear the array and free the data
        while (fontSearchPaths->length > 0)
            free(removeArray(fontSearchPaths, 0, False));
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
    if (fontAliasNames) {
        while (fontAliasNames->length > 0)
            free(removeArray(fontAliasNames, 0, False));
        freeArray(fontAliasNames);
        free(fontAliasNames);
        fontAliasNames = NULL;
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
            char a = p[i], b = needle[i];
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
    /* Discrete short aliases plus a few XLFD prefixes that real X11 clients
     * request when there is no font server. findFontCacheEntry ByName checks
     * the cache first, so an XLFD that does happen to be scanned in is honored
     * verbatim; this list is only the fallback after exact match misses.
     * -adobe-times- is not fixed-width, but x11perf requests it for TR10/TR24
     * tests and would otherwise abort on XOpenFont(None).
     */
    return !strcmp(name, "fixed") || !strcmp(name, "variable") ||
           !strcmp(name, "cursor") || !strcmp(name, "6x13") ||
           !strcmp(name, "7x13") || !strcmp(name, "7x13bold") ||
           !strcmp(name, "8x13") || !strcmp(name, "7x14") ||
           !strcmp(name, "9x13") || !strcmp(name, "9x15") ||
           !strcmp(name, "9x18") || !strcmp(name, "12x24") ||
           !strncmp(name, "-misc-fixed-", 12) ||
           !strncmp(name, "-jis-fixed-", 11) ||
           containsIgnoreCase(name, "helvetica") ||
           containsIgnoreCase(name, "helv") ||
           containsIgnoreCase(name, "courier") ||
           containsIgnoreCase(name, "lucidatypewriter") ||
           containsIgnoreCase(name, "adobe-times") ||
           containsIgnoreCase(name, "times") ||
           containsIgnoreCase(name, "schoolbook") ||
           (name[0] == '-' && containsIgnoreCase(name, "iso8859")) ||
           ((strstr(name, "-medium-r-") || strstr(name, "-bold-r-")) &&
            strstr(name, "-p-"));
}

/* Probe paths for a monospace face when the search-path scan does not pick one
 * up. For example, macOS keeps its fonts as .ttc which the .ttf-only scan
 * skips, and minimal Linux containers may have only freefont present in a
 * deeper subdirectory. The first path that TTF_OpenFont accepts is adopted into
 * the cache so subsequent XLoadFont calls hit the fast path.
 */
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
     * decipoints with the same default DPI: pixels = points * 100 / 72.
     */
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
        /* XLFD numeric fields are caller-controlled. Reject the multiply before
         * it can wrap, and saturate once the running value exceeds the caller's
         * cap. The remaining digits are drained without arithmetic so a bogus
         * trailing non-digit still flags the whole field as non-numeric.
         */
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

static int wildcardStylePixelSize(const char *name)
{
    static const char *const styleMarkers[] = {"-r-*-", "-i-*-", "-o-*-"};
    for (size_t i = 0; i < ARRAY_LENGTH(styleMarkers); i++) {
        const char *start = strstr(name, styleMarkers[i]);
        if (!start)
            continue;
        start += strlen(styleMarkers[i]);
        const char *end = strchr(start, '-');
        if (!end || end == start)
            continue;
        size_t len = (size_t) (end - start);
        if (len >= 16)
            continue;
        char buffer[16];
        int pixelSize = 0;
        memcpy(buffer, start, len);
        buffer[len] = '\0';
        if (parsePositiveInt(buffer, &pixelSize))
            return clampFontSize(pixelSize);
    }
    return 0;
}

static int requestedFontSize(const char *name)
{
    if (!name)
        return DEFAULT_FONT_SIZE;

    const char *x = strchr(name, 'x');
    if (x && x > name && x[1] != '\0') {
        char widthBuffer[16];
        char heightBuffer[16];
        size_t widthLen = (size_t) (x - name);
        const char *heightEnd = x + 1;
        while (*heightEnd >= '0' && *heightEnd <= '9')
            heightEnd++;
        size_t heightLen = (size_t) (heightEnd - (x + 1));
        int width = 0;
        int height = 0;
        if (widthLen < sizeof(widthBuffer) && heightLen > 0 &&
            heightLen < sizeof(heightBuffer)) {
            memcpy(widthBuffer, name, widthLen);
            widthBuffer[widthLen] = '\0';
            memcpy(heightBuffer, x + 1, heightLen);
            heightBuffer[heightLen] = '\0';
            if (parsePositiveInt(widthBuffer, &width) &&
                parsePositiveInt(heightBuffer, &height)) {
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

    int wildcardPixelSize = wildcardStylePixelSize(name);
    if (wildcardPixelSize)
        return wildcardPixelSize;

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
                 * overflow, then let clampFontSize cap the pixel result.
                 */
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

/* Match only the exact Motif fallback XLFD shape
 * "-<foundry>-times-medium-r-normal--14-...-iso8859-1[-encoding]". Substring
 * matches on "times" and "iso8859-1" were too permissive: legitimate
 * proportional Times-Bold-Italic 14pt requests got rerouted to the 6x13
 * fixed-width font. Anchor on the dashed field markers so non-medium,
 * non-roman, non-normal, or wildcarded foundry-but-not-times XLFDs miss.
 */
static Bool isMotifTimesIso14Fallback(const char *name)
{
    if (!name)
        return False;
    if (!containsIgnoreCase(name, "-times-medium-r-normal-"))
        return False;
    if (!containsIgnoreCase(name, "-iso8859-1"))
        return False;
    return requestedFontSize(name) == 14;
}

static Bool coreFontMetricsForName(const char *name,
                                   short *ascent,
                                   short *descent,
                                   short *width)
{
    if (!name)
        return False;
    /* Xvfb's built-in "fixed" resolves to a 6x13 bitmap font: ascent=11,
     * descent=2, width=6. Motif uses this alias in fallback fontLists;
     * reporting smaller TTF metrics lets widgets pack too many text rows
     * compared with native libX11.
     */
    /* Only advertise bitmap-font metrics when the BDF actually loaded;
     * otherwise the renderer falls back to TTF and the layout engine would lay
     * out at 11/2 while glyphs draw at TTF size, overlapping adjacent lines.
     */
    Bool haveBitmap = loadFixedBitmapFont();
    if (haveBitmap && (!strcmp(name, "fixed") || !strcmp(name, "cursor") ||
                       !strcmp(name, "6x13"))) {
        *ascent = 11;
        *descent = 2;
        *width = 6;
        return True;
    }
    if (haveBitmap && (!strcmp(name, "7x13") || !strcmp(name, "7x13bold"))) {
        *ascent = 11;
        *descent = 2;
        *width = 7;
        return True;
    }
    if (haveBitmap && !strcmp(name, "7x14")) {
        *ascent = 12;
        *descent = 2;
        *width = 7;
        return True;
    }
    if (haveBitmap && !strcmp(name, "9x18")) {
        *ascent = 14;
        *descent = 4;
        *width = 9;
        return True;
    }
    if (haveBitmap && !strcmp(name, "12x24")) {
        *ascent = 22;
        *descent = 2;
        *width = 12;
        return True;
    }
    /* thentenaar/motif's hellomotifi18n demo asks Mrm for
     * -*-times-medium-r-normal--14-*-iso8859-1. The native Xvfb baseline on
     * node11 does not have that font and Motif falls back to the default core
     * font. Match that geometry only when the BDF can actually render to it;
     * without BDF, the answer would lie to the layout engine.
     */
    if (haveBitmap && isMotifTimesIso14Fallback(name)) {
        *ascent = 11;
        *descent = 2;
        *width = 6;
        return True;
    }
    /* XLFD family names are case-insensitive per the X11 spec, so "-Helvetica-"
     * / "-HELVETICA-" must take the same override path as the lowercase form.
     */
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

static Bool usesFixedFallbackFont(const char *name)
{
    if (!name)
        return False;
    if (!strcmp(name, "fixed") || !strcmp(name, "cursor") ||
        !strcmp(name, "6x13"))
        return True;
    return isMotifTimesIso14Fallback(name);
}

/* Treat as a bitmap row only when the line is a single hex token of the
 * expected 1-byte width. Rejects COMMENT lines that happen to fall inside a
 * BITMAP block and tails of fgets-truncated long lines, both of which would
 * otherwise be misread as glyph pixel data.
 */
static Bool isFixedBitmapRowLine(const char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;
    int hexCount = 0;
    while (line[hexCount] && isxdigit((unsigned char) line[hexCount]))
        hexCount++;
    if (hexCount == 0 || hexCount > 2)
        return False;
    for (const char *p = line + hexCount; *p; p++) {
        if (*p != '\r' && *p != '\n' && *p != ' ' && *p != '\t')
            return False;
    }
    return True;
}

#include "font-6x13-bitmap.h"

static void useEmbeddedFixedBitmap(void)
{
    /* The renderer reads fixedBitmapFont.rows directly, so a one-shot memcpy
     * from the generated table is enough. Same width / height / char-count
     * contract as the BDF reader below.
     */
    memcpy(fixedBitmapFont.rows, EMBEDDED_FIXED_BITMAP_ROWS,
           sizeof(EMBEDDED_FIXED_BITMAP_ROWS));
    fixedBitmapFont.available = True;
    fixedBitmapFont.loaded = True;
}

static Bool loadFixedBitmapFont(void)
{
    SDL_AtomicLock(&fixedBitmapFontLock);
    if (fixedBitmapFont.loaded) {
        Bool result = fixedBitmapFont.available;
        SDL_AtomicUnlock(&fixedBitmapFontLock);
        return result;
    }

    /* Prefer an external BDF when LIBX11_COMPAT_FONT_DIR points at one; this
     * lets a downstream user swap in a different fixed font without
     * recompiling. Otherwise fall through to the in-tree embedded font
     * generated from 6x13.bdf so CI hosts and packagers do not need to stage a
     * separate data file.
     */
    char path[PATH_MAX];
    const char *fontDir = getenv("LIBX11_COMPAT_FONT_DIR");
    FILE *fp = NULL;
    if (fontDir && fontDir[0] != '\0') {
        snprintf(path, sizeof(path), "%s/6x13.bdf", fontDir);
        fp = fopen(path, "r");
    }
    if (!fp) {
        useEmbeddedFixedBitmap();
        SDL_AtomicUnlock(&fixedBitmapFontLock);
        return True;
    }

    int currentEncoding = -1;
    int bitmapRow = -1;
    int glyphsDecoded = 0;
    Bool glyphHasRows = False;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (!strncmp(line, "STARTCHAR ", 10)) {
            currentEncoding = -1;
            bitmapRow = -1;
            glyphHasRows = False;
            continue;
        }
        if (!strncmp(line, "ENCODING ", 9)) {
            /* strtol over atoi so a malformed BDF line cannot quietly decode to
             * 0; .ci/check-security.sh also bans atoi.
             */
            char *end = NULL;
            long enc = strtol(&line[9], &end, 10);
            currentEncoding =
                end != &line[9] && enc >= 0 && enc <= INT_MAX ? (int) enc : -1;
            continue;
        }
        if (!strncmp(line, "BITMAP", 6)) {
            Bool encodingInRange = currentEncoding >= 0 &&
                                   currentEncoding < FIXED_BITMAP_CHAR_COUNT;
            bitmapRow = encodingInRange ? 0 : -1;
            continue;
        }
        if (!strncmp(line, "ENDCHAR", 7)) {
            if (glyphHasRows)
                glyphsDecoded++;
            bitmapRow = -1;
            glyphHasRows = False;
            continue;
        }
        if (bitmapRow >= 0 && bitmapRow < FIXED_BITMAP_HEIGHT &&
            isFixedBitmapRowLine(line)) {
            unsigned long bits = strtoul(line, NULL, 16);
            fixedBitmapFont.rows[currentEncoding][bitmapRow++] =
                (unsigned char) bits;
            glyphHasRows = True;
        }
    }
    fclose(fp);
    fixedBitmapFont.available = glyphsDecoded >= FIXED_BITMAP_MIN_GLYPHS;
    fixedBitmapFont.loaded = True;
    Bool result = fixedBitmapFont.available;
    SDL_AtomicUnlock(&fixedBitmapFontLock);
    return result;
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
    entry->asciiMetrics = fontHasAsciiTextMetrics(font);
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
        if (entry && entry->asciiMetrics)
            return entry;
    }
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (!entry->fixedWidth && entry->asciiMetrics)
            return entry;
    }
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (entry->asciiMetrics)
            return entry;
    }
    return NULL;
}

static FontCacheEntry *findAliasedFixedWidthFont(void)
{
    for (size_t i = 0; i < ARRAY_LENGTH(MONOSPACE_PROBE_PATHS); i++) {
        FontCacheEntry *entry = adoptProbePath(MONOSPACE_PROBE_PATHS[i]);
        if (entry && entry->asciiMetrics)
            return entry;
    }
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (entry->fixedWidth && entry->asciiMetrics)
            return entry;
    }
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (entry->asciiMetrics)
            return entry;
    }
    return NULL;
}

static FontCacheEntry *findAliasedFontForName(const char *name)
{
    if (usesFixedFallbackFont(name))
        return findAliasedFixedWidthFont();
    if (!strcmp(name, "variable"))
        return findProbeFont(SANS_PROBE_PATHS, ARRAY_LENGTH(SANS_PROBE_PATHS));
    if (containsIgnoreCase(name, "times") ||
        containsIgnoreCase(name, "adobe-times") ||
        containsIgnoreCase(name, "schoolbook"))
        return findProbeFont(SERIF_PROBE_PATHS,
                             ARRAY_LENGTH(SERIF_PROBE_PATHS));
    if (containsIgnoreCase(name, "helvetica") ||
        containsIgnoreCase(name, "helv") ||
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
    if (containsIgnoreCase(name, "bold"))
        return findProbeFont(SANS_BOLD_PROBE_PATHS,
                             ARRAY_LENGTH(SANS_BOLD_PROBE_PATHS));
    return findAliasedFixedWidthFont();
}

static Bool fontCanRenderText(TTF_Font *font)
{
    if (!font)
        return False;
    SDL_Color black = {0, 0, 0, 255};
    SDL_Surface *surface =
        TTF_RenderUTF8_Solid(font, ASCII_RENDER_PROBE_TEXT, black);
    if (!surface)
        return False;
    Bool renderable = surface->w > 0 && surface->h > 0;
    SDL_FreeSurface(surface);
    return renderable;
}

static TTF_Font *openRenderableProbeFont(const char *const *paths,
                                         size_t count,
                                         int size,
                                         const char *skipPath)
{
    for (size_t i = 0; i < count; i++) {
        if (skipPath && !strcmp(paths[i], skipPath))
            continue;
        TTF_Font *font = TTF_OpenFont(paths[i], size);
        if (!font)
            continue;
        if (fontCanRenderText(font))
            return font;
        TTF_CloseFont(font);
    }
    return NULL;
}

static TTF_Font *openRenderableFallbackFont(const char *name,
                                            int size,
                                            const char *skipPath)
{
    TTF_Font *font = NULL;
    if (containsIgnoreCase(name, "times") ||
        containsIgnoreCase(name, "adobe-times") ||
        containsIgnoreCase(name, "schoolbook")) {
        font = openRenderableProbeFont(
            SERIF_PROBE_PATHS, ARRAY_LENGTH(SERIF_PROBE_PATHS), size, skipPath);
        if (font)
            return font;
    }
    if (containsIgnoreCase(name, "helvetica") ||
        containsIgnoreCase(name, "helv") ||
        containsIgnoreCase(name, "lucida") ||
        containsIgnoreCase(name, "arial") ||
        ((strstr(name, "-medium-r-") || strstr(name, "-bold-r-")) &&
         strstr(name, "-p-"))) {
        if (containsIgnoreCase(name, "bold")) {
            font = openRenderableProbeFont(SANS_BOLD_PROBE_PATHS,
                                           ARRAY_LENGTH(SANS_BOLD_PROBE_PATHS),
                                           size, skipPath);
            if (font)
                return font;
        }
        font = openRenderableProbeFont(
            SANS_PROBE_PATHS, ARRAY_LENGTH(SANS_PROBE_PATHS), size, skipPath);
        if (font)
            return font;
    }
    font = openRenderableProbeFont(MONOSPACE_PROBE_PATHS,
                                   ARRAY_LENGTH(MONOSPACE_PROBE_PATHS), size,
                                   skipPath);
    if (font)
        return font;
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (skipPath && !strcmp(entry->filePath, skipPath))
            continue;
        if (!entry->asciiMetrics)
            continue;
        font = TTF_OpenFont(entry->filePath, size);
        if (!font)
            continue;
        if (fontCanRenderText(font))
            return font;
        TTF_CloseFont(font);
    }
    return NULL;
}

/* Public entry point that mirrors the per-family probe chain used by
 * XLoadQueryFont fallbacks (sans / sans-bold / serif / monospace). The Xft shim
 * routes through this so anti-aliased text rendered by client code lands on the
 * same TTF file the core font path would have picked.
 */
struct TTF_Font *compatFontOpenFamilyFallback(const char *familyHint, int size)
{
    const char *name = familyHint ? familyHint : "sans";
    return (struct TTF_Font *) openRenderableFallbackFont(
        name, clampFontSize(size), NULL);
}

static FontCacheEntry *findFontCacheEntryByName(const char *name)
{
    /* Exact cache match wins so a real scanned font is never shadowed by an
     * alias prefix. Aliases only kick in when no entry matches.
     */
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (!strcmp(entry->XLFName, name))
            return entry;
    }
    if (strchr(name, '*') || strchr(name, '?')) {
        Bool aliasRequest = isFontAlias(name);
        if (aliasRequest) {
            FontCacheEntry *fallback = findAliasedFontForName(name);
            if (fallback)
                return fallback;
        }
        for (size_t i = 0; i < fontCache->length; i++) {
            FontCacheEntry *entry = fontCache->array[i];
            if (aliasRequest && !entry->asciiMetrics)
                continue;
            if (matchWildcard(name, entry->XLFName))
                return entry;
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
    /* Use the unified lookup so XLoadFont and the rest of the font code agree
     * on cache-first, alias-second ordering. Open-coding the alias check here
     * used to shadow exact XLFD matches.
     */
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
    int fontSize = requestedFontSize(name);
    resource->ttf = TTF_OpenFont(fontEntry->filePath, fontSize);
    if (!resource->ttf) {
        free(resource);
        FREE_XID(font);
        LOG("Failed to load font %s!\n", name);
        handleError(0, display, None, 0, BadName, 0);
        return None;
    }
    if (!fontCanRenderText(resource->ttf)) {
        TTF_Font *fallback =
            openRenderableFallbackFont(name, fontSize, fontEntry->filePath);
        if (fallback) {
            TTF_CloseFont(resource->ttf);
            resource->ttf = fallback;
        }
    }
    resource->hasCoreMetrics =
        coreFontMetricsForName(name, &resource->coreAscent,
                               &resource->coreDescent, &resource->coreWidth);
    resource->useFixedBitmap = usesFixedFallbackFont(name);
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

static Bool stringArrayContains(const Array *array, const char *string)
{
    if (!array || !string)
        return False;
    for (size_t i = 0; i < array->length; i++) {
        const char *entry = array->array[i];
        if (entry && !strcmp(entry, string))
            return True;
    }
    return False;
}

static char *internAliasFontName(const char *name)
{
    if (!name || !fontAliasNames)
        return NULL;
    for (size_t i = 0; i < fontAliasNames->length; i++) {
        char *cached = fontAliasNames->array[i];
        if (cached && !strcmp(cached, name))
            return cached;
    }
    char *copy = strdup(name);
    if (!copy)
        return NULL;
    if (!insertArray(fontAliasNames, copy)) {
        free(copy);
        return NULL;
    }
    return copy;
}

static const char *aliasFamilyForPattern(const char *pattern)
{
    if (containsIgnoreCase(pattern, "times"))
        return "Times";
    if (containsIgnoreCase(pattern, "helvetica") ||
        containsIgnoreCase(pattern, "helv"))
        return "Helvetica";
    if (containsIgnoreCase(pattern, "courier"))
        return "Courier";
    if (containsIgnoreCase(pattern, "fixed") ||
        containsIgnoreCase(pattern, "jis-fixed"))
        return "Fixed";
    if (!strcmp(pattern, "variable"))
        return "Helvetica";
    return "Fixed";
}

static char aliasSlantForPattern(const char *pattern)
{
    if (containsIgnoreCase(pattern, "-i-") ||
        containsIgnoreCase(pattern, "-o-"))
        return 'i';
    return 'r';
}

static char aliasSpacingForPattern(const char *pattern)
{
    if (containsIgnoreCase(pattern, "fixed") ||
        containsIgnoreCase(pattern, "courier") || !strcmp(pattern, "fixed") ||
        !strcmp(pattern, "cursor"))
        return 'm';
    return 'p';
}

static char *concreteAliasNameForPattern(const char *pattern)
{
    if (!pattern)
        return NULL;
    if (!strchr(pattern, '*') && !strchr(pattern, '?'))
        return internAliasFontName(pattern);
    const char *family = aliasFamilyForPattern(pattern);
    const char *weight =
        containsIgnoreCase(pattern, "bold") ? "bold" : "medium";
    char slant = aliasSlantForPattern(pattern);
    char spacing = aliasSpacingForPattern(pattern);
    int pixelSize = requestedFontSize(pattern);
    char name[256];
    snprintf(name, sizeof(name),
             "-compat-%s-%s-%c-normal--%d-0-0-0-%c-0-iso10646-1", family,
             weight, slant, pixelSize, spacing);
    return internAliasFontName(name);
}

static Bool aliasPatternNeedsSyntheticName(const char *pattern)
{
    return containsIgnoreCase(pattern, "times") ||
           containsIgnoreCase(pattern, "helvetica") ||
           containsIgnoreCase(pattern, "helv") ||
           containsIgnoreCase(pattern, "arial") ||
           containsIgnoreCase(pattern, "lucida") ||
           ((strstr(pattern, "-medium-r-") || strstr(pattern, "-bold-r-")) &&
            strstr(pattern, "-p-"));
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
    if (actual_count_return)
        *actual_count_return = 0;
    if (!actual_count_return || maxnames <= 0)
        return NULL;
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
    Bool aliasPattern = isFontAlias(pattern);
    if (aliasPattern && (int) names.length < maxnames) {
        FontCacheEntry *aliased = findAliasedFontForName(pattern);
        char *aliasName = aliasPatternNeedsSyntheticName(pattern)
                              ? concreteAliasNameForPattern(pattern)
                              : (aliased ? aliased->XLFName : NULL);
        if (aliased && aliasName && !stringArrayContains(&names, aliasName))
            insertArray(&names, aliasName);
    }
    size_t i;
    for (i = 0; i < fontCache->length && (int) names.length < maxnames; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (aliasPattern && !entry->asciiMetrics)
            continue;
        char *name = entry->XLFName;
        if (aliasPattern && stringArrayContains(&names, name))
            continue;
        if (matchWildcard(pattern, name))
            insertArray(&names, name);
    }
    /* Resolve aliases to a stable cache-owned name. Returning the caller's
     * pattern would alias caller-owned memory and could embed wildcards in the
     * result list.
     */
    if (names.length == 0 && isFontAlias(pattern)) {
        FontCacheEntry *aliased = findAliasedFontForName(pattern);
        char *aliasName = aliasPatternNeedsSyntheticName(pattern)
                              ? concreteAliasNameForPattern(pattern)
                              : (aliased ? aliased->XLFName : NULL);
        if (aliased && aliasName)
            insertArray(&names, aliasName);
    }
    *actual_count_return = (int) names.length;
    if (names.length == 0)
        return NULL;
    /* Copy names into caller-owned storage. Returning raw cache or alias-intern
     * pointers would dangle after XSetFontPath rebuilds the font cache, so each
     * entry is duplicated. The trailing NULL lets XFreeFontNames walk the list
     * without needing a separate count.
     */
    char **list = malloc(sizeof(char *) * (names.length + 1));
    if (!list) {
        freeArray(&names);
        *actual_count_return = 0;
        return NULL;
    }
    for (size_t k = 0; k < names.length; k++) {
        list[k] = strdup((const char *) names.array[k]);
        if (!list[k]) {
            for (size_t f = 0; f < k; f++)
                free(list[f]);
            free(list);
            freeArray(&names);
            *actual_count_return = 0;
            return NULL;
        }
    }
    list[names.length] = NULL;
    freeArray(&names);
    return list;
}

int XFreeFontNames(char **list)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XFreeFontNames.html
    if (!list)
        return 1;
    for (char **p = list; *p; p++)
        free(*p);
    free(list);
    return 1;
}

static void freeFontStructContents(XFontStruct *font_struct)
{
    if (!font_struct)
        return;
    free(font_struct->properties);
    free(font_struct->per_char);
    font_struct->properties = NULL;
    font_struct->per_char = NULL;
}

static void freeFontStruct(XFontStruct *font_struct)
{
    if (!font_struct)
        return;
    freeFontStructContents(font_struct);
    free(font_struct);
}

int XFreeFont(Display *display, XFontStruct *font_struct)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XFreeFont.html
    SET_X_SERVER_REQUEST(display, X_CloseFont);
    if (!font_struct)
        return 1;
    if (font_struct->fid != None)
        compatFontClose(display, font_struct->fid);
    freeFontStruct(font_struct);
    return 1;
}

int XFreeFontInfo(char **names, XFontStruct *free_info, int actual_count)
{
    if (names)
        XFreeFontNames(names);
    if (free_info) {
        for (int i = 0; i < actual_count; i++)
            freeFontStructContents(free_info + i);
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
    if (charStruct->lbearing < fontStruct->min_bounds.lbearing)
        fontStruct->min_bounds.lbearing = charStruct->lbearing;
    if (charStruct->rbearing < fontStruct->min_bounds.rbearing)
        fontStruct->min_bounds.rbearing = charStruct->rbearing;
    if (charStruct->width < fontStruct->min_bounds.width)
        fontStruct->min_bounds.width = charStruct->width;
    if (charStruct->ascent < fontStruct->min_bounds.ascent)
        fontStruct->min_bounds.ascent = charStruct->ascent;
    if (charStruct->descent < fontStruct->min_bounds.descent)
        fontStruct->min_bounds.descent = charStruct->descent;

    if (charStruct->lbearing > fontStruct->max_bounds.lbearing)
        fontStruct->max_bounds.lbearing = charStruct->lbearing;
    if (charStruct->rbearing > fontStruct->max_bounds.rbearing)
        fontStruct->max_bounds.rbearing = charStruct->rbearing;
    if (charStruct->width > fontStruct->max_bounds.width)
        fontStruct->max_bounds.width = charStruct->width;
    if (charStruct->ascent > fontStruct->max_bounds.ascent)
        fontStruct->max_bounds.ascent = charStruct->ascent;
    if (charStruct->descent > fontStruct->max_bounds.descent)
        fontStruct->max_bounds.descent = charStruct->descent;
}

static short fallbackPrintableWidth(const XFontStruct *fontStruct)
{
    if (fontStruct->per_char && 'n' >= fontStruct->min_char_or_byte2 &&
        'n' <= fontStruct->max_char_or_byte2) {
        short nWidth =
            fontStruct->per_char['n' - fontStruct->min_char_or_byte2].width;
        if (nWidth > 0)
            return nWidth;
    }
    if (fontStruct->max_bounds.width > 0)
        return fontStruct->max_bounds.width;
    if (fontStruct->min_bounds.width > 0)
        return fontStruct->min_bounds.width;
    short halfHeight = (short) ((fontStruct->ascent + fontStruct->descent) / 2);
    return halfHeight > 0 ? halfHeight : 1;
}

static void normalizePrintableAsciiWidths(XFontStruct *fontStruct)
{
    short fallback = fallbackPrintableWidth(fontStruct);
    if (fontStruct->per_char) {
        for (unsigned int ch = ' '; ch <= '~'; ch++) {
            if (ch < fontStruct->min_char_or_byte2 ||
                ch > fontStruct->max_char_or_byte2) {
                continue;
            }
            XCharStruct *charStruct =
                &fontStruct->per_char[ch - fontStruct->min_char_or_byte2];
            if (charStruct->width <= 0) {
                charStruct->width = fallback;
                if (charStruct->rbearing <= charStruct->lbearing)
                    charStruct->rbearing = fallback;
                updateBounds(fontStruct, charStruct);
            }
        }
    } else if (fontStruct->min_bounds.width <= 0) {
        fontStruct->min_bounds.width = fallback;
        fontStruct->max_bounds.width = fallback;
        fontStruct->min_bounds.rbearing = fallback;
        fontStruct->max_bounds.rbearing = fallback;
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
        short fallback =
            (short) ((fontStruct->ascent + fontStruct->descent) / 2);
        if (fallback <= 0)
            fallback = 1;
        fontStruct->min_bounds.width = fallback;
        fontStruct->min_bounds.lbearing = 0;
        fontStruct->min_bounds.rbearing = fallback;
        fontStruct->min_bounds.ascent = fontStruct->ascent;
        fontStruct->min_bounds.descent = fontStruct->descent;
        fontStruct->max_bounds = fontStruct->min_bounds;
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
    normalizePrintableAsciiWidths(fontStruct);
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
    if (actual_count_return)
        *actual_count_return = 0;
    if (info_return)
        *info_return = NULL;
    if (!actual_count_return || !info_return)
        return NULL;

    char **names = XListFonts(display, pattern, maxnames, actual_count_return);
    if (!names || *actual_count_return == 0)
        return names;

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
        if (!entry)
            continue;
        TTF_Font *font =
            TTF_OpenFont(entry->filePath, requestedFontSize(names[i]));
        if (!font)
            continue;
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
    FontCacheEntry *cacheHit = findFontCacheEntryByName(name);
    if (!cacheHit) {
        LOG("XLoadQueryFont MISS: %s\n", name ? name : "(null)");
        return NULL;
    }
    LOG("XLoadQueryFont HIT: req='%s' -> XLF='%s' file='%s'\n",
        name ? name : "(null)",
        cacheHit->XLFName ? cacheHit->XLFName : "(null)",
        cacheHit->filePath ? cacheHit->filePath : "(null)");
    Font fontId = XLoadFont(display, name);
    if (fontId == None)
        return NULL;
    XFontStruct *fontStruct = XQueryFont(display, fontId);
    if (!fontStruct)
        compatFontClose(display, fontId);
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

/* XDrawString and friends take a length-bounded buffer that may or may not be
 * NUL-terminated. Downstream SDL_ttf calls expect a C string, so decodeString
 * copies "count" bytes into a fresh allocation and NUL-terminates. The payload
 * is rendered as-is: real Xlib treats every byte as a glyph index into the
 * font, so escape interpretation (\n, \xHH, ...) would silently mangle
 * legitimate Latin-1/file-path input.
 */
char *decodeString(const char *string, int count)
{
    if (!string || count < 0)
        return NULL;
    char *text = malloc((size_t) count + 1);
    if (!text)
        return NULL;
    if (count > 0)
        memcpy(text, string, (size_t) count);
    text[count] = '\0';
    return text;
}

static int getTextWidthForChars(XFontStruct *font_struct,
                                const char *string,
                                size_t fixedCharCount,
                                size_t byteLength)
{
    if (!font_struct || !string)
        return 0;
    if (!font_struct->per_char &&
        font_struct->min_bounds.width == font_struct->max_bounds.width) {
        /* Promote to 64-bit so a wide font times a long string can't wrap the
         * int return; clamp at INT_MAX on overflow.
         */
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
                          (int64_t) fixedCharCount;
        return product > INT_MAX ? INT_MAX : (int) product;
    }
    if (strlen(string) != byteLength) {
        int64_t product = (int64_t) font_struct->max_bounds.rbearing *
                          (int64_t) fixedCharCount;
        return product > INT_MAX ? INT_MAX : (int) product;
    }
    return width;
}

int getTextWidth(XFontStruct *font_struct, const char *string)
{
    if (!string)
        return 0;
    size_t length = strlen(string);
    return getTextWidthForChars(font_struct, string, length, length);
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
    /* Reject negative counts and bound the buffer so the worst-case 3 bytes per
     * codepoint plus the NUL cannot wrap size_t on 32-bit.
     */
    if (!string || count < 0)
        return NULL;
    if ((size_t) count > (SIZE_MAX - 1) / 3)
        return NULL;
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

static char *makeMissingGlyphFallbackText(int count)
{
    if (count < 0)
        return NULL;
    char *text = malloc((size_t) count + 1);
    if (!text)
        return NULL;
    for (int i = 0; i < count; i++)
        text[i] = '?';
    text[count] = '\0';
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
    int width = getTextWidthForChars(font_struct, text,
                                     count > 0 ? (size_t) count : 0, length);
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
        getTextWidthForChars(font_struct, text, count > 0 ? (size_t) count : 0,
                             count > 0 ? (size_t) count : 0);
    free(text);
    return width;
}

/* Text-render cache. Motif's expose-driven repaints hit XDrawString with
 * identical (font, string, color) tuples per label/menu/button on every frame;
 * round-tripping through TTF_RenderUTF8_Blended + SDL_CreateTextureFromSurface
 * per call costs milliseconds and shows up as visible jitter in
 * panner/file-manager demos. The cache memoizes the GPU texture per (font,
 * foreground, renderer, string), with LRU eviction so memory stays bounded.
 * Entries are invalidated when their renderer is destroyed (see destroyWindow
 * paths).
 */
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
/* The cache is shared global state, but renderText, the invalidate*ForRenderer
 * / *ForFont helpers, and freeTextCache can be called from multiple X Display
 * threads concurrently. Without a lock a concurrent LRU eviction could free a
 * texture while another thread is mid-SDL_RenderCopy on it; the in-use flag
 * could also tear when two threads race to claim the same empty slot. A single
 * mutex covering every entry into the cache is coarse but matches the cost of a
 * 128-slot linear walk and keeps the eviction lifecycle simple.
 *
 * Held during SDL_RenderCopy as well, so cache evictions cannot free a texture
 * out from under an in-flight draw.
 */
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

static Bool fixedBitmapCanRenderText(const char *string, size_t length)
{
    /* Length-counted: a 16-bit request with a {0,0} XChar2b decodes to a NUL
     * byte mid-buffer that must still render as glyph 0, not end the scan.
     */
    const unsigned char *bytes = (const unsigned char *) string;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] >= FIXED_BITMAP_CHAR_COUNT)
            return False;
    }
    return True;
}

static Bool flushFixedBitmapRects(SDL_Renderer *renderer,
                                  SDL_Rect *rects,
                                  int *count)
{
    if (*count == 0)
        return True;
    if (SDL_RenderFillRects(renderer, rects, *count) != 0)
        return False;
    *count = 0;
    return True;
}

static Bool renderFixedBitmapText(Drawable drawable,
                                  SDL_Renderer *renderer,
                                  GC gc,
                                  int x,
                                  int y,
                                  const char *string,
                                  size_t length,
                                  SDL_Rect *drawnBounds)
{
    if (!loadFixedBitmapFont() || !fixedBitmapCanRenderText(string, length))
        return False;

    GraphicContext *gContext = GET_GC(gc);
    unsigned long foreground = colorWithOpaqueDefault(gContext->foreground);
    applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE, foreground);

    /* Cap so (int)(len * FIXED_BITMAP_WIDTH) cannot overflow SDL_Rect's int
     * width. Pathological inputs would otherwise wrap negative and either skip
     * clipping or paint outside the intended damage area.
     */
    size_t len = length;
    if (len > (size_t) (INT_MAX / FIXED_BITMAP_WIDTH))
        len = (size_t) (INT_MAX / FIXED_BITMAP_WIDTH);
    SDL_Rect bounds = {x, y - FIXED_BITMAP_ASCENT,
                       (int) (len * FIXED_BITMAP_WIDTH), FIXED_BITMAP_HEIGHT};
    if (drawnBounds)
        *drawnBounds = bounds;
    ShapeGuard sg;
    shapeGuardBegin(&sg, drawable, renderer, &bounds);

    int clipCount = getGcClipIterationCount(gc, drawable);
    Bool ok = True;
    for (int clip = 0; clip < clipCount && ok; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, drawable))
            continue;

        SDL_Rect rects[512];
        int rectCount = 0;
        for (size_t i = 0; i < len && ok; i++) {
            const unsigned char ch = (unsigned char) string[i];
            int charX = x + (int) i * FIXED_BITMAP_WIDTH;
            for (int row = 0; row < FIXED_BITMAP_HEIGHT && ok; row++) {
                unsigned char bits = fixedBitmapFont.rows[ch][row];
                /* Pack consecutive set bits on this row into a single SDL_Rect
                 * with w>1. A glyph with full ink on a row used to emit 6
                 * rects; now it emits 1.
                 */
                int col = 0;
                while (col < FIXED_BITMAP_WIDTH && ok) {
                    if (!(bits & (0x80 >> col))) {
                        col++;
                        continue;
                    }
                    int runStart = col;
                    do {
                        col++;
                    } while (col < FIXED_BITMAP_WIDTH &&
                             (bits & (0x80 >> col)));
                    rects[rectCount++] = (SDL_Rect) {
                        charX + runStart, bounds.y + row, col - runStart, 1};
                    if (rectCount == (int) ARRAY_LENGTH(rects))
                        ok = flushFixedBitmapRects(renderer, rects, &rectCount);
                }
            }
        }
        if (ok)
            ok = flushFixedBitmapRects(renderer, rects, &rectCount);
    }
    clearRendererClip(renderer);
    shapeGuardEnd(&sg);
    return ok;
}

/* Returns True if the cache took ownership of "texture". False if the caller is
 * responsible for destroying it (e.g., string too long or out-of-memory key
 * allocation).
 */
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
                const char *string,
                size_t length,
                SDL_Rect *drawnBounds)
{
    if (!string || length == 0)
        return True;
    LOG("Rendering text (length=%zu): '%s'\n", length, string);
    GraphicContext *gContext = GET_GC(gc);
    unsigned long foreground = colorWithOpaqueDefault(gContext->foreground);
    SDL_Color color = {
        GET_RED_FROM_COLOR(foreground),
        GET_GREEN_FROM_COLOR(foreground),
        GET_BLUE_FROM_COLOR(foreground),
        GET_ALPHA_FROM_COLOR(foreground),
    };
    if (gContext->font == None) {
        /* Lazily match Xlib's default-GC behavior: a GC without an explicit
         * font still draws with the server's fixed font.
         */
        gContext->font = XLoadFont(display, "fixed");
        if (gContext->font == None)
            return False;
        if (!compatFontRetainForGC(gContext->font)) {
            compatFontClose(display, gContext->font);
            gContext->font = None;
            return False;
        }
    }
    CompatFont *fontResource = GET_FONT_RESOURCE(gContext->font);
    Bool stringHasEmbeddedNul = strlen(string) != length;
    if (stringHasEmbeddedNul && fontResource && fontResource->useFixedBitmap &&
        renderFixedBitmapText(drawable, renderer, gc, x, y, string, length,
                              drawnBounds)) {
        return True;
    }

    /* TTF_RenderUTF8_Solid stops at the first NUL, so embedded NULs only render
     * their prefix here (the fixed-bitmap path above covers Motif's default
     * "fixed" font, which is the realistic case). The cache key uses strdup, so
     * skip lookup and insert when length != strlen to avoid storing or
     * mis-hitting a truncated key.
     */
    SDL_Texture *fontTexture = NULL;
    int textureWidth = 0;
    int textureHeight = 0;
    int textureAscent = 0;
    Bool textureOwned = False; /* True means caller frees. */
    /* Hold the cache lock across lookup, possible insert, and the
     * SDL_RenderCopy loop. Releasing earlier would let a concurrent
     * invalidate-eviction free the texture out from under the active blit.
     */
    textCacheLock();
    TextCacheEntry *hit =
        stringHasEmbeddedNul
            ? NULL
            : textCacheLookup(gContext->font, (Uint32) foreground, renderer,
                              string);
    if (hit) {
        fontTexture = hit->texture;
        textureWidth = hit->width;
        textureHeight = hit->height;
        textureAscent = hit->ascent;
    } else {
        SDL_Surface *fontSurface =
            TTF_RenderUTF8_Blended(GET_FONT(gContext->font), string, color);
        if (!fontSurface) {
            textCacheUnlock();
            if (fontResource && fontResource->useFixedBitmap &&
                renderFixedBitmapText(drawable, renderer, gc, x, y, string,
                                      length, drawnBounds)) {
                return True;
            }
            return False;
        }
        textureWidth = fontSurface->w;
        textureHeight = fontSurface->h;
        fontTexture = SDL_CreateTextureFromSurface(renderer, fontSurface);
        SDL_FreeSurface(fontSurface);
        if (!fontTexture) {
            textCacheUnlock();
            if (fontResource && fontResource->useFixedBitmap &&
                renderFixedBitmapText(drawable, renderer, gc, x, y, string,
                                      length, drawnBounds)) {
                return True;
            }
            return False;
        }
        SDL_SetTextureBlendMode(fontTexture, SDL_BLENDMODE_BLEND);
        textureAscent = TTF_FontAscent(GET_FONT(gContext->font));
        if (stringHasEmbeddedNul ||
            !textCacheInsert(gContext->font, (Uint32) foreground, renderer,
                             string, fontTexture, textureWidth, textureHeight,
                             textureAscent)) {
            textureOwned = True;
        }
    }

    SDL_Rect destR;
    destR.x = x;
    if (fontResource && fontResource->hasCoreMetrics &&
        fontResource->coreWidth > 0) {
        size_t textLen = length;
        if (textLen > (size_t) (INT_MAX / fontResource->coreWidth))
            textLen = (size_t) (INT_MAX / fontResource->coreWidth);
        destR.w = (int) textLen * fontResource->coreWidth;
        destR.h = fontResource->coreAscent + fontResource->coreDescent;
        destR.y = y - fontResource->coreAscent;
    } else {
        destR.w = textureWidth;
        destR.h = textureHeight;
        destR.y = y - textureAscent;
    }
    if (drawnBounds)
        *drawnBounds = destR;
    ShapeGuard sg;
    shapeGuardBegin(&sg, drawable, renderer, &destR);
    int clipCount = getGcClipIterationCount(gc, drawable);
    Bool ok = True;
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, drawable))
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
    if (!ok && drawnBounds)
        drawnBounds->w = drawnBounds->h = 0;
    return ok;
}

static int drawImageString(Display *display,
                           Drawable drawable,
                           GC gc,
                           int x,
                           int y,
                           char *text,
                           size_t length)
{
    TYPE_CHECK(drawable, DRAWABLE, display, 0);
    if (!gc) {
        handleError(0, display, None, 0, BadGC, 0);
        return 0;
    }
    if (!text || length == 0)
        return 1;

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
        if (gContext->font == None)
            return 0;
        if (!compatFontRetainForGC(gContext->font)) {
            compatFontClose(display, gContext->font);
            gContext->font = None;
            return 0;
        }
    }
    int width = 0;
    int height = 0;
    int ascent = 0;
    int descent = 0;
    CompatFont *fontResource = GET_FONT_RESOURCE(gContext->font);
    /* The clear box must match the renderer that will actually run. The fixed
     * aliases render through SDL_ttf now, but their X11-visible metrics stay at
     * the historical 6x13 cell so Xt/Xaw layout does not shift.
     */
    if (fontResource && fontResource->hasCoreMetrics &&
        fontResource->coreWidth > 0) {
        size_t textLen = length;
        if (textLen > (size_t) (INT_MAX / fontResource->coreWidth))
            textLen = (size_t) (INT_MAX / fontResource->coreWidth);
        width = (int) textLen * fontResource->coreWidth;
        ascent = fontResource->coreAscent;
        descent = fontResource->coreDescent;
    } else {
        TTF_Font *font = GET_FONT(gContext->font);
        if (TTF_SizeUTF8(font, text, &width, &height) != 0) {
            XFontStruct *metrics = XQueryFont(display, gContext->font);
            width = metrics ? getTextWidth(metrics, text) : 0;
            freeFontStruct(metrics);
        }
        ascent = TTF_FontAscent(font);
        descent = abs(TTF_FontDescent(font));
    }
    SDL_Rect background = {x, y - ascent, width, ascent + descent};
    applySdlDrawState(renderer, gc, SDL_BLENDMODE_NONE, gContext->background);
    ShapeGuard sg;
    shapeGuardBegin(&sg, drawable, renderer, &background);
    int clipCount = getGcClipIterationCount(gc, drawable);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip, drawable))
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
    SDL_Rect damage = {0, 0, 0, 0};
    int result =
        renderText(display, drawable, renderer, gc, x, y, text, length, &damage)
            ? 1
            : 0;
    if (result) {
        /* The background fill above paints across the entire text width x font
         * height rectangle, which can extend past the glyph damage on both
         * sides (e.g. trailing whitespace, descent gap). The dirty-region
         * pipeline needs the union of both rects, or pixels in the cleared
         * background outside the glyph extents stay stale on the SDL window
         * surface.
         */
        SDL_Rect imageDamage;
        unionRect(&background, &damage, &imageDamage);
        finishTextDamage(display, drawable, &imageDamage);
    }
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
    if (length <= 0 || !string)
        return 1;
    char *text = decodeString(string, length);
    if (!text) {
        handleError(0, display, drawable, 0, BadAlloc, 0);
        return 0;
    }
    int result =
        drawImageString(display, drawable, gc, x, y, text, (size_t) length);
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
    /* Length-counted Xlib API: glyph U+0000 is still part of the request and
     * must be drawn. Only short-circuit on a missing buffer or zero length,
     * never on a leading NUL glyph.
     */
    if (length <= 0 || !string)
        return 1;
    size_t size;
    char *text = decodeChar2bString(string, length, &size);
    if (!text) {
        handleError(0, display, drawable, 0, BadAlloc, 0);
        return 0;
    }
    int result = drawImageString(display, drawable, gc, x, y, text, size);
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
    /* See XDrawImageString16: a leading U+0000 glyph is real data. */
    if (length <= 0 || !string)
        return 1;
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
    SDL_Rect damage = {0, 0, 0, 0};
    if (renderText(display, drawable, renderer, gc, x, y, text, size,
                   &damage)) {
        free(text);
        finishTextDamage(display, drawable, &damage);
        return 1;
    }

    LOG("Rendering 16-bit text failed in %s, retrying with fallback glyphs: "
        "%s\n",
        __func__, SDL_GetError());
    char *fallback = makeMissingGlyphFallbackText(length);
    if (fallback) {
        SDL_Rect fallbackDamage = {0, 0, 0, 0};
        if (renderText(display, drawable, renderer, gc, x, y, fallback,
                       (size_t) length, &fallbackDamage)) {
            free(fallback);
            free(text);
            finishTextDamage(display, drawable, &fallbackDamage);
            return 1;
        }
        free(fallback);
    }

    LOG("Rendering the text failed in %s: %s\n", __func__, SDL_GetError());
    handleError(0, display, drawable, 0, BadMatch, 0);
    free(text);
    return 0;
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
    if (length <= 0 || !string)
        return 1;
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
    SDL_Rect damage = {0, 0, 0, 0};
    if (!renderText(display, drawable, renderer, gc, x, y, text,
                    (size_t) length, &damage)) {
        LOG("Rendering the text failed in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, drawable, 0, BadMatch, 0);
        res = 0;
    }
    free(text);
    if (res)
        finishTextDamage(display, drawable, &damage);
    return res;
}
