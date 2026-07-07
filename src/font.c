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
#include <X11/Xatom.h>

#include "sdl-compat.h"
#include "sdl-ttf-compat.h"
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
typedef struct {
    Bool loaded;
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
    /* Use the stamp-preserving present: renderText has just recorded a stamp
     * for this cell, and the generic present path would evict it again.
     */
    presentDrawableRectIfVisibleNoStampInvalidate(drawable, damage);
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
static Array *fontSearchPaths = NULL;
static Array *fontCache = NULL;

/* coreFontMetricsForName needs to consult loadFixedBitmapFont() (defined
 * further down) to decide whether to advertise the bitmap-font metrics.
 */
static Bool loadFixedBitmapFont(void);
static Array *fontAliasNames = NULL;

// Check if the given path points to an existing directory
static Bool checkFontPath(const char *path)
{
    if (!path)
        return False;
    struct stat s;
    int err;
    while ((err = stat(path, &s)) == -1 && errno == EAGAIN)
        ;
    return err == 0 && S_ISDIR(s.st_mode);
}

static char *getFontXLFDName(TTF_Font *font)
{
    /* FOUNDRY - FAMILY_NAME - WEIGHT_NAME - SLANT - SETWIDTH_NAME - ADD_STYLE -
     * PIXEL_SIZE - POINT_SIZE - RESOLUTION_X - RESOLUTION_Y - SPACING -
     * AVERAGE_WIDTH - CHARSET_REGISTRY - CHARSET_ENCODING
     */
    if (!font)
        return NULL;
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

static Bool fontCacheEntryFileNameCmp(void *entry, void *name)
{
    size_t nameLen = strlen(name);
    size_t pathLen = strlen(((FontCacheEntry *) entry)->filePath);
    if (nameLen > pathLen)
        return False;
    return !strcmp(&((FontCacheEntry *) entry)->filePath[pathLen - nameLen],
                   name);
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

static Bool updateFontCache()
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
    freeTextStamps();
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
    return !strcasecmp(name, "fixed") || !strcasecmp(name, "variable") ||
           !strcasecmp(name, "cursor") || !strcasecmp(name, "6x13") ||
           !strcasecmp(name, "7x13") || !strcasecmp(name, "7x13bold") ||
           !strcasecmp(name, "8x13") || !strcasecmp(name, "7x14") ||
           !strcasecmp(name, "9x13") || !strcasecmp(name, "9x15") ||
           !strcasecmp(name, "9x18") || !strcasecmp(name, "12x24") ||
           !strncasecmp(name, "-misc-fixed-", 12) ||
           !strncasecmp(name, "-jis-fixed-", 11) ||
           containsIgnoreCase(name, "helvetica") ||
           containsIgnoreCase(name, "helv") ||
           containsIgnoreCase(name, "courier") ||
           containsIgnoreCase(name, "lucidatypewriter") ||
           containsIgnoreCase(name, "adobe-times") ||
           containsIgnoreCase(name, "times") ||
           containsIgnoreCase(name, "schoolbook") ||
           (name[0] == '-' && containsIgnoreCase(name, "iso8859")) ||
           ((containsIgnoreCase(name, "-medium-r-") ||
             containsIgnoreCase(name, "-bold-r-")) &&
            containsIgnoreCase(name, "-p-"));
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
    /* The in-loop guard uses the value before the current digit, so a final
     * digit can push the result just past saturationCap (e.g. "21474833"
     * against a decipoint cap). Clamp on exit so callers can rely on the return
     * never exceeding the cap, keeping downstream math (decipoints * 100 + 360)
     * inside int range.
     */
    if (value > saturationCap)
        value = saturationCap;
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

/* Read the number the compact XEphem/Motif wildcard puts between its 2nd and
 * 3rd dash, e.g. "10" in "*-lucidatypewriter*medium*-10-*-iso8859-1" or "13" in
 * "*-fixed-13-*".
 *
 * Returns the raw field value; the caller decides whether it is a pixel or a
 * point size (see xlfdHasWeightToken).
 */
static Bool compactWildcardField(const char *name, int *valueReturn)
{
    if (!name || name[0] == '-')
        return False;
    const char *firstDash = strchr(name, '-');
    if (!firstDash)
        return False;
    const char *sizeStart = strchr(firstDash + 1, '-');
    if (!sizeStart)
        return False;
    sizeStart++;
    const char *sizeEnd = strchr(sizeStart, '-');
    if (!sizeEnd || sizeEnd == sizeStart)
        return False;
    size_t len = (size_t) (sizeEnd - sizeStart);
    if (len >= 16)
        return False;
    char buffer[16];
    memcpy(buffer, sizeStart, len);
    buffer[len] = '\0';
    return parsePositiveInt(buffer, valueReturn);
}

static Bool isFontNameAlpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* A weight token proves an XLFD carries its foundry/family/weight prefix, so a
 * trailing compact number sits past the pixel slot and is a POINT size. Its
 * absence (e.g. "*-fixed-13-*") means the number is a PIXEL size. Match only a
 * delimited token ("*medium*", "-bold-"), never a bare substring: otherwise a
 * family like Bookman ("book") or Enlighten ("light") would flip a pixel field
 * into a point field.
 */
static Bool xlfdHasWeightToken(const char *name)
{
    static const char *const weights[] = {
        "medium", "bold",    "demibold", "light", "book",
        "black",  "regular", "thin",     "heavy",
    };
    for (const char *p = name; *p; p++) {
        for (size_t i = 0; i < ARRAY_LENGTH(weights); i++) {
            size_t len = strlen(weights[i]);
            if (strncasecmp(p, weights[i], len) != 0)
                continue;
            char before = p == name ? '\0' : p[-1];
            if (!isFontNameAlpha(before) && !isFontNameAlpha(p[len]))
                return True;
        }
    }
    return False;
}

/* Extract the dash-delimited XLFD field at the given logical index (a leading
 * '-' makes field 0 empty) and parse it as a positive font dimension.
 *
 * Returns False for an absent field, a wildcard/empty field, a literal zero, or
 * any non-numeric text, so a caller can fall through to the next field. The cap
 * is generous enough for a decipoint POINT_SIZE (e.g. 140); pixel callers clamp
 * the result with clampFontSize.
 */
static Bool xlfdNumericField(const char *name, int index, int *valueReturn)
{
    int cap = (INT_MAX - 360) / 100;
    const char *fieldStart = name;
    int field = 0;
    for (const char *p = name;; p++) {
        if (*p != '-' && *p != '\0')
            continue;
        if (field == index) {
            size_t len = (size_t) (p - fieldStart);
            if (len == 0 || len >= 16)
                return False;
            char buffer[16];
            int value = 0;
            memcpy(buffer, fieldStart, len);
            buffer[len] = '\0';
            if (!parsePositiveIntBounded(buffer, cap, &value) || value == 0)
                return False;
            *valueReturn = value;
            return True;
        }
        if (*p == '\0')
            return False;
        field++;
        fieldStart = p + 1;
    }
}

/* Convert an XLFD POINT_SIZE (decipoints) to pixels at the catalog DPI, then
 * apply the one differential-verified nudge: Helvetica's 12pt (120 decipoints)
 * baseline renders at 16px, not the 17px the naive conversion yields.
 */
static int decipointsToPixelsForName(const char *name, int decipoints)
{
    int pixels = decipointsToPixelSize(decipoints);
    if (pixels == 17 && containsIgnoreCase(name, "helvetica"))
        return 16;
    return pixels;
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

    /* A well-formed XLFD carries PIXEL_SIZE and POINT_SIZE at fixed field
     * positions (7 and 8 with the leading '-', one less without it). Read them
     * by position and prefer a real pixel size, falling back to the point size.
     * A wildcard, empty, or zero pixel field means "unspecified" and defers to
     * the point size, exactly as a font server resolves a scalable request.
     * This is the unambiguous path; the shape heuristics below only catch
     * glob-merged wildcards that are not strictly 14-field.
     */
    int pixelField = name[0] == '-' ? 7 : 6;
    int pointField = name[0] == '-' ? 8 : 7;
    int pixelSize = 0;
    if (xlfdNumericField(name, pixelField, &pixelSize))
        return clampFontSize(pixelSize);
    int pointSize = 0;
    if (xlfdNumericField(name, pointField, &pointSize))
        return decipointsToPixelsForName(name, pointSize);

    const char *doubleDash = strstr(name, "--");
    if (doubleDash) {
        int dashPixelSize = 0;
        const char *start = doubleDash + 2;
        const char *end = strchr(start, '-');
        if (end && end > start) {
            char buffer[16];
            size_t len = (size_t) (end - start);
            if (len < sizeof(buffer)) {
                memcpy(buffer, start, len);
                buffer[len] = '\0';
                if (parsePositiveInt(buffer, &dashPixelSize))
                    return clampFontSize(dashPixelSize);
            }
        }
    }

    int wildcardPixelSize = wildcardStylePixelSize(name);
    if (wildcardPixelSize)
        return wildcardPixelSize;

    /* Compact XEphem names put a size where a strict XLFD would carry many
     * fields. Read it as a POINT size only when a weight token proves the field
     * sits past the pixel slot; otherwise ("*-fixed-13-*") the number is a
     * pixel size and must not be inflated through the decipoint conversion.
     */
    int compactValue = 0;
    if (compactWildcardField(name, &compactValue)) {
        if (xlfdHasWeightToken(name))
            return decipointsToPixelsForName(name, compactValue * 10);
        return clampFontSize(compactValue);
    }

    return DEFAULT_FONT_SIZE;
}

/* Test hook: expose the resolved XLFD pixel size so the unit suite can assert
 * exact resolution for names that never load as a full font (for example the
 * "*-fixed-13-*" pixel/point ambiguity). The lowercase name keeps it out of the
 * public X11 symbol audit (tests/check-api-symbols.py).
 */
int x11compat_font_pixel_size(const char *name)
{
    return requestedFontSize(name);
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
    /* Legacy misc-fixed bitmap fonts. The name spells width x pixel-height, but
     * the ascent/descent split is font-specific, so keep it in an explicit
     * table rather than deriving it from the height. Reporting these instead of
     * the smaller TTF metrics stops Motif/Xt widgets packing too many text rows
     * compared with native libX11. Only advertise them when the bundled BDF
     * actually loaded; otherwise the renderer falls back to TTF and the layout
     * engine would lay out at the bitmap metrics while glyphs draw at TTF size,
     * overlapping adjacent lines.
     */
    static const struct {
        const char *name;
        short ascent, descent, width;
    } fixedFonts[] = {
        {"6x13", 11, 2, 6}, {"7x13", 11, 2, 7}, {"7x13bold", 11, 2, 7},
        {"8x13", 11, 2, 8}, {"9x13", 11, 2, 9}, {"7x14", 12, 2, 7},
        {"9x15", 12, 3, 9}, {"9x18", 14, 4, 9}, {"12x24", 22, 2, 12},
    };
    if (loadFixedBitmapFont()) {
        /* "fixed"/"cursor" and thentenaar/motif's hellomotifi18n Mrm request
         * for -*-times-medium-r-normal--14-*-iso8859-1 both fall back to the
         * 6x13 geometry on the native Xvfb baseline, so resolve them to that
         * entry.
         */
        const char *lookup = name;
        if (!strcasecmp(name, "fixed") || !strcasecmp(name, "cursor") ||
            isMotifTimesIso14Fallback(name))
            lookup = "6x13";
        for (size_t i = 0; i < sizeof(fixedFonts) / sizeof(fixedFonts[0]);
             i++) {
            if (!strcasecmp(lookup, fixedFonts[i].name)) {
                *ascent = fixedFonts[i].ascent;
                *descent = fixedFonts[i].descent;
                *width = fixedFonts[i].width;
                return True;
            }
        }
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
    if (!strcasecmp(name, "fixed") || !strcasecmp(name, "cursor") ||
        !strcasecmp(name, "6x13"))
        return True;
    return isMotifTimesIso14Fallback(name);
}

#include "font-6x13-bitmap.h"

static void useEmbeddedFixedBitmap(void)
{
    /* The renderer reads fixedBitmapFont.rows directly, so a one-shot memcpy
     * from the generated table is enough.
     */
    memcpy(fixedBitmapFont.rows, EMBEDDED_FIXED_BITMAP_ROWS,
           sizeof(EMBEDDED_FIXED_BITMAP_ROWS));
    fixedBitmapFont.loaded = True;
}

static Bool loadFixedBitmapFont(void)
{
    SDL_AtomicLock(&fixedBitmapFontLock);
    if (fixedBitmapFont.loaded) {
        SDL_AtomicUnlock(&fixedBitmapFontLock);
        return True;
    }

    useEmbeddedFixedBitmap();
    SDL_AtomicUnlock(&fixedBitmapFontLock);
    return True;
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
    if (!strcasecmp(name, "variable"))
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

static Bool fontProvidesCodepoint(TTF_Font *font, Uint32 codepoint)
{
    if (!font)
        return False;
    return xc_TTF_GlyphIsProvidedUcs4(font, codepoint) ? True : False;
}

static TTF_Font *openRenderableProbeFontForChar(const char *const *paths,
                                                size_t count,
                                                int size,
                                                const char *skipPath,
                                                Uint32 codepoint)
{
    for (size_t i = 0; i < count; i++) {
        if (skipPath && !strcmp(paths[i], skipPath))
            continue;
        TTF_Font *font = TTF_OpenFont(paths[i], size);
        if (!font)
            continue;
        if (fontCanRenderText(font) && fontProvidesCodepoint(font, codepoint))
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
    if (!fontCache)
        return NULL;
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

static TTF_Font *openRenderableFallbackFontForChar(const char *name,
                                                   int size,
                                                   const char *skipPath,
                                                   Uint32 codepoint)
{
    TTF_Font *font = NULL;
    if (containsIgnoreCase(name, "times") ||
        containsIgnoreCase(name, "adobe-times") ||
        containsIgnoreCase(name, "schoolbook")) {
        font = openRenderableProbeFontForChar(SERIF_PROBE_PATHS,
                                              ARRAY_LENGTH(SERIF_PROBE_PATHS),
                                              size, skipPath, codepoint);
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
            font = openRenderableProbeFontForChar(
                SANS_BOLD_PROBE_PATHS, ARRAY_LENGTH(SANS_BOLD_PROBE_PATHS),
                size, skipPath, codepoint);
            if (font)
                return font;
        }
        font = openRenderableProbeFontForChar(SANS_PROBE_PATHS,
                                              ARRAY_LENGTH(SANS_PROBE_PATHS),
                                              size, skipPath, codepoint);
        if (font)
            return font;
    }
    font = openRenderableProbeFontForChar(MONOSPACE_PROBE_PATHS,
                                          ARRAY_LENGTH(MONOSPACE_PROBE_PATHS),
                                          size, skipPath, codepoint);
    if (font)
        return font;
    if (!fontCache)
        return NULL;
    for (size_t i = 0; i < fontCache->length; i++) {
        FontCacheEntry *entry = fontCache->array[i];
        if (skipPath && !strcmp(entry->filePath, skipPath))
            continue;
        if (!entry->asciiMetrics)
            continue;
        font = TTF_OpenFont(entry->filePath, size);
        if (!font)
            continue;
        if (fontCanRenderText(font) && fontProvidesCodepoint(font, codepoint))
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
TTF_Font *compatFontOpenFamilyFallback(const char *familyHint, int size)
{
    const char *name = familyHint ? familyHint : "sans";
    return openRenderableFallbackFont(name, clampFontSize(size), NULL);
}

TTF_Font *compatFontOpenFamilyFallbackForChar(const char *familyHint,
                                              int size,
                                              Uint32 codepoint)
{
    const char *name = familyHint ? familyHint : "sans";
    return openRenderableFallbackFontForChar(name, clampFontSize(size), NULL,
                                             codepoint);
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

static Bool fillXCharStruct(TTF_Font *font,
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

static XFontStruct *queryFontStruct(Display *display,
                                    XID fontId,
                                    Bool requireClientUsable)
{
    TYPE_CHECK(fontId, FONT, display, NULL);
    if (requireClientUsable && !compatFontIsClientUsable(fontId)) {
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

XFontStruct *XQueryFont(Display *display, XID fontId)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XQueryFont.html
    SET_X_SERVER_REQUEST(display, X_QueryFont);
    return queryFontStruct(display, fontId, True);
}

XFontStruct *compatFontQueryRetained(Display *display, Font fontXid)
{
    return queryFontStruct(display, fontXid, False);
}

/* XDrawString and friends take a length-bounded buffer that may or may not be
 * NUL-terminated. Downstream SDL_ttf calls expect a C string, so decodeString
 * copies "count" bytes into a fresh allocation and NUL-terminates. The payload
 * is rendered as-is: real Xlib treats every byte as a glyph index into the
 * font, so escape interpretation (\n, \xHH, ...) would silently mangle
 * legitimate Latin-1/file-path input.
 */
static char *decodeString(const char *string, int count)
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
    /* A font id can resolve to a NULL face (for example after the client closed
     * and reopened its display, retiring the old font table, as Motif fileview
     * does after its language dialog). SDL_ttf dereferences the font pointer,
     * so a NULL here must not reach TTF_SizeUTF8. Fall back to the fixed-width
     * estimate, matching how real Xlib keeps measuring with a default font
     * rather than crashing the client.
     */
    TTF_Font *ttfFont = GET_FONT(font_struct->fid);
    if (!ttfFont || TTF_SizeUTF8(ttfFont, string, &width, &height) != 0) {
        if (ttfFont)
            LOG("Failed to calculate the text width in XTextWidth[16]: %s! "
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

static int getTextWidth(XFontStruct *font_struct, const char *string)
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

static Bool renderFixedBitmapTextAndInvalidate(Drawable drawable,
                                               SDL_Renderer *renderer,
                                               GC gc,
                                               int x,
                                               int y,
                                               const char *string,
                                               size_t length,
                                               SDL_Rect *drawnBounds)
{
    SDL_Rect bounds;
    SDL_Rect *boundsOut = drawnBounds ? drawnBounds : &bounds;
    if (!renderFixedBitmapText(drawable, renderer, gc, x, y, string, length,
                               boundsOut))
        return False;
    invalidateTextStampsForDrawableRect(drawable, boundsOut);
    return True;
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

/* Stretch a blended glyph surface so its densest pixel is fully opaque while
 * preserving the relative anti-aliasing gradient. FreeType/SDL_ttf builds
 * differ in how much coverage a small glyph reaches: some never produce a fully
 * covered pixel, so opaque text composites to gray instead of its intended
 * color. Normalizing the peak to 255 keeps the core solid (opaque black stays
 * black) without flattening the anti-aliased edges. A no-op when the peak is
 * already 0 or 255.
 */
static void normalizeGlyphAlpha(SDL_Surface *surface)
{
    if (!surface || XC_SURFACE_BYTESPERPIXEL(surface) != 4)
        return;
    XcPixelFormat fmt = XC_SURFACE_FORMAT(surface);
    if (SDL_MUSTLOCK(surface) && SDL_LockSurface(surface) != 0)
        return;
    Uint8 peak = 0;
    for (int yy = 0; yy < surface->h; yy++) {
        Uint32 *row =
            (Uint32 *) ((Uint8 *) surface->pixels + yy * surface->pitch);
        for (int xx = 0; xx < surface->w; xx++) {
            Uint8 r, g, b, a;
            SDL_GetRGBA(row[xx], fmt, &r, &g, &b, &a);
            if (a > peak)
                peak = a;
        }
    }
    /* peak == 0 (blank) or 255 (already opaque somewhere) needs no rescale.
     * Otherwise a <= peak guarantees a * 255 / peak stays within 255.
     */
    if (peak != 0 && peak != 255) {
        for (int yy = 0; yy < surface->h; yy++) {
            Uint32 *row =
                (Uint32 *) ((Uint8 *) surface->pixels + yy * surface->pitch);
            for (int xx = 0; xx < surface->w; xx++) {
                Uint8 r, g, b, a;
                SDL_GetRGBA(row[xx], fmt, &r, &g, &b, &a);
                Uint8 scaled = (Uint8) ((unsigned) a * 255u / peak);
                row[xx] = SDL_MapRGBA(fmt, r, g, b, scaled);
            }
        }
    }
    if (SDL_MUSTLOCK(surface))
        SDL_UnlockSurface(surface);
}

static Bool renderText(Display *display,
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
    /* A font id with no loaded face (NULL ttf, for example after the client
     * closed and reopened its display) must not reach SDL_ttf: TTF_Render* and
     * TTF_FontAscent below dereference the font pointer. Prefer the
     * fixed-bitmap fallback; otherwise skip the draw rather than crash,
     * matching Xlib's tolerance of a missing font.
     */
    if (!GET_FONT(gContext->font)) {
        if (fontResource && fontResource->useFixedBitmap &&
            renderFixedBitmapTextAndInvalidate(drawable, renderer, gc, x, y,
                                               string, length, drawnBounds))
            return True;
        return False;
    }
    Bool stringHasEmbeddedNul = strlen(string) != length;
    if (stringHasEmbeddedNul && fontResource && fontResource->useFixedBitmap &&
        renderFixedBitmapTextAndInvalidate(drawable, renderer, gc, x, y, string,
                                           length, drawnBounds)) {
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
                renderFixedBitmapTextAndInvalidate(drawable, renderer, gc, x, y,
                                                   string, length,
                                                   drawnBounds)) {
                return True;
            }
            return False;
        }
        normalizeGlyphAlpha(fontSurface);
        textureWidth = fontSurface->w;
        textureHeight = fontSurface->h;
        fontTexture = SDL_CreateTextureFromSurface(renderer, fontSurface);
        SDL_FreeSurface(fontSurface);
        if (!fontTexture) {
            textCacheUnlock();
            if (fontResource && fontResource->useFixedBitmap &&
                renderFixedBitmapTextAndInvalidate(drawable, renderer, gc, x, y,
                                                   string, length,
                                                   drawnBounds)) {
                return True;
            }
            return False;
        }
        SDL_SetTextureBlendMode(fontTexture, SDL_BLENDMODE_BLEND);
#if defined(LIBX11_COMPAT_SDL3) || SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetTextureScaleMode(fontTexture,
                                fontResource && fontResource->useFixedBitmap
                                    ? XC_SCALEMODE_NEAREST
                                    : XC_SCALEMODE_LINEAR);
#endif
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
    /* Anti-aliased glyph edges are not idempotent under SDL alpha blending, so
     * Motif's expose/arm redraws (an identical XDrawString to the same cell
     * with no interior clear) would thicken the text on every pass. When the
     * draw is an unclipped GXcopy of NUL-free text, skip the re-blit if the
     * identical label is already stamped at this cell, and record a stamp once
     * it is drawn. Clipped draws and other raster ops are left untouched, as
     * they are not safely idempotent. Any other draw over the cell, or a
     * structural change, drops the stamp (see drawing.c).
     */
    Bool idempotent = !stringHasEmbeddedNul && gContext->function == GXcopy &&
                      gContext->clipMask == None &&
                      !gContext->clipRectanglesSet;
    if (idempotent && textStampLookup(drawable, &destR, gContext->font,
                                      (Uint32) foreground, string)) {
        textCacheUnlock();
        if (textureOwned)
            SDL_DestroyTexture(fontTexture);
        return True;
    }
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
    if (ok) {
        /* finishTextDamage marks this cell present without touching stamps, so
         * keep the stamp set consistent here: record the freshly drawn label
         * when it is safely idempotent, otherwise drop any stamp this draw
         * overwrote.
         */
        if (idempotent)
            textStampRecord(drawable, &destR, gContext->font,
                            (Uint32) foreground, string);
        else
            invalidateTextStampsForDrawableRect(drawable, &destR);
    }
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
        /* A NULL face (font id no longer backed by a loaded font) must not
         * reach SDL_ttf, which dereferences it. Take width, ascent and descent
         * from XQueryFont instead, so the image-string background box below
         * keeps its height rather than collapsing to zero and skipping the
         * background clear XDrawImageString owes its caller.
         */
        TTF_Font *font = GET_FONT(gContext->font);
        if (!font || TTF_SizeUTF8(font, text, &width, &height) != 0) {
            XFontStruct *metrics = XQueryFont(display, gContext->font);
            width = metrics ? getTextWidth(metrics, text) : 0;
            ascent = metrics ? metrics->ascent : 0;
            descent = metrics ? metrics->descent : 0;
            freeFontStruct(metrics);
        } else {
            ascent = TTF_FontAscent(font);
            descent = abs(TTF_FontDescent(font));
        }
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
        /* The opaque background fill plus text overwrote this region without
         * recording a stamp, so drop any stamp it covered before the
         * stamp-preserving present in finishTextDamage runs.
         */
        invalidateTextStampsForDrawableRect(drawable, &imageDamage);
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
