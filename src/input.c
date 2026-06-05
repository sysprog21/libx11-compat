#include "input.h"
#include "X11/Xlibint.h"
#include "X11/Xutil.h"
#include "X11/keysym.h"
#include "X11/HPkeysym.h"
#include "X11/XKBlib.h"
#include "keysymlist.h"
#include "errors.h"
#include "display.h"

Window keyboardFocus = None;
int revertTo = RevertToParent;

static const struct {
    KeySym keysym;
    const char *name;
} osfKeysyms[] = {
    {osfXK_Copy, "osfCopy"},
    {osfXK_Cut, "osfCut"},
    {osfXK_Paste, "osfPaste"},
    {osfXK_BackTab, "osfBackTab"},
    {osfXK_BackSpace, "osfBackSpace"},
    {osfXK_Clear, "osfClear"},
    {osfXK_Escape, "osfEscape"},
    {osfXK_AddMode, "osfAddMode"},
    {osfXK_PrimaryPaste, "osfPrimaryPaste"},
    {osfXK_QuickPaste, "osfQuickPaste"},
    {osfXK_PageLeft, "osfPageLeft"},
    {osfXK_PageUp, "osfPageUp"},
    {osfXK_PageDown, "osfPageDown"},
    {osfXK_PageRight, "osfPageRight"},
    {osfXK_Activate, "osfActivate"},
    {osfXK_MenuBar, "osfMenuBar"},
    {osfXK_Left, "osfLeft"},
    {osfXK_Up, "osfUp"},
    {osfXK_Right, "osfRight"},
    {osfXK_Down, "osfDown"},
    {osfXK_EndLine, "osfEndLine"},
    {osfXK_BeginLine, "osfBeginLine"},
    {osfXK_EndData, "osfEndData"},
    {osfXK_BeginData, "osfBeginData"},
    {osfXK_PrevMenu, "osfPrevMenu"},
    {osfXK_NextMenu, "osfNextMenu"},
    {osfXK_PrevField, "osfPrevField"},
    {osfXK_NextField, "osfNextField"},
    {osfXK_Select, "osfSelect"},
    {osfXK_Insert, "osfInsert"},
    {osfXK_Undo, "osfUndo"},
    {osfXK_Menu, "osfMenu"},
    {osfXK_Cancel, "osfCancel"},
    {osfXK_Help, "osfHelp"},
    {osfXK_SelectAll, "osfSelectAll"},
    {osfXK_DeselectAll, "osfDeselectAll"},
    {osfXK_Reselect, "osfReselect"},
    {osfXK_Extend, "osfExtend"},
    {osfXK_Restore, "osfRestore"},
    {osfXK_Delete, "osfDelete"},
};

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
    TYPE_CHECK(window, WINDOW, display, 0);
    GET_WINDOW_STRUCT(window)->eventMask = event_mask;
    LOG("%s: window=%lu mask=0x%lx keyPress=0x%lx keyRelease=0x%lx "
        "exposure=0x%lx structure=0x%lx property=0x%lx\n",
        __func__, window, event_mask, event_mask & KeyPressMask,
        event_mask & KeyReleaseMask, event_mask & ExposureMask,
        event_mask & StructureNotifyMask, event_mask & PropertyChangeMask);
    if (event_mask & KeyPressMask || event_mask & KeyReleaseMask) {
        /* Suppress SDL_TEXTINPUT so each key produces a single XKey event
         * instead of doubling up with a translated text event. */
        SDL_StopTextInput();
    }
    /* Previously this also called setKeyboardFocus(window) whenever
     * KeyPress/Release was selected. That made every widget that called
     * XSelectInput steal focus from whatever Motif had explicitly
     * focused via XSetInputFocus, and the focus dance broke Motif
     * dialogs (e.g., piano's Help popup never appeared because the
     * focus restored to the parent before the dialog finished mapping).
     * XSetInputFocus is now the single source of truth. */
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
    if (keysym >= XK_A && keysym <= XK_Z)
        return SDLK_a + (keysym - XK_A);
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
    for (size_t i = 0; i < sizeof(osfKeysyms) / sizeof(osfKeysyms[0]); i++) {
        if (strcmp(osfKeysyms[i].name, string) == 0)
            return osfKeysyms[i].keysym;
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
    for (size_t i = 0; i < sizeof(osfKeysyms) / sizeof(osfKeysyms[0]); i++) {
        if (osfKeysyms[i].keysym == keysym)
            return (char *) osfKeysyms[i].name;
    }
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
    case XK_Alt_R:
        /* Match the standard X server convention: both Alt halves share
         * Mod1Mask. Keeping them separate broke modmap/state consistency
         * for clients that fold convertModifierState(KMOD_ALT) against
         * XGetModifierMapping. */
        return Mod1Mask;
    case XK_Num_Lock:
        return Mod2Mask;
    case XK_Scroll_Lock:
        return Mod3Mask;
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
        {Mod1MapIndex, 1, XK_Alt_R},        {Mod2MapIndex, 0, XK_Num_Lock},
        {Mod3MapIndex, 0, XK_Scroll_Lock},
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
    (void) time;
    revertTo = revert_to;
    /* PointerRoot and None are valid Xlib targets that the X server
     * interprets as "follow pointer" and "no focus." Both map to our
     * keyboardFocus = None state since the SDL backend doesn't track a
     * separate pointer-following focus. */
    if (focus == PointerRoot || focus == None) {
        setKeyboardFocus(None);
    } else {
        setKeyboardFocus(focus);
    }
    return 1;
}

/* Active keyboard grab (XGrabKeyboard). Single slot since the X protocol
 * allows only one keyboard grab at a time per client. Motif uses this
 * to redirect keystrokes to modal dialogs (e.g., the Help popup); the
 * Ungrab clears the slot so the previous focus regains routing. */
static struct {
    Bool active;
    Window grab_window;
    Bool owner_events;
} keyboardGrab;

int XGrabKeyboard(Display *display,
                  Window grab_window,
                  Bool owner_events,
                  int pointer_mode,
                  int keyboard_mode,
                  Time time)
{
    // https://tronche.com/gui/x/xlib/input/XGrabKeyboard.html
    SET_X_SERVER_REQUEST(display, X_GrabKeyboard);
    (void) pointer_mode;
    (void) keyboard_mode;
    (void) time;
    keyboardGrab.active = True;
    keyboardGrab.grab_window = grab_window;
    keyboardGrab.owner_events = owner_events;
    return GrabSuccess;
}

int XUngrabKeyboard(Display *display, Time time)
{
    // https://tronche.com/gui/x/xlib/input/XUngrabKeyboard.html
    SET_X_SERVER_REQUEST(display, X_UngrabKeyboard);
    (void) time;
    keyboardGrab.active = False;
    keyboardGrab.grab_window = None;
    keyboardGrab.owner_events = False;
    return 1;
}

Window getGrabbedKeyboardWindow(void)
{
    return keyboardGrab.active ? keyboardGrab.grab_window : None;
}

Bool getKeyboardGrabOwnerEvents(void)
{
    return keyboardGrab.active && keyboardGrab.owner_events;
}

/* Passive key grabs registered via XGrabKey. Motif menus install one
 * grab per accelerator (Alt-F for File, Alt-E for Edit, etc.) on the
 * top-level shell at widget realize time; without tracking the grabs
 * here every key event landed at the focus window, accelerators
 * silently failed, and DEBUG builds spammed one warning per grab call.
 *
 * One global list across Displays mirrors what xlibe does. Motif demos
 * are single-display; if multi-Display threading becomes a goal the
 * list moves under a Display pointer field. */
typedef struct KeyGrab {
    int key;                /* X keycode, or AnyKey for all keys */
    unsigned int modifiers; /* modifier mask, or AnyModifier */
    Window grab_window;
    Bool owner_events;
    int pointer_mode;
    int keyboard_mode;
    struct KeyGrab *next;
} KeyGrab;

static KeyGrab *keyGrabs = NULL;

int XGrabKey(Display *display,
             int key,
             unsigned int modifiers,
             Window grab_window,
             Bool owner_events,
             int pointer_mode,
             int keyboard_mode)
{
    (void) display;
    KeyGrab *grab = calloc(1, sizeof(*grab));
    if (!grab)
        return BadAlloc;
    grab->key = key;
    grab->modifiers = modifiers;
    grab->grab_window = grab_window;
    grab->owner_events = owner_events;
    grab->pointer_mode = pointer_mode;
    grab->keyboard_mode = keyboard_mode;
    grab->next = keyGrabs;
    keyGrabs = grab;
    return Success;
}

int XUngrabKey(Display *display,
               int key,
               unsigned int modifiers,
               Window grab_window)
{
    (void) display;
    /* AnyKey / AnyModifier match every grab for that window per the
     * Xlib spec, so loop without short-circuit and remove every match. */
    KeyGrab **p = &keyGrabs;
    while (*p) {
        KeyGrab *g = *p;
        Bool keyMatch = (key == AnyKey || g->key == key);
        Bool modMatch = (modifiers == AnyModifier || g->modifiers == modifiers);
        Bool winMatch = (g->grab_window == grab_window);
        if (keyMatch && modMatch && winMatch) {
            *p = g->next;
            free(g);
        } else {
            p = &g->next;
        }
    }
    return Success;
}

Window findKeyGrabWindow(int keycode, unsigned int modifiers)
{
    for (KeyGrab *g = keyGrabs; g; g = g->next) {
        if ((g->key == AnyKey || g->key == keycode) &&
            (g->modifiers == AnyModifier || g->modifiers == modifiers)) {
            return g->grab_window;
        }
    }
    return None;
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
