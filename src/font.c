#include "X11/Xatom.h"
#include <stdio.h>
#include <wchar.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
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

#define GET_FONT(fontXID) ((TTF_Font *) GET_XID_VALUE(fontXID))
#define FONT_SIZE 12

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
    }
    LOG("Font name = '%s'\n", name);
    return name;
}

Bool fontCacheEntryFileNameCmp(void *entry, void *name)
{
    size_t nameLen = strlen(name);
    size_t pathLen = strlen(((FontCacheEntry *) entry)->filePath);
    if (nameLen > pathLen)
        return False;
    return strcmp(&((FontCacheEntry *) entry)->filePath[pathLen - nameLen],
                  name) == 0;
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
                    TTF_Font *font = TTF_OpenFont(pathBuffer, FONT_SIZE);
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
           !strcmp(name, "9x13") || !strcmp(name, "9x15") ||
           !strncmp(name, "-misc-fixed-", 12) ||
           !strncmp(name, "-jis-fixed-", 11) ||
           !strncmp(name, "-adobe-times-", 13);
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

static FontCacheEntry *adoptProbePath(const char *path)
{
    TTF_Font *font = TTF_OpenFont(path, FONT_SIZE);
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
    if (isFontAlias(name)) {
        FontCacheEntry *aliased = findAliasedFixedWidthFont();
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
    SET_XID_VALUE(font, TTF_OpenFont(fontEntry->filePath, FONT_SIZE));
    if (!GET_XID_VALUE(font)) {
        FREE_XID(font);
        LOG("Failed to load font %s!\n", name);
        handleError(0, display, None, 0, BadName, 0);
        return None;
    }
    return font;
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
        TTF_CloseFont(GET_FONT(font_struct->fid));
        FREE_XID(font_struct->fid);
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
        TTF_Font *font = TTF_OpenFont(entry->filePath, FONT_SIZE);
        if (!font) {
            continue;
        }
        fillFontStructFromTTF(display, None, font, &infos[i]);
        TTF_CloseFont(font);
    }

    *info_return = infos;
    return names;
}

XFontStruct *XLoadQueryFont(Display *display, _Xconst char *name)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XLoadQueryFont.html
    Font fontId = XLoadFont(display, name);
    if (fontId == None) {
        return NULL;
    }
    XFontStruct *fontStruct = XQueryFont(display, fontId);
    if (!fontStruct) {
        TTF_CloseFont(GET_FONT(fontId));
        FREE_XID(fontId);
    }
    return fontStruct;
}

XFontStruct *XQueryFont(Display *display, XID fontId)
{
    // https://tronche.com/gui/x/xlib/graphics/font-metrics/XQueryFont.html
    SET_X_SERVER_REQUEST(display, X_QueryFont);
    TYPE_CHECK(fontId, FONT, display, NULL);
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
    return fontStruct;
}

static __inline__ char hexCharToNum(char chr)
{
    return (char) (chr >= 'a' ? 10 + chr - 'a' : chr - '0');
}

/* Resolve all X11 controll characters */
char *decodeString(const char *string, int count)
{
    int i, counter = 0;
    char *text = malloc(sizeof(char) * (count + 1));
    if (!text) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        if (string[i] == '\\') {
            switch (string[++i]) {
            case 'x': /* \xFF */
                text[counter] = (hexCharToNum(string[++i]) << 4);
                text[counter] |= hexCharToNum(string[++i]);
                break;
            case 'u': /* \uFFFF */
                text[counter] = (hexCharToNum(string[++i]) << 4);
                text[counter] |= hexCharToNum(string[++i]);
                text[++counter] = (hexCharToNum(string[++i]) << 4);
                text[counter] |= hexCharToNum(string[++i]);
                break;
            case 'n':
                text[counter] = '\n';
                break;
            case 'r':
                text[counter] = '\r';
                break;
            case 'a':
                text[counter] = '\a';
                break;
            case 'b':
                text[counter] = '\b';
                break;
            case 't':
                text[counter] = '\t';
                break;
            case 'v':
                text[counter] = '\v';
                break;
            case 'f':
                text[counter] = '\f';
                break;
            default:
                LOG("Warn: Got unknown control character in %s: '\\%c'\n",
                    __func__, string[i]);
                text[counter] = '\\';
                text[++counter] = string[i];
            }
        } else {
            text[counter] = string[i];
        }
        counter++;
    }
    text[counter] = '\0';
    return text;
}

int getTextWidth(XFontStruct *font_struct, const char *string)
{
    int width, height;
    if (TTF_SizeUTF8(GET_FONT(font_struct->fid), string, &width, &height) !=
        0) {
        LOG("Failed to calculate the text with in XTextWidth[16]: %s! "
            "Returning max width of font.\n",
            TTF_GetError());
        return (int) (font_struct->max_bounds.rbearing * strlen(string));
    }
    return width;
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
    int width = getTextWidth(font_struct, text);
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
    int width = getTextWidth(font_struct, text);
    free(text);
    return width;
}

Bool renderText(Display *display,
                SDL_Renderer *renderer,
                GC gc,
                int x,
                int y,
                const char *string)
{
    LOG("Rendering text: '%s'\n", string);
    if (!string || string[0] == '\0') {
        return True;
    }
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
    }
    SDL_Surface *fontSurface =
        TTF_RenderUTF8_Blended(GET_FONT(gContext->font), string, color);
    if (!fontSurface) {
        return False;
    }
    SDL_Rect destR;
    destR.w = fontSurface->w;
    destR.h = fontSurface->h;
    SDL_Texture *fontTexture =
        SDL_CreateTextureFromSurface(renderer, fontSurface);
    SDL_FreeSurface(fontSurface);
    if (!fontTexture) {
        return False;
    }
    destR.x = x;
    destR.y = y - TTF_FontAscent(GET_FONT(gContext->font)) /* - 6*/;
    // h and w are ignored
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (SDL_RenderCopy(renderer, fontTexture, NULL, &destR) != 0) {
            clearRendererClip(renderer);
            SDL_DestroyTexture(fontTexture);
            return False;
        }
    }
    clearRendererClip(renderer);
    SDL_DestroyTexture(fontTexture);
    return True;
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
    int clipCount = getGcClipIterationCount(gc);
    for (int clip = 0; clip < clipCount; clip++) {
        if (!setGcClipForIteration(renderer, gc, clip))
            continue;
        if (SDL_RenderFillRect(renderer, &background) != 0) {
            clearRendererClip(renderer);
            LOG("SDL_RenderFillRect failed in %s: %s\n", __func__,
                SDL_GetError());
            handleError(0, display, drawable, 0, BadMatch, 0);
            return 0;
        }
    }
    clearRendererClip(renderer);
    return renderText(display, renderer, gc, x, y, text) ? 1 : 0;
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
    if (!renderText(display, renderer, gc, x, y, text)) {
        LOG("Rendering the text failed in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, drawable, 0, BadMatch, 0);
        free(text);
        res = 0;
    }
    free(text);
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
    if (!renderText(display, renderer, gc, x, y, text)) {
        LOG("Rendering the text failed in %s: %s\n", __func__, SDL_GetError());
        handleError(0, display, drawable, 0, BadMatch, 0);
        res = 0;
    }
    free(text);
    return res;
}
