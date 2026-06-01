#include "input.h"
#include "X11/Xlibint.h"
#include "X11/Xutil.h"
#include "X11/keysym.h"
#include "X11/XKBlib.h"
#include "keysymlist.h"
#include "errors.h"
#include "display.h"

Window keyboardFocus = None;
int revertTo = RevertToParent;

Window getKeyboardFocus()
{
    LOG("GET keyboard focus is %lu\n", keyboardFocus);
    return keyboardFocus;
}

void setKeyboardFocus(Window window)
{
    LOG("SET keyboard focus is %lu\n", window);
    keyboardFocus = window;
}

int XSelectInput(Display *display, Window window, long event_mask)
{
    // https://tronche.com/gui/x/xlib/event-handling/XSelectInput.html
    WARN_UNIMPLEMENTED;
    TYPE_CHECK(window, WINDOW, display, 0);
    GET_WINDOW_STRUCT(window)->eventMask = event_mask;
    LOG("%s: %ld, %ld\n", __func__, event_mask & KeyPressMask,
        event_mask & KeyReleaseMask);
    if (event_mask & KeyPressMask || event_mask & KeyReleaseMask) {
        /* Suppress SDL_TEXTINPUT so each key produces a single XKey event
         * instead of doubling up with a translated text event. */
        SDL_StopTextInput();
        setKeyboardFocus(window);
    }
    return 1;
}

KeySym *XGetKeyboardMapping(Display *display,
                            KeyCode first_keycode,
                            int count,
                            int *keysyms_per_keycode)
{
    if (count < 0) {
        handleError(0, display, None, 0, BadValue, 0);
        return NULL;
    }
    if (first_keycode < display->min_keycode) {
        LOG("The value specified in first_keycode must be greater than or "
            "equal to min_keycode %s: %d\n",
            __func__, first_keycode);
        handleError(0, display, None, 0, BadValue, 0);
        return NULL;
    }
    if (first_keycode + count - 1 > display->max_keycode) {
        LOG("The (first_keycode + keycode_count - 1) expression must be less "
            "than or equal to max_keycode %s: %d\n",
            __func__, first_keycode);
        handleError(0, display, None, 0, BadValue, 0);
        return NULL;
    }

    KeySym *mapping = calloc((size_t) count, sizeof(KeySym));
    if (!mapping) {
        LOG("Cannot alloc mapping %s\n", __func__);
        return NULL;
    }

    int firstAfterRange = first_keycode + count;
    for (int kc = SDLK_0; kc <= SDLK_9; kc++) {
        if (first_keycode <= kc && kc < firstAfterRange)
            mapping[kc - first_keycode] = XkbKeycodeToKeysym(display, kc, 0, 0);
    }
    for (int kc = SDLK_a; kc <= SDLK_z; kc++) {
        if (first_keycode <= kc && kc < firstAfterRange)
            mapping[kc - first_keycode] = XkbKeycodeToKeysym(display, kc, 0, 0);
    }
    /* Iterate in reverse so earlier table entries win for keycodes that map
     * to multiple keysyms. */
    for (int i = SDL_KEYCODE_TO_KEYSYM_LENGTH - 1; i >= 0; i--) {
        KeyCode kc = SDLKeycodeToKeySym[i].keycode & 0xFF;
        if (first_keycode <= kc && kc < firstAfterRange &&
            mapping[kc - first_keycode] == NoSymbol) {
            mapping[kc - first_keycode] = SDLKeycodeToKeySym[i].keysym;
        }
    }

    if (keysyms_per_keycode) {
        *keysyms_per_keycode = 1;
    }
    return mapping;
}

KeyCode XKeysymToKeycode(Display *display, KeySym keysym)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XKeysymToKeycode.html
    if (keysym >= XK_0 && keysym <= XK_9)
        return SDLK_0 + (keysym - XK_0);
    if (keysym >= XK_a && keysym <= XK_z)
        return SDLK_a + (keysym - XK_a);
    for (int i = SDL_KEYCODE_TO_KEYSYM_LENGTH - 1; i >= 0; i--) {
        if (SDLKeycodeToKeySym[i].keysym == keysym) {
            return SDLKeycodeToKeySym[i].keycode & 0xFF;
        }
    }
    LOG("%s: Got unimplemented keysym %lu\n", __func__, keysym);
    return 0;
}

KeySym XLookupKeysym(XKeyEvent *key_event, int index)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XLookupKeysym.html
    return XkbKeycodeToKeysym(key_event->display, key_event->keycode, 0, index);
}

KeySym XStringToKeysym(_Xconst char *string)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XStringToKeysym.html
    if (!string)
        return NoSymbol;
    if (strlen(string) == 1) {
        char chr = string[0];
        if ((chr >= '0' && chr <= '9') || (chr >= 'a' && chr <= 'z') ||
            (chr >= 'A' && chr <= 'Z')) {
            return (KeySym) ((long) chr);
        }
    }
    for (size_t i = 0; i < KEY_SYM_LIST_LENGTH; i++) {
        if (strcmp(KEY_SYM_LIST[i].name, string) == 0) {
            return KEY_SYM_LIST[i].keySym;
        }
    }
    return NoSymbol;
}

char *XKeysymToString(KeySym keysym)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XKeysymToString.html
    for (size_t i = 0; i < KEY_SYM_LIST_LENGTH; i++) {
        if (KEY_SYM_LIST[i].keySym == keysym) {
            return (char *) KEY_SYM_LIST[i].name;
        }
    }
    return NULL;
}

KeySym XkbKeycodeToKeysym(Display *display,
                          KeyCode keycode,
                          int group,
                          int level)
{
    (void) display;
    (void) group;
    (void) level;
    if (keycode >= SDLK_0 && keycode <= SDLK_9)
        return XK_0 + (keycode - SDLK_0);
    if (keycode >= SDLK_a && keycode <= SDLK_z)
        return XK_a + (keycode - SDLK_a);
    for (int i = SDL_KEYCODE_TO_KEYSYM_LENGTH - 1; i >= 0; i--) {
        if ((SDLKeycodeToKeySym[i].keycode & 0xFF) == keycode) {
            return SDLKeycodeToKeySym[i].keysym;
        }
    }
    LOG("%s: Got unimplemented keycode %c\n", __func__, keycode);
    return NoSymbol;
}

KeySym XKeycodeToKeysym(Display *display, KeyCode keycode, int index)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XKeycodeToKeysym.html
    return XkbKeycodeToKeysym(display, keycode, 0, index);
}

unsigned int XkbKeysymToModifiers(Display *display, KeySym keysym)
{
    (void) display;
    if (keysym >= XK_A && keysym <= XK_Z) {
        return ShiftMask;
    }
    switch (keysym) {
    case XK_Shift_L:
    case XK_Shift_R:
        return ShiftMask;
    case XK_Caps_Lock:
    case XK_Shift_Lock:
        return LockMask;
    case XK_Control_L:
    case XK_Control_R:
        return ControlMask;
    case XK_Alt_L:
        return Mod1Mask;
    case XK_Num_Lock:
        return Mod2Mask;
    case XK_Scroll_Lock:
        return Mod3Mask;
    case XK_Alt_R:
        return Mod4Mask;
    }
    return 0;
}

Bool XkbLookupKeySym(Display *display,
                     KeyCode keycode,
                     unsigned int modifiers,
                     unsigned int *modifiers_return,
                     KeySym *keysym_return)
{
    KeySym keysym = XkbKeycodeToKeysym(display, keycode, 0, 0);
    unsigned int consumedModifiers = 0;
    if ((modifiers & ShiftMask) && keysym >= XK_a && keysym <= XK_z) {
        keysym = XK_A + (keysym - XK_a);
        consumedModifiers |= ShiftMask;
    }
    if (modifiers_return) {
        *modifiers_return = consumedModifiers;
    }
    if (keysym_return) {
        *keysym_return = keysym;
    }
    return keysym != NoSymbol;
}

int XLookupString(XKeyEvent *event_struct,
                  char *buffer_return,
                  int bytes_buffer,
                  KeySym *keysym_return,
                  XComposeStatus *status_in_out)
{
    // https://tronche.com/gui/x/xlib/utilities/XLookupString.html
    if (buffer_return)
        *buffer_return = event_struct->keycode;
    if (keysym_return)
        *keysym_return = XkbKeycodeToKeysym(event_struct->display,
                                            event_struct->keycode, 0, 0);
    return 1;
}

XModifierKeymap *XGetModifierMapping(Display *display)
{
    // https://tronche.com/gui/x/xlib/input/XGetModifierMapping.html
    SET_X_SERVER_REQUEST(display, X_GetModifierMapping);
    enum { KEYS_PER_MODIFIER = 2, MODIFIER_COUNT = 8 };
    static const struct {
        int mapIndex;
        int slot;
        KeySym keysym;
    } bindings[] = {
        {ShiftMapIndex, 0, XK_Shift_L},     {ShiftMapIndex, 1, XK_Shift_R},
        {LockMapIndex, 0, XK_Caps_Lock},    {ControlMapIndex, 0, XK_Control_L},
        {ControlMapIndex, 1, XK_Control_R}, {Mod1MapIndex, 0, XK_Alt_L},
        {Mod2MapIndex, 0, XK_Num_Lock},     {Mod3MapIndex, 0, XK_Scroll_Lock},
        {Mod4MapIndex, 0, XK_Alt_R},
    };
    XModifierKeymap *modifierKeymap = malloc(sizeof(XModifierKeymap));
    if (!modifierKeymap) {
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    modifierKeymap->max_keypermod = KEYS_PER_MODIFIER;
    modifierKeymap->modifiermap =
        calloc(MODIFIER_COUNT * KEYS_PER_MODIFIER, sizeof(KeyCode));
    if (!modifierKeymap->modifiermap) {
        free(modifierKeymap);
        handleOutOfMemory(0, display, 0, 0);
        return NULL;
    }
    for (size_t i = 0; i < sizeof(bindings) / sizeof(bindings[0]); i++) {
        modifierKeymap->modifiermap[bindings[i].mapIndex * KEYS_PER_MODIFIER +
                                    bindings[i].slot] =
            XKeysymToKeycode(display, bindings[i].keysym);
    }
    return modifierKeymap;
}

int XFreeModifiermap(XModifierKeymap *modmap)
{
    // https://tronche.com/gui/x/xlib/input/XFreeModifiermap.html
    free(modmap->modifiermap);
    free(modmap);
    return 1;
}

int XGetInputFocus(Display *display,
                   Window *focus_return,
                   int *revert_to_return)
{
    // https://tronche.com/gui/x/xlib/input/XGetInputFocus.html
    SET_X_SERVER_REQUEST(display, X_GetInputFocus);
    *focus_return = getKeyboardFocus();
    if (*focus_return == None)
        *focus_return = (Window) PointerRoot;
    *revert_to_return = revertTo;
    return 1;
}

int XSetInputFocus(Display *display, Window focus, int revert_to, Time time)
{
    // https://tronche.com/gui/x/xlib/input/XSetInputFocus.html
    SET_X_SERVER_REQUEST(display, X_SetInputFocus);
    WARN_UNIMPLEMENTED;
    revertTo = revert_to;
    return 1;
}

int XGrabKeyboard(Display *display,
                  Window grab_window,
                  Bool owner_events,
                  int pointer_mode,
                  int keyboard_mode,
                  Time time)
{
    // https://tronche.com/gui/x/xlib/input/XGrabKeyboard.html
    SET_X_SERVER_REQUEST(display, X_GrabKeyboard);
    WARN_UNIMPLEMENTED;
    return 1;
}

int XUngrabKeyboard(Display *display, Time time)
{
    // https://tronche.com/gui/x/xlib/input/XUngrabKeyboard.html
    SET_X_SERVER_REQUEST(display, X_UngrabKeyboard);
    WARN_UNIMPLEMENTED;
    return 1;
}

int XRefreshKeyboardMapping(XMappingEvent *event_map)
{
    // https://tronche.com/gui/x/xlib/utilities/keyboard/XRefreshKeyboardMapping.html
    WARN_UNIMPLEMENTED;
    return 1;
}

int XDisplayKeycodes(Display *display,
                     int *minKeyCodesReturn,
                     int *maxKeyCodesReturn)
{
    // https://linux.die.net/man/3/xdisplaykeycodes
    if (minKeyCodesReturn) {
        *minKeyCodesReturn = display->min_keycode;
    }
    if (maxKeyCodesReturn) {
        *maxKeyCodesReturn = display->max_keycode;
    }
    return 1;
}
