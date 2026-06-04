#ifndef INPUTMETHOD_H
#define INPUTMETHOD_H

#include "X11/Xlib.h"
#include "window.h"

typedef Display _XIM;

typedef struct CompatFontSet {
    Display *display;
    XFontStruct *font;
    XFontStruct *fontStructList[1];
    char *fontNameList[1];
    char *baseName;
    char *locale;
    XFontSetExtents extents;
} CompatFontSet;
#define GET_FONT_SET(fontSet) ((CompatFontSet *) (fontSet))

typedef struct {
    Display *display;
    XIMStyle style;
    Window client;
    Window focus;
    XFontSet fontSet;
    SDL_Rect *inputRect;
    unsigned long eventFilter;
    unsigned long preeditForeground;
    unsigned long preeditBackground;
    unsigned long statusForeground;
    unsigned long statusBackground;
} _XIC;
#define GET_XIC_STRUCT(inputConnection) ((_XIC *) inputConnection)

#define XIMUndefined 0x0000L
#define defaultLocaleModifierList "DEFAULT"
static const XIMStyle SUPPORTED_STYLES[] = {
    XIMPreeditArea | XIMStatusNothing,     XIMPreeditArea | XIMStatusNone,
    XIMPreeditPosition | XIMStatusNothing, XIMPreeditPosition | XIMStatusNone,
    XIMPreeditNothing | XIMStatusNothing,  XIMPreeditNothing | XIMStatusNone,
    XIMPreeditNone | XIMStatusNothing,     XIMPreeditNone | XIMStatusNone};
static const XIMStyles supportedStyles = {
    sizeof(SUPPORTED_STYLES) / sizeof(SUPPORTED_STYLES[0]),
    (XIMStyle *) &SUPPORTED_STYLES[0],
};

void inputMethodSetCurrentText(char *text);

#endif /* INPUTMETHOD_H */
