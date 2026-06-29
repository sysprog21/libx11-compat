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

/* One committed IM string awaiting a lookup. SDL can deliver several commits in
 * a burst that all get converted to keycode==0 KeyPress events before the
 * client looks any up, and event scans (XCheckWindowEvent and friends) can
 * deliver those events out of order. Each commit therefore carries a unique id
 * that the synthetic KeyPress stores in xkey.subwindow, so the lookup retrieves
 * its own commit by id regardless of delivery order rather than relying on a
 * queue.
 */
typedef struct PendingCommit {
    unsigned long id;
    char *text;
    struct PendingCommit *next;
} PendingCommit;

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
    Bool hasPreeditBackground;
    unsigned long statusForeground;
    unsigned long statusBackground;
    XIMCallback preeditStartCallback;
    XIMCallback preeditDoneCallback;
    XIMCallback preeditDrawCallback;
    XIMCallback preeditCaretCallback;
    Bool hasPreeditStartCallback;
    Bool hasPreeditDoneCallback;
    Bool hasPreeditDrawCallback;
    Bool hasPreeditCaretCallback;
    Bool preeditActive;
    int preeditLength;
    Bool hasPreeditDrawRect;
    Window preeditDrawWindow;
    SDL_Rect preeditDrawRect;
    XFontStruct *preeditFont;
    Bool preeditFontResolved;
    Bool destroying;
} _XIC;
#define GET_XIC_STRUCT(inputConnection) ((_XIC *) inputConnection)

#define XIMUndefined 0x0000L
#define defaultLocaleModifierList "DEFAULT"
/* The styles XCreateIC accepts and XGetIMValues advertises must be the same
 * set, otherwise a client that queries XNQueryInputStyle never selects a style
 * that actually works. Area and Position are served by the internal over-the-
 * spot renderer; Callbacks by the client's own draw callbacks.
 */
static const XIMStyle SUPPORTED_STYLES[] = {
    XIMPreeditArea | XIMStatusNothing,      XIMPreeditArea | XIMStatusNone,
    XIMPreeditPosition | XIMStatusNothing,  XIMPreeditPosition | XIMStatusNone,
    XIMPreeditCallbacks | XIMStatusNothing, XIMPreeditCallbacks | XIMStatusNone,
    XIMPreeditNothing | XIMStatusNothing,   XIMPreeditNothing | XIMStatusNone,
    XIMPreeditNone | XIMStatusNothing,      XIMPreeditNone | XIMStatusNone};
static const XIMStyles supportedStyles = {
    sizeof(SUPPORTED_STYLES) / sizeof(SUPPORTED_STYLES[0]),
    (XIMStyle *) &SUPPORTED_STYLES[0],
};

unsigned long inputMethodSetCurrentText(char *text);
void inputMethodReset(void);
Bool inputMethodHasFocusedIC(void);
Bool inputMethodHasActiveTextInput(void);
void inputMethodNoteTextInputStopped(void);
void inputMethodUnsetFocus(XIC inputConnection);
void inputMethodHandlePreedit(const char *text);
char *inputMethodPendingText(unsigned long commitId);
void inputMethodConsumePendingText(unsigned long commitId);
KeySym getKeySymForChar(char c);

#endif /* INPUTMETHOD_H */
