#include "input-method.h"
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "X11/keysym.h"
#include "X11/XKBlib.h"
#include "display.h"
#include "errors.h"

// http://www.x.org/archive/X11R7.6/doc/man/man3/XOpenIM.3.xhtml
// http://www.x.org/archive/X11R7.6/doc/man/man3/XCreateIC.3.xhtml

static char *currLocaleModifierList = defaultLocaleModifierList;
char *pendingText = NULL;

void inputMethodSetCurrentText(char *text)
{
    pendingText = text;
}

KeySym getKeySymForChar(char c)
{
    const static struct {
        char character;
        KeySym keySym;
    } charMapping[] = {
        {' ', XK_space},        {'\n', XK_Return},     {'\r', XK_Linefeed},
        {'\t', XK_Tab},         {'\b', XK_BackSpace},  {'-', XK_minus},
        {'+', XK_plus},         {'#', XK_numbersign},  {'*', XK_multiply},
        {'~', XK_asciitilde},   {'\'', XK_quoteright}, {'"', XK_quotedbl},
        {'!', XK_exclam},       {'@', XK_at},          {'%', XK_percent},
        {'&', XK_ampersand},    {'$', XK_dollar},      {'/', XK_slash},
        {'(', XK_parenleft},    {')', XK_parenright},  {'[', XK_bracketleft},
        {']', XK_bracketright}, {'{', XK_braceleft},   {'}', XK_braceright},
        {'?', XK_question},     {'\\', XK_backslash},  {'<', XK_less},
        {'>', XK_greater},      {'|', XK_bar},         {'_', XK_underscore},
        {'^', XK_asciicircum},  {',', XK_comma},       {';', XK_semicolon},
        {'.', XK_period},       {':', XK_colon},       {'=', XK_equal},
        {'`', XK_quoteleft},
    };
    if (c >= 'a' && c <= 'z')
        return XK_a + (c - 'a');
    if (c >= 'A' && c <= 'Z')
        return XK_A + (c - 'A');
    if (c >= '0' && c <= '9')
        return XK_0 + (c - '0');
    for (size_t i = 0; i < sizeof(charMapping) / sizeof(charMapping[0]); i++) {
        if (charMapping[i].character == c) {
            return charMapping[i].keySym;
        }
    }
    LOG("Did not Found mapping for char '%c' (%d)\n", c, c);
    return NoSymbol;
}

Status XCloseIM(XIM inputMethod)
{
    return 0;
}

char *XSetLocaleModifiers(_Xconst char *modifier_list)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XSetLocaleModifiers.3.xhtml
    if (!modifier_list || modifier_list[0] == '\0')
        return currLocaleModifierList;
    Bool isImSpec = modifier_list[0] == '@' && modifier_list[1] == 'i' &&
                    modifier_list[2] == 'm' && modifier_list[3] == '=';
    Bool currentIsDefault =
        !strcmp(currLocaleModifierList, defaultLocaleModifierList);
    if (!isImSpec && !currentIsDefault)
        return NULL;
    char *curr = currLocaleModifierList;
    currLocaleModifierList = strdup(modifier_list);
    if (!currLocaleModifierList) {
        currLocaleModifierList = curr;
        return NULL;
    }
    return curr;
}

XIM XOpenIM(Display *display,
            struct _XrmHashBucketRec *db,
            char *res_name,
            char *res_class)
{
    char *envXModifiers = getenv("XMODIFIERS");
    if (envXModifiers) {
        XSetLocaleModifiers(envXModifiers);
    }
    return (XIM) display;
}

void XDestroyIC(XIC inputConnection)
{
    if (GET_XIC_STRUCT(inputConnection)->inputRect) {
        free(GET_XIC_STRUCT(inputConnection)->inputRect);
    }
    SDL_StopTextInput();
    free(inputConnection);
}

static Bool styleIsSupported(XIMStyle style)
{
    for (int i = 0; i < supportedStyles.count_styles; i++) {
        if (supportedStyles.supported_styles[i] == style)
            return True;
    }
    return False;
}

static void consumeICAttributeValue(void **attrs, int *index)
{
    (void) attrs[(*index)++];
}

static Bool parseCommonICAttributes(XIC inputConnection,
                                    XVaNestedList attributes,
                                    Bool preedit)
{
    if (!attributes)
        return False;
    void **attrs = (void **) attributes;
    int i = 0;
    char *key;
    while ((key = attrs[i++])) {
        if (!strcmp(key, XNVaNestedList)) {
            if (!parseCommonICAttributes(inputConnection, attrs[i++], preedit))
                return False;
        } else if (!strcmp(key, XNArea)) {
            XRectangle *rect = attrs[i++];
            if ((XIMPreeditArea & GET_XIC_STRUCT(inputConnection)->style) !=
                0) {
                SDL_Rect *inputRect;
                if ((inputRect = GET_XIC_STRUCT(inputConnection)->inputRect) ==
                    NULL) {
                    inputRect = malloc(sizeof(SDL_Rect));
                    if (!inputRect)
                        return False;
                    GET_XIC_STRUCT(inputConnection)->inputRect = inputRect;
                }
                inputRect->x = rect->x;
                inputRect->y = rect->y;
                inputRect->w = rect->width;
                inputRect->h = rect->height;
                SDL_SetTextInputRect(inputRect);
            }
        } else if (!strcmp(key, XNAreaNeeded)) {
            consumeICAttributeValue(attrs, &i);
        } else if (!strcmp(key, XNSpotLocation)) {
            XPoint *point = attrs[i++];
            if ((XIMPreeditPosition & GET_XIC_STRUCT(inputConnection)->style) !=
                0) {
                SDL_Rect *inputRect;
                if ((inputRect = GET_XIC_STRUCT(inputConnection)->inputRect) ==
                    NULL) {
                    inputRect = malloc(sizeof(SDL_Rect));
                    if (!inputRect)
                        return False;
                    GET_XIC_STRUCT(inputConnection)->inputRect = inputRect;
                }
                inputRect->x = point->x;
                inputRect->y = point->y;
                inputRect->w = 0;
                inputRect->h = 0;
                SDL_SetTextInputRect(inputRect);
            }
        } else if (!strcmp(key, XNColormap) || !strcmp(key, XNStdColormap) ||
                   !strcmp(key, XNBackgroundPixmap) ||
                   !strcmp(key, XNLineSpace) || !strcmp(key, XNCursor) ||
                   !strcmp(key, XNPreeditStartCallback) ||
                   !strcmp(key, XNPreeditDoneCallback) ||
                   !strcmp(key, XNPreeditDrawCallback) ||
                   !strcmp(key, XNPreeditCaretCallback) ||
                   !strcmp(key, XNStatusStartCallback) ||
                   !strcmp(key, XNStatusDoneCallback) ||
                   !strcmp(key, XNStatusDrawCallback)) {
            consumeICAttributeValue(attrs, &i);
        } else if (!strcmp(key, XNForeground)) {
            unsigned long value = (unsigned long) (uintptr_t) attrs[i++];
            if (preedit)
                GET_XIC_STRUCT(inputConnection)->preeditForeground = value;
            else
                GET_XIC_STRUCT(inputConnection)->statusForeground = value;
        } else if (!strcmp(key, XNBackground)) {
            unsigned long value = (unsigned long) (uintptr_t) attrs[i++];
            if (preedit)
                GET_XIC_STRUCT(inputConnection)->preeditBackground = value;
            else
                GET_XIC_STRUCT(inputConnection)->statusBackground = value;
        } else if (!strcmp(key, XNFontSet)) {
            GET_XIC_STRUCT(inputConnection)->fontSet = attrs[i++];
        } else {
            LOG("parseCommonICAttributes failed for key %s\n", key);
            return False;
        }
    }
    return True;
}

Bool parsePreEditAttributes(XIC inputConnection, XVaNestedList attributes)
{
    return parseCommonICAttributes(inputConnection, attributes, True);
}

static Bool parseStatusAttributes(XIC inputConnection, XVaNestedList attributes)
{
    return parseCommonICAttributes(inputConnection, attributes, False);
}

static Bool fillCommonICAttributes(XIC inputConnection,
                                   XVaNestedList returnArgs,
                                   Bool preedit)
{
    if (!returnArgs)
        return False;
    void **attrs = (void **) returnArgs;
    int i = 0;
    char *key;
    while ((key = attrs[i++])) {
        if (!strcmp(key, XNVaNestedList)) {
            if (!fillCommonICAttributes(inputConnection, attrs[i++], preedit))
                return False;
        } else if (!strcmp(key, XNArea)) {
            if (!GET_XIC_STRUCT(inputConnection)->inputRect) {
                XRectangle *rect = attrs[i++];
                rect->x = 0;
                rect->y = 0;
                rect->width = 0;
                rect->height = 0;
                continue;
            }
            XRectangle *rect = attrs[i++];
            rect->x = GET_XIC_STRUCT(inputConnection)->inputRect->x;
            rect->y = GET_XIC_STRUCT(inputConnection)->inputRect->y;
            rect->width = GET_XIC_STRUCT(inputConnection)->inputRect->w;
            rect->height = GET_XIC_STRUCT(inputConnection)->inputRect->h;
        } else if (!strcmp(key, XNAreaNeeded)) {
            XRectangle *rect = attrs[i++];
            rect->x = 0;
            rect->y = 0;
            rect->width = 0;
            rect->height = 0;
        } else if (!strcmp(key, XNSpotLocation)) {
            if (!GET_XIC_STRUCT(inputConnection)->inputRect) {
                XPoint *point = attrs[i++];
                point->x = 0;
                point->y = 0;
                continue;
            }
            XPoint *point = attrs[i++];
            point->x = GET_XIC_STRUCT(inputConnection)->inputRect->x;
            point->y = GET_XIC_STRUCT(inputConnection)->inputRect->y;
        } else if (!strcmp(key, XNFontSet)) {
            XFontSet *fontSet = attrs[i++];
            *fontSet = GET_XIC_STRUCT(inputConnection)->fontSet;
        } else if (!strcmp(key, XNForeground)) {
            unsigned long *value = attrs[i++];
            *value = preedit
                         ? GET_XIC_STRUCT(inputConnection)->preeditForeground
                         : GET_XIC_STRUCT(inputConnection)->statusForeground;
        } else if (!strcmp(key, XNBackground)) {
            unsigned long *value = attrs[i++];
            *value = preedit
                         ? GET_XIC_STRUCT(inputConnection)->preeditBackground
                         : GET_XIC_STRUCT(inputConnection)->statusBackground;
        } else if (!strcmp(key, XNColormap) || !strcmp(key, XNStdColormap) ||
                   !strcmp(key, XNBackgroundPixmap) ||
                   !strcmp(key, XNLineSpace) || !strcmp(key, XNCursor) ||
                   !strcmp(key, XNPreeditStartCallback) ||
                   !strcmp(key, XNPreeditDoneCallback) ||
                   !strcmp(key, XNPreeditDrawCallback) ||
                   !strcmp(key, XNPreeditCaretCallback) ||
                   !strcmp(key, XNStatusStartCallback) ||
                   !strcmp(key, XNStatusDoneCallback) ||
                   !strcmp(key, XNStatusDrawCallback)) {
            consumeICAttributeValue(attrs, &i);
        } else {
            LOG("fillCommonICAttributes failed for key %s\n", key);
            return False;
        }
    }
    return True;
}

Bool fillPreEditAttributes(XIC inputConnection, XVaNestedList returnArgs)
{
    return fillCommonICAttributes(inputConnection, returnArgs, True);
}

static Bool fillStatusAttributes(XIC inputConnection, XVaNestedList returnArgs)
{
    return fillCommonICAttributes(inputConnection, returnArgs, False);
}

static char *setICListValues(XIC inputConnection,
                             XVaNestedList attributes,
                             Bool allowSetReadOnly)
{
    if (!attributes)
        return NULL;
    void **attrs = (void **) attributes;
    int i = 0;
    char *key;
    while ((key = attrs[i++])) {
        if (!strcmp(key, XNVaNestedList)) {
            char *failed =
                setICListValues(inputConnection, attrs[i++], allowSetReadOnly);
            if (failed)
                return failed;
        } else if (!strcmp(key, XNInputStyle)) {
            if (!allowSetReadOnly)
                return key;
            XIMStyle style = (XIMStyle) (uintptr_t) attrs[i++];
            if (!styleIsSupported(style))
                return key;
            GET_XIC_STRUCT(inputConnection)->style = style;
        } else if (!strcmp(key, XNClientWindow)) {
            Window clientWindow = (Window) attrs[i++];
            if (!IS_TYPE(clientWindow, WINDOW) ||
                clientWindow == SCREEN_WINDOW) {
                return key;
            }
            Window topLevel = clientWindow;
            while (GET_PARENT(topLevel) != SCREEN_WINDOW) {
                topLevel = GET_PARENT(topLevel);
            }
            if (IS_MAPPED_TOP_LEVEL_WINDOW(topLevel)) {
                SDL_Window *sdlWindow = GET_WINDOW_STRUCT(topLevel)->sdlWindow;
                SDL_RaiseWindow(sdlWindow);
                if (GET_XIC_STRUCT(inputConnection)->inputRect) {
                    SDL_SetTextInputRect(
                        GET_XIC_STRUCT(inputConnection)->inputRect);
                }
                SDL_StopTextInput();
            }
            GET_XIC_STRUCT(inputConnection)->client = clientWindow;
            if (GET_XIC_STRUCT(inputConnection)->focus == None)
                GET_XIC_STRUCT(inputConnection)->focus = clientWindow;
        } else if (!strcmp(key, XNFocusWindow)) {
            Window focusWindow = (Window) attrs[i++];
            if (!IS_TYPE(focusWindow, WINDOW) || focusWindow == SCREEN_WINDOW)
                return key;
            GET_XIC_STRUCT(inputConnection)->focus = focusWindow;
        } else if (!strcmp(key, XNPreeditAttributes)) {
            if (!parsePreEditAttributes(inputConnection, attrs[i++]))
                return key;
        } else if (!strcmp(key, XNStatusAttributes)) {
            if (!parseStatusAttributes(inputConnection, attrs[i++]))
                return key;
        } else if (!strcmp(key, XNFilterEvents)) {
            GET_XIC_STRUCT(inputConnection)->eventFilter =
                (unsigned long) (uintptr_t) attrs[i++];
        } else if (!strcmp(key, XNGeometryCallback) ||
                   !strcmp(key, XNDestroyCallback) ||
                   !strcmp(key, XNResourceName) ||
                   !strcmp(key, XNResourceClass)) {
            consumeICAttributeValue(attrs, &i);
        } else {
            return key;
        }
    }
    return NULL;
}

char *setICValues(XIC inputConnection, va_list arguments, Bool allowSetReadOnly)
{
    char *key = NULL;
    while ((key = va_arg(arguments, char *))) {
        if (!strcmp(key, XNVaNestedList)) {
            char *failed = setICListValues(inputConnection,
                                           va_arg(arguments, XVaNestedList),
                                           allowSetReadOnly);
            if (failed)
                return failed;
        } else if (!strcmp(key, XNInputStyle)) {
            if (!allowSetReadOnly) {
                break;
            }
            GET_XIC_STRUCT(inputConnection)->style =
                va_arg(arguments, XIMStyle);
            if (!styleIsSupported(GET_XIC_STRUCT(inputConnection)->style))
                break;
        } else if (!strcmp(key, XNClientWindow)) {
            Window clientWindow = va_arg(arguments, Window);
            if (!IS_TYPE(clientWindow, WINDOW) ||
                clientWindow == SCREEN_WINDOW) {
                break;
            }
            Window topLevel = clientWindow;
            while (GET_PARENT(topLevel) != SCREEN_WINDOW) {
                topLevel = GET_PARENT(topLevel);
            }
            if (IS_MAPPED_TOP_LEVEL_WINDOW(topLevel)) {
                SDL_Window *sdlWindow = GET_WINDOW_STRUCT(topLevel)->sdlWindow;
                SDL_RaiseWindow(sdlWindow);
                if (GET_XIC_STRUCT(inputConnection)->inputRect) {
                    SDL_SetTextInputRect(
                        GET_XIC_STRUCT(inputConnection)->inputRect);
                }
                /* SDL text input tracks the focused IC; XSelectInput restarts
                 * it when the selected key masks require text events. */
                SDL_StopTextInput();
            }
            GET_XIC_STRUCT(inputConnection)->client = clientWindow;
            if (GET_XIC_STRUCT(inputConnection)->focus == None)
                GET_XIC_STRUCT(inputConnection)->focus = clientWindow;
        } else if (!strcmp(key, XNFocusWindow)) {
            Window focusWindow = va_arg(arguments, Window);
            if (!IS_TYPE(focusWindow, WINDOW) || focusWindow == SCREEN_WINDOW) {
                break;
            }
            GET_XIC_STRUCT(inputConnection)->focus = focusWindow;
        } else if (!strcmp(key, XNPreeditAttributes)) {
            if (!parsePreEditAttributes(inputConnection,
                                        va_arg(arguments, XVaNestedList)))
                break;
        } else if (!strcmp(key, XNStatusAttributes)) {
            if (!parseStatusAttributes(inputConnection,
                                       va_arg(arguments, XVaNestedList)))
                break;
        } else if (!strcmp(key, XNFilterEvents)) {
            GET_XIC_STRUCT(inputConnection)->eventFilter =
                va_arg(arguments, unsigned long);
        } else if (!strcmp(key, XNGeometryCallback) ||
                   !strcmp(key, XNDestroyCallback) ||
                   !strcmp(key, XNResourceName) ||
                   !strcmp(key, XNResourceClass)) {
            (void) va_arg(arguments, void *);
        } else {
            break;
        }
    }
    return key;
}

XIC XCreateIC(XIM inputMethod, ...)
{
    XIC inputConnection = malloc(sizeof(_XIC));
    if (!inputConnection) {
        handleOutOfMemory(0, (Display *) inputMethod, 0, 0);
        return NULL;
    }
    GET_XIC_STRUCT(inputConnection)->display = (Display *) inputMethod;
    GET_XIC_STRUCT(inputConnection)->style = XIMUndefined;
    GET_XIC_STRUCT(inputConnection)->client = None;
    GET_XIC_STRUCT(inputConnection)->focus = None;
    GET_XIC_STRUCT(inputConnection)->fontSet = NULL;
    GET_XIC_STRUCT(inputConnection)->inputRect = NULL;
    GET_XIC_STRUCT(inputConnection)->eventFilter =
        KeyPressMask | KeyReleaseMask;
    GET_XIC_STRUCT(inputConnection)->preeditForeground = 0;
    GET_XIC_STRUCT(inputConnection)->preeditBackground = 0;
    GET_XIC_STRUCT(inputConnection)->statusForeground = 0;
    GET_XIC_STRUCT(inputConnection)->statusBackground = 0;
    va_list argumentList;
    va_start(argumentList, inputMethod);
    char *key;
    if ((key = setICValues(inputConnection, argumentList, True))) {
        LOG("setICValues failed in %s because of key %s!\n", __func__, key);
        free(inputConnection);
        va_end(argumentList);
        return NULL;
    }
    va_end(argumentList);
    if (GET_XIC_STRUCT(inputConnection)->style == XIMUndefined) {
        free(inputConnection);
        return NULL;
    }
    return inputConnection;
}

char *XSetICValues(XIC inputConnection, ...)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XGetICValues.3.xhtml
    va_list argumentList;
    va_start(argumentList, inputConnection);
    char *res = setICValues(inputConnection, argumentList, False);
    va_end(argumentList);
    return res;
}

XVaNestedList XVaCreateNestedList(int dummy, ...)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XVaCreateNestedList.3.xhtml
    (void) dummy;
    va_list argumentList, argCount;
    size_t i = 0, nArgs = 0;
    void *item;
    va_start(argumentList, dummy);
    va_copy(argCount, argumentList);
    while (va_arg(argCount, void *)) {
        nArgs++;
    }
    va_end(argCount);
    void **list = malloc(sizeof(void *) * (nArgs + 1));
    if (!list) {
        va_end(argumentList);
        return NULL;
    }
    while ((item = va_arg(argumentList, void *))) {
        list[i++] = item;
    }
    va_end(argumentList);
    list[i] = NULL;
    return list;
}

char *XGetICValues(XIC inputConnection, ...)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XGetICValues.3.xhtml
    va_list argumentList;
    va_start(argumentList, inputConnection);
    char *key = NULL;
    while ((key = va_arg(argumentList, char *))) {
        if (!strcmp(key, XNInputStyle)) {
            XIMStyle *styleReturn = va_arg(argumentList, XIMStyle *);
            *styleReturn = GET_XIC_STRUCT(inputConnection)->style;
        } else if (!strcmp(key, XNClientWindow)) {
            Window *clientReturn = va_arg(argumentList, Window *);
            *clientReturn = GET_XIC_STRUCT(inputConnection)->client;
        } else if (!strcmp(key, XNFocusWindow)) {
            Window *focusReturn = va_arg(argumentList, Window *);
            *focusReturn = GET_XIC_STRUCT(inputConnection)->focus;
        } else if (!strcmp(key, XNPreeditAttributes)) {
            if (!fillPreEditAttributes(inputConnection,
                                       va_arg(argumentList, XVaNestedList)))
                break;
        } else if (!strcmp(key, XNStatusAttributes)) {
            if (!fillStatusAttributes(inputConnection,
                                      va_arg(argumentList, XVaNestedList)))
                break;
        } else if (!strcmp(key, XNFilterEvents)) {
            unsigned long *filterEvents = va_arg(argumentList, unsigned long *);
            *filterEvents = GET_XIC_STRUCT(inputConnection)->eventFilter;
        } else if (!strcmp(key, XNGeometryCallback) ||
                   !strcmp(key, XNDestroyCallback) ||
                   !strcmp(key, XNResourceName) ||
                   !strcmp(key, XNResourceClass)) {
            (void) va_arg(argumentList, void *);
        } else {
            break;
        }
    }
    va_end(argumentList);
    return key;
}

void XSetICFocus(XIC inputConnection)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XSetICFocus.3.
    // Nothing to do, focus management handled by SDL.
}

char *XGetIMValues(XIM inputMethod, ...)
{
    va_list argumentList;
    va_start(argumentList, inputMethod);
    char *key;
    while ((key = va_arg(argumentList, char *))) {
        if (!strcmp(key, XNQueryInputStyle)) {
            XIMStyles **styles = va_arg(argumentList, XIMStyles **);
            if (!styles) {
                break;
            }
            XIMStyles *copy = malloc(sizeof(*copy) + sizeof(SUPPORTED_STYLES));
            if (!copy)
                break;
            copy->count_styles = supportedStyles.count_styles;
            copy->supported_styles = (XIMStyle *) (copy + 1);
            memcpy(copy->supported_styles, SUPPORTED_STYLES,
                   sizeof(SUPPORTED_STYLES));
            *styles = copy;
        } else {
            break;
        }
    }
    va_end(argumentList);
    return key;
}

char *XSetIMValues(XIM inputMethod, ...)
{
    // https://www.x.org/archive/X11R7.6/doc/man/man3/XSetIMValues.3.xhtml
    WARN_UNIMPLEMENTED;
    return NULL;
}

void XFreeFontSet(Display *display, XFontSet font_set)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XCreateFontSet.3.xhtml
    if (!font_set)
        return;
    CompatFontSet *set = GET_FONT_SET(font_set);
    if (set->font)
        XFreeFont(display ? display : set->display, set->font);
    free(set->baseName);
    free(set->locale);
    free(set);
}

void XFreeStringList(char **list)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XFreeStringList.3.xhtml
    if (!list)
        return;
    for (char **item = list; *item; item++)
        free(*item);
    free(list);
}

static char *firstFontSetPattern(_Xconst char *base_font_name_list)
{
    if (!base_font_name_list || !*base_font_name_list)
        return strdup("fixed");
    const char *start = base_font_name_list;
    while (*start == ' ' || *start == '\t')
        start++;
    const char *end = start;
    while (*end && *end != ',')
        end++;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        end--;
    if (end == start)
        return strdup("fixed");
    size_t len = (size_t) (end - start);
    char *pattern = malloc(len + 1);
    if (!pattern)
        return NULL;
    memcpy(pattern, start, len);
    pattern[len] = '\0';
    return pattern;
}

static char *nativeLikeFontSetAlias(const char *pattern)
{
    if (!pattern)
        return NULL;
    if (strstr(pattern, "*medium*") || strstr(pattern, "*medium-r*")) {
        if (strstr(pattern, "--14"))
            return strdup("7x14");
        if (strstr(pattern, "--18"))
            return strdup("9x18");
        if (strstr(pattern, "--24"))
            return strdup("12x24");
    }
    if (pattern[0] == '*' && strstr(pattern, "-times-"))
        return strdup("fixed");
    return NULL;
}

XFontSet XCreateFontSet(Display *display,
                        _Xconst char *base_font_name_list,
                        char ***missing_charset_list_return,
                        int *missing_charset_count_return,
                        char **def_string_return)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/XCreateFontSet.3.xhtml
    if (missing_charset_list_return)
        *missing_charset_list_return = NULL;
    if (missing_charset_count_return)
        *missing_charset_count_return = 0;
    if (def_string_return)
        *def_string_return = "";

    char *pattern = firstFontSetPattern(base_font_name_list);
    if (!pattern)
        return NULL;
    /* Try the alias first when one exists; if it fails (or there's no
     * alias) try the original caller-supplied pattern before falling
     * back to "fixed" — otherwise an alias miss silently downgrades a
     * loadable user pattern to the default font. */
    char *loadName = nativeLikeFontSetAlias(pattern);
    XFontStruct *font = NULL;
    if (loadName)
        font = XLoadQueryFont(display, loadName);
    if (!font)
        font = XLoadQueryFont(display, pattern);
    free(loadName);
    if (!font && strcmp(pattern, "fixed") != 0)
        font = XLoadQueryFont(display, "fixed");
    if (!font) {
        free(pattern);
        return NULL;
    }

    CompatFontSet *set = calloc(1, sizeof(*set));
    if (!set) {
        XFreeFont(display, font);
        free(pattern);
        return NULL;
    }
    set->display = display;
    set->font = font;
    set->fontStructList[0] = font;
    set->fontNameList[0] = pattern;
    set->baseName = pattern;
    const char *locale = setlocale(LC_CTYPE, NULL);
    set->locale = strdup(locale ? locale : "C");
    set->extents.max_ink_extent.x = 0;
    set->extents.max_ink_extent.y = (short) -font->ascent;
    set->extents.max_ink_extent.width = font->max_bounds.width;
    set->extents.max_ink_extent.height =
        (unsigned short) (font->ascent + font->descent);
    set->extents.max_logical_extent = set->extents.max_ink_extent;
    return (XFontSet) set;
}

int Xutf8LookupString(XIC inputConnection,
                      XKeyPressedEvent *event,
                      char *buffer_return,
                      int bytes_buffer,
                      KeySym *keysym_return,
                      Status *status_return)
{
    // http://www.x.org/archive/X11R7.6/doc/man/man3/Xutf8LookupString.3.xhtml
    if (event->keycode == 0) {
        if (!pendingText) {
            *status_return = XLookupNone;
            return 0;
        }
        LOG("InputMethod Event! text = '%s'.\n", pendingText);
        int textLen = strlen(pendingText) + 1;
        if (textLen > bytes_buffer) {
            *status_return = XBufferOverflow;
            return textLen;
        }
        if (textLen > 0) {
            *status_return = XLookupBoth;
            *keysym_return = getKeySymForChar(pendingText[textLen - 2]);
        } else {
            *status_return = XLookupChars;
        }
        memcpy(buffer_return, pendingText, textLen);
        pendingText = NULL;
        return textLen;
    } else {
        LOG("Normal Event, Keycode = %d, '%c'\n", event->keycode,
            event->keycode);
        if (event->keycode <= 127) {
            *status_return = XLookupBoth;
            *buffer_return = event->keycode;
        } else {
            *status_return = XLookupKeySym;
        }
        *keysym_return =
            XkbKeycodeToKeysym(event->display, event->keycode, 0, 0);
        return 1;
    }
}

Bool XRegisterIMInstantiateCallback(Display *display,
                                    struct _XrmHashBucketRec *rdb,
                                    char *resName,
                                    char *resClass,
                                    XIDProc callback,
                                    XPointer callbackData)
{
    // https://linux.die.net/man/3/xregisteriminstantiatecallback
    WARN_UNIMPLEMENTED;
    return False;
}
Bool XUnregisterIMInstantiateCallback(Display *display,
                                      struct _XrmHashBucketRec *rdb,
                                      char *resName,
                                      char *resClass,
                                      XIDProc callback,
                                      XPointer callbackData)
{
    // https://linux.die.net/man/3/xunregisteriminstantiatecallback
    WARN_UNIMPLEMENTED;
    return False;
}
