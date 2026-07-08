#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XKB.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/shape.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xatom.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/Xlibint.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include "sdl-compat.h"
#include <wchar.h>
#include <fontconfig/fontconfig.h>

#include "drawing.h"
#include "events.h"
#include "gc.h"
#include "image.h"
#include "input.h"
#include "path/compose.h"
#include "path/edges.h"
#include "path/path.h"
#include "replay-target.h"
#include "state-snapshot.h"
#include "timeline.h"
#include "util.h"

int convertEvent(Display *display,
                 SDL_Event *sdlEvent,
                 XEvent *xEvent,
                 Bool freeInternalEvents);
extern Bool mouseFrozen;

/* Test hook: resolve an XLFD/wildcard name to its pixel size without loading a
 * font, so the table below can pin names that never resolve to a loadable face
 * (e.g. the "*-fixed-13-*" pixel/point ambiguity).
 */
extern int x11compat_font_pixel_size(const char *name);

#include <stdio.h>

static int preeditDrawCount;
static int preeditStartCount;
static int preeditDoneCount;
static int preeditCaretCount;
static int preeditCaretLastPos = -1;
static char preeditDrawText[32];

static void test_preedit_start_done_callback(XIM im,
                                             XPointer client_data,
                                             XPointer call_data)
{
    (void) im;
    (void) call_data;
    int *count = (int *) client_data;
    (*count)++;
}

static void test_preedit_draw_callback(XIM im,
                                       XPointer client_data,
                                       XPointer call_data)
{
    (void) im;
    (void) client_data;
    XIMPreeditDrawCallbackStruct *draw =
        (XIMPreeditDrawCallbackStruct *) call_data;
    preeditDrawCount++;
    preeditDrawText[0] = '\0';
    if (draw && draw->text && !draw->text->encoding_is_wchar &&
        draw->text->string.multi_byte) {
        snprintf(preeditDrawText, sizeof(preeditDrawText), "%.*s",
                 draw->text->length, draw->text->string.multi_byte);
    }
}

static void test_preedit_caret_callback(XIM im,
                                        XPointer client_data,
                                        XPointer call_data)
{
    (void) im;
    (void) client_data;
    XIMPreeditCaretCallbackStruct *caret =
        (XIMPreeditCaretCallbackStruct *) call_data;
    if (caret) {
        preeditCaretCount++;
        preeditCaretLastPos = caret->position;
    }
}

static void set_text_input_event(SDL_Event *event, const char *text)
{
    SDL_zero(*event);
    event->type = SDL_TEXTINPUT;
    XC_SET_TEXT_EVENT(*event, text);
}

static XIC reentrantIC;
static int reentrantDrawCount;
static int reentrantCaretCount;
static Bool reentrantDestroyOnDraw;

/* Pathological client: destroys its own IC from inside the draw callback. The
 * handler must not recurse, double free, or use the IC after this returns.
 */
static void test_preedit_reentrant_draw_callback(XIM im,
                                                 XPointer client_data,
                                                 XPointer call_data)
{
    (void) im;
    (void) client_data;
    (void) call_data;
    reentrantDrawCount++;
    if (reentrantDestroyOnDraw && reentrantIC) {
        XIC victim = reentrantIC;
        reentrantIC = NULL;
        XDestroyIC(victim);
    }
}

static void test_preedit_reentrant_caret_callback(XIM im,
                                                  XPointer client_data,
                                                  XPointer call_data)
{
    (void) im;
    (void) client_data;
    (void) call_data;
    reentrantCaretCount++;
}

static XIC focusSwitchVictim;
static int focusSwitchDoneCount;

/* Pathological client: the outgoing IC's done callback destroys the IC being
 * focused. XSetICFocus must not install or dereference that freed IC.
 */
static void test_focus_switch_done_callback(XIM im,
                                            XPointer client_data,
                                            XPointer call_data)
{
    (void) im;
    (void) client_data;
    (void) call_data;
    focusSwitchDoneCount++;
    if (focusSwitchVictim) {
        XIC victim = focusSwitchVictim;
        focusSwitchVictim = NULL;
        XDestroyIC(victim);
    }
}

#include <dirent.h>
#include <locale.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures = 0;

#define CHECK(condition, message)                                          \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, (message)); \
            failures++;                                                    \
            return 0;                                                      \
        }                                                                  \
    } while (0)

/* Fetch the next queued event of a given type, skipping unrelated events that a
 * process-global SDL event queue can leave ahead of a freshly pushed one (stale
 * StructureNotify, focus changes, in-process WM housekeeping). A bare
 * XNextEvent returns whichever event is at the head of the queue, so a
 * push-then-assert sequence is flaky whenever an earlier async event has not
 * been drained. XCheckTypedEvent scans the whole queue for the wanted type and
 * puts the rest back; the bounded pump covers events SDL has not surfaced yet,
 * and the trailing check picks up an event produced by the final pump. Matching
 * by type only (not window) mirrors the bare XNextEvent this replaces, so it
 * cannot regress when the host reports keyboard focus on a different window.
 *
 * Returns True and fills event on match, False if none arrived.
 */
static Bool next_typed_event(Display *display, int type, XEvent *event)
{
    for (int spins = 0; spins < 500; spins++) {
        if (XCheckTypedEvent(display, type, event))
            return True;
        SDL_PumpEvents();
        SDL_Delay(1);
    }
    return XCheckTypedEvent(display, type, event);
}

/* Wait for an SDL window flag to reach the wanted state. SDL applies window
 * state changes such as fullscreen asynchronously on real video drivers, so a
 * flag read immediately after the request can miss it. Poll briefly with an
 * event pump; a state that genuinely never lands still times out and fails.
 * Returns True once the flag matches wantSet, False on timeout.
 */
static Bool wait_window_flag(SDL_Window *w, Uint32 mask, Bool wantSet)
{
    for (int spins = 0; spins < 500; spins++) {
        if (((SDL_GetWindowFlags(w) & mask) != 0) == (wantSet != False))
            return True;
        SDL_PumpEvents();
        SDL_Delay(1);
    }
    return ((SDL_GetWindowFlags(w) & mask) != 0) == (wantSet != False);
}

static XEvent make_event(int type, Window window)
{
    XEvent event;
    memset(&event, 0, sizeof(event));
    event.xany.type = type;
    event.xany.window = window;
    if (type == ClientMessage) {
        event.xclient.type = ClientMessage;
        event.xclient.window = window;
        event.xclient.format = 32;
    } else if (type == Expose) {
        event.xexpose.type = Expose;
        event.xexpose.window = window;
        event.xexpose.width = 10;
        event.xexpose.height = 10;
    }
    return event;
}

static int font_struct_char_width(XFontStruct *font, unsigned int ch)
{
    if (!font)
        return 0;
    if (font->per_char && ch >= font->min_char_or_byte2 &&
        ch <= font->max_char_or_byte2) {
        return font->per_char[ch - font->min_char_or_byte2].width;
    }
    return font->min_bounds.width;
}

static int expect_map_state(Display *display, Window window, int expected)
{
    XWindowAttributes attrs;
    CHECK(XGetWindowAttributes(display, window, &attrs),
          "XGetWindowAttributes failed");
    CHECK(attrs.map_state == expected, "unexpected map_state");
    return 1;
}

static int modifier_slot_has(XModifierKeymap *map,
                             int modifier,
                             KeyCode keycode)
{
    for (int i = 0; i < map->max_keypermod; i++) {
        if (map->modifiermap[modifier * map->max_keypermod + i] == keycode) {
            return 1;
        }
    }
    return 0;
}

static int ignored_error(Display *display, XErrorEvent *event)
{
    (void) display;
    (void) event;
    return 0;
}

static int last_error_code = 0;

static int record_error(Display *display, XErrorEvent *event)
{
    (void) display;
    last_error_code = event->error_code;
    return 0;
}

static int extension_close_count = 0;

static int counted_extension_close(Display *display, XExtCodes *codes)
{
    (void) display;
    (void) codes;
    extension_close_count++;
    return 0;
}

static int pixel_is_rgb(SDL_Surface *surface,
                        int x,
                        int y,
                        Uint8 red,
                        Uint8 green,
                        Uint8 blue);
static int pixel_is_rgba(SDL_Surface *surface,
                         int x,
                         int y,
                         Uint8 red,
                         Uint8 green,
                         Uint8 blue,
                         Uint8 alpha);
static int pixel_is_between_black_and_white(SDL_Surface *surface, int x, int y);

static int count_open_file_descriptors(void)
{
#if defined(__APPLE__)
    const char *fdDir = "/dev/fd";
#elif defined(__linux__)
    const char *fdDir = "/proc/self/fd";
#else
    return -1;
#endif

    DIR *dir = opendir(fdDir);
    if (dir == NULL) {
        return -1;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static int ignored_io_error(Display *display)
{
    (void) display;
    return 0;
}

static int test_smoke(Display *display)
{
    pid_t pid = fork();
    CHECK(pid >= 0, "fork for default error handler check failed");
    if (pid == 0) {
        XSetErrorHandler(NULL);
        XCreatePixmap(display, RootWindow(display, DefaultScreen(display)), 1,
                      1, 7);
        _exit(0);
    }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid,
          "waitpid for default error handler check failed");
    CHECK(
        (WIFEXITED(status) && WEXITSTATUS(status) != 0) || WIFSIGNALED(status),
        "default error handler did not terminate on protocol error");

    int minKeycode = 0;
    int maxKeycode = 0;
    XDisplayKeycodes(display, &minKeycode, &maxKeycode);
    CHECK(minKeycode > 0 && maxKeycode >= minKeycode, "invalid keycode range");

    Atom atom = XInternAtom(display, "SDL2X11_SMOKE", False);
    CHECK(atom != None, "XInternAtom returned None");

    Window root = RootWindow(display, DefaultScreen(display));
    Window window =
        XCreateSimpleWindow(display, root, 0, 0, 32, 32, 0,
                            BlackPixel(display, 0), WhitePixel(display, 0));
    CHECK(window != None, "XCreateSimpleWindow returned None");
    XDestroyWindow(display, window);
    return 1;
}

static int test_atoms(Display *display)
{
    Atom wmName = XInternAtom(display, "WM_NAME", True);
    CHECK(wmName == XA_WM_NAME, "WM_NAME did not map to predefined XA_WM_NAME");

    char *predefinedName = XGetAtomName(display, wmName);
    CHECK(predefinedName != NULL, "XGetAtomName failed for predefined atom");
    CHECK(!strcmp(predefinedName, "WM_NAME"),
          "unexpected predefined atom name");
    XFree(predefinedName);

    Atom netWmIcon = XInternAtom(display, "_NET_WM_ICON", True);
    CHECK(netWmIcon != None, "_NET_WM_ICON predefined atom was not found");
    char *netName = XGetAtomName(display, netWmIcon);
    CHECK(netName != NULL, "XGetAtomName failed for _NET_WM_ICON");
    CHECK(!strcmp(netName, "_NET_WM_ICON"), "unexpected _NET atom name");
    XFree(netName);

    Atom dynamic = XInternAtom(display, "SDL2X11_DYNAMIC_ATOM", False);
    CHECK(dynamic != None, "dynamic atom intern failed");
    CHECK(dynamic <= UINT32_MAX, "dynamic atom exceeded 32 bits");
    CHECK(XInternAtom(display, "SDL2X11_DYNAMIC_ATOM", True) == dynamic,
          "dynamic atom lookup did not return the existing atom");

    char *dynamicName = XGetAtomName(display, dynamic);
    CHECK(dynamicName != NULL, "XGetAtomName failed for dynamic atom");
    CHECK(!strcmp(dynamicName, "SDL2X11_DYNAMIC_ATOM"),
          "unexpected dynamic atom name");
    XFree(dynamicName);

    char *names[] = {"SDL2X11_BATCH_ATOM", "SDL2X11_MISSING_BATCH_ATOM"};
    Atom atoms[2] = {None, None};
    CHECK(XInternAtoms(display, names, 2, True, atoms) == 0,
          "XInternAtoms unexpectedly found missing atoms");
    CHECK(atoms[0] == None && atoms[1] == None,
          "missing batch atoms should be None");
    CHECK(XInternAtoms(display, names, 2, False, atoms) != 0,
          "XInternAtoms failed to create atoms");
    CHECK(atoms[0] != None && atoms[1] != None,
          "created batch atoms should be non-None");

    char *returnedNames[2] = {NULL, NULL};
    CHECK(XGetAtomNames(display, atoms, 2, returnedNames) != 0,
          "XGetAtomNames failed");
    CHECK(!strcmp(returnedNames[0], names[0]),
          "first batch atom name mismatch");
    CHECK(!strcmp(returnedNames[1], names[1]),
          "second batch atom name mismatch");
    XFree(returnedNames[0]);
    XFree(returnedNames[1]);
    return 1;
}

static int test_keyboard(Display *display)
{
    KeyCode aCode = XKeysymToKeycode(display, XK_a);
    CHECK(aCode != 0, "XKeysymToKeycode(XK_a) returned 0");
    CHECK(XKeysymToKeycode(display, XK_A) == aCode,
          "XKeysymToKeycode(XK_A) did not map to the A key");

    int keysymsPerKeycode = 0;
    KeySym *mapping =
        XGetKeyboardMapping(display, aCode, 2, &keysymsPerKeycode);
    CHECK(mapping != NULL, "XGetKeyboardMapping returned NULL");
    CHECK(keysymsPerKeycode == 1, "expected 1 keysym per keycode");
    CHECK(mapping[0] == XK_a && mapping[1] == XK_b,
          "unexpected keyboard mapping");
    XFree(mapping);

    CHECK(XStringToKeysym("a") == XK_a, "XStringToKeysym(\"a\") failed");
    CHECK(XStringToKeysym("A") == XK_A, "XStringToKeysym(\"A\") failed");
    CHECK(XStringToKeysym("1") == XK_1, "XStringToKeysym(\"1\") failed");
    CHECK(XStringToKeysym("KDelete") == XK_Delete,
          "XStringToKeysym(\"KDelete\") failed");

    unsigned int consumedModifiers = ShiftMask;
    KeySym lookupSym = NoSymbol;
    CHECK(XkbLookupKeySym(display, aCode, ShiftMask, &consumedModifiers,
                          &lookupSym),
          "XkbLookupKeySym failed");
    CHECK(lookupSym == XK_A, "XkbLookupKeySym did not return XK_A for Shift+a");
    CHECK(consumedModifiers == ShiftMask,
          "XkbLookupKeySym did not consume ShiftMask");

    /* Shift maps the whole US top row / punctuation, not just letters. */
    KeySym shiftSym = NoSymbol;
    CHECK(XkbLookupKeySym(display, XKeysymToKeycode(display, XK_1), ShiftMask,
                          NULL, &shiftSym) &&
              shiftSym == XK_exclam,
          "Shift+1 did not produce '!'");
    CHECK(XkbLookupKeySym(display, XKeysymToKeycode(display, XK_slash),
                          ShiftMask, NULL, &shiftSym) &&
              shiftSym == XK_question,
          "Shift+/ did not produce '?'");
    CHECK(XkbLookupKeySym(display, XKeysymToKeycode(display, XK_minus),
                          ShiftMask, NULL, &shiftSym) &&
              shiftSym == XK_underscore,
          "Shift+- did not produce '_'");

    /* Reverse direction must agree: a shifted symbol resolves to its base key
     * plus ShiftMask, so accelerators bound on the keysym match Shift+key.
     */
    CHECK(
        XKeysymToKeycode(display, XK_exclam) == XKeysymToKeycode(display, XK_1),
        "XKeysymToKeycode(!) did not resolve to the '1' key");
    CHECK(XkbKeysymToModifiers(display, XK_exclam) == ShiftMask,
          "XkbKeysymToModifiers(!) did not report ShiftMask");
    CHECK(XKeysymToKeycode(display, XK_question) ==
              XKeysymToKeycode(display, XK_slash),
          "XKeysymToKeycode(?) did not resolve to the '/' key");

    /* XKeysymToKeycode must not alias a scancode key (XK_Execute is
     * SDLK_EXECUTE 0x40000074, whose low byte is the 't' keycode) onto an ASCII
     * letter; otherwise Motif binds osfActivate onto 't'. Every keycode it
     * returns must round-trip back to the requested keysym.
     */
    KeyCode tCode = XKeysymToKeycode(display, XK_t);
    KeyCode execCode = XKeysymToKeycode(display, XK_Execute);
    CHECK(tCode != 0 && XkbKeycodeToKeysym(display, tCode, 0, 0) == XK_t,
          "XKeysymToKeycode(XK_t) does not round-trip");
    CHECK(execCode == 0 || execCode != tCode,
          "XKeysymToKeycode(XK_Execute) aliases onto the 't' keycode");
    /* Forward direction: an ASCII/canonical key must win over a 0x4000xxxx
     * scancode key that truncates onto the same 8-bit keycode. The raw keycode
     * 13 that a Return key event carries is shared with SDLK_AC_HOME
     * (0x4000010d); it must resolve to XK_Return, not XK_Home, so Enter reaches
     * clients (e.g. opening a file in xwpe) instead of being swallowed.
     */
    CHECK(XkbKeycodeToKeysym(display, (KeyCode) 13, 0, 0) == XK_Return,
          "keycode 13 did not resolve to XK_Return (shadowed by XK_Home)");
    CHECK(XkbKeysymToModifiers(display, XK_A) == ShiftMask,
          "XkbKeysymToModifiers did not report ShiftMask for XK_A");
    CHECK(XkbKeysymToModifiers(display, XK_Control_L) == ControlMask,
          "XkbKeysymToModifiers did not report ControlMask for XK_Control_L");
    CHECK(XkbKeysymToModifiers(display, XK_Alt_L) == Mod1Mask,
          "XkbKeysymToModifiers did not report Mod1Mask for XK_Alt_L");
    CHECK(XkbKeysymToModifiers(display, XK_Alt_R) == Mod1Mask,
          "XkbKeysymToModifiers did not report Mod1Mask for XK_Alt_R");
    CHECK(XkbKeysymToModifiers(display, XK_Caps_Lock) == LockMask,
          "XkbKeysymToModifiers did not report LockMask for XK_Caps_Lock");
    CHECK(XkbKeysymToModifiers(display, XK_Num_Lock) == Mod2Mask,
          "XkbKeysymToModifiers did not report Mod2Mask for XK_Num_Lock");

    XModifierKeymap *modmap = XGetModifierMapping(display);
    CHECK(modmap != NULL, "XGetModifierMapping returned NULL");
    CHECK(modmap->max_keypermod == 2, "unexpected max_keypermod");
    CHECK(modifier_slot_has(modmap, ShiftMapIndex,
                            XKeysymToKeycode(display, XK_Shift_L)),
          "modifier map missing left shift");
    CHECK(modifier_slot_has(modmap, ShiftMapIndex,
                            XKeysymToKeycode(display, XK_Shift_R)),
          "modifier map missing right shift");
    CHECK(modifier_slot_has(modmap, ControlMapIndex,
                            XKeysymToKeycode(display, XK_Control_L)),
          "modifier map missing left control");
    CHECK(modifier_slot_has(modmap, ControlMapIndex,
                            XKeysymToKeycode(display, XK_Control_R)),
          "modifier map missing right control");
    CHECK(modifier_slot_has(modmap, LockMapIndex,
                            XKeysymToKeycode(display, XK_Caps_Lock)),
          "modifier map missing caps lock");
    /* Both Alt halves share Mod1 to match the standard X server convention and
     * stay consistent with convertModifierState's KMOD_ALT -> Mod1Mask mapping.
     */
    CHECK(modifier_slot_has(modmap, Mod1MapIndex,
                            XKeysymToKeycode(display, XK_Alt_L)),
          "modifier map missing left alt");
    CHECK(modifier_slot_has(modmap, Mod1MapIndex,
                            XKeysymToKeycode(display, XK_Alt_R)),
          "modifier map missing right alt on Mod1");
    CHECK(modifier_slot_has(modmap, Mod2MapIndex,
                            XKeysymToKeycode(display, XK_Num_Lock)),
          "modifier map missing num lock");
    CHECK(XFreeModifiermap(modmap) == 1, "XFreeModifiermap failed");

    /* XSetInputFocus updates internal focus tracking; XGetInputFocus reads it
     * back. None and PointerRoot both collapse to "no focus." Motif's modal
     * dialog path relies on the round-trip.
     */
    Window focusedBefore = None;
    int revertBefore = 0;
    XGetInputFocus(display, &focusedBefore, &revertBefore);
    Window probeWindow = XCreateSimpleWindow(
        display, DefaultRootWindow(display), 0, 0, 10, 10, 0, 0, 0);
    CHECK(probeWindow != None, "XCreateSimpleWindow for focus probe failed");
    XSelectInput(display, probeWindow, FocusChangeMask);
    XEvent keyEvent;
    memset(&keyEvent, 0, sizeof(keyEvent));
    keyEvent.xkey.type = KeyPress;
    keyEvent.xkey.display = display;
    keyEvent.xkey.window = probeWindow;
    keyEvent.xkey.keycode = XKeysymToKeycode(display, XK_a);
    /* XFilterEvent must never swallow a real key, regardless of X focus state.
     * The host input method consumes composition at the SDL layer, so anything
     * that reaches a client here is a real key or a committed-IM delivery that
     * the client must process. An earlier build dropped keys whenever
     * getKeyboardFocus() was None, which made Enter and IM commits vanish in a
     * single-window client that never sets X focus.
     */
    syncKeyboardFocusFromHost(None);
    CHECK(!XFilterEvent(&keyEvent, probeWindow),
          "XFilterEvent dropped a real key with no host focus");
    keyEvent.xkey.type = KeyRelease;
    CHECK(!XFilterEvent(&keyEvent, probeWindow),
          "XFilterEvent dropped a KeyRelease with no host focus");
    keyEvent.xkey.type = KeyPress;
    syncKeyboardFocusFromHost(probeWindow);
    CHECK(!XFilterEvent(&keyEvent, probeWindow),
          "XFilterEvent dropped a real key with host focus set");
    /* A committed-IM KeyPress rides a synthetic event with keycode 0 and the
     * commit id in subwindow; it must pass through too so the client can pull
     * the text with XmbLookupString.
     */
    XEvent imCommitEvent;
    memset(&imCommitEvent, 0, sizeof(imCommitEvent));
    imCommitEvent.xkey.type = KeyPress;
    imCommitEvent.xkey.display = display;
    imCommitEvent.xkey.window = probeWindow;
    imCommitEvent.xkey.keycode = 0;
    imCommitEvent.xkey.subwindow = (Window) 1;
    syncKeyboardFocusFromHost(None);
    CHECK(!XFilterEvent(&imCommitEvent, probeWindow),
          "XFilterEvent dropped a committed IM KeyPress with no host focus");
    XSetInputFocus(display, probeWindow, RevertToParent, CurrentTime);
    Window focused = None;
    int revert = 0;
    XGetInputFocus(display, &focused, &revert);
    CHECK(focused == probeWindow,
          "XSetInputFocus did not update keyboard focus");
    CHECK(revert == RevertToParent, "XSetInputFocus did not record revert_to");
    XEvent focusEvent;
    CHECK(XCheckTypedWindowEvent(display, probeWindow, FocusIn, &focusEvent),
          "XSetInputFocus did not queue FocusIn");
    Window focusChild =
        XCreateSimpleWindow(display, probeWindow, 1, 1, 5, 5, 0, 0, 0);
    CHECK(focusChild != None, "XCreateSimpleWindow for focus child failed");
    XSelectInput(display, focusChild, FocusChangeMask);
    XSetInputFocus(display, focusChild, RevertToParent, CurrentTime);
    CHECK(XCheckTypedWindowEvent(display, probeWindow, FocusOut, &focusEvent),
          "parent-to-child focus change did not queue parent FocusOut");
    CHECK(focusEvent.xfocus.detail == NotifyInferior,
          "parent FocusOut to focused child did not use NotifyInferior");
    CHECK(XCheckTypedWindowEvent(display, focusChild, FocusIn, &focusEvent),
          "parent-to-child focus change did not queue child FocusIn");
    CHECK(focusEvent.xfocus.detail == NotifyAncestor,
          "child FocusIn from parent did not use NotifyAncestor");
    XSetInputFocus(display, None, RevertToParent, CurrentTime);
    XGetInputFocus(display, &focused, &revert);
    /* Per Xlib spec XGetInputFocus reports the actual target: None for None,
     * PointerRoot for PointerRoot. The previous behavior collapsed both into
     * PointerRoot, which was non-conformant.
     */
    CHECK(focused == None,
          "XGetInputFocus after None focus should report None");
    /* Window-to-None per Xlib 10.7.1: the relationship between a window and a
     * non-window focus target is nonlinear (no shared hierarchy), so the leaf
     * gets NotifyNonlinear and each strict ancestor below the root gets
     * NotifyNonlinearVirtual. The root also receives a companion FocusIn with
     * detail NotifyDetailNone, not asserted here because the root has no
     * FocusChangeMask.
     */
    CHECK(XCheckTypedWindowEvent(display, focusChild, FocusOut, &focusEvent),
          "XSetInputFocus(None) did not queue FocusOut on focus leaf");
    CHECK(focusEvent.xfocus.detail == NotifyNonlinear,
          "FocusOut to None on focus leaf did not use NotifyNonlinear");
    CHECK(XCheckTypedWindowEvent(display, probeWindow, FocusOut, &focusEvent),
          "XSetInputFocus(None) did not queue FocusOut on focus ancestor");
    CHECK(focusEvent.xfocus.detail == NotifyNonlinearVirtual,
          "FocusOut to None on focus ancestor did not use "
          "NotifyNonlinearVirtual");
    /* Round-trip the PointerRoot case so a future regression that re-collapses
     * the two non-window targets in XGetInputFocus is caught at this test.
     */
    XSetInputFocus(display, (Window) PointerRoot, RevertToParent, CurrentTime);
    XGetInputFocus(display, &focused, &revert);
    CHECK(focused == (Window) PointerRoot,
          "XGetInputFocus after PointerRoot focus should report PointerRoot");

    /* Destroying the focus window must auto-revert per the recorded revert_to
     * (Xlib spec). Without this the focus state stayed pinned to a freed XID
     * and subsequent key events routed to dead memory. The parent must be
     * mapped: RevertToParent walks past unviewable ancestors, which is what a
     * real X server does too.
     */
    Window revertParent = XCreateSimpleWindow(
        display, DefaultRootWindow(display), 0, 0, 8, 8, 0, 0, 0);
    CHECK(revertParent != None, "XCreateSimpleWindow for revert parent failed");
    XMapWindow(display, revertParent);
    Window revertChild =
        XCreateSimpleWindow(display, revertParent, 0, 0, 4, 4, 0, 0, 0);
    CHECK(revertChild != None, "XCreateSimpleWindow for revert child failed");
    XMapWindow(display, revertChild);
    XSetInputFocus(display, revertChild, RevertToParent, CurrentTime);
    XDestroyWindow(display, revertChild);
    XGetInputFocus(display, &focused, &revert);
    CHECK(focused == revertParent,
          "destroying focus window with RevertToParent did not revert to "
          "parent");
    CHECK(revert == RevertToNone,
          "post-revert revert_to should reset to RevertToNone per spec");
    XSetInputFocus(display, revertParent, RevertToPointerRoot, CurrentTime);
    XDestroyWindow(display, revertParent);
    XGetInputFocus(display, &focused, &revert);
    CHECK(focused == (Window) PointerRoot,
          "destroying focus window with RevertToPointerRoot did not revert "
          "to PointerRoot");

    /* RevertToParent with an unmapped parent walks past it to the next viewable
     * ancestor (per spec's "closest viewable ancestor" clause). Setup note:
     * strict Xlib would BadMatch the XSetInputFocus call below because
     * mappedChild is not viewable (its parent is unmapped). The compat layer
     * does not enforce that check by design - Motif sets focus on widgets
     * before they are fully realized and depends on the call landing. This
     * regression is for the compat-layer revert-walk behavior, not for a
     * spec-conformant client scenario.
     */
    Window unmappedParent = XCreateSimpleWindow(
        display, DefaultRootWindow(display), 0, 0, 8, 8, 0, 0, 0);
    CHECK(unmappedParent != None,
          "XCreateSimpleWindow for unmapped revert parent failed");
    Window mappedChild =
        XCreateSimpleWindow(display, unmappedParent, 0, 0, 4, 4, 0, 0, 0);
    CHECK(mappedChild != None,
          "XCreateSimpleWindow for unmapped-parent child failed");
    XMapWindow(display, mappedChild);
    XSetInputFocus(display, mappedChild, RevertToParent, CurrentTime);
    XDestroyWindow(display, mappedChild);
    XGetInputFocus(display, &focused, &revert);
    CHECK(focused != unmappedParent,
          "RevertToParent landed on an unmapped ancestor instead of "
          "walking up");
    XDestroyWindow(display, unmappedParent);

    /* Cascading destroy must preserve the FocusIn(parent) generated by a
     * child's revert. Earlier code drained the parent's queue after recursion
     * (which discarded that just-queued event); the discard now runs before
     * recursion. Verify by selecting FocusChangeMask on the parent, focusing a
     * mapped child with RevertToParent, then destroying the parent (which
     * cascades to the child). The intermediate FocusIn(parent, NotifyInferior)
     * from the child's revert (child is descendant of parent, so the spec
     * detail is NotifyInferior, not NotifyAncestor) must reach the queue.
     */
    Window cascadeParent = XCreateSimpleWindow(
        display, DefaultRootWindow(display), 0, 0, 8, 8, 0, 0, 0);
    CHECK(cascadeParent != None,
          "XCreateSimpleWindow for cascade parent failed");
    XSelectInput(display, cascadeParent, FocusChangeMask);
    XMapWindow(display, cascadeParent);
    Window cascadeChild =
        XCreateSimpleWindow(display, cascadeParent, 0, 0, 4, 4, 0, 0, 0);
    CHECK(cascadeChild != None, "XCreateSimpleWindow for cascade child failed");
    XMapWindow(display, cascadeChild);
    XSetInputFocus(display, cascadeChild, RevertToParent, CurrentTime);
    while (
        XCheckTypedWindowEvent(display, cascadeParent, FocusIn, &focusEvent)) {
    }
    while (
        XCheckTypedWindowEvent(display, cascadeParent, FocusOut, &focusEvent)) {
    }
    XDestroyWindow(display, cascadeParent);
    Bool sawRevertFocusIn = False;
    while (
        XCheckTypedWindowEvent(display, cascadeParent, FocusIn, &focusEvent)) {
        if (focusEvent.xfocus.detail == NotifyInferior)
            sawRevertFocusIn = True;
    }
    CHECK(sawRevertFocusIn,
          "cascading destroy discarded the child-revert FocusIn on the "
          "parent");
    /* Drain any other events the cascade emitted (FocusOut on the parent from
     * its own auto-revert, DestroyNotify, etc.) so they do not bleed into later
     * subtests that assert XNextEvent ordering.
     */
    while (XPending(display) > 0)
        XNextEvent(display, &focusEvent);

    XDestroyWindow(display, probeWindow);
    XSetInputFocus(display, focusedBefore, revertBefore, CurrentTime);

    CHECK(
        XGrabKeyboard(display, DefaultRootWindow(display), False, GrabModeAsync,
                      GrabModeAsync, CurrentTime) == GrabSuccess,
        "XGrabKeyboard did not report GrabSuccess");
    CHECK(XUngrabKeyboard(display, CurrentTime) == 1, "XUngrabKeyboard failed");

    /* Lookup APIs decode physical keycodes through modifiers. */
    XKeyEvent ev = {0};
    ev.type = KeyPress;
    ev.display = display;
    ev.keycode = XKeysymToKeycode(display, XK_a);
    char buf[8] = {0};
    KeySym lookedUp = NoSymbol;
    Status lookupStatus = 0;

    int n = XLookupString(&ev, buf, sizeof(buf), &lookedUp, NULL);
    CHECK(n == 1 && buf[0] == 'a', "XLookupString did not return 'a'");
    CHECK(lookedUp == XK_a, "XLookupString returned wrong keysym");

    memset(buf, 0, sizeof(buf));
    lookedUp = NoSymbol;
    ev.state = ShiftMask;
    n = XLookupString(&ev, buf, sizeof(buf), &lookedUp, NULL);
    CHECK(n == 1 && buf[0] == 'A', "XLookupString did not return shifted 'A'");
    CHECK(lookedUp == XK_A, "XLookupString returned wrong shifted keysym");

    memset(buf, 0, sizeof(buf));
    lookedUp = NoSymbol;
    lookupStatus = 0;
    n = XmbLookupString(NULL, &ev, buf, sizeof(buf), &lookedUp, &lookupStatus);
    CHECK(n == 1 && buf[0] == 'A',
          "XmbLookupString did not return shifted 'A'");
    CHECK(lookedUp == XK_A, "XmbLookupString returned wrong shifted keysym");
    CHECK(lookupStatus == XLookupBoth,
          "XmbLookupString status should be XLookupBoth for ASCII");

    memset(buf, 0, sizeof(buf));
    lookedUp = NoSymbol;
    lookupStatus = 0;
    n = Xutf8LookupString(NULL, &ev, buf, sizeof(buf), &lookedUp,
                          &lookupStatus);
    CHECK(n == 1 && buf[0] == 'A',
          "Xutf8LookupString did not return shifted 'A'");
    CHECK(lookedUp == XK_A, "Xutf8LookupString returned wrong shifted keysym");
    CHECK(lookupStatus == XLookupBoth,
          "Xutf8LookupString status should be XLookupBoth for ASCII");

    struct {
        KeySym keysym;
        char byte;
        const char *name;
    } controls[] = {
        {XK_Return, '\r', "Return"},       {XK_Tab, '\t', "Tab"},
        {XK_BackSpace, '\b', "BackSpace"}, {XK_Escape, 0x1b, "Escape"},
        {XK_Delete, 0x7f, "Delete"},
    };
    for (size_t i = 0; i < ARRAY_LENGTH(controls); i++) {
        ev.keycode = XKeysymToKeycode(display, controls[i].keysym);
        ev.state = 0;
        memset(buf, 0, sizeof(buf));
        lookedUp = NoSymbol;
        n = XLookupString(&ev, buf, sizeof(buf), &lookedUp, NULL);
        CHECK(n == 1 && buf[0] == controls[i].byte,
              "XLookupString did not return control byte");
        CHECK(lookedUp == controls[i].keysym,
              "XLookupString returned wrong control keysym");

        memset(buf, 0, sizeof(buf));
        lookedUp = NoSymbol;
        lookupStatus = 0;
        n = Xutf8LookupString(NULL, &ev, buf, sizeof(buf), &lookedUp,
                              &lookupStatus);
        CHECK(n == 1 && buf[0] == controls[i].byte,
              "Xutf8LookupString did not return control byte");
        CHECK(lookedUp == controls[i].keysym,
              "Xutf8LookupString returned wrong control keysym");
        CHECK(lookupStatus == XLookupBoth,
              "Xutf8LookupString status should be XLookupBoth for controls");

        memset(buf, 0, sizeof(buf));
        lookedUp = NoSymbol;
        lookupStatus = 0;
        n = XmbLookupString(NULL, &ev, buf, sizeof(buf), &lookedUp,
                            &lookupStatus);
        CHECK(n == 1 && buf[0] == controls[i].byte,
              "XmbLookupString did not return control byte");
        CHECK(lookedUp == controls[i].keysym,
              "XmbLookupString returned wrong control keysym");
        CHECK(lookupStatus == XLookupBoth,
              "XmbLookupString status should be XLookupBoth for controls");
    }
    return 1;
}

/* Drain key events until one for keycode and type arrives, decode it, and
 * report whether ShiftMask was set and which UTF-8 byte the lookup produced.
 * Returns False if no matching event is delivered within the guard budget.
 */
static Bool awaitFakeKeyEvent(Display *display,
                              int type,
                              unsigned int keycode,
                              Bool *shiftOut,
                              char *charOut)
{
    for (int guard = 0; guard < 400; guard++) {
        if (XPending(display) <= 0) {
            XSync(display, False);
            continue;
        }
        XEvent ev;
        XNextEvent(display, &ev);
        if (ev.type != type || ev.xkey.keycode != (keycode & 0xFF))
            continue;
        char buf[8] = {0};
        KeySym keysym = NoSymbol;
        Status status = 0;
        int n = Xutf8LookupString(NULL, &ev.xkey, buf, sizeof(buf), &keysym,
                                  &status);
        *shiftOut = (ev.xkey.state & ShiftMask) ? True : False;
        *charOut = (n == 1) ? buf[0] : '\0';
        return True;
    }
    return False;
}

/* Regression guard for XTestFakeKeyEvent modifier threading: synthetic key
 * events carry no modifier state of their own, so a held fake Shift must be
 * stamped onto later fake key events. Without it, replays cannot type
 * uppercase. Mapping a large top-level window registers it as the XTest replay
 * target so the injected events have somewhere to land.
 */
static int test_xtest_modifiers(Display *display)
{
    Window root = DefaultRootWindow(display);
    int sw = DisplayWidth(display, DefaultScreen(display));
    int sh = DisplayHeight(display, DefaultScreen(display));
    Window win = XCreateSimpleWindow(display, root, 0, 0, (unsigned) sw,
                                     (unsigned) sh, 0, 0, 0);
    CHECK(win != None, "XCreateSimpleWindow failed");
    XSelectInput(display, win, KeyPressMask | KeyReleaseMask);
    XMapWindow(display, win);
    XSync(display, False);
    XSetInputFocus(display, win, RevertToParent, CurrentTime);
    CHECK(replayTargetWindowId() != 0,
          "mapped window was not registered as the XTest target");

    KeyCode shiftCode = XKeysymToKeycode(display, XK_Shift_L);
    KeyCode hCode = XKeysymToKeycode(display, XK_h);
    CHECK(shiftCode != 0 && hCode != 0,
          "modifier/letter keycode lookup failed");

    while (XPending(display) > 0) {
        XEvent drain;
        XNextEvent(display, &drain);
    }

    Bool shifted = False;
    char produced = '\0';
    XTestFakeKeyEvent(display, shiftCode, True, 0);
    CHECK(awaitFakeKeyEvent(display, KeyPress, shiftCode, &shifted, &produced),
          "no KeyPress delivered for fake Shift press");
    CHECK(!shifted, "fake Shift press carried post-press ShiftMask");

    XTestFakeKeyEvent(display, hCode, True, 0);
    CHECK(awaitFakeKeyEvent(display, KeyPress, hCode, &shifted, &produced),
          "no KeyPress delivered for fake Shift+h");
    CHECK(shifted, "held fake Shift was not threaded onto the key event");
    CHECK(produced == 'H', "fake Shift+h did not decode to uppercase 'H'");

    XTestFakeKeyEvent(display, shiftCode, False, 0);
    CHECK(
        awaitFakeKeyEvent(display, KeyRelease, shiftCode, &shifted, &produced),
        "no KeyRelease delivered for fake Shift release");
    CHECK(shifted, "fake Shift release did not carry pre-release ShiftMask");

    XTestFakeKeyEvent(display, hCode, True, 0);
    CHECK(awaitFakeKeyEvent(display, KeyPress, hCode, &shifted, &produced),
          "no KeyPress delivered after Shift release");
    CHECK(!shifted, "Shift state persisted after the fake release");
    CHECK(produced == 'h', "post-release key did not decode to lowercase 'h'");

    XDestroyWindow(display, win);
    return 1;
}

static int test_gc(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    GC defaultGc = DefaultGC(display, DefaultScreen(display));
    CHECK(defaultGc != NULL, "DefaultGC returned NULL");
    CHECK(XGContextFromGC(defaultGc) != None, "DefaultGC has no XID");
    CHECK(GET_GC(defaultGc)->generation == 0,
          "DefaultGC generation was not initialized");
    XGCValues defaultValues;
    CHECK(XGetGCValues(display, defaultGc, GCForeground | GCBackground,
                       &defaultValues),
          "XGetGCValues failed for DefaultGC");
    CHECK(
        defaultValues.foreground == BlackPixel(display, DefaultScreen(display)),
        "DefaultGC foreground did not match BlackPixel");
    CHECK(
        defaultValues.background == WhitePixel(display, DefaultScreen(display)),
        "DefaultGC background did not match WhitePixel");
    CHECK(defaultValues.foreground <
                  (1ul << DefaultDepth(display, DefaultScreen(display))) &&
              defaultValues.background <
                  (1ul << DefaultDepth(display, DefaultScreen(display))),
          "DefaultGC pixels exceeded DefaultDepth range");

    GC gc = XCreateGC(display, root, 0, NULL);
    CHECK(gc != NULL, "XCreateGC returned NULL");
    CHECK(GET_GC(gc)->generation == 0, "new GC generation was not initialized");

    CHECK(XSetState(display, gc, 0x11223344, 0x55667788, GXxor, 0x00ffffff),
          "XSetState failed");

    XGCValues values;
    CHECK(XGetGCValues(display, gc,
                       GCForeground | GCBackground | GCFunction | GCPlaneMask,
                       &values),
          "XGetGCValues failed");
    CHECK(values.foreground == 0x11223344, "foreground did not round-trip");
    CHECK(values.background == 0x55667788, "background did not round-trip");
    CHECK(values.function == GXxor, "function did not round-trip");
    CHECK(values.plane_mask == 0x00ffffff, "plane mask did not round-trip");

    Pixmap pixmap = XCreatePixmap(
        display, root, 2, 2, DefaultDepth(display, DefaultScreen(display)));
    CHECK(pixmap != None, "XCreatePixmap for GC state failed");
    CHECK(XSetTile(display, gc, pixmap), "XSetTile failed");
    CHECK(XSetStipple(display, gc, pixmap), "XSetStipple failed");
    CHECK(XSetClipMask(display, gc, pixmap), "XSetClipMask failed");
    CHECK(XGetGCValues(display, gc, GCTile | GCStipple | GCClipMask, &values),
          "XGetGCValues for pixmap state failed");
    CHECK(values.tile == pixmap, "tile did not round-trip");
    CHECK(values.stipple == pixmap, "stipple did not round-trip");
    CHECK(values.clip_mask == pixmap, "clip mask did not round-trip");

    CHECK(XSetArcMode(display, gc, ArcChord), "XSetArcMode failed");
    CHECK(XSetFillStyle(display, gc, FillTiled), "XSetFillStyle failed");
    CHECK(XSetFillRule(display, gc, WindingRule), "XSetFillRule failed");
    CHECK(XGetGCValues(display, gc, GCArcMode | GCFillStyle | GCFillRule,
                       &values),
          "XGetGCValues for GC convenience setters failed");
    CHECK(values.arc_mode == ArcChord && values.fill_style == FillTiled &&
              values.fill_rule == WindingRule,
          "GC convenience setters did not round-trip");

    /* Generation counter: every GC mutator bumps it so a future per-renderer
     * state cache can skip redundant SDL pushes.
     */
    GraphicContext *gContext = GET_GC(gc);
    CHECK(gContext != NULL, "GET_GC returned NULL");
    unsigned long gen = gContext->generation;
    XSetForeground(display, gc, 0x123456);
    CHECK(gContext->generation > gen,
          "XSetForeground did not bump GC generation");
    gen = gContext->generation;
    XSetBackground(display, gc, 0x654321);
    CHECK(gContext->generation > gen,
          "XSetBackground did not bump GC generation");
    gen = gContext->generation;
    XGCValues genValues = {.function = GXcopy};
    XChangeGC(display, gc, GCFunction, &genValues);
    CHECK(gContext->generation > gen, "XChangeGC did not bump GC generation");

    /* XSetClipRectangles: rectangles stick, clip origin round-trips. */
    XRectangle clipRects[2] = {
        {.x = 0, .y = 0, .width = 4, .height = 4},
        {.x = 10, .y = 10, .width = 4, .height = 4},
    };
    CHECK(XSetClipRectangles(display, gc, 5, 6, clipRects, 2, Unsorted),
          "XSetClipRectangles failed");
    XGCValues clipOut = {0};
    CHECK(XGetGCValues(display, gc, GCClipXOrigin | GCClipYOrigin, &clipOut),
          "XGetGCValues for clip origin failed");
    CHECK(clipOut.clip_x_origin == 5 && clipOut.clip_y_origin == 6,
          "clip origin did not round-trip after XSetClipRectangles");
    Region region = XCreateRegion();
    CHECK(region != NULL, "XCreateRegion failed");
    XUnionRectWithRegion(&clipRects[0], region, region);
    XUnionRectWithRegion(&clipRects[1], region, region);
    CHECK(XSetRegion(display, gc, region), "XSetRegion failed");
    XDestroyRegion(region);
    CHECK(XSetClipMask(display, gc, None), "XSetClipMask(None) failed");

    /* Clip rectangles must actually clip the SDL render path. */
    Pixmap clipPixmap = XCreatePixmap(
        display, root, 16, 16, DefaultDepth(display, DefaultScreen(display)));
    CHECK(clipPixmap != None, "XCreatePixmap for clip drawing failed");
    GC clipGc = XCreateGC(display, clipPixmap, 0, NULL);
    CHECK(clipGc != NULL, "XCreateGC for clip drawing failed");
    CHECK(XSetForeground(display, clipGc, 0xff000000),
          "XSetForeground black failed");
    CHECK(XFillRectangle(display, clipPixmap, clipGc, 0, 0, 16, 16),
          "initial fill failed");
    XRectangle innerRect = {.x = 4, .y = 4, .width = 4, .height = 4};
    CHECK(XSetClipRectangles(display, clipGc, 0, 0, &innerRect, 1, Unsorted),
          "XSetClipRectangles for draw failed");
    CHECK(XSetForeground(display, clipGc, 0xffff0000),
          "XSetForeground red failed");
    CHECK(XFillRectangle(display, clipPixmap, clipGc, 0, 0, 16, 16),
          "clipped fill failed");
    SDL_Renderer *clipRenderer = NULL;
    GET_RENDERER(clipPixmap, clipRenderer);
    SDL_Surface *clipSurface = getRenderSurface(clipRenderer);
    CHECK(clipSurface != NULL, "getRenderSurface for clip drawing failed");
    CHECK(pixel_is_rgb(clipSurface, 5, 5, 255, 0, 0),
          "clip rectangle did not allow inside pixel");
    CHECK(pixel_is_rgb(clipSurface, 2, 2, 0, 0, 0),
          "clip rectangle allowed outside pixel");
    SDL_FreeSurface(clipSurface);
    XFreeGC(display, clipGc);
    XFreePixmap(display, clipPixmap);

    Window clipTop =
        XCreateSimpleWindow(display, root, 0, 0, 16, 24, 0, 0, 0xFF000000);
    CHECK(clipTop != None, "ancestor clip top-level creation failed");
    Window clipParent =
        XCreateSimpleWindow(display, clipTop, 0, 0, 16, 10, 0, 0, 0xFF000000);
    CHECK(clipParent != None, "ancestor clip parent creation failed");
    Window clipOverflow = XCreateSimpleWindow(display, clipParent, 0, 0, 16, 24,
                                              0, 0, 0xFF000000);
    CHECK(clipOverflow != None, "ancestor clip child creation failed");
    CHECK(XMapWindow(display, clipOverflow), "ancestor clip child map failed");
    CHECK(XMapWindow(display, clipParent), "ancestor clip parent map failed");
    CHECK(XMapWindow(display, clipTop), "ancestor clip top-level map failed");

    GC ancestorGc = XCreateGC(display, clipTop, 0, NULL);
    CHECK(ancestorGc != NULL, "ancestor clip GC creation failed");
    CHECK(XSetForeground(display, ancestorGc, 0xFF000000),
          "ancestor clip black foreground failed");
    CHECK(XFillRectangle(display, clipTop, ancestorGc, 0, 0, 16, 24),
          "ancestor clip top-level clear failed");
    CHECK(XSetForeground(display, ancestorGc, 0xFFFF0000),
          "ancestor clip red foreground failed");
    CHECK(XFillRectangle(display, clipOverflow, ancestorGc, 0, 0, 16, 24),
          "ancestor clip overflow draw failed");
    SDL_Renderer *ancestorRenderer = NULL;
    GET_RENDERER(clipTop, ancestorRenderer);
    SDL_Surface *ancestorSurface = getRenderSurface(ancestorRenderer);
    CHECK(ancestorSurface != NULL, "ancestor clip surface readback failed");
    CHECK(pixel_is_rgb(ancestorSurface, 2, 8, 255, 0, 0),
          "ancestor clip removed visible child pixel");
    CHECK(pixel_is_rgb(ancestorSurface, 2, 12, 0, 0, 0),
          "ancestor clip allowed child to draw outside parent");
    SDL_FreeSurface(ancestorSurface);

    XFreeGC(display, ancestorGc);
    XDestroyWindow(display, clipTop);

    /* Aggressive GC clipping must not block Expose generation. */
    Window exposeWindow =
        XCreateSimpleWindow(display, root, 0, 0, 64, 64, 0, 0, 0);
    XSelectInput(display, exposeWindow, ExposureMask);
    XMapWindow(display, exposeWindow);
    XEvent drainEv;
    while (XCheckWindowEvent(display, exposeWindow, ExposureMask, &drainEv)) {
    }
    GC exposeGc = XCreateGC(display, exposeWindow, 0, NULL);
    XRectangle smallClip = {.x = 0, .y = 0, .width = 4, .height = 4};
    XSetClipRectangles(display, exposeGc, 0, 0, &smallClip, 1, Unsorted);
    CHECK(XClearArea(display, exposeWindow, 8, 8, 16, 16, True),
          "XClearArea failed");
    CHECK(XCheckWindowEvent(display, exposeWindow, ExposureMask, &drainEv),
          "Expose was suppressed by GC clip");
    CHECK(drainEv.xexpose.x == 8 && drainEv.xexpose.y == 8,
          "Expose rectangle was wrong");
    XFreeGC(display, exposeGc);
    XDestroyWindow(display, exposeWindow);

    XFreeGC(display, gc);
    XFreePixmap(display, pixmap);
    return 1;
}

static int test_compat_stubs(Display *display)
{
    CHECK(XMaxRequestSize(display) > 0,
          "XMaxRequestSize returned no usable limit");
    CHECK(XExtendedMaxRequestSize(display) == 0,
          "XExtendedMaxRequestSize should report no BIG-REQUESTS support");

    int major = 0;
    int minor = 0;
    CHECK(XkbUseExtension(display, &major, &minor),
          "XkbUseExtension probe failed");
    CHECK(major == XkbMajorVersion && minor == XkbMinorVersion,
          "XkbUseExtension returned unexpected version");
    Bool supported = False;
    CHECK(XkbSetDetectableAutoRepeat(display, True, &supported) && supported,
          "XkbSetDetectableAutoRepeat did not report support");

    CHECK(XAutoRepeatOff(display) == 1 && XAutoRepeatOn(display) == 1,
          "auto-repeat compatibility calls failed");
    int accelNumer = 0;
    int accelDenom = 0;
    int threshold = -1;
    CHECK(
        XGetPointerControl(display, &accelNumer, &accelDenom, &threshold) == 1,
        "XGetPointerControl failed");
    CHECK(accelNumer == 1 && accelDenom == 1 && threshold == 0,
          "unexpected pointer control defaults");
    CHECK(XChangePointerControl(display, True, True, 1, 1, 0) == 1,
          "XChangePointerControl failed");

    XKeyboardState keyboardState;
    CHECK(XGetKeyboardControl(display, &keyboardState) == 1,
          "XGetKeyboardControl failed");
    CHECK(keyboardState.global_auto_repeat == AutoRepeatModeOn,
          "unexpected keyboard auto-repeat default");
    XKeyboardControl keyboardControl;
    memset(&keyboardControl, 0, sizeof(keyboardControl));
    CHECK(XChangeKeyboardControl(display, 0, &keyboardControl) == 1,
          "XChangeKeyboardControl failed");

    Window root = RootWindow(display, DefaultScreen(display));
    unsigned int bestWidth = 0;
    unsigned int bestHeight = 0;
    CHECK(XQueryBestSize(display, CursorShape, root, 17, 19, &bestWidth,
                         &bestHeight),
          "XQueryBestSize failed");
    CHECK(bestWidth == 17 && bestHeight == 19,
          "XQueryBestSize changed dimensions");
    CHECK(XQueryBestTile(display, root, 11, 13, &bestWidth, &bestHeight),
          "XQueryBestTile failed");
    CHECK(bestWidth == 11 && bestHeight == 13,
          "XQueryBestTile changed dimensions");
    CHECK(XQueryBestStipple(display, root, 7, 5, &bestWidth, &bestHeight),
          "XQueryBestStipple failed");
    CHECK(bestWidth == 7 && bestHeight == 5,
          "XQueryBestStipple changed dimensions");

    char errorText[32];
    CHECK(XGetErrorText(display, BadWindow, errorText, sizeof(errorText)) == 0,
          "XGetErrorText failed");
    CHECK(errorText[0] != '\0', "XGetErrorText returned an empty message");
    CHECK(XGetErrorDatabaseText(display, "XlibMessage", "Missing", "fallback",
                                errorText, sizeof(errorText)) == 0,
          "XGetErrorDatabaseText failed");
    CHECK(!strcmp(errorText, "fallback"),
          "XGetErrorDatabaseText ignored fallback");

    XIOErrorHandler previous = XSetIOErrorHandler(ignored_io_error);
    CHECK(XSetIOErrorHandler(previous) == ignored_io_error,
          "XSetIOErrorHandler did not return previous handler");

    /* XParseGeometry parses the standard X geometry string form. */
    int gx = -1, gy = -1;
    unsigned int gw = 0, gh = 0;
    int mask = XParseGeometry("800x600+10-20", &gx, &gy, &gw, &gh);
    CHECK(mask == (WidthValue | HeightValue | XValue | YValue | YNegative),
          "XParseGeometry full spec mask wrong");
    CHECK(gw == 800 && gh == 600 && gx == 10 && gy == -20,
          "XParseGeometry full spec values wrong");
    gx = gy = 99;
    gw = gh = 99;
    mask = XParseGeometry("+10+10", &gx, &gy, &gw, &gh);
    CHECK(mask == (XValue | YValue) && gx == 10 && gy == 10,
          "XParseGeometry position-only failed");
    CHECK(gw == 99 && gh == 99, "XParseGeometry overwrote unset width/height");
    gx = gy = 99;
    mask = XParseGeometry("-0-0", &gx, &gy, &gw, &gh);
    CHECK(
        mask == (XValue | XNegative | YValue | YNegative) && gx == 0 && gy == 0,
        "XParseGeometry negative-zero failed");
    mask = XParseGeometry("", &gx, &gy, &gw, &gh);
    CHECK(mask == NoValue, "XParseGeometry empty string should return NoValue");
    mask = XParseGeometry("bogus", &gx, &gy, &gw, &gh);
    CHECK(mask == 0, "XParseGeometry bogus should return 0");

    XSizeHints geomHints;
    memset(&geomHints, 0, sizeof(geomHints));
    geomHints.flags = PBaseSize | PMinSize | PResizeInc;
    geomHints.base_width = 10;
    geomHints.base_height = 20;
    geomHints.min_width = 30;
    geomHints.min_height = 40;
    geomHints.width_inc = 5;
    geomHints.height_inc = 7;
    int wx = -1, wy = -1, ww = 0, wh = 0, gravity = 0;
    mask = XWMGeometry(display, DefaultScreen(display), NULL, "8x6-0-0", 2,
                       &geomHints, &wx, &wy, &ww, &wh, &gravity);
    CHECK(mask == (XNegative | YNegative),
          "XWMGeometry default negative mask wrong");
    CHECK(ww == 50 && wh == 62, "XWMGeometry did not apply increments/base");
    CHECK(wx == DisplayWidth(display, DefaultScreen(display)) - ww - 4 &&
              wy == DisplayHeight(display, DefaultScreen(display)) - wh - 4,
          "XWMGeometry default negative position wrong");
    CHECK(gravity == SouthEastGravity,
          "XWMGeometry default negative gravity wrong");

    mask = XWMGeometry(display, DefaultScreen(display), "4x3-10+12", "8x6+1+2",
                       1, &geomHints, &wx, &wy, &ww, &wh, &gravity);
    CHECK(mask == (WidthValue | HeightValue | XValue | XNegative | YValue),
          "XWMGeometry user mask wrong");
    CHECK(ww == 30 && wh == 41, "XWMGeometry user size did not apply hints");
    CHECK(wx == DisplayWidth(display, DefaultScreen(display)) - 10 - ww - 2 &&
              wy == 12,
          "XWMGeometry user position wrong");
    CHECK(gravity == NorthEastGravity, "XWMGeometry user gravity wrong");

    mask = XGeometry(display, DefaultScreen(display), "80x24", "120x40-0-0", 1,
                     8, 16, 4, 6, &wx, &wy, &ww, &wh);
    CHECK(mask == (WidthValue | HeightValue),
          "XGeometry returned default position bits");
    CHECK(ww == 80 && wh == 24, "XGeometry did not preserve unit width/height");
    CHECK(wx == DisplayWidth(display, DefaultScreen(display)) - (120 * 8 + 4) -
                      2 &&
              wy == DisplayHeight(display, DefaultScreen(display)) -
                        (40 * 16 + 6) - 2,
          "XGeometry did not use default size for default negative position");

    mask = XGeometry(display, DefaultScreen(display), "80x24+7-9", "120x40+1+2",
                     2, 8, 16, 4, 6, &wx, &wy, &ww, &wh);
    CHECK(mask == (WidthValue | HeightValue | XValue | YValue | YNegative),
          "XGeometry user mask wrong");
    CHECK(ww == 80 && wh == 24, "XGeometry user size was converted to pixels");
    CHECK(wx == 7 && wy == DisplayHeight(display, DefaultScreen(display)) -
                               (24 * 16 + 6) - 4 - 9,
          "XGeometry user position wrong");

    mask = XGeometry(display, DefaultScreen(display), NULL, "120x40-0-0", 1, 0,
                     0, 4, 6, &wx, &wy, &ww, &wh);
    CHECK(mask == NoValue, "XGeometry default-only mask wrong");
    CHECK(wx == DisplayWidth(display, DefaultScreen(display)) - 4 - 2 &&
              wy == DisplayHeight(display, DefaultScreen(display)) - 6 - 2,
          "XGeometry did not preserve zero unit sizes");

    /* The negative-anchor pixel-arithmetic path must saturate when the font
     * cell times the parsed unit count overflows int. The test drives the
     * satMulAdd path with a small parseable spec (so upstream XParseGeometry's
     * signed int math stays in range) and a huge font cell argument;
     * clampInt64ToInt clamps the result.
     */
    int saturatedX = -1;
    int saturatedY = -1;
    int saturatedW = -1;
    int saturatedH = -1;
    XGeometry(display, DefaultScreen(display), "100x100-0-0", "10x10+0+0", 0,
              INT_MAX / 2, INT_MAX / 2, 0, 0, &saturatedX, &saturatedY,
              &saturatedW, &saturatedH);
    CHECK(saturatedW == 100 && saturatedH == 100,
          "XGeometry parsed width/height was clobbered");
    /* anchorPixelWidth = 100 * (INT_MAX/2) is far above INT_MAX, so
     * DisplayWidth - anchorPixelWidth - border clamps to INT_MIN.
     */
    CHECK(saturatedX == INT_MIN,
          "XGeometry negative-anchor X did not saturate at INT_MIN");
    CHECK(saturatedY == INT_MIN,
          "XGeometry negative-anchor Y did not saturate at INT_MIN");

    return 1;
}

static int test_colors(Display *display)
{
    Colormap colormap = DefaultColormap(display, DefaultScreen(display));
    CHECK(colormap != None && colormap != (Colormap) 1 &&
              colormap != (Colormap) 2,
          "DefaultColormap still uses a placeholder id");
    Colormap created = XCreateColormap(
        display, RootWindow(display, DefaultScreen(display)),
        DefaultVisual(display, DefaultScreen(display)), AllocNone);
    CHECK(created != None && created != colormap && created != (Colormap) 1 &&
              created != (Colormap) 2,
          "XCreateColormap did not return a real resource id");
    CHECK(XFreeColormap(display, created),
          "XFreeColormap failed for created colormap");

    XColor exact;
    memset(&exact, 0, sizeof(exact));
    CHECK(XParseColor(display, colormap, "#123456", &exact),
          "XParseColor #rrggbb failed");
    CHECK(exact.red == 0x1200 && exact.green == 0x3400 && exact.blue == 0x5600,
          "XParseColor #rrggbb returned wrong components");
    CHECK((exact.flags & (DoRed | DoGreen | DoBlue)) ==
              (DoRed | DoGreen | DoBlue),
          "XParseColor did not set component flags");

    memset(&exact, 0, sizeof(exact));
    CHECK(XParseColor(display, colormap, "#abc", &exact),
          "XParseColor #rgb failed");
    CHECK(exact.red == 0xa000 && exact.green == 0xb000 && exact.blue == 0xc000,
          "XParseColor #rgb returned wrong components");

    memset(&exact, 0, sizeof(exact));
    CHECK(XParseColor(display, colormap, "rgb:e5/e5/e5", &exact),
          "XParseColor rgb:r/g/b failed");
    CHECK(exact.red == 0xe5e5 && exact.green == 0xe5e5 &&
              exact.blue == 0xe5e5 && exact.pixel == 0xFFE5E5E5,
          "XParseColor rgb:r/g/b returned wrong components");
    memset(&exact, 0, sizeof(exact));
    CHECK(XParseColor(display, colormap, "rgb:f/f/f", &exact),
          "XParseColor rgb:f/f/f failed");
    CHECK(exact.red == 0xffff && exact.green == 0xffff && exact.blue == 0xffff,
          "XParseColor rgb: single-digit not bit-replicated to full intensity");

    XColor screen;
    memset(&exact, 0, sizeof(exact));
    memset(&screen, 0, sizeof(screen));
    CHECK(XLookupColor(display, colormap, "lightgray", &exact, &screen),
          "XLookupColor named color failed");
    CHECK(exact.pixel == screen.pixel && exact.red == screen.red,
          "XLookupColor did not copy exact color to screen color");

    memset(&exact, 0, sizeof(exact));
    memset(&screen, 0, sizeof(screen));
    CHECK(XAllocNamedColor(display, colormap, "red", &screen, &exact),
          "XAllocNamedColor red failed");
    CHECK(exact.pixel == 0xFFFF0000 && screen.pixel == exact.pixel,
          "named red did not use ARGB pixel encoding");
    CHECK(exact.red == 0xffff && exact.green == 0 && exact.blue == 0,
          "named red returned wrong components");

    XColor query = {.pixel = exact.pixel};
    CHECK(XQueryColors(display, colormap, &query, 1), "XQueryColors failed");
    CHECK(query.red == 0xffff && query.green == 0 && query.blue == 0,
          "XQueryColors decoded ARGB pixels incorrectly");

    Window root = RootWindow(display, DefaultScreen(display));
    Pixmap pixmap = XCreatePixmap(
        display, root, 2, 2, DefaultDepth(display, DefaultScreen(display)));
    CHECK(pixmap != None, "XCreatePixmap for named color draw failed");
    GC gc = XCreateGC(display, pixmap, 0, NULL);
    CHECK(gc != NULL, "XCreateGC for named color draw failed");
    CHECK(XSetForeground(display, gc, screen.pixel),
          "XSetForeground for named color draw failed");
    CHECK(XDrawPoint(display, pixmap, gc, 0, 0),
          "XDrawPoint for named color draw failed");
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(pixmap, renderer);
    SDL_Surface *surface = getRenderSurface(renderer);
    CHECK(surface != NULL, "getRenderSurface for named color draw failed");
    CHECK(pixel_is_rgb(surface, 0, 0, 255, 0, 0),
          "drawing with named red rendered the wrong color");
    SDL_FreeSurface(surface);
    XFreeGC(display, gc);
    XFreePixmap(display, pixmap);

    exact.pixel = 0x12345678;
    screen.pixel = 0x87654321;
    CHECK(!XParseColor(display, colormap, "#12xx56", &exact),
          "XParseColor accepted invalid hex");
    CHECK(!XParseColor(display, colormap, "rgb:e5/e5", &exact),
          "XParseColor accepted invalid rgb spec");
    CHECK(!XLookupColor(display, colormap, "not-a-real-color", &exact, &screen),
          "XLookupColor accepted invalid color");
    CHECK(!XAllocNamedColor(display, colormap, "not-a-real-color", &screen,
                            &exact),
          "XAllocNamedColor accepted invalid color");

    /* XSetRGBColormaps / XGetRGBColormaps round-trip. Locks in the ICCCM
     * section 6.4 wire-format ordering (visualid, killid, colormap, red_max,
     * red_mult, green_max, green_mult, blue_max, blue_mult, base_pixel). If
     * anyone edits the field order in one direction without the other, this
     * test fails.
     */
    Window stdCmapTarget = RootWindow(display, DefaultScreen(display));
    Atom rgbProbe = XInternAtom(display, "_LIBX11_COMPAT_TEST_RGB", False);
    XStandardColormap inCmap = {0};
    inCmap.colormap = colormap;
    inCmap.red_max = 31;
    inCmap.red_mult = 0x800;
    inCmap.green_max = 63;
    inCmap.green_mult = 0x20;
    inCmap.blue_max = 31;
    inCmap.blue_mult = 1;
    inCmap.base_pixel = 0x42;
    inCmap.visualid =
        XVisualIDFromVisual(DefaultVisual(display, DefaultScreen(display)));
    inCmap.killid = (XID) 0x99;
    XSetRGBColormaps(display, stdCmapTarget, &inCmap, 1, rgbProbe);
    XStandardColormap *outCmap = NULL;
    int outCount = 0;
    CHECK(
        XGetRGBColormaps(display, stdCmapTarget, &outCmap, &outCount, rgbProbe),
        "XGetRGBColormaps did not find the entry just installed");
    CHECK(outCount == 1, "XGetRGBColormaps returned wrong count");
    CHECK(outCmap[0].visualid == inCmap.visualid &&
              outCmap[0].killid == inCmap.killid &&
              outCmap[0].colormap == inCmap.colormap &&
              outCmap[0].red_max == 31 && outCmap[0].red_mult == 0x800 &&
              outCmap[0].green_max == 63 && outCmap[0].green_mult == 0x20 &&
              outCmap[0].blue_max == 31 && outCmap[0].blue_mult == 1 &&
              outCmap[0].base_pixel == 0x42,
          "XStandardColormap fields did not round-trip");
    XFree(outCmap);
    XDeleteProperty(display, stdCmapTarget, rgbProbe);

    return 1;
}

static int pixel_is_rgb(SDL_Surface *surface,
                        int x,
                        int y,
                        Uint8 red,
                        Uint8 green,
                        Uint8 blue)
{
    return pixel_is_rgba(surface, x, y, red, green, blue, 255);
}

static int pixel_is_rgba(SDL_Surface *surface,
                         int x,
                         int y,
                         Uint8 red,
                         Uint8 green,
                         Uint8 blue,
                         Uint8 alpha)
{
    Uint8 gotRed = 0;
    Uint8 gotGreen = 0;
    Uint8 gotBlue = 0;
    Uint8 gotAlpha = 0;
    SDL_GetRGBA(getPixel(surface, (unsigned int) x, (unsigned int) y),
                XC_SURFACE_FORMAT(surface), &gotRed, &gotGreen, &gotBlue,
                &gotAlpha);
    return gotRed == red && gotGreen == green && gotBlue == blue &&
           gotAlpha == alpha;
}

static int pixel_is_between_black_and_white(SDL_Surface *surface, int x, int y)
{
    Uint8 red = 0;
    Uint8 green = 0;
    Uint8 blue = 0;
    Uint8 alpha = 0;
    SDL_GetRGBA(getPixel(surface, (unsigned int) x, (unsigned int) y),
                XC_SURFACE_FORMAT(surface), &red, &green, &blue, &alpha);
    return alpha == 255 && red == green && green == blue && red > 0 &&
           red < 255;
}

static int count_rgb_pixels(SDL_Surface *surface,
                            Uint8 red,
                            Uint8 green,
                            Uint8 blue)
{
    int count = 0;
    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            if (pixel_is_rgb(surface, x, y, red, green, blue))
                count++;
        }
    }
    return count;
}

/* Read the pixmap back and count pure-black pixels, or -1 if readback fails.
 * Used to prove each text draw put ink down on its own, not riding on a
 * previous draw's pixels.
 */
static int readback_black_count(SDL_Renderer *renderer)
{
    SDL_Surface *surface = getRenderSurface(renderer);
    if (!surface)
        return -1;
    int count = count_rgb_pixels(surface, 0, 0, 0);
    SDL_FreeSurface(surface);
    return count;
}

static int exercise_fixed_font_program(Display *display)
{
    XFontStruct *fixed = XLoadQueryFont(display, "fixed");
    CHECK(fixed != NULL && fixed->fid != None, "fixed alias did not load");
    CHECK(fixed->ascent == 11 && fixed->descent == 2,
          "fixed alias did not use core 6x13 ascent/descent");
    CHECK(fixed->min_bounds.width == 6 && fixed->max_bounds.width == 6,
          "fixed alias did not use core 6x13 width");
    CHECK(XTextWidth(fixed, "Motif", 5) == 30,
          "fixed alias XTextWidth did not use core width");
    CHECK(XTextWidth(fixed, "\xc3\xa9", 2) == 12,
          "fixed alias XTextWidth did not count core-font bytes");
    const char fixedWithNul[] = {'A', '\0', 'B'};
    CHECK(XTextWidth(fixed, fixedWithNul, 3) == 18,
          "fixed alias XTextWidth did not honor byte count through NUL");
    XChar2b fixedWide[] = {{0, 0xe9}};
    CHECK(XTextWidth16(fixed, fixedWide, 1) == 6,
          "fixed alias XTextWidth16 did not count 16-bit characters");

    XFontStruct *upperFixed = XLoadQueryFont(display, "Fixed");
    CHECK(upperFixed != NULL && upperFixed->fid != None,
          "capitalized Fixed alias did not load");
    CHECK(upperFixed->ascent == 11 && upperFixed->descent == 2,
          "capitalized Fixed alias did not use core 6x13 ascent/descent");
    CHECK(
        upperFixed->min_bounds.width == 6 && upperFixed->max_bounds.width == 6,
        "capitalized Fixed alias did not use core 6x13 width");
    CHECK(XTextWidth(upperFixed, "A", 1) == 6,
          "capitalized Fixed alias XTextWidth did not use core width");
    XFreeFont(display, upperFixed);

    XFontStruct *sixByThirteen = XLoadQueryFont(display, "6x13");
    CHECK(sixByThirteen != NULL && sixByThirteen->fid != None,
          "6x13 alias did not load");
    CHECK(sixByThirteen->ascent == 11 && sixByThirteen->descent == 2,
          "6x13 alias did not use native ascent/descent");
    CHECK(XTextWidth(sixByThirteen, "Motif", 5) == 30,
          "6x13 alias XTextWidth did not use native width");
    XFreeFont(display, sixByThirteen);

    Window root = RootWindow(display, DefaultScreen(display));
    Pixmap pixmap = XCreatePixmap(
        display, root, 128, 48, DefaultDepth(display, DefaultScreen(display)));
    CHECK(pixmap != None, "fixed-font program pixmap creation failed");
    GC gc = XCreateGC(display, pixmap, 0, NULL);
    CHECK(gc != NULL, "fixed-font program GC creation failed");
    CHECK(XSetFont(display, gc, fixed->fid),
          "fixed-font program XSetFont failed");
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(pixmap, renderer);

    /* Verify each text API in isolation: clear to white, draw once, and confirm
     * that draw alone put black pixels down. Sharing one pixmap across all
     * three draws would let a dead XDrawImageString or XDrawText ride on the
     * pixels left by XDrawString and still pass.
     */
    CHECK(XSetForeground(display, gc, 0x00FFFFFF),
          "fixed-font program white setup failed");
    CHECK(XFillRectangle(display, pixmap, gc, 0, 0, 128, 48),
          "fixed-font program background fill failed");
    CHECK(XSetForeground(display, gc, 0x00000000),
          "fixed-font program black setup failed");
    CHECK(XDrawString(display, pixmap, gc, 2, fixed->ascent, "fixed", 5),
          "fixed-font program XDrawString failed");
    CHECK(readback_black_count(renderer) > 0,
          "fixed-font program XDrawString rendered no pixels");

    CHECK(XSetForeground(display, gc, 0x00FFFFFF),
          "fixed-font program image-string clear failed");
    CHECK(XFillRectangle(display, pixmap, gc, 0, 0, 128, 48),
          "fixed-font program image-string fill failed");
    CHECK(XSetForeground(display, gc, 0x00000000),
          "fixed-font program image-string black setup failed");
    CHECK(XDrawImageString(display, pixmap, gc, 2, fixed->ascent, "image", 5),
          "fixed-font program XDrawImageString failed");
    CHECK(readback_black_count(renderer) > 0,
          "fixed-font program XDrawImageString rendered no pixels");

    CHECK(XSetForeground(display, gc, 0x00FFFFFF),
          "fixed-font program text-item clear failed");
    CHECK(XFillRectangle(display, pixmap, gc, 0, 0, 128, 48),
          "fixed-font program text-item fill failed");
    CHECK(XSetForeground(display, gc, 0x00000000),
          "fixed-font program text-item black setup failed");
    XTextItem item = {
        .chars = "text",
        .nchars = 4,
        .delta = 0,
        .font = fixed->fid,
    };
    CHECK(XDrawText(display, pixmap, gc, 2, fixed->ascent, &item, 1),
          "fixed-font program XDrawText failed");
    CHECK(readback_black_count(renderer) > 0,
          "fixed-font program XDrawText rendered no pixels");

    CHECK(XSetForeground(display, gc, 0x00FFFFFF),
          "fixed-font program clear color setup failed");
    int rowHeight = fixed->ascent + fixed->descent;
    CHECK(XFillRectangle(display, pixmap, gc, 0, 0, 128, 48),
          "fixed-font program second clear failed");
    CHECK(XSetForeground(display, gc, 0x00000000),
          "fixed-font program second black setup failed");
    CHECK(XDrawString(display, pixmap, gc, 0, fixed->ascent, "gjpqy", 5),
          "fixed-font program descender draw failed");
    CHECK(XSetForeground(display, gc, 0x00FFFFFF),
          "fixed-font program row clear color setup failed");
    CHECK(XFillRectangle(display, pixmap, gc, 0, 0, 128,
                         (unsigned int) rowHeight),
          "fixed-font program row clear failed");
    SDL_Surface *surface = getRenderSurface(renderer);
    CHECK(surface, "fixed-font program row readback failed");
    int escapedPixel = 0;
    for (int y = rowHeight; y < surface->h && !escapedPixel; y++) {
        for (int x = 0; x < surface->w; x++) {
            if (pixel_is_rgb(surface, x, y, 0, 0, 0)) {
                escapedPixel = 1;
                break;
            }
        }
    }
    SDL_FreeSurface(surface);
    CHECK(!escapedPixel,
          "fixed-font program rendered outside advertised row metrics");

    XFreeGC(display, gc);
    XFreePixmap(display, pixmap);
    XFreeFont(display, fixed);
    return 1;
}

static int test_pixmaps(Display *display)
{
    int formatCount = 0;
    XPixmapFormatValues *formats = XListPixmapFormats(display, &formatCount);
    CHECK(formats != NULL, "XListPixmapFormats returned NULL");
    int hasDepth1 = 0;
    int hasDepth16 = 0;
    int hasDepth24 = 0;
    int hasDepth32 = 0;
    for (int i = 0; i < formatCount; i++) {
        if (formats[i].depth == 1)
            hasDepth1 = 1;
        if (formats[i].depth == 16)
            hasDepth16 = 1;
        if (formats[i].depth == 24)
            hasDepth24 = 1;
        if (formats[i].depth == 32)
            hasDepth32 = 1;
    }
    CHECK(hasDepth1 && hasDepth16 && hasDepth24 && hasDepth32,
          "XListPixmapFormats missing common depths");
    XFree(formats);

    Window root = RootWindow(display, DefaultScreen(display));
    unsigned int pixmapDepths[] = {1, 16, 24, 32};
    for (size_t i = 0; i < sizeof(pixmapDepths) / sizeof(pixmapDepths[0]);
         i++) {
        Pixmap depthPixmap =
            XCreatePixmap(display, root, 3, 2, pixmapDepths[i]);
        CHECK(depthPixmap != None, "XCreatePixmap supported depth failed");
        Window geomRoot = None;
        int geomX = 0;
        int geomY = 0;
        unsigned int geomWidth = 0;
        unsigned int geomHeight = 0;
        unsigned int geomBorder = 0;
        unsigned int geomDepth = 0;
        CHECK(XGetGeometry(display, depthPixmap, &geomRoot, &geomX, &geomY,
                           &geomWidth, &geomHeight, &geomBorder, &geomDepth),
              "XGetGeometry on pixmap failed");
        CHECK(geomRoot == root && geomX == 0 && geomY == 0 && geomWidth == 3 &&
                  geomHeight == 2 && geomBorder == 0 &&
                  geomDepth == pixmapDepths[i],
              "pixmap geometry/depth metadata was incorrect");
        XFreePixmap(display, depthPixmap);
    }
    int (*oldErrorHandler)(Display *, XErrorEvent *) =
        XSetErrorHandler(ignored_error);
    CHECK(XCreatePixmap(display, root, 3, 2, 7) == None,
          "XCreatePixmap accepted an unsupported depth");
    XSetErrorHandler(oldErrorHandler);

    /* XCreateBitmapFromData expects ceil(width/8) * height packed bytes. For a
     * 2x2 bitmap that is two bytes (one per row); supplying a single byte
     * tripped AddressSanitizer's stack-buffer-overflow check.
     */
    char bits[] = {0x01, 0x00};
    Pixmap bitmap = XCreateBitmapFromData(display, root, bits, 2, 2);
    CHECK(bitmap != None, "XCreateBitmapFromData failed");
    Window geomRoot = None;
    int geomX = 0;
    int geomY = 0;
    unsigned int geomWidth = 0;
    unsigned int geomHeight = 0;
    unsigned int geomBorder = 0;
    unsigned int geomDepth = 0;
    CHECK(XGetGeometry(display, bitmap, &geomRoot, &geomX, &geomY, &geomWidth,
                       &geomHeight, &geomBorder, &geomDepth),
          "XGetGeometry on bitmap failed");
    CHECK(geomDepth == 1, "XCreateBitmapFromData did not preserve depth 1");
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(bitmap, renderer);
    SDL_Surface *surface = getRenderSurface(renderer);
    CHECK(surface != NULL, "getRenderSurface for bitmap failed");
    CHECK(pixel_is_rgb(surface, 0, 0, 255, 255, 255),
          "bitmap set bit did not become white");
    CHECK(pixel_is_rgb(surface, 1, 0, 0, 0, 0),
          "bitmap clear bit did not become black");
    SDL_FreeSurface(surface);
    XImage *bitmapImage =
        XGetImage(display, bitmap, 0, 0, 2, 2, AllPlanes, ZPixmap);
    CHECK(bitmapImage != NULL, "XGetImage for bitmap failed");
    CHECK(XGetPixel(bitmapImage, 0, 0) == 1,
          "XGetImage lost the set bitmap bit");
    CHECK(XGetPixel(bitmapImage, 1, 0) == 0,
          "XGetImage turned a clear bitmap bit into one");
    CHECK(XGetPixel(bitmapImage, 0, 1) == 0,
          "XGetImage treated opaque black as a set bitmap bit");
    XDestroyImage(bitmapImage);

    Pixmap planeDest =
        XCreatePixmap(display, root, 2, 2, DefaultDepth(display, 0));
    CHECK(planeDest != None, "XCopyPlane destination pixmap failed");
    GC planeGc = XCreateGC(display, planeDest, 0, NULL);
    CHECK(planeGc, "XCopyPlane GC failed");
    XSetForeground(display, planeGc, 0xFFFF0000);
    XSetBackground(display, planeGc, 0xFF000000);
    CHECK(XCopyPlane(display, bitmap, planeDest, planeGc, 0, 0, 2, 2, 0, 0, 1),
          "XCopyPlane failed");
    oldErrorHandler = XSetErrorHandler(ignored_error);
    CHECK(!XCopyPlane(display, bitmap, planeDest, planeGc, 0, 0, 2, 2, 0, 0, 3),
          "XCopyPlane accepted a multi-bit plane mask");
    XSetErrorHandler(oldErrorHandler);
    GET_RENDERER(planeDest, renderer);
    surface = getRenderSurface(renderer);
    CHECK(surface, "getRenderSurface for XCopyPlane failed");
    CHECK(pixel_is_rgb(surface, 0, 0, 255, 0, 0),
          "XCopyPlane set bit did not use GC foreground");
    CHECK(pixel_is_rgb(surface, 1, 0, 0, 0, 0),
          "XCopyPlane clear bit did not use GC background");
    SDL_FreeSurface(surface);
    XFreeGC(display, planeGc);
    XFreePixmap(display, planeDest);
    XFreePixmap(display, bitmap);

    Pixmap stalePixmap =
        XCreatePixmap(display, root, 4, 4, DefaultDepth(display, 0));
    Window staleDest = XCreateSimpleWindow(display, root, 0, 0, 4, 4, 0, 0, 0);
    CHECK(stalePixmap != None && staleDest != None,
          "stale pixmap XCopyArea setup failed");
    CHECK(XMapWindow(display, staleDest),
          "stale pixmap destination map failed");
    GC staleGc = XCreateGC(display, staleDest, 0, NULL);
    CHECK(staleGc != NULL, "stale pixmap GC creation failed");
    CHECK(XFreePixmap(display, stalePixmap), "stale pixmap free failed");
    last_error_code = 0;
    oldErrorHandler = XSetErrorHandler(record_error);
    CHECK(
        !XCopyArea(display, stalePixmap, staleDest, staleGc, 0, 0, 1, 1, 0, 0),
        "XCopyArea accepted a freed source pixmap");
    XSetErrorHandler(oldErrorHandler);
    CHECK(last_error_code == BadDrawable,
          "XCopyArea freed source pixmap did not report BadDrawable");
    XFreeGC(display, staleGc);
    XDestroyWindow(display, staleDest);
    while (XPending(display)) {
        XEvent pending;
        XNextEvent(display, &pending);
    }

    Window staleSource =
        XCreateSimpleWindow(display, root, 0, 0, 4, 4, 0, 0, 0);
    staleDest = XCreateSimpleWindow(display, root, 0, 0, 4, 4, 0, 0, 0);
    CHECK(staleSource != None && staleDest != None,
          "stale window XCopyArea setup failed");
    CHECK(XMapWindow(display, staleSource), "stale source window map failed");
    CHECK(XMapWindow(display, staleDest), "stale dest window map failed");
    staleGc = XCreateGC(display, staleDest, 0, NULL);
    CHECK(staleGc != NULL, "stale window GC creation failed");
    CHECK(XDestroyWindow(display, staleSource), "stale source destroy failed");
    last_error_code = 0;
    oldErrorHandler = XSetErrorHandler(record_error);
    CHECK(
        !XCopyArea(display, staleSource, staleDest, staleGc, 0, 0, 1, 1, 0, 0),
        "XCopyArea accepted a destroyed source window");
    XSetErrorHandler(oldErrorHandler);
    CHECK(last_error_code == BadDrawable,
          "XCopyArea destroyed source window did not report BadDrawable");
    XFreeGC(display, staleGc);
    XDestroyWindow(display, staleDest);
    while (XPending(display)) {
        XEvent pending;
        XNextEvent(display, &pending);
    }

    Pixmap arcPixmap =
        XCreatePixmap(display, root, 16, 16, DefaultDepth(display, 0));
    CHECK(arcPixmap != None, "arc pixmap creation failed");
    GC arcGc = XCreateGC(display, arcPixmap, 0, NULL);
    CHECK(arcGc, "arc GC creation failed");
    XSetForeground(display, arcGc, 0xFFFF0000);
    CHECK(XFillArc(display, arcPixmap, arcGc, 2, 2, 12, 12, 0, 360 * 64),
          "XFillArc failed");
    GET_RENDERER(arcPixmap, renderer);
    surface = getRenderSurface(renderer);
    CHECK(surface, "getRenderSurface for XFillArc failed");
    CHECK(pixel_is_rgb(surface, 8, 8, 255, 0, 0),
          "XFillArc did not fill ellipse center");
    SDL_FreeSurface(surface);
    XSetForeground(display, arcGc, 0xFF00FF00);
    CHECK(XDrawArc(display, arcPixmap, arcGc, 2, 2, 12, 12, 0, 90 * 64),
          "XDrawArc failed");
    surface = getRenderSurface(renderer);
    CHECK(surface, "getRenderSurface for XDrawArc failed");
    CHECK(pixel_is_rgb(surface, 14, 8, 0, 255, 0),
          "XDrawArc did not draw the arc start point");
    SDL_FreeSurface(surface);
    XFreeGC(display, arcGc);
    XFreePixmap(display, arcPixmap);

    char colorBits[] = {0x02};
    Pixmap colorPixmap = XCreatePixmapFromBitmapData(
        display, root, colorBits, 2, 1, 0xFFFF0000, 0xFF000000, 24);
    CHECK(colorPixmap != None, "XCreatePixmapFromBitmapData failed");
    CHECK(XGetGeometry(display, colorPixmap, &geomRoot, &geomX, &geomY,
                       &geomWidth, &geomHeight, &geomBorder, &geomDepth),
          "XGetGeometry on color pixmap failed");
    CHECK(geomDepth == 24,
          "XCreatePixmapFromBitmapData did not preserve requested depth");
    GET_RENDERER(colorPixmap, renderer);
    surface = getRenderSurface(renderer);
    CHECK(surface != NULL, "getRenderSurface for color pixmap failed");
    CHECK(pixel_is_rgb(surface, 0, 0, 0, 0, 0),
          "pixmap clear bit did not use background");
    CHECK(pixel_is_rgb(surface, 1, 0, 255, 0, 0),
          "pixmap set bit did not use foreground");
    SDL_FreeSurface(surface);
    XFreePixmap(display, colorPixmap);

    colorPixmap = XCreatePixmapFromBitmapData(display, root, colorBits, 2, 1, 0,
                                              0x00FFFFFF, 24);
    CHECK(colorPixmap != None,
          "XCreatePixmapFromBitmapData with X11 pixel colors failed");
    GET_RENDERER(colorPixmap, renderer);
    surface = getRenderSurface(renderer);
    CHECK(surface != NULL,
          "getRenderSurface for X11 pixel color pixmap failed");
    CHECK(pixel_is_rgb(surface, 0, 0, 255, 255, 255),
          "pixmap clear bit treated alpha-less white as transparent");
    CHECK(pixel_is_rgb(surface, 1, 0, 0, 0, 0),
          "pixmap set bit treated X11 black pixel as transparent");
    SDL_FreeSurface(surface);
    XFreePixmap(display, colorPixmap);

    Pixmap copySrc = XCreatePixmap(
        display, root, 8, 8, DefaultDepth(display, DefaultScreen(display)));
    Pixmap copyDest = XCreatePixmap(
        display, root, 8, 8, DefaultDepth(display, DefaultScreen(display)));
    CHECK(copySrc != None && copyDest != None,
          "XCopyArea pixmap regression pixmap creation failed");
    GC copyGc = XCreateGC(display, copySrc, 0, NULL);
    CHECK(copyGc != NULL, "XCopyArea pixmap regression GC creation failed");
    CHECK(XSetForeground(display, copyGc, 0xFFFFFFFF),
          "XCopyArea pixmap regression source clear color failed");
    CHECK(XFillRectangle(display, copySrc, copyGc, 0, 0, 8, 8),
          "XCopyArea pixmap regression source fill failed");
    CHECK(XSetForeground(display, copyGc, 0xFF000000),
          "XCopyArea pixmap regression source mark color failed");
    CHECK(XFillRectangle(display, copySrc, copyGc, 2, 2, 3, 3),
          "XCopyArea pixmap regression source mark failed");
    Drawable narrowedCopySrc = (Drawable) (unsigned int) copySrc;
    CHECK(narrowedCopySrc == copySrc,
          "allocated pixmap XID does not round-trip through 32 bits");
    CHECK(XSetForeground(display, copyGc, 0xFFFFFFFF),
          "XCopyArea pixmap regression destination clear color failed");
    CHECK(XFillRectangle(display, copyDest, copyGc, 0, 0, 8, 8),
          "XCopyArea pixmap regression destination clear failed");
    CHECK(XCopyArea(display, copySrc, copyDest, copyGc, 2, 2, 3, 3, 0, 0),
          "XCopyArea to pixmap failed");
    CHECK(
        XCopyArea(display, narrowedCopySrc, copyDest, copyGc, 2, 2, 3, 3, 0, 0),
        "XCopyArea rejected a 32-bit-round-tripped pixmap XID");
    GET_RENDERER(copyDest, renderer);
    surface = getRenderSurface(renderer);
    CHECK(surface != NULL,
          "getRenderSurface for XCopyArea pixmap destination failed");
    CHECK(pixel_is_rgb(surface, 0, 0, 0, 0, 0),
          "XCopyArea to pixmap did not copy source pixels");
    CHECK(pixel_is_rgb(surface, 4, 4, 255, 255, 255),
          "XCopyArea to pixmap damaged unrelated pixels");
    SDL_FreeSurface(surface);

    /* Non-GXcopy XCopyArea: the raster-op fallback should now respect the GC
     * function. GXxor of opaque white over black yields white, GXand yields
     * black. Skip the assertion if SDL_RenderReadPixels returns 0 (some
     * headless SDL configs), but at minimum the call must report success.
     */
    CHECK(XSetForeground(display, copyGc, 0xFFFFFFFF),
          "raster-op src fill color setup failed");
    CHECK(XFillRectangle(display, copySrc, copyGc, 0, 0, 8, 8),
          "raster-op src fill failed");
    CHECK(XSetForeground(display, copyGc, 0xFF000000),
          "raster-op dest clear color setup failed");
    CHECK(XFillRectangle(display, copyDest, copyGc, 0, 0, 8, 8),
          "raster-op dest clear failed");
    XGCValues rasterValues;
    rasterValues.function = GXxor;
    CHECK(XChangeGC(display, copyGc, GCFunction, &rasterValues),
          "raster-op GXxor configuration failed");
    CHECK(XCopyArea(display, copySrc, copyDest, copyGc, 0, 0, 8, 8, 0, 0) == 1,
          "XCopyArea with GXxor failed");
    rasterValues.function = GXcopy;
    XChangeGC(display, copyGc, GCFunction, &rasterValues);
    /* Verify actual pixel output: white XOR black = white (0xFFFFFF). Without
     * this assertion the test would pass even if the raster-op helper read the
     * wrong renderer or fed uninitialized memory into applyRasterFunction.
     */
    {
        SDL_Renderer *rasterRenderer = NULL;
        GET_RENDERER(copyDest, rasterRenderer);
        SDL_Surface *rasterSurface = getRenderSurface(rasterRenderer);
        if (rasterSurface) {
            CHECK(pixel_is_rgb(rasterSurface, 0, 0, 255, 255, 255),
                  "GXxor XCopyArea did not produce white = black ^ white");
            CHECK(pixel_is_rgb(rasterSurface, 4, 4, 255, 255, 255),
                  "GXxor XCopyArea did not cover the full rect");
            SDL_FreeSurface(rasterSurface);
        }
    }

    CHECK(XSetForeground(display, copyGc, 0xFF000000),
          "clipped raster-op dest clear color setup failed");
    CHECK(XFillRectangle(display, copyDest, copyGc, 0, 0, 8, 8),
          "clipped raster-op dest clear failed");
    rasterValues.function = GXxor;
    CHECK(XChangeGC(display, copyGc, GCFunction, &rasterValues),
          "clipped raster-op GXxor configuration failed");
    CHECK(
        XCopyArea(display, copySrc, copyDest, copyGc, 0, 0, 4, 4, -2, -2) == 1,
        "clipped GXxor XCopyArea failed");
    CHECK(XCopyArea(display, copySrc, copyDest, copyGc, 0, 2, 1,
                    (unsigned int) -39, 0, 6) == 1,
          "raster-op XCopyArea rejected protocol-truncated height");
    rasterValues.function = GXcopy;
    XChangeGC(display, copyGc, GCFunction, &rasterValues);
    {
        SDL_Renderer *rasterRenderer = NULL;
        GET_RENDERER(copyDest, rasterRenderer);
        SDL_Surface *rasterSurface = getRenderSurface(rasterRenderer);
        if (rasterSurface) {
            CHECK(pixel_is_rgb(rasterSurface, 0, 0, 255, 255, 255),
                  "clipped GXxor XCopyArea did not write visible pixels");
            CHECK(pixel_is_rgb(rasterSurface, 2, 2, 0, 0, 0),
                  "clipped GXxor XCopyArea damaged outside clipped region");
            SDL_FreeSurface(rasterSurface);
        }
    }

    /* XCopyArea must reject geometries whose extents would overflow signed int
     * instead of wrapping into SDL_Rect math. Combine a near- INT_MAX
     * coordinate with a non-zero width to force the extent over the int
     * boundary.
     */
    int (*xcOldHandler)(Display *, XErrorEvent *) =
        XSetErrorHandler(record_error);
    last_error_code = 0;
    CHECK(XCopyArea(display, copySrc, copyDest, copyGc, INT_MAX, 0, 16u, 1u, 0,
                    0) == 0,
          "XCopyArea accepted src_x + width past INT_MAX");
    XSync(display, False);
    CHECK(last_error_code == BadValue,
          "XCopyArea src extent overflow did not raise BadValue");
    last_error_code = 0;
    CHECK(XCopyArea(display, copySrc, copyDest, copyGc, 0, 0, 16u, 1u, INT_MAX,
                    0) == 0,
          "XCopyArea accepted dest_x + width past INT_MAX");
    XSync(display, False);
    CHECK(last_error_code == BadValue,
          "XCopyArea dest extent overflow did not raise BadValue");
    last_error_code = 0;
    CHECK(XCopyArea(display, copySrc, copyDest, copyGc, 0, 0,
                    (unsigned int) INT_MAX + 1u, 1u, 0, 0) == 0,
          "XCopyArea accepted width over INT_MAX");
    XSync(display, False);
    CHECK(last_error_code == BadValue,
          "XCopyArea width over INT_MAX did not raise BadValue");
    XSetErrorHandler(xcOldHandler);

    XFreeGC(display, copyGc);
    XFreePixmap(display, copySrc);
    XFreePixmap(display, copyDest);

    /* XBM file round-trip: parse a hand-written file, then read back a file
     * produced by XWriteBitmapFile.
     */
    char xbmPath[] = "/tmp/libx11-compat-bitmap-XXXXXX";
    int xbmFd = mkstemp(xbmPath);
    CHECK(xbmFd >= 0, "mkstemp for XBM failed");
    close(xbmFd);
    FILE *xbmFile = fopen(xbmPath, "w");
    CHECK(xbmFile != NULL, "fopen XBM for write failed");
    fprintf(xbmFile,
            "#define img_width 8\n"
            "#define img_height 2\n"
            "#define img_x_hot 1\n"
            "#define img_y_hot 1\n"
            "static char img_bits[] = {\n"
            "  0xff, 0x00\n"
            "};\n");
    fclose(xbmFile);

    unsigned int rw = 0, rh = 0;
    int xh = -99, yh = -99;
    Pixmap loaded = None;
    int rcXbm =
        XReadBitmapFile(display, root, xbmPath, &rw, &rh, &loaded, &xh, &yh);
    CHECK(rcXbm == BitmapSuccess, "XReadBitmapFile failed");
    CHECK(rw == 8 && rh == 2, "XReadBitmapFile returned wrong dimensions");
    CHECK(xh == 1 && yh == 1, "XReadBitmapFile lost hotspot");
    CHECK(loaded != None, "XReadBitmapFile produced no pixmap");
    XFreePixmap(display, loaded);

    char xbmOutPath[] = "/tmp/libx11-compat-bitmap-out-XXXXXX";
    int xbmOutFd = mkstemp(xbmOutPath);
    CHECK(xbmOutFd >= 0, "mkstemp for XBM out failed");
    close(xbmOutFd);
    Pixmap roundTrip = XCreateBitmapFromData(
        display, root, (char[]) {(char) 0xff, 0x00}, 8, 2);
    CHECK(roundTrip != None, "XCreateBitmapFromData for XBM out failed");
    rcXbm = XWriteBitmapFile(display, xbmOutPath, roundTrip, 8, 2, 0, 0);
    CHECK(rcXbm == BitmapSuccess, "XWriteBitmapFile failed");
    XFreePixmap(display, roundTrip);

    Pixmap reloaded = None;
    rcXbm = XReadBitmapFile(display, root, xbmOutPath, &rw, &rh, &reloaded, &xh,
                            &yh);
    CHECK(rcXbm == BitmapSuccess, "XReadBitmapFile on written file failed");
    CHECK(rw == 8 && rh == 2,
          "XReadBitmapFile after write got wrong dimensions");
    CHECK(reloaded != None, "XReadBitmapFile after write produced no pixmap");
    XFreePixmap(display, reloaded);

    unlink(xbmPath);
    unlink(xbmOutPath);
    while (XPending(display)) {
        XEvent pending;
        XNextEvent(display, &pending);
    }
    return 1;
}

static int test_drawables_and_gcs(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Pixmap pixmap =
        XCreatePixmap(display, root, 32, 32, DefaultDepth(display, 0));
    CHECK(pixmap != None, "drawables pixmap creation failed");
    GC gc = XCreateGC(display, pixmap, 0, NULL);
    CHECK(gc, "drawables GC creation failed");

    CHECK(XSetForeground(display, gc, 0xFF000000), "set black failed");
    CHECK(XFillRectangle(display, pixmap, gc, 0, 0, 32, 32),
          "clear drawable failed");

    XPoint triangle[] = {{2, 2}, {14, 2}, {2, 14}};
    CHECK(XSetForeground(display, gc, 0xFFFF0000), "set red failed");
    CHECK(
        XFillPolygon(display, pixmap, gc, triangle, 3, Convex, CoordModeOrigin),
        "XFillPolygon convex triangle failed");

    XPoint previousTriangle[] = {{18, 2}, {12, 0}, {-12, 12}};
    CHECK(XSetForeground(display, gc, 0xFF0000FF), "set blue failed");
    CHECK(XFillPolygon(display, pixmap, gc, previousTriangle, 3, Convex,
                       CoordModePrevious),
          "XFillPolygon CoordModePrevious failed");

    XPoint bowtie[] = {{4, 18}, {14, 30}, {14, 18}, {4, 30}};
    CHECK(XSetFillRule(display, gc, EvenOddRule), "set even-odd failed");
    CHECK(XSetForeground(display, gc, 0xFFFFFF00), "set yellow failed");
    CHECK(
        XFillPolygon(display, pixmap, gc, bowtie, 4, Complex, CoordModeOrigin),
        "XFillPolygon bowtie failed");

    XRectangle clip = {.x = 24, .y = 24, .width = 4, .height = 4};
    CHECK(XSetClipRectangles(display, gc, 0, 0, &clip, 1, Unsorted),
          "set polygon clip failed");
    XPoint clippedPoly[] = {{22, 22}, {31, 22}, {31, 31}, {22, 31}};
    CHECK(XSetForeground(display, gc, 0xFF00FF00), "set green failed");
    CHECK(XFillPolygon(display, pixmap, gc, clippedPoly, 4, Convex,
                       CoordModeOrigin),
          "XFillPolygon clipped square failed");
    CHECK(XSetClipMask(display, gc, None), "clear polygon clip failed");

    XArc fillArcs[] = {
        {.x = 18,
         .y = 18,
         .width = 8,
         .height = 8,
         .angle1 = 0,
         .angle2 = 360 * 64},
        {.x = 24,
         .y = 2,
         .width = 6,
         .height = 6,
         .angle1 = 0,
         .angle2 = 360 * 64},
    };
    CHECK(XSetForeground(display, gc, 0xFFFF00FF), "set magenta failed");
    CHECK(XFillArcs(display, pixmap, gc, fillArcs, 2), "XFillArcs failed");
    XArc drawArcs[] = {
        {.x = 2,
         .y = 22,
         .width = 8,
         .height = 8,
         .angle1 = 0,
         .angle2 = 90 * 64},
        {.x = 16,
         .y = 2,
         .width = 8,
         .height = 8,
         .angle1 = 0,
         .angle2 = 90 * 64},
    };
    CHECK(XSetForeground(display, gc, 0xFFFFFFFF), "set white failed");
    CHECK(XDrawArcs(display, pixmap, gc, drawArcs, 2), "XDrawArcs failed");

    CHECK(XSetForeground(display, gc, 0xFF112233), "set base failed");
    CHECK(XSetFunction(display, gc, GXcopy), "set GXcopy failed");
    CHECK(XFillRectangle(display, pixmap, gc, 0, 0, 3, 3),
          "raster base fill failed");
    CHECK(XSetForeground(display, gc, 0xFF010203), "set xor source failed");
    CHECK(XSetFunction(display, gc, GXxor), "set GXxor failed");
    CHECK(XFillRectangle(display, pixmap, gc, 0, 0, 1, 1),
          "GXxor fill rectangle failed");
    CHECK(XDrawPoint(display, pixmap, gc, 1, 0), "GXxor draw point failed");
    CHECK(XDrawLine(display, pixmap, gc, 0, 1, 2, 1), "GXxor draw line failed");
    CHECK(XSetFunction(display, gc, GXnoop), "set GXnoop failed");
    CHECK(XSetForeground(display, gc, 0xFFFFFFFF), "set noop source failed");
    CHECK(XFillRectangle(display, pixmap, gc, 2, 2, 1, 1),
          "GXnoop fill rectangle failed");
    CHECK(XSetFunction(display, gc, GXcopy), "set mask base copy failed");
    CHECK(XSetForeground(display, gc, 0xFF112233), "set mask base failed");
    CHECK(XFillRectangle(display, pixmap, gc, 3, 0, 1, 1),
          "plane-mask base fill failed");
    CHECK(XSetPlaneMask(display, gc, 0x000000FF), "set plane mask failed");
    CHECK(XSetForeground(display, gc, 0xFF010203),
          "set mask xor source failed");
    CHECK(XSetFunction(display, gc, GXxor), "set masked GXxor failed");
    CHECK(XFillRectangle(display, pixmap, gc, 3, 0, 1, 1),
          "plane-masked GXxor fill rectangle failed");
    CHECK(XSetPlaneMask(display, gc, 0xFFFFFFFF), "reset plane mask failed");
    CHECK(XSetFunction(display, gc, GXcopy), "reset GXcopy failed");
    XPoint alphaPoly[] = {{4, 0}, {8, 0}, {4, 4}};
    CHECK(XSetForeground(display, gc, 0x80112233), "set alpha polygon failed");
    CHECK(XFillPolygon(display, pixmap, gc, alphaPoly, 3, Convex,
                       CoordModeOrigin),
          "alpha XFillPolygon failed");
    CHECK(XSetForeground(display, gc, 0x80667788), "set alpha arc failed");
    CHECK(XFillArc(display, pixmap, gc, 10, 0, 6, 6, 0, 360 * 64),
          "alpha XFillArc failed");

    SDL_Renderer *renderer = NULL;
    GET_RENDERER(pixmap, renderer);
    SDL_Surface *surface = getRenderSurface(renderer);
    CHECK(surface, "getRenderSurface for drawables failed");
    int bowtieYellowPixels = 0;
    for (int by = 18; by < 31; by++) {
        for (int bx = 4; bx < 15; bx++) {
            if (pixel_is_rgb(surface, bx, by, 255, 255, 0))
                bowtieYellowPixels++;
        }
    }
    CHECK(pixel_is_rgb(surface, 4, 4, 255, 0, 0),
          "convex triangle did not fill expected pixel");
    CHECK(pixel_is_rgb(surface, 22, 4, 0, 0, 255),
          "CoordModePrevious polygon did not fill expected pixel");
    CHECK(bowtieYellowPixels > 0, "even-odd bowtie produced no filled pixels");
    CHECK(pixel_is_rgb(surface, 25, 25, 0, 255, 0),
          "clipped polygon did not fill inside clip");
    CHECK(pixel_is_rgb(surface, 28, 28, 0, 0, 0),
          "clipped polygon wrote outside clip");
    CHECK(pixel_is_rgb(surface, 22, 22, 255, 0, 255),
          "first XFillArcs arc did not land");
    CHECK(pixel_is_rgb(surface, 27, 5, 255, 0, 255),
          "second XFillArcs arc did not land");
    CHECK(pixel_is_rgb(surface, 10, 26, 255, 255, 255),
          "first XDrawArcs arc did not land");
    CHECK(pixel_is_rgb(surface, 24, 6, 255, 255, 255),
          "second XDrawArcs arc did not land");
    CHECK(pixel_is_rgb(surface, 0, 0, 0x10, 0x20, 0x30),
          "GXxor fill rectangle produced wrong pixel");
    CHECK(pixel_is_rgb(surface, 1, 0, 0x10, 0x20, 0x30),
          "GXxor draw point produced wrong pixel");
    CHECK(pixel_is_rgb(surface, 2, 1, 0x10, 0x20, 0x30),
          "GXxor draw line produced wrong pixel");
    CHECK(pixel_is_rgba(surface, 0, 0, 0x10, 0x20, 0x30, 255),
          "GXxor fill rectangle did not preserve alpha");
    CHECK(pixel_is_rgba(surface, 1, 0, 0x10, 0x20, 0x30, 255),
          "GXxor draw point did not preserve alpha");
    CHECK(pixel_is_rgb(surface, 2, 2, 0x11, 0x22, 0x33),
          "GXnoop changed the destination pixel");
    CHECK(pixel_is_rgba(surface, 3, 0, 0x11, 0x22, 0x30, 255),
          "GXxor ignored the GC plane mask");
    CHECK(pixel_is_rgba(surface, 5, 1, 0x11, 0x22, 0x33, 0x80),
          "GXcopy polygon fill blended instead of replacing");
    CHECK(pixel_is_rgba(surface, 13, 3, 0x66, 0x77, 0x88, 0x80),
          "GXcopy arc fill blended instead of replacing");
    SDL_FreeSurface(surface);

    Pixmap starPixmap =
        XCreatePixmap(display, root, 32, 32, DefaultDepth(display, 0));
    CHECK(starPixmap != None, "star pixmap creation failed");
    GC starGc = XCreateGC(display, starPixmap, 0, NULL);
    CHECK(starGc, "star GC creation failed");
    CHECK(XSetForeground(display, starGc, 0xFF0000FF),
          "star background blue failed");
    CHECK(XFillRectangle(display, starPixmap, starGc, 0, 0, 32, 32),
          "star background fill failed");
    CHECK(XSetFillRule(display, starGc, EvenOddRule),
          "star even-odd setup failed");
    CHECK(XSetForeground(display, starGc, 0xFFFFFFFF),
          "star foreground white failed");
    XPoint star[] = {{16, 4},  {19, 12}, {28, 12}, {21, 17}, {24, 28},
                     {16, 22}, {8, 28},  {11, 17}, {4, 12},  {13, 12}};
    CHECK(XFillPolygon(display, starPixmap, starGc, star, 10, Nonconvex,
                       CoordModeOrigin),
          "nonconvex star fill failed");
    SDL_Renderer *starRenderer = NULL;
    GET_RENDERER(starPixmap, starRenderer);
    SDL_Surface *starSurface = getRenderSurface(starRenderer);
    CHECK(starSurface, "star surface readback failed");
    CHECK(pixel_is_rgb(starSurface, 16, 8, 255, 255, 255),
          "nonconvex star missed top arm");
    CHECK(pixel_is_rgb(starSurface, 16, 16, 255, 255, 255),
          "nonconvex star missed center");
    CHECK(pixel_is_rgb(starSurface, 5, 5, 0, 0, 255),
          "nonconvex star overfilled upper-left bounding box");
    CHECK(pixel_is_rgb(starSurface, 27, 5, 0, 0, 255),
          "nonconvex star overfilled upper-right bounding box");
    SDL_FreeSurface(starSurface);
    XFreeGC(display, starGc);
    XFreePixmap(display, starPixmap);

    Window clipTop =
        XCreateSimpleWindow(display, root, 120, 40, 24, 24, 0, 0, 0);
    CHECK(clipTop != None, "child clip top-level creation failed");
    Window clipChild =
        XCreateSimpleWindow(display, clipTop, 8, 8, 8, 8, 0, 0, 0);
    CHECK(clipChild != None, "child clip window creation failed");
    CHECK(XMapWindow(display, clipChild), "child clip child map failed");
    CHECK(XMapWindow(display, clipTop), "child clip top-level map failed");
    GC childClipGc = XCreateGC(display, clipChild, 0, NULL);
    CHECK(childClipGc != NULL, "child clip GC creation failed");
    CHECK(XSetForeground(display, childClipGc, 0xFF000000),
          "child clip black failed");
    CHECK(XFillRectangle(display, clipTop, childClipGc, 0, 0, 24, 24),
          "child clip top clear failed");
    XRectangle childClip = {.x = 0, .y = 0, .width = 4, .height = 4};
    CHECK(
        XSetClipRectangles(display, childClipGc, 0, 0, &childClip, 1, Unsorted),
        "child clip rect setup failed");
    CHECK(XSetForeground(display, childClipGc, 0xFFFF0000),
          "child clip red failed");
    CHECK(XFillRectangle(display, clipChild, childClipGc, 0, 0, 8, 8),
          "child clipped fill failed");
    XSetClipMask(display, childClipGc, None);
    SDL_Renderer *clipTopRenderer = NULL;
    GET_RENDERER(clipTop, clipTopRenderer);
    SDL_Surface *clipTopSurface = getRenderSurface(clipTopRenderer);
    CHECK(clipTopSurface != NULL, "child clip surface readback failed");
    CHECK(pixel_is_rgb(clipTopSurface, 8, 8, 255, 0, 0),
          "child clip missed drawable-local inside pixel");
    CHECK(pixel_is_rgb(clipTopSurface, 12, 8, 0, 0, 0),
          "child clip allowed outside pixel");
    SDL_FreeSurface(clipTopSurface);

    CHECK(XSetClipMask(display, childClipGc, None),
          "child copy clip reset failed");
    CHECK(XSetForeground(display, childClipGc, 0xFF000000),
          "child copy black failed");
    CHECK(XFillRectangle(display, clipTop, childClipGc, 0, 0, 24, 24),
          "child copy top reset failed");
    CHECK(XMoveResizeWindow(display, clipChild, 5, 5, 8, 8),
          "child copy geometry reset failed");
    CHECK(XSetForeground(display, childClipGc, 0xFFFF0000),
          "child copy red failed");
    CHECK(XFillRectangle(display, clipChild, childClipGc, 0, 0, 8, 4),
          "child copy top fill failed");
    CHECK(XSetForeground(display, childClipGc, 0xFF00FF00),
          "child copy green failed");
    CHECK(XFillRectangle(display, clipChild, childClipGc, 0, 4, 8, 4),
          "child copy bottom fill failed");
    CHECK(
        XCopyArea(display, clipChild, clipChild, childClipGc, 0, 4, 8, 4, 0, 0),
        "child same-window XCopyArea failed");
    GET_RENDERER(clipTop, clipTopRenderer);
    clipTopSurface = getRenderSurface(clipTopRenderer);
    CHECK(clipTopSurface != NULL, "child copy surface readback failed");
    CHECK(pixel_is_rgb(clipTopSurface, 5, 5, 0, 255, 0),
          "child same-window XCopyArea read from wrong coordinates");
    SDL_FreeSurface(clipTopSurface);

    CHECK(XSetClipMask(display, childClipGc, None),
          "child move clear clip reset failed");
    CHECK(XSetForeground(display, childClipGc, 0xFF000000),
          "child move clear black failed");
    CHECK(XFillRectangle(display, clipTop, childClipGc, 0, 0, 24, 24),
          "child move clear top reset failed");
    CHECK(XSetForeground(display, childClipGc, 0xFFFF0000),
          "child move clear red failed");
    CHECK(XFillRectangle(display, clipChild, childClipGc, 0, 0, 8, 8),
          "child move clear initial child fill failed");
    CHECK(XMoveWindow(display, clipChild, 12, 12),
          "child move clear move failed");
    GET_RENDERER(clipTop, clipTopRenderer);
    clipTopSurface = getRenderSurface(clipTopRenderer);
    CHECK(clipTopSurface != NULL, "child move clear surface readback failed");
    CHECK(pixel_is_rgb(clipTopSurface, 8, 8, 0, 0, 0),
          "moving child left stale pixels in old parent area");
    SDL_FreeSurface(clipTopSurface);

    CHECK(XSetForeground(display, childClipGc, 0xFF000000),
          "child resize gravity black failed");
    CHECK(XFillRectangle(display, clipTop, childClipGc, 0, 0, 24, 24),
          "child resize gravity top reset failed");
    CHECK(XMoveResizeWindow(display, clipChild, 4, 4, 8, 8),
          "child resize gravity reset failed");
    CHECK(XSetForeground(display, childClipGc, 0xFFFF0000),
          "child resize gravity red failed");
    CHECK(XFillRectangle(display, clipChild, childClipGc, 0, 0, 8, 8),
          "child resize gravity initial fill failed");
    CHECK(XResizeWindow(display, clipChild, 10, 10),
          "child resize gravity resize failed");
    GET_RENDERER(clipTop, clipTopRenderer);
    clipTopSurface = getRenderSurface(clipTopRenderer);
    CHECK(clipTopSurface != NULL,
          "child resize gravity surface readback failed");
    CHECK(pixel_is_rgb(clipTopSurface, 4, 4, 0, 0, 0),
          "ForgetGravity resize preserved stale child pixels");
    SDL_FreeSurface(clipTopSurface);
    XFreeGC(display, childClipGc);
    XDestroyWindow(display, clipTop);

    Pixmap pathArcPixmap =
        XCreatePixmap(display, root, 40, 40, DefaultDepth(display, 0));
    CHECK(pathArcPixmap != None, "path arc pixmap creation failed");
    GC pathArcGc = XCreateGC(display, pathArcPixmap, 0, NULL);
    CHECK(pathArcGc, "path arc GC creation failed");
    CHECK(XSetForeground(display, pathArcGc, 0xFF000000),
          "path arc black failed");
    CHECK(XFillRectangle(display, pathArcPixmap, pathArcGc, 0, 0, 40, 40),
          "path arc clear failed");
    CHECK(XSetForeground(display, pathArcGc, 0xFF00FFFF),
          "path arc cyan failed");
    CHECK(
        XFillArc(display, pathArcPixmap, pathArcGc, 8, 8, 24, 24, 0, 360 * 64),
        "large path XFillArc failed");
    CHECK(XSetForeground(display, pathArcGc, 0xFFFFFFFF),
          "path arc white failed");
    CHECK(XDrawArc(display, pathArcPixmap, pathArcGc, 2, 2, 24, 24, 0, 90 * 64),
          "large path XDrawArc failed");
    XArc overlappingArcs[] = {
        {.x = 4,
         .y = 16,
         .width = 20,
         .height = 20,
         .angle1 = 0,
         .angle2 = 360 * 64},
        {.x = 14,
         .y = 16,
         .width = 20,
         .height = 20,
         .angle1 = 0,
         .angle2 = 360 * 64},
    };
    CHECK(XSetForeground(display, pathArcGc, 0xFFFFAA00),
          "path arc orange failed");
    CHECK(XFillArcs(display, pathArcPixmap, pathArcGc, overlappingArcs, 2),
          "overlapping path XFillArcs failed");
    GET_RENDERER(pathArcPixmap, renderer);
    surface = getRenderSurface(renderer);
    CHECK(surface, "getRenderSurface for path arcs failed");
    CHECK(pixel_is_rgb(surface, 20, 12, 0, 255, 255),
          "path XFillArc did not fill center");
    CHECK(pixel_is_rgb(surface, 26, 14, 255, 255, 255),
          "path XDrawArc did not draw expected point");
    CHECK(pixel_is_rgb(surface, 19, 26, 255, 170, 0),
          "overlapping XFillArcs canceled the overlap");
    SDL_FreeSurface(surface);
    XFreeGC(display, pathArcGc);
    XFreePixmap(display, pathArcPixmap);

    Pixmap widePixmap =
        XCreatePixmap(display, root, 48, 48, DefaultDepth(display, 0));
    CHECK(widePixmap != None, "wide stroke pixmap creation failed");
    GC wideGc = XCreateGC(display, widePixmap, 0, NULL);
    CHECK(wideGc, "wide stroke GC creation failed");
    CHECK(XSetForeground(display, wideGc, 0xFF000000), "wide black failed");
    CHECK(XFillRectangle(display, widePixmap, wideGc, 0, 0, 48, 48),
          "wide clear failed");
    CHECK(XSetForeground(display, wideGc, 0xFFFF0000), "wide red failed");
    CHECK(XSetLineAttributes(display, wideGc, 6, LineSolid, CapButt, JoinMiter),
          "wide line attributes failed");
    CHECK(XDrawLine(display, widePixmap, wideGc, 4, 8, 28, 8),
          "wide XDrawLine failed");
    CHECK(XSetForeground(display, wideGc, 0xFF00FF00), "wide green failed");
    CHECK(
        XSetLineAttributes(display, wideGc, 6, LineSolid, CapRound, JoinRound),
        "round line attributes failed");
    XPoint widePoints[] = {{8, 20}, {20, 32}, {32, 20}};
    CHECK(
        XDrawLines(display, widePixmap, wideGc, widePoints, 3, CoordModeOrigin),
        "wide XDrawLines failed");
    CHECK(XSetForeground(display, wideGc, 0xFF0000FF), "wide blue failed");
    CHECK(XSetLineAttributes(display, wideGc, 6, LineSolid, CapProjecting,
                             JoinBevel),
          "projecting line attributes failed");
    CHECK(XDrawRectangle(display, widePixmap, wideGc, 34, 4, 8, 8),
          "wide XDrawRectangle failed");
    CHECK(XSetForeground(display, wideGc, 0xFFFFFF00), "wide yellow failed");
    CHECK(
        XSetLineAttributes(display, wideGc, 6, LineSolid, CapRound, JoinRound),
        "zero-length round attributes failed");
    CHECK(XDrawLine(display, widePixmap, wideGc, 8, 40, 8, 40),
          "zero-length round XDrawLine failed");
    CHECK(XSetForeground(display, wideGc, 0xFFFF00FF), "wide magenta failed");
    CHECK(XSetLineAttributes(display, wideGc, 6, LineSolid, CapProjecting,
                             JoinBevel),
          "zero-length projecting attributes failed");
    XSegment zeroSegment = {28, 40, 28, 40};
    CHECK(XDrawSegments(display, widePixmap, wideGc, &zeroSegment, 1),
          "zero-length projecting XDrawSegments failed");
    char zeroDash[] = {4, 4};
    CHECK(XSetDashes(display, wideGc, 0, zeroDash, 2),
          "zero-length dash setup failed");
    CHECK(XSetForeground(display, wideGc, 0xFF00FFFF), "wide cyan failed");
    CHECK(XSetLineAttributes(display, wideGc, 6, LineOnOffDash, CapRound,
                             JoinRound),
          "zero-length dashed round attributes failed");
    CHECK(XDrawLine(display, widePixmap, wideGc, 40, 40, 40, 40),
          "zero-length dashed round XDrawLine failed");
    GET_RENDERER(widePixmap, renderer);
    surface = getRenderSurface(renderer);
    CHECK(surface, "getRenderSurface for wide strokes failed");
    CHECK(pixel_is_rgb(surface, 12, 8, 255, 0, 0),
          "wide butt line missed center");
    CHECK(pixel_is_rgb(surface, 12, 10, 255, 0, 0),
          "wide butt line missed half-width pixel");
    CHECK(pixel_is_rgb(surface, 20, 28, 0, 255, 0),
          "wide round join missed interior");
    CHECK(pixel_is_rgb(surface, 42, 8, 0, 0, 255),
          "wide rectangle missed right edge");
    CHECK(pixel_is_rgb(surface, 8, 40, 255, 255, 0),
          "zero-length round line was dropped");
    CHECK(pixel_is_rgb(surface, 28, 40, 255, 0, 255),
          "zero-length projecting segment was dropped");
    CHECK(pixel_is_rgb(surface, 40, 40, 0, 255, 255),
          "zero-length dashed round line was dropped");
    SDL_FreeSurface(surface);
    XFreeGC(display, wideGc);
    XFreePixmap(display, widePixmap);

    Pixmap dashPixmap =
        XCreatePixmap(display, root, 32, 20, DefaultDepth(display, 0));
    CHECK(dashPixmap != None, "dash pixmap creation failed");
    GC dashGc = XCreateGC(display, dashPixmap, 0, NULL);
    CHECK(dashGc, "dash GC creation failed");
    CHECK(XSetForeground(display, dashGc, 0xFF000000), "dash black failed");
    CHECK(XFillRectangle(display, dashPixmap, dashGc, 0, 0, 32, 20),
          "dash clear failed");
    char dashList[] = {4, 4};
    CHECK(XSetDashes(display, dashGc, 0, dashList, 2), "XSetDashes failed");
    CHECK(XSetForeground(display, dashGc, 0xFFFF0000), "dash red failed");
    CHECK(XSetLineAttributes(display, dashGc, 1, LineOnOffDash, CapButt,
                             JoinMiter),
          "on-off dash attributes failed");
    CHECK(XDrawLine(display, dashPixmap, dashGc, 2, 4, 24, 4),
          "LineOnOffDash draw failed");
    CHECK(XSetForeground(display, dashGc, 0xFF00FF00), "dash green failed");
    CHECK(XSetBackground(display, dashGc, 0xFF0000FF), "dash blue failed");
    CHECK(XSetLineAttributes(display, dashGc, 1, LineDoubleDash, CapButt,
                             JoinMiter),
          "double dash attributes failed");
    CHECK(XDrawLine(display, dashPixmap, dashGc, 2, 12, 24, 12),
          "LineDoubleDash draw failed");
    GET_RENDERER(dashPixmap, renderer);
    surface = getRenderSurface(renderer);
    CHECK(surface, "getRenderSurface for dashed strokes failed");
    CHECK(pixel_is_rgb(surface, 3, 4, 255, 0, 0),
          "LineOnOffDash missed on dash");
    CHECK(pixel_is_rgb(surface, 7, 4, 0, 0, 0), "LineOnOffDash drew off dash");
    CHECK(pixel_is_rgb(surface, 3, 12, 0, 255, 0),
          "LineDoubleDash missed foreground dash");
    CHECK(pixel_is_rgb(surface, 7, 12, 0, 0, 255),
          "LineDoubleDash missed background dash");
    SDL_FreeSurface(surface);
    XFreeGC(display, dashGc);
    XFreePixmap(display, dashPixmap);

    XFreeGC(display, gc);
    XFreePixmap(display, pixmap);
    return 1;
}

/* Regression coverage for the fixes that closed out the Drawables / Pixmaps /
 * GC review pass: small-arc ArcChord routing, batched point and rectangle
 * primitives, non-GXcopy line batches, and dashed small arcs.
 */
static int test_drawing_coverage(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));

    Pixmap arcPx =
        XCreatePixmap(display, root, 24, 12, DefaultDepth(display, 0));
    CHECK(arcPx != None, "arc-mode pixmap creation failed");
    GC arcGc = XCreateGC(display, arcPx, 0, NULL);
    CHECK(arcGc, "arc-mode GC creation failed");
    CHECK(XSetForeground(display, arcGc, 0xFF000000), "arc-mode black failed");
    CHECK(XFillRectangle(display, arcPx, arcGc, 0, 0, 24, 12),
          "arc-mode clear failed");
    CHECK(XSetForeground(display, arcGc, 0xFFFF0000), "arc-mode red failed");
    /* Width 10 stays under the legacy 16-pixel cutoff. Before the fix, the
     * small-arc fallback always rendered as ArcPieSlice and would have filled
     * the center pixel for both modes. After the fix the path accelerator
     * handles ArcChord regardless of size, so the center pixel stays unfilled
     * for chord but stays filled for pie.
     */
    CHECK(XSetArcMode(display, arcGc, ArcChord), "set ArcChord failed");
    CHECK(XFillArc(display, arcPx, arcGc, 0, 1, 10, 10, 0, 90 * 64),
          "small ArcChord fill failed");
    CHECK(XSetArcMode(display, arcGc, ArcPieSlice), "set ArcPieSlice failed");
    CHECK(XFillArc(display, arcPx, arcGc, 12, 1, 10, 10, 0, 90 * 64),
          "small ArcPieSlice fill failed");
    SDL_Renderer *arcRenderer;
    GET_RENDERER(arcPx, arcRenderer);
    SDL_Surface *arcSurface = getRenderSurface(arcRenderer);
    CHECK(arcSurface, "arc-mode getRenderSurface failed");
    /* Pixel (cx+2, cy-1) relative to each arc lies inside the pie wedge
     * triangle but on the center side of the chord, i.e. inside pie and outside
     * chord. Chord arc is at (0,1); pie arc is at (12,1).
     */
    CHECK(pixel_is_rgb(arcSurface, 7, 5, 0, 0, 0),
          "small ArcChord still filled the triangle (legacy pie fallback?)");
    CHECK(pixel_is_rgb(arcSurface, 19, 5, 255, 0, 0),
          "small ArcPieSlice failed to fill the wedge triangle");
    SDL_FreeSurface(arcSurface);
    XFreeGC(display, arcGc);
    XFreePixmap(display, arcPx);

    /* XDrawPoints / XDrawRectangles were previously WARN_UNIMPLEMENTED stubs in
     * missing.c. Exercise both batched forms and verify the pixels actually
     * land.
     */
    Pixmap batchPx =
        XCreatePixmap(display, root, 16, 16, DefaultDepth(display, 0));
    CHECK(batchPx != None, "batch pixmap creation failed");
    GC batchGc = XCreateGC(display, batchPx, 0, NULL);
    CHECK(batchGc, "batch GC creation failed");
    CHECK(XSetForeground(display, batchGc, 0xFF000000), "batch black failed");
    CHECK(XFillRectangle(display, batchPx, batchGc, 0, 0, 16, 16),
          "batch clear failed");
    CHECK(XSetForeground(display, batchGc, 0xFFFFFFFF), "batch white failed");
    XPoint originPts[] = {{1, 1}, {2, 2}, {3, 3}};
    CHECK(XDrawPoints(display, batchPx, batchGc, originPts, 3, CoordModeOrigin),
          "XDrawPoints CoordModeOrigin failed");
    XPoint prevPts[] = {{10, 1}, {1, 1}, {1, 1}};
    CHECK(XDrawPoints(display, batchPx, batchGc, prevPts, 3, CoordModePrevious),
          "XDrawPoints CoordModePrevious failed");
    CHECK(XSetForeground(display, batchGc, 0xFF0000FF), "batch blue failed");
    XRectangle rects[] = {{5, 8, 3, 3}, {10, 10, 4, 4}};
    CHECK(XDrawRectangles(display, batchPx, batchGc, rects, 2),
          "XDrawRectangles failed");
    SDL_Renderer *batchRenderer;
    GET_RENDERER(batchPx, batchRenderer);
    SDL_Surface *batchSurface = getRenderSurface(batchRenderer);
    CHECK(batchSurface, "batch getRenderSurface failed");
    CHECK(pixel_is_rgb(batchSurface, 1, 1, 255, 255, 255),
          "XDrawPoints origin point 0 missing");
    CHECK(pixel_is_rgb(batchSurface, 3, 3, 255, 255, 255),
          "XDrawPoints origin point 2 missing");
    CHECK(pixel_is_rgb(batchSurface, 10, 1, 255, 255, 255),
          "XDrawPoints previous start point missing");
    CHECK(pixel_is_rgb(batchSurface, 11, 2, 255, 255, 255),
          "XDrawPoints CoordModePrevious accumulation broke");
    CHECK(pixel_is_rgb(batchSurface, 12, 3, 255, 255, 255),
          "XDrawPoints CoordModePrevious second accumulation broke");
    /* XDrawRectangles per X11 spec outlines (w+1)x(h+1): rect {5,8,3,3} has
     * corners (5,8) and (8,11); rect {10,10,4,4} has corners (10,10) and
     * (14,14). The far corner check would fail under the old SDL w-by-h
     * behavior, which only reached (13,13).
     */
    CHECK(pixel_is_rgb(batchSurface, 5, 8, 0, 0, 255),
          "XDrawRectangles first rect missing top-left corner");
    CHECK(pixel_is_rgb(batchSurface, 8, 11, 0, 0, 255),
          "XDrawRectangles first rect missing bottom-right corner");
    CHECK(pixel_is_rgb(batchSurface, 14, 14, 0, 0, 255),
          "XDrawRectangles second rect missing X11-spec bottom-right corner");
    /* Interior must stay background to confirm the call outlined, not filled.
     */
    CHECK(pixel_is_rgb(batchSurface, 6, 9, 0, 0, 0),
          "XDrawRectangles filled instead of outlined");
    CHECK(pixel_is_rgb(batchSurface, 12, 12, 0, 0, 0),
          "XDrawRectangles filled second rect instead of outlining");
    SDL_FreeSurface(batchSurface);
    XFreeGC(display, batchGc);
    XFreePixmap(display, batchPx);

    /* Non-GXcopy XDrawSegments and XDrawLines: before the fix, batched line
     * primitives fell through to plain SDL blending and silently dropped the GC
     * function. With the software walker the XOR pattern applies to every span
     * pixel.
     */
    Pixmap xorPx =
        XCreatePixmap(display, root, 16, 16, DefaultDepth(display, 0));
    CHECK(xorPx != None, "xor pixmap creation failed");
    GC xorGc = XCreateGC(display, xorPx, 0, NULL);
    CHECK(xorGc, "xor GC creation failed");
    CHECK(XSetForeground(display, xorGc, 0xFF112233), "xor base failed");
    CHECK(XFillRectangle(display, xorPx, xorGc, 0, 0, 16, 16),
          "xor clear failed");
    CHECK(XSetForeground(display, xorGc, 0xFF010203), "xor source failed");
    CHECK(XSetFunction(display, xorGc, GXxor), "xor set GXxor failed");
    XSegment segments[] = {{0, 4, 5, 4}, {8, 4, 12, 4}};
    CHECK(XDrawSegments(display, xorPx, xorGc, segments, 2),
          "GXxor XDrawSegments failed");
    XPoint linePts[] = {{0, 8}, {5, 8}, {10, 8}};
    CHECK(XDrawLines(display, xorPx, xorGc, linePts, 3, CoordModeOrigin),
          "GXxor XDrawLines failed");
    SDL_Renderer *xorRenderer;
    GET_RENDERER(xorPx, xorRenderer);
    SDL_Surface *xorSurface = getRenderSurface(xorRenderer);
    CHECK(xorSurface, "xor getRenderSurface failed");
    /* 0x11^0x01=0x10, 0x22^0x02=0x20, 0x33^0x03=0x30. */
    CHECK(pixel_is_rgb(xorSurface, 0, 4, 0x10, 0x20, 0x30),
          "GXxor XDrawSegments left endpoint not XORed");
    CHECK(pixel_is_rgb(xorSurface, 12, 4, 0x10, 0x20, 0x30),
          "GXxor XDrawSegments right endpoint not XORed");
    /* Gap between the two segments stays at the base color. */
    CHECK(pixel_is_rgb(xorSurface, 6, 4, 0x11, 0x22, 0x33),
          "GXxor XDrawSegments filled the gap between disjoint segments");
    CHECK(pixel_is_rgb(xorSurface, 0, 8, 0x10, 0x20, 0x30),
          "GXxor XDrawLines start not XORed");
    CHECK(pixel_is_rgb(xorSurface, 5, 8, 0x10, 0x20, 0x30),
          "GXxor XDrawLines join was XORed twice");
    CHECK(pixel_is_rgb(xorSurface, 10, 8, 0x10, 0x20, 0x30),
          "GXxor XDrawLines end not XORed");
    SDL_FreeSurface(xorSurface);
    XFreeGC(display, xorGc);
    XFreePixmap(display, xorPx);

    Pixmap dashSegPx =
        XCreatePixmap(display, root, 16, 16, DefaultDepth(display, 0));
    CHECK(dashSegPx != None, "dash segment pixmap creation failed");
    GC dashSegGc = XCreateGC(display, dashSegPx, 0, NULL);
    CHECK(dashSegGc, "dash segment GC creation failed");
    CHECK(XSetForeground(display, dashSegGc, 0xFF000000),
          "dash segment black failed");
    CHECK(XFillRectangle(display, dashSegPx, dashSegGc, 0, 0, 16, 16),
          "dash segment clear failed");
    /* Pattern {2, 2} with length-8 segments gives a verifiable on/off/on/off
     * sequence. The off-dash pixels (3 and 7 along each segment) must be
     * background; pass a length where only an end-pixel check would pass
     * trivially even if dashes were ignored.
     */
    char dashSegmentsPattern[] = {2, 2};
    CHECK(XSetDashes(display, dashSegGc, 0, dashSegmentsPattern, 2),
          "dash segment XSetDashes failed");
    CHECK(XSetForeground(display, dashSegGc, 0xFFFFFFFF),
          "dash segment white failed");
    CHECK(XSetLineAttributes(display, dashSegGc, 1, LineOnOffDash, CapButt,
                             JoinMiter),
          "dash segment attributes failed");
    XSegment dashSegments[] = {{0, 12, 8, 12}, {0, 14, 8, 14}};
    CHECK(XDrawSegments(display, dashSegPx, dashSegGc, dashSegments, 2),
          "dashed XDrawSegments failed");
    SDL_Renderer *dashSegRenderer;
    GET_RENDERER(dashSegPx, dashSegRenderer);
    SDL_Surface *dashSegSurface = getRenderSurface(dashSegRenderer);
    CHECK(dashSegSurface, "dash segment getRenderSurface failed");
    CHECK(pixel_is_rgb(dashSegSurface, 0, 12, 255, 255, 255),
          "dashed XDrawSegments first segment did not start on");
    CHECK(pixel_is_rgb(dashSegSurface, 3, 12, 0, 0, 0),
          "dashed XDrawSegments off-gap pixel was drawn");
    CHECK(pixel_is_rgb(dashSegSurface, 4, 12, 255, 255, 255),
          "dashed XDrawSegments second on-dash missing");
    /* Segment 2 must restart the dash pattern: the same gap at the same
     * relative offset confirms the per-segment phase reset.
     */
    CHECK(pixel_is_rgb(dashSegSurface, 0, 14, 255, 255, 255),
          "dashed XDrawSegments did not reset dash phase");
    CHECK(pixel_is_rgb(dashSegSurface, 3, 14, 0, 0, 0),
          "dashed XDrawSegments reset broke off-dash on segment 2");
    SDL_FreeSurface(dashSegSurface);
    XFreeGC(display, dashSegGc);
    XFreePixmap(display, dashSegPx);

    /* Dashed small arcs: before shouldUsePathArc gated on lineStyle, a sub-16
     * arc with LineOnOffDash routed to the legacy point spray which ignores the
     * dash list entirely. Now the dashed arc must leave visible gaps. The test
     * compares against the same arc drawn solid and requires the dashed pixel
     * count to be strictly smaller.
     */
    Pixmap dashArcPx =
        XCreatePixmap(display, root, 32, 14, DefaultDepth(display, 0));
    CHECK(dashArcPx != None, "dash-arc pixmap creation failed");
    GC dashArcGc = XCreateGC(display, dashArcPx, 0, NULL);
    CHECK(dashArcGc, "dash-arc GC creation failed");
    CHECK(XSetForeground(display, dashArcGc, 0xFF000000),
          "dash-arc black failed");
    CHECK(XFillRectangle(display, dashArcPx, dashArcGc, 0, 0, 32, 14),
          "dash-arc clear failed");
    CHECK(XSetForeground(display, dashArcGc, 0xFFFFFFFF),
          "dash-arc white failed");
    CHECK(XDrawArc(display, dashArcPx, dashArcGc, 1, 1, 12, 12, 0, 360 * 64),
          "solid small XDrawArc failed");
    char dashPattern[] = {2, 2};
    CHECK(XSetDashes(display, dashArcGc, 0, dashPattern, 2),
          "dash-arc XSetDashes failed");
    CHECK(XSetLineAttributes(display, dashArcGc, 1, LineOnOffDash, CapButt,
                             JoinMiter),
          "dash-arc XSetLineAttributes failed");
    CHECK(XDrawArc(display, dashArcPx, dashArcGc, 17, 1, 12, 12, 0, 360 * 64),
          "dashed small XDrawArc failed");
    SDL_Renderer *dashArcRenderer;
    GET_RENDERER(dashArcPx, dashArcRenderer);
    SDL_Surface *dashArcSurface = getRenderSurface(dashArcRenderer);
    CHECK(dashArcSurface, "dash-arc getRenderSurface failed");
    int solidCount = 0;
    int dashedCount = 0;
    for (int yy = 0; yy < 14; yy++) {
        for (int xx = 0; xx < 16; xx++) {
            if (pixel_is_rgb(dashArcSurface, xx, yy, 255, 255, 255))
                solidCount++;
        }
        for (int xx = 16; xx < 32; xx++) {
            if (pixel_is_rgb(dashArcSurface, xx, yy, 255, 255, 255))
                dashedCount++;
        }
    }
    CHECK(solidCount > 0, "solid small XDrawArc rendered nothing");
    CHECK(dashedCount > 0, "dashed small XDrawArc rendered nothing");
    CHECK(dashedCount < solidCount,
          "dashed XDrawArc covered as many pixels as solid (legacy fallback?)");
    SDL_FreeSurface(dashArcSurface);
    XFreeGC(display, dashArcGc);
    XFreePixmap(display, dashArcPx);

    return 1;
}

/* GXxor must be self-inverse: drawing the same primitive twice through a GXxor
 * GC with the same foreground must restore the original pixels bit-for-bit.
 * xfig's rubber-band relies on this for the drag preview: each mouse-move
 * XOR-erases the previous frame and XOR-draws the new one. If the read / modify
 * / write round-trip through SDL is not bit-exact (e.g. because SDL_RenderCopy
 * of the staging texture goes through a shader pipeline with float conversion /
 * premultiplied alpha / sRGB), the second XOR fails to cancel and we leak a
 * trail of stale rubber-band outlines on screen.
 */
static int test_gxxor_self_inverse(Display *display)
{
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    enum { TEST_W = 48, TEST_H = 32 };
    Pixmap pm = XCreatePixmap(display, root, TEST_W, TEST_H,
                              (unsigned int) DefaultDepth(display, screen));
    CHECK(pm != None, "XCreatePixmap for GXxor test failed");

    /* Solid background fill so XGetImage returns a deterministic baseline. */
    GC fill = XCreateGC(display, pm, 0, NULL);
    XSetForeground(display, fill, WhitePixel(display, screen));
    XFillRectangle(display, pm, fill, 0, 0, TEST_W, TEST_H);
    XFreeGC(display, fill);

    XImage *before =
        XGetImage(display, pm, 0, 0, TEST_W, TEST_H, AllPlanes, ZPixmap);
    CHECK(before != NULL, "XGetImage(before) failed");

    XGCValues gcv;
    gcv.function = GXxor;
    gcv.foreground = 0xFFAABBCCul;
    GC xorGc = XCreateGC(display, pm, GCFunction | GCForeground, &gcv);
    CHECK(xorGc != NULL, "XCreateGC(GXxor) failed");

    XDrawRectangle(display, pm, xorGc, 4, 4, 32, 18);
    XDrawRectangle(display, pm, xorGc, 4, 4, 32, 18);

    XImage *after =
        XGetImage(display, pm, 0, 0, TEST_W, TEST_H, AllPlanes, ZPixmap);
    CHECK(after != NULL, "XGetImage(after) failed");

    int mismatches = 0;
    unsigned long beforeSample = 0, afterSample = 0;
    int sampleX = -1, sampleY = -1;
    for (int y = 0; y < TEST_H; y++) {
        for (int x = 0; x < TEST_W; x++) {
            unsigned long b = XGetPixel(before, x, y);
            unsigned long a = XGetPixel(after, x, y);
            if (b != a) {
                if (mismatches == 0) {
                    beforeSample = b;
                    afterSample = a;
                    sampleX = x;
                    sampleY = y;
                }
                mismatches++;
            }
        }
    }
    if (mismatches) {
        fprintf(stderr,
                "GXxor self-inverse leaked %d pixels; first at (%d, %d): "
                "before=0x%08lx after=0x%08lx\n",
                mismatches, sampleX, sampleY, beforeSample, afterSample);
    }
    XDestroyImage(before);
    XDestroyImage(after);
    XFreeGC(display, xorGc);
    XFreePixmap(display, pm);
    CHECK(mismatches == 0,
          "GXxor was not self-inverse - rubber-band drag will leak trails");

    Window win = XCreateSimpleWindow(display, root, 0, 0, TEST_W, TEST_H, 0, 0,
                                     WhitePixel(display, screen));
    CHECK(win != None, "XCreateSimpleWindow for GXxor window test failed");
    XMapWindow(display, win);
    GC winGc = XCreateGC(display, win, 0, NULL);
    CHECK(winGc != NULL, "XCreateGC for GXxor window test failed");
    CHECK(XSetForeground(display, winGc, WhitePixel(display, screen)),
          "GXxor window background setup failed");
    CHECK(XSetFunction(display, winGc, GXcopy),
          "GXxor window copy setup failed");
    CHECK(XFillRectangle(display, win, winGc, 0, 0, TEST_W, TEST_H),
          "GXxor window background fill failed");

    unsigned long outline = 0x0000AA33ul;
    unsigned long background = WhitePixel(display, screen);
    CHECK(XSetForeground(display, winGc, outline),
          "GXxor window outline setup failed");
    CHECK(XDrawRectangle(display, win, winGc, 4, 4, 32, 18),
          "GXcopy initial hover rectangle failed");
    CHECK(XSetForeground(display, winGc, outline ^ background),
          "GXxor erase source setup failed");
    CHECK(XSetFunction(display, winGc, GXxor),
          "GXxor window erase setup failed");
    CHECK(XDrawRectangle(display, win, winGc, 4, 4, 32, 18),
          "GXxor hover erase failed");

    SDL_Renderer *winRenderer = NULL;
    GET_RENDERER(win, winRenderer);
    SDL_Surface *winSurface = getRenderSurface(winRenderer);
    CHECK(winSurface, "GXxor window readback failed");
    int hoverLeaks = 0;
    for (int y = 0; y < TEST_H; y++) {
        for (int x = 0; x < TEST_W; x++) {
            if (!pixel_is_rgb(winSurface, x, y, 255, 255, 255))
                hoverLeaks++;
        }
    }
    SDL_FreeSurface(winSurface);
    XFreeGC(display, winGc);
    XDestroyWindow(display, win);
    CHECK(hoverLeaks == 0,
          "GXcopy hover rectangle was not erased by the following GXxor draw");
    return 1;
}

/* The lazy readback cache must collapse a run of XGetImage calls on an
 * unchanged Pixmap into a single SDL_RenderReadPixels, and a draw between reads
 * must invalidate the cache so the next read reflects the new pixels.
 */
static int test_pixmap_readback_cache(Display *display)
{
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    enum { TEST_W = 32, TEST_H = 24, LOOPS = 8 };
    Pixmap pm = XCreatePixmap(display, root, TEST_W, TEST_H,
                              (unsigned int) DefaultDepth(display, screen));
    CHECK(pm != None, "XCreatePixmap for readback-cache test failed");

    GC gc = XCreateGC(display, pm, 0, NULL);
    XSetForeground(display, gc, WhitePixel(display, screen));
    XFillRectangle(display, pm, gc, 0, 0, TEST_W, TEST_H);

    unsigned long before = x11compat_pixmap_readback_reads;
    unsigned long whiteVal = 0;
    for (int i = 0; i < LOOPS; i++) {
        XImage *img =
            XGetImage(display, pm, 0, 0, TEST_W, TEST_H, AllPlanes, ZPixmap);
        CHECK(img != NULL, "XGetImage in readback-cache loop failed");
        unsigned long px = XGetPixel(img, 1, 1);
        if (i == 0)
            whiteVal = px;
        CHECK(px == whiteVal, "cached readback returned inconsistent pixels");
        XDestroyImage(img);
    }
    unsigned long reads = x11compat_pixmap_readback_reads - before;
    CHECK(reads == 1,
          "N XGetImage calls on an unchanged Pixmap issued more than one "
          "SDL_RenderReadPixels");

    /* A draw between reads must dirty the cache: the next read reflects it. */
    XSetForeground(display, gc, BlackPixel(display, screen));
    XFillRectangle(display, pm, gc, 0, 0, TEST_W, TEST_H);
    XImage *afterDraw =
        XGetImage(display, pm, 0, 0, TEST_W, TEST_H, AllPlanes, ZPixmap);
    CHECK(afterDraw != NULL, "XGetImage after invalidating draw failed");
    CHECK(XGetPixel(afterDraw, 1, 1) != whiteVal,
          "draw did not invalidate the pixmap readback cache");
    CHECK(x11compat_pixmap_readback_reads - before == 2,
          "a draw between reads did not force exactly one cache refresh");
    XDestroyImage(afterDraw);

    /* Test that an active clip rect on the shared renderer from a previous draw
     * does not clip or corrupt the pixmap readback cache when it gets
     * refreshed.
     */
    Window win = XCreateSimpleWindow(display, root, 0, 0, 100, 100, 0, 0, 0);
    GC winGC = XCreateGC(display, win, 0, NULL);

    Pixmap pm2 = XCreatePixmap(display, root, TEST_W, TEST_H,
                               (unsigned int) DefaultDepth(display, screen));
    GC gc2 = XCreateGC(display, pm2, 0, NULL);
    XSetForeground(display, gc2, WhitePixel(display, screen));
    XFillRectangle(display, pm2, gc2, 0, 0, TEST_W, TEST_H);

    /* Populate the cache once. */
    XImage *imgCheck =
        XGetImage(display, pm2, 0, 0, TEST_W, TEST_H, AllPlanes, ZPixmap);
    CHECK(imgCheck != NULL, "initial XGetImage failed");
    XDestroyImage(imgCheck);

    /* Force the cache to be dirty again. */
    markPixmapReadbackDirty(pm2);

    /* Set a clip rect on the window and draw to it. The renderer now has a clip
     * rect.
     */
    XRectangle clipRect = {10, 10, 5, 5};
    XSetClipRectangles(display, winGC, 0, 0, &clipRect, 1, Unsorted);
    XSetForeground(display, winGC, BlackPixel(display, screen));
    XFillRectangle(display, win, winGC, 0, 0, 100, 100);

    /* Get the image of the pixmap again. The cache will refresh. If the clip
     * rect from the window GC is still active, it will corrupt/restrict the
     * read.
     */
    XImage *imgCheck2 =
        XGetImage(display, pm2, 0, 0, TEST_W, TEST_H, AllPlanes, ZPixmap);
    CHECK(imgCheck2 != NULL, "XGetImage after window draw failed");
    unsigned long px0_0 = XGetPixel(imgCheck2, 0, 0);
    XDestroyImage(imgCheck2);

    XFreeGC(display, gc2);
    XFreePixmap(display, pm2);
    XFreeGC(display, winGC);
    XDestroyWindow(display, win);
    CHECK(px0_0 == whiteVal,
          "active clip rect corrupted cached pixmap readback");

    XFreeGC(display, gc);
    XFreePixmap(display, pm);
    return 1;
}

/* A pixmap larger than the cache pixel cap must read through the uncached
 * direct path (readPixmapRectUncached), returning correct pixels without
 * populating the persistent cache. The readback counter stays flat because that
 * path bypasses ensurePixmapReadback, which is how the test proves the size
 * guard routed here.
 */
static int test_pixmap_readback_uncached(Display *display)
{
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    /* BIG * BIG must exceed MAX_CACHED_PIXMAP_PIXELS (2048 * 2048); both sides
     * stay well under common max-texture limits.
     */
    enum { BIG = 2050 };
    Pixmap pm = XCreatePixmap(display, root, BIG, BIG,
                              (unsigned int) DefaultDepth(display, screen));
    CHECK(pm != None, "XCreatePixmap for uncached readback test failed");

    GC gc = XCreateGC(display, pm, 0, NULL);
    XSetForeground(display, gc, WhitePixel(display, screen));
    XFillRectangle(display, pm, gc, 0, 0, BIG, BIG);
    /* A distinct patch so the sub-rect read returns real drawn content. */
    XSetForeground(display, gc, BlackPixel(display, screen));
    XFillRectangle(display, pm, gc, 10, 10, 8, 8);

    unsigned long before = x11compat_pixmap_readback_reads;
    XImage *img = XGetImage(display, pm, 8, 8, 12, 12, AllPlanes, ZPixmap);
    CHECK(img != NULL, "XGetImage on large (uncached) pixmap failed");
    /* XGetImage returns the compat's canonical XColor (0xAARRGGBB), so mask off
     * the alpha byte to compare RGB. Patch (10,10) maps to (2,2) in a read that
     * starts at (8,8).
     */
    CHECK((XGetPixel(img, 2, 2) & 0x00FFFFFFul) == 0,
          "uncached readback returned the wrong pixel for the drawn patch");
    CHECK((XGetPixel(img, 0, 0) & 0x00FFFFFFul) != 0,
          "uncached readback lost the background pixel");
    XDestroyImage(img);
    CHECK(
        x11compat_pixmap_readback_reads == before,
        "large pixmap read went through the cache instead of the direct path");

    XFreeGC(display, gc);
    XFreePixmap(display, pm);
    return 1;
}

/* A colour read back from a Pixmap must round-trip through XPutImage and
 * through the compat's GET_*_FROM_COLOR accessors unchanged. Regression guard
 * for the XGetImage/XPutImage channel-order bug: XGetImage used to hand back
 * RGBA8888 words while XPutImage and the Xft glyph blend read them as canonical
 * 0xAARRGGBB, so a saturated non-grey fill (cyan) came back a byte-shifted
 * lavender and swallowed white glyphs drawn on it (issue #33). Grey hid the
 * shift, which is why the existing readback tests missed it - so fill with
 * cyan.
 */
static int test_xgetimage_color_roundtrip(Display *display)
{
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    Colormap cmap = DefaultColormap(display, screen);
    enum { W = 8, H = 8 };
    /* Compare RGB only; the pixmap has no meaningful alpha and the bug was a
     * channel shift, not an alpha change. RGB occupies the low 24 bits of the
     * compat's XColor.
     */
    const unsigned long RGB = 0x00FFFFFFul;
    Pixmap pm = XCreatePixmap(display, root, W, H,
                              (unsigned int) DefaultDepth(display, screen));
    CHECK(pm != None, "XCreatePixmap for colour round-trip test failed");

    /* Turquoise3 (xwpe's File-Manager button face): a saturated non-grey colour
     * so a byte-shifted read is visible. Grey hid the bug from other tests.
     */
    XColor col;
    col.red = 0;
    col.green = 197 * 257;
    col.blue = 205 * 257;
    col.flags = DoRed | DoGreen | DoBlue;
    CHECK(XAllocColor(display, cmap, &col), "XAllocColor(cyan) failed");

    GC gc = XCreateGC(display, pm, 0, NULL);
    XSetForeground(display, gc, col.pixel);
    XFillRectangle(display, pm, gc, 0, 0, W, H);

    XImage *img = XGetImage(display, pm, 0, 0, W, H, AllPlanes, ZPixmap);
    CHECK(img != NULL, "XGetImage for colour round-trip failed");
    CHECK((XGetPixel(img, 4, 4) & RGB) == (col.pixel & RGB),
          "XGetImage shifted the channels of a cyan Pixmap fill");

    /* The XGetImage->XPutImage software-copy path apps rely on must survive
     * too. Overwrite the Pixmap with black first so the readback below reports
     * cyan only if XPutImage actually wrote the image back, not the
     * pre-existing fill, and check XPutImage's own status so a rejected put
     * fails the test.
     */
    XSetForeground(display, gc, BlackPixel(display, screen));
    XFillRectangle(display, pm, gc, 0, 0, W, H);
    CHECK(XPutImage(display, pm, gc, img, 0, 0, 0, 0, W, H),
          "XPutImage of the read-back cyan image failed");
    XDestroyImage(img);
    XImage *again = XGetImage(display, pm, 0, 0, W, H, AllPlanes, ZPixmap);
    CHECK(again != NULL, "second XGetImage for colour round-trip failed");
    CHECK((XGetPixel(again, 4, 4) & RGB) == (col.pixel & RGB),
          "XGetImage->XPutImage round-trip corrupted a cyan fill");
    XDestroyImage(again);

    XFreeGC(display, gc);
    XFreePixmap(display, pm);
    return 1;
}

static int test_images(Display *display)
{
    CHECK(XImageByteOrder(display) == ImageByteOrder(display),
          "XImageByteOrder did not match display byte order");

#if defined(LIBX11_COMPAT_SDL3)
    SDL_Surface *scaleSrc =
        SDL_CreateRGBSurfaceWithFormat(0, 2, 1, 32, SDL_PIXELFORMAT_RGBA8888);
    SDL_Surface *scaleDst =
        SDL_CreateRGBSurfaceWithFormat(0, 4, 1, 32, SDL_PIXELFORMAT_RGBA8888);
    CHECK(scaleSrc && scaleDst, "scale-mode surface creation failed");
    putPixel(scaleSrc, 0, 0,
             SDL_MapRGBA(XC_SURFACE_FORMAT(scaleSrc), 0, 0, 0, 255));
    putPixel(scaleSrc, 1, 0,
             SDL_MapRGBA(XC_SURFACE_FORMAT(scaleSrc), 255, 255, 255, 255));
    SDL_Rect scaleDstRect = {0, 0, 4, 1};
    CHECK(SDL_BlitScaled(scaleSrc, NULL, scaleDst, &scaleDstRect) == 0,
          "SDL3 scaled blit wrapper failed");
    CHECK(pixel_is_between_black_and_white(scaleDst, 1, 0),
          "SDL3 scaled blit wrapper used nearest-neighbor sampling");
    SDL_FreeSurface(scaleDst);
    SDL_FreeSurface(scaleSrc);
#endif

    char *data = calloc(1, 16);
    CHECK(data != NULL, "image data allocation failed");
    XImage *image =
        XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                     32, ZPixmap, 0, data, 2, 2, 32, 0);
    CHECK(image != NULL, "XCreateImage failed");
    CHECK(image->bits_per_pixel == 32 && image->bytes_per_line == 8,
          "unexpected 32-bit image layout");
    CHECK(XPutPixel(image, 0, 0, 0x11223344), "XPutPixel 32-bit failed");
    CHECK(XPutPixel(image, 1, 0, 0x55667788), "XPutPixel second pixel failed");
    CHECK(XGetPixel(image, 0, 0) == 0x11223344, "XGetPixel 32-bit failed");
    CHECK(image->f.add_pixel, "XImage add_pixel function was not installed");
    CHECK(image->f.add_pixel(image, 1), "XImage add_pixel failed");
    CHECK(XGetPixel(image, 0, 0) == 0x11223345,
          "XImage add_pixel did not update first pixel");
    CHECK(XGetPixel(image, 1, 0) == 0x55667789,
          "XImage add_pixel did not update second pixel");
    CHECK(!XGetPixel(image, -1, 0), "XGetPixel accepted negative x");
    CHECK(!XGetPixel(image, 0, -1), "XGetPixel accepted negative y");
    CHECK(!XGetPixel(image, 2, 0), "XGetPixel accepted x past width");
    CHECK(!XGetPixel(image, 0, 2), "XGetPixel accepted y past height");
    XImage *subImage = XSubImage(image, 1, 0, 1, 1);
    CHECK(subImage != NULL, "XSubImage failed");
    CHECK(XGetPixel(subImage, 0, 0) == 0x55667789,
          "XSubImage copied wrong pixel");
    XDestroyImage(subImage);
    XDestroyImage(image);

    XImage *noDataImage =
        XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                     32, ZPixmap, 0, NULL, 1, 1, 32, 0);
    CHECK(noDataImage, "NULL-data XCreateImage failed");
    CHECK(!XGetPixel(noDataImage, 0, 0), "XGetPixel accepted NULL image data");
    CHECK(!XPutPixel(noDataImage, 0, 0, 1),
          "XPutPixel accepted NULL image data");
    XDestroyImage(noDataImage);

    char *byteData = calloc(1, 2);
    CHECK(byteData != NULL, "8-bit image data allocation failed");
    XImage *byteImage =
        XCreateImage(display, NULL, 8, ZPixmap, 0, byteData, 2, 1, 8, 0);
    CHECK(byteImage != NULL, "8-bit XCreateImage failed");
    CHECK(byteImage->bytes_per_line == 2, "unexpected 8-bit image stride");
    XPutPixel(byteImage, 1, 0, 0x7f);
    CHECK(XGetPixel(byteImage, 1, 0) == 0x7f, "8-bit pixel did not round-trip");
    XDestroyImage(byteImage);

    char *bitData = calloc(1, 1);
    CHECK(bitData != NULL, "1-bit image data allocation failed");
    XImage *bitImage =
        XCreateImage(display, NULL, 1, XYBitmap, 0, bitData, 3, 1, 8, 0);
    CHECK(bitImage != NULL, "1-bit XCreateImage failed");
    CHECK(bitImage->bytes_per_line == 1, "unexpected 1-bit image stride");
    XPutPixel(bitImage, 2, 0, 1);
    CHECK(XGetPixel(bitImage, 2, 0) == 1 && XGetPixel(bitImage, 1, 0) == 0,
          "1-bit pixel did not round-trip");
    XDestroyImage(bitImage);

    char *xyData = calloc(1, 2);
    CHECK(xyData, "XYPixmap image data allocation failed");
    XImage *xyImage =
        XCreateImage(display, NULL, 2, XYPixmap, 0, xyData, 4, 1, 8, 0);
    CHECK(xyImage, "XYPixmap XCreateImage failed");
    CHECK(XPutPixel(xyImage, 1, 0, 3), "XYPixmap XPutPixel failed");
    CHECK(XGetPixel(xyImage, 1, 0) == 3, "XYPixmap pixel did not round-trip");
    XDestroyImage(xyImage);

    Window firstWindow = XCreateSimpleWindow(
        display, DefaultRootWindow(display), 0, 0, 2, 2, 0, 0, 0);
    CHECK(firstWindow != None, "first XPutImage cache window creation failed");
    Window secondWindow = XCreateSimpleWindow(
        display, DefaultRootWindow(display), 0, 0, 2, 2, 0, 0, 0);
    CHECK(secondWindow != None,
          "second XPutImage cache window creation failed");
    CHECK(XMapWindow(display, firstWindow),
          "first XPutImage window map failed");
    CHECK(XMapWindow(display, secondWindow),
          "second XPutImage window map failed");
    GC imageGc = XCreateGC(display, firstWindow, 0, NULL);
    CHECK(imageGc, "XPutImage cache GC creation failed");
    XImage *nullDataImage =
        XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                     32, ZPixmap, 0, NULL, 2, 2, 32, 0);
    CHECK(nullDataImage, "NULL-data XPutImage image creation failed");
    int (*oldErrorHandler)(Display *, XErrorEvent *) =
        XSetErrorHandler(ignored_error);
    CHECK(XPutImage(display, firstWindow, imageGc, nullDataImage, 0, 0, 0, 0, 2,
                    2) != 1,
          "NULL-data XPutImage should fail with BadMatch");
    XSetErrorHandler(oldErrorHandler);
    XDestroyImage(nullDataImage);
    char *cacheData = calloc(1, 16);
    CHECK(cacheData, "XPutImage cache image data allocation failed");
    XImage *cacheImage =
        XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                     32, ZPixmap, 0, cacheData, 2, 2, 32, 0);
    CHECK(cacheImage, "XPutImage cache image creation failed");
    CHECK(XPutImage(display, firstWindow, imageGc, cacheImage, 0, 0, 0, 0, 2,
                    2) == 1,
          "first XPutImage cache draw failed");
    /* Renderer-unification: mapped top-level windows share the SCREEN renderer,
     * so XDestroyWindow no longer triggers a renderer destruction or a
     * corresponding cache invalidation. The load- bearing safety property is
     * that the cached renderer stays valid across the destroy boundary; if a
     * future change reverts to per- window renderers, the invalidate hook must
     * still fire and this assertion must be tightened back to NULL.
     */
    SDL_Renderer *firstStagingRenderer = getPutImageStagingTextureRenderer();
    CHECK(firstStagingRenderer,
          "XPutImage cache renderer not recorded after first draw");
    XDestroyWindow(display, firstWindow);
    CHECK(XPutImage(display, secondWindow, imageGc, cacheImage, 0, 0, 0, 0, 2,
                    2) == 1,
          "second XPutImage cache draw after first-window destroy failed");
    SDL_Renderer *secondStagingRenderer = getPutImageStagingTextureRenderer();
    CHECK(secondStagingRenderer,
          "XPutImage cache renderer cleared unexpectedly");
    CHECK(secondStagingRenderer == firstStagingRenderer,
          "shared SCREEN renderer should survive window destroy");
    XImage *windowImage =
        XGetImage(display, secondWindow, 0, 0, 2, 2, AllPlanes, ZPixmap);
    CHECK(windowImage, "XGetImage on window failed");
    CHECK(windowImage->depth == DefaultDepth(display, DefaultScreen(display)),
          "XGetImage window used the wrong depth");
    XDestroyImage(windowImage);

    Pixmap imagePixmap =
        XCreatePixmap(display, secondWindow, 2, 2,
                      DefaultDepth(display, DefaultScreen(display)));
    CHECK(imagePixmap != None, "XGetImage pixmap creation failed");
    XSetForeground(display, imageGc, 0xFFFFFFFF);
    CHECK(XDrawPoint(display, imagePixmap, imageGc, 0, 0),
          "XGetImage pixmap draw failed");
    XImage *pixmapImage =
        XGetImage(display, imagePixmap, 0, 0, 2, 2, AllPlanes, ZPixmap);
    CHECK(pixmapImage, "XGetImage on pixmap failed");
    CHECK(pixmapImage->depth == DefaultDepth(display, DefaultScreen(display)),
          "XGetImage pixmap used the wrong depth");
    CHECK(XGetPixel(pixmapImage, 0, 0), "XGetImage pixmap missed drawn pixel");
    XDestroyImage(pixmapImage);
    XImage *xyReadback =
        XGetImage(display, imagePixmap, 0, 0, 2, 2, AllPlanes, XYPixmap);
    CHECK(xyReadback, "XGetImage XYPixmap readback failed");
    CHECK(XGetPixel(xyReadback, 0, 0),
          "XGetImage XYPixmap returned a zero-filled image");
    XDestroyImage(xyReadback);

    /* Verify the ZPixmap 32-bit fast path promotes X11 pixel colors (upper byte
     * == 0) to opaque alpha so SDL2 renders them visible. Without the alpha
     * promotion the entire surface would read as fully transparent.
     */
    char *alphaTestData = calloc(1, 16);
    CHECK(alphaTestData, "alpha-promotion image data allocation failed");
    XImage *alphaImage =
        XCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                     32, ZPixmap, 0, alphaTestData, 2, 2, 32, 0);
    CHECK(alphaImage, "alpha-promotion XCreateImage failed");
    XPutPixel(alphaImage, 0, 0, 0x00FFFFFF);
    XPutPixel(alphaImage, 1, 0, 0x00000000);
    Pixmap alphaPixmap =
        XCreatePixmap(display, secondWindow, 2, 1,
                      DefaultDepth(display, DefaultScreen(display)));
    CHECK(alphaPixmap != None, "alpha-promotion pixmap creation failed");
    CHECK(XPutImage(display, alphaPixmap, imageGc, alphaImage, 0, 0, 0, 0, 2,
                    1) == 1,
          "ZPixmap XPutImage with X11 pixel colors failed");
    SDL_Renderer *alphaRenderer = NULL;
    GET_RENDERER(alphaPixmap, alphaRenderer);
    SDL_Surface *alphaSurface = getRenderSurface(alphaRenderer);
    CHECK(alphaSurface, "alpha-promotion render surface unavailable");
    CHECK(pixel_is_rgb(alphaSurface, 0, 0, 255, 255, 255),
          "ZPixmap XPutImage treated alpha-less white as transparent");
    CHECK(pixel_is_rgb(alphaSurface, 1, 0, 0, 0, 0),
          "ZPixmap XPutImage treated alpha-less black as transparent");
    SDL_FreeSurface(alphaSurface);
    XFreePixmap(display, alphaPixmap);
    XDestroyImage(alphaImage);

    XSetWindowAttributes inputAttrs;
    memset(&inputAttrs, 0, sizeof(inputAttrs));
    Window inputOnly =
        XCreateWindow(display, DefaultRootWindow(display), 0, 0, 2, 2, 0, 0,
                      InputOnly, CopyFromParent, 0, &inputAttrs);
    CHECK(inputOnly != None, "InputOnly window creation failed");
    oldErrorHandler = XSetErrorHandler(ignored_error);
    CHECK(!XGetImage(display, inputOnly, 0, 0, 1, 1, AllPlanes, ZPixmap),
          "XGetImage accepted an InputOnly window");
    CHECK(XPutImage(display, inputOnly, imageGc, cacheImage, 0, 0, 0, 0, 1,
                    1) != 1,
          "XPutImage accepted an InputOnly window");
    XSetErrorHandler(oldErrorHandler);
    XDestroyWindow(display, inputOnly);
    XFreePixmap(display, imagePixmap);

    XDestroyImage(cacheImage);
    XFreeGC(display, imageGc);
    XDestroyWindow(display, secondWindow);
    return 1;
}

static int test_path_accelerator(Display *display)
{
    (void) display;

    Path cubic;
    CHECK(pathInit(&cubic), "pathInit failed");
    CHECK(pathMoveTo(&cubic, 0.0, 0.0), "pathMoveTo failed");
    CHECK(pathCubicTo(&cubic, 0.0, 10.0, 10.0, 10.0, 10.0, 0.0),
          "pathCubicTo failed");
    PathPoint *points = NULL;
    size_t count = 0;
    CHECK(pathFlatten(&cubic, 0.25, &points, &count), "pathFlatten failed");
    CHECK(count > 2, "cubic flattened to too few points");
    for (size_t i = 0; i < count; i++) {
        CHECK(points[i].x >= -0.001 && points[i].x <= 10.001,
              "flattened x left expected bounds");
        CHECK(points[i].y >= -0.001 && points[i].y <= 10.001,
              "flattened y left expected bounds");
    }
    free(points);
    pathFree(&cubic);

    Path cusp;
    CHECK(pathInit(&cusp), "cusp pathInit failed");
    CHECK(pathMoveTo(&cusp, 0.0, 0.0), "cusp pathMoveTo failed");
    CHECK(pathCubicTo(&cusp, 1000.0, 0.001, -1000.0, -0.001, 1.0, 0.0),
          "cusp pathCubicTo failed");
    points = NULL;
    count = 0;
    CHECK(pathFlatten(&cusp, 0.25, &points, &count),
          "near-cusp cubic should flatten with default tolerance");
    CHECK(count < 65536, "near-cusp cubic exceeded output cap");
    free(points);
    pathFree(&cusp);

    Path overdeep;
    CHECK(pathInit(&overdeep), "overdeep pathInit failed");
    CHECK(pathMoveTo(&overdeep, 0.0, 0.0), "overdeep pathMoveTo failed");
    CHECK(pathCubicTo(&overdeep, 0.0, 100.0, 100.0, -100.0, 100.0, 0.0),
          "overdeep pathCubicTo failed");
    points = NULL;
    count = 0;
    CHECK(!pathFlatten(&overdeep, 1e-30, &points, &count),
          "overdeep flatten unexpectedly succeeded");
    CHECK(points == NULL && count == 0, "failed flatten returned output");
    pathFree(&overdeep);

    Path pie;
    CHECK(pathInit(&pie), "pie pathInit failed");
    CHECK(pathAddArc(&pie, 10.0, 10.0, 5.0, 5.0, 0.0, M_PI / 2.0, ArcPieSlice),
          "pie pathAddArc failed");
    CHECK(pie.commandCount >= 4, "pie arc emitted too few commands");
    CHECK(pie.commands[0] == PATH_CMD_MOVE && pie.commands[1] == PATH_CMD_LINE,
          "pie arc did not start through center");
    CHECK(pie.commands[pie.commandCount - 1] == PATH_CMD_CLOSE,
          "pie arc did not close");
    pathFree(&pie);

    Path chord;
    CHECK(pathInit(&chord), "chord pathInit failed");
    CHECK(pathAddArc(&chord, 10.0, 10.0, 5.0, 5.0, 0.0, M_PI / 2.0, ArcChord),
          "chord pathAddArc failed");
    CHECK(chord.commandCount >= 3, "chord arc emitted too few commands");
    CHECK(chord.commands[0] == PATH_CMD_MOVE &&
              chord.commands[1] == PATH_CMD_CUBIC,
          "chord arc should connect endpoints directly");
    CHECK(chord.commands[chord.commandCount - 1] == PATH_CMD_CLOSE,
          "chord arc did not close");
    pathFree(&chord);

    Uint32 buffer[4] = {0};
    PathSpan composeSpans[] = {
        {.y = 0, .xStart = 0, .xEnd = 1, .coverage = 255},
        {.y = 1, .xStart = 1, .xEnd = 1, .coverage = 255},
    };
    PathSpanList composeList = {
        .spans = composeSpans,
        .count = 2,
        .capacity = 2,
    };
    CHECK(pathComposeSpansToBuffer(buffer, 2, 2, &composeList, 0xFF112233),
          "pathComposeSpansToBuffer failed");
    CHECK(buffer[0] == 0x112233FF && buffer[1] == 0x112233FF,
          "compose did not write RGBA8888 pixels");
    CHECK(buffer[2] == 0 && buffer[3] == 0x112233FF,
          "compose wrote the wrong span locations");

    PathPoint huge[] = {
        {.x = 0.0, .y = 0.0},
        {.x = 100000000000.0, .y = 1.0},
    };
    PathEdgeList edges;
    CHECK(!pathBuildEdges(huge, 2, &edges),
          "pathBuildEdges accepted out-of-range fixed coordinates");
    CHECK(edges.edges == NULL && edges.count == 0,
          "failed edge build leaked partial storage");

    Uint32 pixel = 0;
    PathSpan singleSpan = {.y = 0, .xStart = 0, .xEnd = 0, .coverage = 255};
    PathSpanList singleSpanList = {
        .spans = &singleSpan,
        .count = 1,
        .capacity = 1,
    };
    CHECK(!pathComposeSpansToBuffer(&pixel, -1, 1, &singleSpanList, 0xFFFFFFFF),
          "compose accepted negative width");
    CHECK(!pathComposeSpansToBuffer(NULL, 1, 1, &singleSpanList, 0xFFFFFFFF),
          "compose accepted NULL buffer");
    return 1;
}

static int test_regions(Display *display)
{
    (void) display;
    Region a = XCreateRegion();
    Region b = XCreateRegion();
    Region out = XCreateRegion();
    CHECK(a != NULL && b != NULL && out != NULL, "XCreateRegion failed");

    XRectangle rectA = {0, 0, 10, 10};
    XRectangle rectB = {5, 0, 10, 10};
    CHECK(XUnionRectWithRegion(&rectA, a, a), "XUnionRectWithRegion A failed");
    CHECK(XUnionRectWithRegion(&rectB, b, b), "XUnionRectWithRegion B failed");
    CHECK(!XEmptyRegion(a), "region unexpectedly empty");
    CHECK(XPointInRegion(a, 2, 2), "XPointInRegion missed included point");
    CHECK(!XPointInRegion(a, 20, 20), "XPointInRegion accepted excluded point");
    CHECK(XRectInRegion(a, 1, 1, 2, 2) == RectangleIn,
          "XRectInRegion in failed");
    CHECK(XRectInRegion(a, 8, 8, 8, 8) == RectanglePart,
          "XRectInRegion part failed");
    CHECK(XRectInRegion(a, 20, 20, 1, 1) == RectangleOut,
          "XRectInRegion out failed");

    CHECK(XUnionRegion(a, b, out), "XUnionRegion failed");
    XRectangle clip;
    CHECK(XClipBox(out, &clip), "XClipBox failed");
    CHECK(clip.x == 0 && clip.y == 0 && clip.width == 15 && clip.height == 10,
          "XUnionRegion clip box wrong");

    CHECK(XIntersectRegion(a, b, out), "XIntersectRegion failed");
    CHECK(XClipBox(out, &clip), "XClipBox intersect failed");
    CHECK(clip.x == 5 && clip.y == 0 && clip.width == 5 && clip.height == 10,
          "XIntersectRegion clip box wrong");

    CHECK(XSubtractRegion(a, b, out), "XSubtractRegion failed");
    CHECK(XClipBox(out, &clip), "XClipBox subtract failed");
    CHECK(clip.x == 0 && clip.y == 0 && clip.width == 5 && clip.height == 10,
          "XSubtractRegion clip box wrong");

    CHECK(XXorRegion(a, b, out), "XXorRegion failed");
    CHECK(XPointInRegion(out, 2, 2) && !XPointInRegion(out, 7, 2) &&
              XPointInRegion(out, 12, 2),
          "XXorRegion membership wrong");

    CHECK(XOffsetRegion(out, 10, 5), "XOffsetRegion failed");
    CHECK(XPointInRegion(out, 12, 7), "XOffsetRegion did not move region");
    CHECK(XShrinkRegion(out, 1, 1), "XShrinkRegion failed");
    CHECK(!XPointInRegion(out, 10, 5), "XShrinkRegion did not shrink edge");

    Region copy = XCreateRegion();
    CHECK(copy != NULL, "copy XCreateRegion failed");
    CHECK(XUnionRegion(a, a, copy), "copy via XUnionRegion failed");
    CHECK(XEqualRegion(a, copy), "XEqualRegion failed for identical regions");
    CHECK(!XEqualRegion(a, b), "XEqualRegion accepted different regions");

    XPoint triangle[] = {{0, 0}, {6, 0}, {0, 6}};
    Region polygon = XPolygonRegion(triangle, 3, EvenOddRule);
    CHECK(polygon != NULL, "XPolygonRegion failed");
    CHECK(XPointInRegion(polygon, 1, 1),
          "XPolygonRegion missed interior point");
    CHECK(!XPointInRegion(polygon, 5, 5),
          "XPolygonRegion accepted exterior point");
    XDestroyRegion(polygon);

    XDestroyRegion(copy);
    XDestroyRegion(out);
    XDestroyRegion(b);
    XDestroyRegion(a);
    return 1;
}

static int test_events(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 0, 0, 32, 32, 0, 0, 0);
    CHECK(window != None, "XCreateSimpleWindow returned None");

    XSelectInput(display, window, ExposureMask);
    XMapWindow(display, window);
    SDL_Renderer *renderer;
    GET_RENDERER(window, renderer);
    CHECK(renderer, "mapped window renderer was not available");
    XEvent out;
    int mapExposeCount = 0;
    while (XCheckWindowEvent(display, window, ExposureMask, &out)) {
        CHECK(out.type == Expose && out.xexpose.count == 0,
              "map Expose had unexpected type/count");
        mapExposeCount++;
    }
    CHECK(mapExposeCount == 1, "XMapWindow posted duplicate Expose events");
    while (XCheckWindowEvent(display, window, ExposureMask, &out)) {
    }
    CHECK(XClearArea(display, window, 3, 4, 0, 0, True), "XClearArea failed");
    CHECK(XCheckWindowEvent(display, window, ExposureMask, &out),
          "XClearArea did not generate Expose");
    CHECK(out.xexpose.x == 3 && out.xexpose.y == 4 && out.xexpose.width == 29 &&
              out.xexpose.height == 28,
          "XClearArea Expose rectangle was incorrect");
    Window clipChild =
        XCreateSimpleWindow(display, window, 10, 10, 8, 8, 0, 0, 0);
    CHECK(clipChild != None, "exposure clipping child creation failed");
    XSelectInput(display, clipChild, ExposureMask);
    CHECK(XMapWindow(display, clipChild), "exposure clipping child map failed");
    while (XCheckTypedWindowEvent(display, clipChild, Expose, &out)) {
    }
    CHECK(XClearArea(display, window, 12, 13, 10, 10, True),
          "XClearArea clipping case failed");
    CHECK(XCheckTypedWindowEvent(display, clipChild, Expose, &out),
          "child did not receive clipped Expose");
    CHECK(out.xexpose.x == 2 && out.xexpose.y == 3 && out.xexpose.width == 6 &&
              out.xexpose.height == 5,
          "child Expose was not clipped to child-local coordinates");
    while (XCheckTypedWindowEvent(display, clipChild, Expose, &out)) {
    }
    while (XCheckTypedWindowEvent(display, window, Expose, &out)) {
    }

    Window restackLower =
        XCreateSimpleWindow(display, window, 1, 1, 10, 10, 0, 0, 0);
    CHECK(restackLower != None, "restack lower child creation failed");
    Window restackUpper =
        XCreateSimpleWindow(display, window, 5, 5, 10, 10, 0, 0, 0);
    CHECK(restackUpper != None, "restack upper child creation failed");
    XSelectInput(display, restackLower, ExposureMask);
    CHECK(XMapWindow(display, restackLower), "restack lower child map failed");
    CHECK(XMapWindow(display, restackUpper), "restack upper child map failed");
    while (XCheckTypedWindowEvent(display, restackLower, Expose, &out)) {
    }
    GC restackGc = XCreateGC(display, restackLower, 0, NULL);
    CHECK(restackGc != NULL, "restack GC creation failed");
    CHECK(XDrawLine(display, restackLower, restackGc, 0, 0, 9, 9),
          "covered lower child draw failed");
    CHECK(XRaiseWindow(display, restackLower), "restack raise failed");
    CHECK(XCheckTypedWindowEvent(display, restackLower, Expose, &out),
          "restack did not expose newly uncovered child pixels");
    CHECK(out.xexpose.x == 4 && out.xexpose.y == 4 && out.xexpose.width == 6 &&
              out.xexpose.height == 6,
          "restack Expose rectangle was not newly uncovered child area");
    while (XCheckTypedWindowEvent(display, restackLower, Expose, &out)) {
    }
    XFreeGC(display, restackGc);
    CHECK(XDestroyWindow(display, restackUpper),
          "restack upper child destroy failed");
    CHECK(XDestroyWindow(display, restackLower),
          "restack lower child destroy failed");
    while (XCheckTypedEvent(display, Expose, &out)) {
    }
    /* Drain any other events the window setup left queued (async map/configure
     * notifications from the shared SDL queue) so the ordering assertion below
     * sees exactly the send-event followed by the generated Expose, not a stale
     * event that happens to sit at the head.
     */
    while (XPending(display) > 0)
        XNextEvent(display, &out);

    /* XSendEvent's contract is sizeof(XEvent), so the buffer must be full-union
     * sized; passing a bare XClientMessageEvent triggers a
     * stack-buffer-overflow under ASan.
     */
    XEvent olderClient;
    memset(&olderClient, 0, sizeof(olderClient));
    olderClient.xclient.type = ClientMessage;
    olderClient.xclient.display = display;
    olderClient.xclient.window = window;
    olderClient.xclient.message_type = XA_STRING;
    olderClient.xclient.format = 32;
    olderClient.xclient.data.l[0] = 0x1234;
    CHECK(XSendEvent(display, window, False, NoEventMask, &olderClient),
          "event-order ClientMessage send failed");
    CHECK(XClearArea(display, window, 21, 22, 5, 6, True),
          "event-order XClearArea failed");
    XNextEvent(display, &out);
    CHECK(out.type == ClientMessage && out.xany.window == window,
          "generated Expose jumped ahead of older queued SDL event");
    XNextEvent(display, &out);
    CHECK(out.type == Expose && out.xany.window == window &&
              out.xexpose.x == 21 && out.xexpose.y == 22,
          "generated Expose was not delivered after older SDL event");

    GC parentDrawGc = XCreateGC(display, window, 0, NULL);
    CHECK(parentDrawGc != NULL, "parent child-overlap GC creation failed");
    CHECK(XDrawLine(display, window, parentDrawGc, 0, 14, 31, 14),
          "parent draw across child failed");
    CHECK(XCheckTypedWindowEvent(display, clipChild, Expose, &out),
          "parent drawing across mapped child did not schedule child repaint");
    CHECK(out.xexpose.x == 0 && out.xexpose.y == 3 && out.xexpose.width == 8 &&
              out.xexpose.height == 3,
          "child repaint damage from parent draw was not child-local");
    while (XCheckTypedWindowEvent(display, clipChild, Expose, &out)) {
    }

    Window unmaskedChild =
        XCreateSimpleWindow(display, window, 4, 4, 20, 20, 0, 0, 0);
    CHECK(unmaskedChild != None, "unmasked exposure child creation failed");
    Window nestedExposeChild =
        XCreateSimpleWindow(display, unmaskedChild, 6, 6, 8, 8, 0, 0, 0);
    CHECK(nestedExposeChild != None, "nested exposure child creation failed");
    XSelectInput(display, nestedExposeChild, ExposureMask);
    CHECK(XMapWindow(display, nestedExposeChild),
          "nested exposure child map failed");
    CHECK(XMapWindow(display, unmaskedChild),
          "unmasked exposure child map failed");
    while (XCheckTypedWindowEvent(display, nestedExposeChild, Expose, &out)) {
    }
    CHECK(XDrawLine(display, window, parentDrawGc, 0, 12, 31, 12),
          "parent draw across nested child failed");
    CHECK(XCheckTypedWindowEvent(display, nestedExposeChild, Expose, &out),
          "nested child did not receive repaint through unmasked parent");
    CHECK(out.xexpose.x == 0 && out.xexpose.y == 1 && out.xexpose.width == 8 &&
              out.xexpose.height == 3,
          "nested child repaint damage was not child-local");
    while (XCheckTypedWindowEvent(display, nestedExposeChild, Expose, &out)) {
    }
    while (XCheckTypedWindowEvent(display, clipChild, Expose, &out)) {
    }
    CHECK(XDestroyWindow(display, unmaskedChild),
          "unmasked exposure child destroy failed");
    while (XCheckTypedEvent(display, Expose, &out)) {
    }

    XPoint framePolyline[] = {
        {0, 9},
        {31, 9},
        {31, 14},
        {0, 14},
    };
    CHECK(XDrawLines(display, window, parentDrawGc, framePolyline,
                     (int) (sizeof(framePolyline) / sizeof(framePolyline[0])),
                     CoordModeOrigin),
          "parent polyline across child failed");
    CHECK(XCheckTypedWindowEvent(display, clipChild, Expose, &out),
          "parent polyline across mapped child did not schedule child repaint");
    CHECK(out.xexpose.x == 0 && out.xexpose.y == 0 && out.xexpose.width == 8 &&
              out.xexpose.height == 6,
          "child repaint damage from parent polyline was not child-local");
    while (XCheckTypedWindowEvent(display, clipChild, Expose, &out)) {
    }
    XFreeGC(display, parentDrawGc);

    Window offsetWindow =
        XCreateSimpleWindow(display, root, 70, 80, 30, 30, 0, 0, 0);
    CHECK(offsetWindow != None, "offset exposure parent creation failed");
    Window offsetChild =
        XCreateSimpleWindow(display, offsetWindow, 4, 5, 10, 10, 0, 0, 0);
    CHECK(offsetChild != None, "offset exposure child creation failed");
    XSelectInput(display, offsetChild, ExposureMask);
    CHECK(XMapWindow(display, offsetChild),
          "offset exposure child map request failed");
    CHECK(XMapWindow(display, offsetWindow),
          "offset exposure parent map failed");
    CHECK(XCheckTypedWindowEvent(display, offsetChild, Expose, &out),
          "offset top-level map did not expose mapped child");
    CHECK(out.xexpose.x == 0 && out.xexpose.y == 0 && out.xexpose.width == 10 &&
              out.xexpose.height == 10,
          "offset child Expose was not child-local");
    while (XCheckTypedWindowEvent(display, offsetChild, Expose, &out)) {
    }

    int translatedX = -1;
    int translatedY = -1;
    Window translatedChild = None;
    CHECK(XTranslateCoordinates(display, offsetWindow, offsetWindow, 5, 16,
                                &translatedX, &translatedY, &translatedChild),
          "XTranslateCoordinates same-window failed");
    CHECK(translatedX == 5 && translatedY == 16,
          "XTranslateCoordinates same-window changed coordinates");
    CHECK(translatedChild == None,
          "XTranslateCoordinates returned child outside y bounds");
    CHECK(XTranslateCoordinates(display, offsetWindow, offsetWindow, 5, 6,
                                &translatedX, &translatedY, &translatedChild),
          "XTranslateCoordinates child lookup failed");
    CHECK(translatedX == 5 && translatedY == 6,
          "XTranslateCoordinates child lookup changed coordinates");
    CHECK(translatedChild == offsetChild,
          "XTranslateCoordinates did not return containing child");
    CHECK(XTranslateCoordinates(display, offsetChild, root, 2, 3, &translatedX,
                                &translatedY, &translatedChild),
          "XTranslateCoordinates child-to-root failed");
    CHECK(translatedX == 76 && translatedY == 88,
          "XTranslateCoordinates child-to-root used wrong absolute origin");
    CHECK(XTranslateCoordinates(display, root, offsetChild, 76, 88,
                                &translatedX, &translatedY, &translatedChild),
          "XTranslateCoordinates root-to-child failed");
    CHECK(translatedX == 2 && translatedY == 3,
          "XTranslateCoordinates root-to-child used wrong relative origin");

    Window lowerOverlap =
        XCreateSimpleWindow(display, offsetWindow, 6, 6, 8, 8, 0, 0, 0);
    CHECK(lowerOverlap != None, "lower overlap child creation failed");
    Window upperOverlap =
        XCreateSimpleWindow(display, offsetWindow, 8, 8, 8, 8, 0, 0, 0);
    CHECK(upperOverlap != None, "upper overlap child creation failed");
    CHECK(XMapWindow(display, lowerOverlap), "lower overlap child map failed");
    CHECK(XMapWindow(display, upperOverlap), "upper overlap child map failed");
    CHECK(XTranslateCoordinates(display, offsetWindow, offsetWindow, 9, 9,
                                &translatedX, &translatedY, &translatedChild),
          "XTranslateCoordinates overlap lookup failed");
    CHECK(translatedChild == upperOverlap,
          "XTranslateCoordinates did not return topmost child");
    XUnmapWindow(display, upperOverlap);
    CHECK(XTranslateCoordinates(display, offsetWindow, offsetWindow, 9, 9,
                                &translatedX, &translatedY, &translatedChild),
          "XTranslateCoordinates unmapped overlap lookup failed");
    CHECK(translatedChild == lowerOverlap,
          "XTranslateCoordinates returned an unmapped child");
    /* Tear the offsetWindow subtree down before the rest of the test reuses
     * `window` for unrelated checks; XDestroyWindow recursively frees
     * offsetChild, lowerOverlap, and upperOverlap.
     */
    XDestroyWindow(display, offsetWindow);

    Pixmap backgroundPixmap = XCreatePixmap(
        display, window, 4, 4, DefaultDepth(display, DefaultScreen(display)));
    CHECK(backgroundPixmap != None, "XCreatePixmap for background failed");
    CHECK(XSetWindowBackgroundPixmap(display, window, backgroundPixmap),
          "XSetWindowBackgroundPixmap failed");
    CHECK(XSetWindowBackgroundPixmap(display, window, (Pixmap) ParentRelative),
          "XSetWindowBackgroundPixmap ParentRelative failed");
    CHECK(XSetWindowBackgroundPixmap(display, window, None),
          "XSetWindowBackgroundPixmap None failed");
    while (XCheckTypedEvent(display, Expose, &out)) {
    }

    XEvent expose = make_event(Expose, window);
    XEvent client = make_event(ClientMessage, window);
    XSelectInput(display, window, NoEventMask);
    CHECK(XSendEvent(display, window, False, ExposureMask, &expose),
          "XSendEvent rejected unmatched Expose");
    CHECK(!XCheckTypedEvent(display, Expose, &out),
          "XSendEvent delivered Expose without selected ExposureMask");

    XSelectInput(display, window, ExposureMask);
    XSendEvent(display, window, False, ExposureMask, &expose);
    XSendEvent(display, window, False, 0, &client);

    CHECK(XCheckTypedEvent(display, ClientMessage, &out),
          "XCheckTypedEvent did not find ClientMessage");
    CHECK(out.type == ClientMessage && out.xany.window == window,
          "unexpected typed event");

    CHECK(XCheckWindowEvent(display, window, ExposureMask, &out),
          "XCheckWindowEvent did not preserve Expose");
    CHECK(out.type == Expose && out.xany.window == window,
          "unexpected window event");

    uint64_t exposeTaps = timelineCounter(TIMELINE_KIND_EXPOSE);
    XSendEvent(display, window, False, ExposureMask, &expose);
    XSendEvent(display, window, False, 0, &client);
    CHECK(XCheckTypedEvent(display, ClientMessage, &out),
          "XCheckTypedEvent did not find ClientMessage after Expose");
    CHECK(timelineCounter(TIMELINE_KIND_EXPOSE) == exposeTaps + 1,
          "deferred converted Expose was not tapped once");
    XNextEvent(display, &out);
    CHECK(out.type == Expose && out.xany.window == window,
          "XNextEvent did not preserve deferred Expose");
    CHECK(timelineCounter(TIMELINE_KIND_EXPOSE) == exposeTaps + 1,
          "deferred converted Expose was tapped twice");

    Window ignoredChild =
        XCreateSimpleWindow(display, window, 0, 0, 8, 8, 0, 0, 0);
    CHECK(ignoredChild != None, "child creation failed");
    CHECK(!XCheckTypedEvent(display, CreateNotify, &out),
          "CreateNotify was delivered without SubstructureNotifyMask");

    XSelectInput(display, window, ExposureMask | SubstructureNotifyMask);
    Window notifiedChild =
        XCreateSimpleWindow(display, window, 0, 0, 8, 8, 0, 0, 0);
    CHECK(notifiedChild != None, "notified child creation failed");
    CHECK(XCheckTypedWindowEvent(display, window, CreateNotify, &out),
          "CreateNotify was not delivered with SubstructureNotifyMask");
    CHECK(out.xcreatewindow.parent == window &&
              out.xcreatewindow.window == notifiedChild,
          "CreateNotify fields were incorrect");

    XSendEvent(display, window, False, ExposureMask, &expose);
    int queuedBeforePutBack = XEventsQueued(display, QueuedAlready);
    CHECK(XPutBackEvent(display, &client) == 0, "XPutBackEvent failed");
    CHECK(XEventsQueued(display, QueuedAlready) == queuedBeforePutBack + 1,
          "QueuedAlready double-counted put-back event");
    CHECK(XPending(display) > 0, "XPending did not see put-back event");
    XNextEvent(display, &out);
    CHECK(out.type == ClientMessage && out.xany.window == window,
          "XPutBackEvent did not place event at the head");
    XNextEvent(display, &out);
    CHECK(out.type == Expose && out.xany.window == window,
          "XPutBackEvent disturbed queued event order");

    XSendEvent(display, window, False, 0, &client);
    XNextEvent(display, &out);
    CHECK(out.type == ClientMessage && out.xany.window == window,
          "XNextEvent did not drain normal event after empty put-back check");
    CHECK(XPutBackEvent(display, &client) == 0,
          "XPutBackEvent deadlocked after empty put-back pop");
    XNextEvent(display, &out);
    CHECK(out.type == ClientMessage && out.xany.window == window,
          "post-empty-pop XPutBackEvent was not readable");

    XSendEvent(display, window, False, ExposureMask, &expose);
    CHECK(XCheckMaskEvent(display, ExposureMask, &out),
          "XCheckMaskEvent did not find Expose");
    CHECK(out.type == Expose && out.xany.window == window,
          "XCheckMaskEvent returned unexpected event");

    XSendEvent(display, window, False, ExposureMask, &expose);
    CHECK(XPeekEvent(display, &out) == 0, "XPeekEvent failed");
    CHECK(out.type == Expose && out.xany.window == window,
          "XPeekEvent returned unexpected event");
    CHECK(XCheckMaskEvent(display, ExposureMask, &out),
          "XPeekEvent removed the event");

    XSendEvent(display, window, False, ExposureMask, &expose);
    CHECK(XMaskEvent(display, ExposureMask, &out) == 0, "XMaskEvent failed");
    CHECK(out.type == Expose && out.xany.window == window,
          "XMaskEvent returned unexpected event");

    Window resetWindow =
        XCreateSimpleWindow(display, root, 40, 0, 24, 24, 0, 0, 0);
    CHECK(resetWindow != None, "reset coverage window creation failed");
    XSelectInput(display, window, ExposureMask | PointerMotionMask);
    XSelectInput(display, resetWindow, ExposureMask);
    CHECK(XMapWindow(display, resetWindow), "reset coverage map failed");
    while (XCheckTypedWindowEvent(display, window, Expose, &out)) {
    }
    while (XCheckTypedWindowEvent(display, resetWindow, Expose, &out)) {
    }
    SDL_Event resetEvent;
    SDL_zero(resetEvent);
    resetEvent.type = SDL_RENDER_DEVICE_RESET;
    SDL_PushEvent(&resetEvent);
    SDL_Event motionEvent;
    SDL_zero(motionEvent);
    motionEvent.type = SDL_MOUSEMOTION;
    motionEvent.motion.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    motionEvent.motion.x = 3;
    motionEvent.motion.y = 4;
    SDL_PushEvent(&motionEvent);
    XNextEvent(display, &out);
    CHECK(out.type == Expose,
          "reset-induced Expose was not prioritized before normal input");
    CHECK(out.xany.window == window || out.xany.window == resetWindow,
          "reset-induced Expose used an unexpected window");
    SDL_FlushEvent(SDL_MOUSEMOTION);
    while (XPending(display)) {
        XNextEvent(display, &out);
    }

    SDL_zero(motionEvent);
    motionEvent.type = SDL_MOUSEMOTION;
    motionEvent.motion.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    motionEvent.motion.x = 7;
    motionEvent.motion.y = 8;
    unsigned long requestBeforeMotion = XNextRequest(display);
    SDL_PushEvent(&motionEvent);
    XNextEvent(display, &out);
    CHECK(out.type == MotionNotify,
          "synthetic SDL motion did not convert to MotionNotify");
    CHECK(XNextRequest(display) == requestBeforeMotion,
          "internal event coordinate translation changed request serial");

    Uint32 presentType = SDL_RegisterEvents(1);
    CHECK(presentType != (Uint32) -1,
          "failed to register present wake test event");
    SDL_Event presentEvent;
    SDL_zero(presentEvent);
    presentEvent.type = presentType;
    presentEvent.user.code = PRESENT_EVENT_CODE;
    SDL_PushEvent(&presentEvent);
    SDL_zero(motionEvent);
    motionEvent.type = SDL_MOUSEMOTION;
    motionEvent.motion.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    motionEvent.motion.x = 5;
    motionEvent.motion.y = 6;
    SDL_PushEvent(&motionEvent);
    XNextEvent(display, &out);
    CHECK(out.type == MotionNotify,
          "present wake was not deferred behind queued motion");
    CHECK(XEventsQueued(display, QueuedAfterReading) == 0,
          "deferred present wake left a phantom queued event");

    SDL_zero(resetEvent);
    resetEvent.type = SDL_RENDER_DEVICE_RESET;
    SDL_PushEvent(&resetEvent);
    CHECK(XCheckTypedWindowEvent(display, window, Expose, &out),
          "device reset did not expose first mapped top-level");
    CHECK(XCheckTypedWindowEvent(display, resetWindow, Expose, &out),
          "device reset did not expose second mapped top-level");
    while (XCheckTypedEvent(display, Expose, &out)) {
    }

    GC copyGc = XCreateGC(display, window, 0, NULL);
    CHECK(copyGc, "NoExpose GC creation failed");
    CHECK(XCopyArea(display, window, window, copyGc, 0, 0, 4, 4, 1, 1),
          "XCopyArea for NoExpose failed");
    CHECK(XCheckTypedEvent(display, NoExpose, &out),
          "XCopyArea did not generate NoExpose");
    CHECK(out.xnoexpose.drawable == window &&
              out.xnoexpose.major_code == X_CopyArea,
          "NoExpose fields were incorrect");
    CHECK(XCopyArea(display, window, window, copyGc, -1, 0, 4, 4, 0, 0),
          "XCopyArea for GraphicsExpose failed");
    CHECK(XCheckTypedEvent(display, GraphicsExpose, &out),
          "XCopyArea did not generate GraphicsExpose");
    CHECK(
        out.xgraphicsexpose.drawable == window && out.xgraphicsexpose.x == 0 &&
            out.xgraphicsexpose.y == 0 && out.xgraphicsexpose.width == 1 &&
            out.xgraphicsexpose.height == 4 &&
            out.xgraphicsexpose.major_code == X_CopyArea,
        "GraphicsExpose covered the copied pixels instead of the missing area");
    XFreeGC(display, copyGc);

    XSelectInput(display, root, ColormapChangeMask);
    CHECK(XInstallColormap(display,
                           DefaultColormap(display, DefaultScreen(display))),
          "XInstallColormap failed");
    CHECK(XCheckTypedWindowEvent(display, root, ColormapNotify, &out),
          "XInstallColormap did not generate ColormapNotify");
    CHECK(out.xcolormap.state == ColormapInstalled,
          "install ColormapNotify state was incorrect");
    CHECK(XUninstallColormap(display,
                             DefaultColormap(display, DefaultScreen(display))),
          "XUninstallColormap failed");
    CHECK(XCheckTypedWindowEvent(display, root, ColormapNotify, &out),
          "XUninstallColormap did not generate ColormapNotify");
    CHECK(out.xcolormap.state == ColormapUninstalled,
          "uninstall ColormapNotify state was incorrect");

    SDL_Event textEvent;
    set_text_input_event(&textEvent, "a");
    int queuedBeforeText = XEventsQueued(display, QueuedAlready);
    SDL_PushEvent(&textEvent);
    CHECK(XEventsQueued(display, QueuedAlready) == queuedBeforeText + 1,
          "side-queued SDL_TEXTINPUT was not reflected in X event accounting");
    CHECK(SDL_HasEvent(SDL_TEXTINPUT),
          "SDL_HasEvent did not see queued SDL_TEXTINPUT");
    SDL_FlushEvent(SDL_TEXTINPUT);
    CHECK(XEventsQueued(display, QueuedAlready) == queuedBeforeText,
          "flushed SDL_TEXTINPUT left stale X event accounting");
    CHECK(!SDL_HasEvent(SDL_TEXTINPUT),
          "SDL_FlushEvent did not remove queued SDL_TEXTINPUT");
    SDL_PushEvent(&textEvent);
    SDL_Event peepedTextEvent;
    SDL_zero(peepedTextEvent);
    CHECK(SDL_PeepEvents(&peepedTextEvent, 1, SDL_GETEVENT, SDL_TEXTINPUT,
                         SDL_TEXTINPUT) == 1,
          "SDL_PeepEvents did not return queued SDL_TEXTINPUT");
    CHECK(XEventsQueued(display, QueuedAlready) == queuedBeforeText,
          "peeped SDL_TEXTINPUT left stale X event accounting");
    CHECK(peepedTextEvent.type == SDL_TEXTINPUT,
          "SDL_PeepEvents returned the wrong text event type");
    CHECK(strcmp(peepedTextEvent.text.text, "a") == 0,
          "SDL_PeepEvents returned the wrong text payload");
    SDL_PushEvent(&textEvent);
    CHECK(XEventsQueued(display, QueuedAlready) == queuedBeforeText + 1,
          "side-queued SDL_TEXTINPUT was not reflected before wait");
    SDL_Event waitedTextEvent;
    SDL_zero(waitedTextEvent);
    CHECK(SDL_WaitEvent(&waitedTextEvent) == 1,
          "SDL_WaitEvent did not return queued SDL_TEXTINPUT");
    CHECK(XEventsQueued(display, QueuedAlready) == queuedBeforeText,
          "waited SDL_TEXTINPUT left stale X event accounting");
    CHECK(waitedTextEvent.type == SDL_TEXTINPUT,
          "SDL_WaitEvent returned the wrong text event type");
    CHECK(strcmp(waitedTextEvent.text.text, "a") == 0,
          "SDL_WaitEvent returned the wrong text payload");
    CHECK(!XCheckTypedEvent(display, KeyPress, &out),
          "SDL_TEXTINPUT was converted into a duplicate KeyPress");

    /* Wheel up -> ButtonPress + matching ButtonRelease for Button4. Real X11
     * always pairs wheel button events, so the conversion layer queues both;
     * the test must drain both so leftover Releases do not pollute the put-back
     * queue ahead of the crossing assertions further down.
     */
    SDL_Event wheelEvent;
    XSelectInput(display, window, ButtonPressMask | ButtonReleaseMask);
    SDL_zero(wheelEvent);
    wheelEvent.type = SDL_MOUSEWHEEL;
    wheelEvent.wheel.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    wheelEvent.wheel.y = 1;
    wheelEvent.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&wheelEvent);
    CHECK(XCheckTypedEvent(display, ButtonPress, &out),
          "SDL_MOUSEWHEEL did not produce ButtonPress");
    CHECK(out.xbutton.button == Button4, "wheel-up did not map to Button4");
    CHECK(XCheckTypedEvent(display, ButtonRelease, &out),
          "SDL_MOUSEWHEEL did not produce paired ButtonRelease");
    CHECK(out.xbutton.button == Button4,
          "wheel-up ButtonRelease did not match Button4");

    SDL_zero(wheelEvent);
    wheelEvent.type = SDL_MOUSEWHEEL;
    wheelEvent.wheel.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    wheelEvent.wheel.y = -1;
    wheelEvent.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&wheelEvent);
    CHECK(XCheckTypedEvent(display, ButtonPress, &out),
          "wheel-down did not produce ButtonPress");
    CHECK(out.xbutton.button == Button5, "wheel-down did not map to Button5");
    CHECK(XCheckTypedEvent(display, ButtonRelease, &out),
          "wheel-down did not produce paired ButtonRelease");
    CHECK(out.xbutton.button == Button5,
          "wheel-down ButtonRelease did not match Button5");

#if SDL_VERSION_ATLEAST(2, 0, 18)
    SDL_zero(wheelEvent);
    wheelEvent.type = SDL_MOUSEWHEEL;
    wheelEvent.wheel.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    XC_SET_WHEEL_Y(&wheelEvent, 0.25f);
    wheelEvent.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    CHECK(convertEvent(display, &wheelEvent, &out, True) == -1,
          "fractional precise wheel delta converted directly");
    CHECK(!XCheckTypedEvent(display, ButtonPress, &out),
          "fractional precise wheel delta produced a full click");

    SDL_zero(wheelEvent);
    wheelEvent.type = SDL_MOUSEWHEEL;
    wheelEvent.wheel.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    XC_SET_WHEEL_Y(&wheelEvent, 0.75f);
    wheelEvent.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    CHECK(convertEvent(display, &wheelEvent, &out, True) == -1,
          "accumulated precise wheel delta converted directly");
    CHECK(XCheckTypedEvent(display, ButtonPress, &out),
          "accumulated precise wheel delta did not produce ButtonPress");
    CHECK(out.xbutton.button == Button4,
          "accumulated precise wheel delta did not map to Button4");
    CHECK(XCheckTypedEvent(display, ButtonRelease, &out),
          "accumulated precise wheel delta did not produce ButtonRelease");
    CHECK(out.xbutton.button == Button4,
          "accumulated precise wheel ButtonRelease did not match Button4");
#endif

    SDL_Event hintMotion;
    SDL_zero(hintMotion);
    hintMotion.type = SDL_MOUSEMOTION;
    hintMotion.motion.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    hintMotion.motion.x = 20;
    hintMotion.motion.y = 20;
    XSelectInput(display, window, PointerMotionMask);
    CHECK(convertEvent(display, &hintMotion, &out, True) == 0,
          "normal SDL motion did not convert");
    CHECK(out.xmotion.is_hint == NotifyNormal,
          "motion without PointerMotionHintMask used NotifyHint");
    XSelectInput(display, window, PointerMotionMask | PointerMotionHintMask);
    CHECK(convertEvent(display, &hintMotion, &out, True) == 0,
          "hint SDL motion did not convert");
    CHECK(out.xmotion.is_hint == NotifyHint,
          "motion with PointerMotionHintMask did not use NotifyHint");

    Window pointerParent =
        XCreateSimpleWindow(display, root, 0, 0, 80, 80, 0, 0, 0);
    CHECK(pointerParent != None, "pointer parent creation failed");
    Window pointerChild =
        XCreateSimpleWindow(display, pointerParent, 10, 12, 20, 22, 0, 0, 0);
    CHECK(pointerChild != None, "pointer child creation failed");
    XSelectInput(display, pointerChild,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    CHECK(XMapWindow(display, pointerChild), "pointer child map failed");
    CHECK(XMapWindow(display, pointerParent), "pointer parent map failed");
    while (XCheckTypedWindowEvent(display, pointerChild, Expose, &out)) {
    }

    XSelectInput(display, pointerChild, EnterWindowMask);
    SDL_Event enterOnlyMotion;
    SDL_zero(enterOnlyMotion);
    enterOnlyMotion.type = SDL_MOUSEMOTION;
    enterOnlyMotion.motion.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(pointerParent)->sdlWindow);
    enterOnlyMotion.motion.x = 15;
    enterOnlyMotion.motion.y = 18;
    CHECK(convertEvent(display, &enterOnlyMotion, &out, True) != 0,
          "enter-only child unexpectedly received MotionNotify");
    CHECK(XCheckTypedWindowEvent(display, pointerChild, EnterNotify, &out),
          "enter-only child did not receive crossing event");
    CHECK(out.xcrossing.x == 5 && out.xcrossing.y == 6,
          "enter-only child crossing coordinates were not child-local");
    CHECK(!XCheckTypedWindowEvent(display, pointerChild, MotionNotify, &out),
          "enter-only child queued MotionNotify without selecting motion");

    XSelectInput(display, pointerChild, LeaveWindowMask);
    enterOnlyMotion.motion.x = 70;
    enterOnlyMotion.motion.y = 70;
    CHECK(convertEvent(display, &enterOnlyMotion, &out, True) != 0,
          "leave-only child unexpectedly received MotionNotify");
    CHECK(XCheckTypedWindowEvent(display, pointerChild, LeaveNotify, &out),
          "leave-only child did not receive crossing event");

    XSelectInput(display, pointerChild, EnterWindowMask | PointerMotionMask);
    enterOnlyMotion.motion.x = 15;
    enterOnlyMotion.motion.y = 18;
    CHECK(convertEvent(display, &enterOnlyMotion, &out, True) != 0,
          "crossing motion should be queued behind EnterNotify");
    XNextEvent(display, &out);
    CHECK(out.type == EnterNotify && out.xcrossing.window == pointerChild,
          "crossing motion did not deliver EnterNotify first");
    XNextEvent(display, &out);
    CHECK(out.type == MotionNotify && out.xmotion.window == pointerChild,
          "crossing motion did not deliver MotionNotify after EnterNotify");

    XSelectInput(display, pointerParent, EnterWindowMask | LeaveWindowMask);
    XSelectInput(display, pointerChild, EnterWindowMask | LeaveWindowMask);
    enterOnlyMotion.motion.x = 70;
    enterOnlyMotion.motion.y = 70;
    CHECK(convertEvent(display, &enterOnlyMotion, &out, True) != 0,
          "child-to-parent crossing unexpectedly produced MotionNotify");
    CHECK(XCheckTypedWindowEvent(display, pointerChild, LeaveNotify, &out),
          "child-to-parent crossing did not leave child");
    CHECK(out.xcrossing.detail == NotifyAncestor,
          "child-to-parent LeaveNotify did not use NotifyAncestor");
    CHECK(XCheckTypedWindowEvent(display, pointerParent, EnterNotify, &out),
          "child-to-parent crossing did not enter parent");
    CHECK(out.xcrossing.detail == NotifyInferior,
          "child-to-parent EnterNotify did not use NotifyInferior");

    enterOnlyMotion.motion.x = 15;
    enterOnlyMotion.motion.y = 18;
    CHECK(convertEvent(display, &enterOnlyMotion, &out, True) != 0,
          "parent-to-child crossing unexpectedly produced MotionNotify");
    CHECK(XCheckTypedWindowEvent(display, pointerParent, LeaveNotify, &out),
          "parent-to-child crossing did not leave parent");
    CHECK(out.xcrossing.detail == NotifyInferior,
          "parent-to-child LeaveNotify did not use NotifyInferior");
    CHECK(XCheckTypedWindowEvent(display, pointerChild, EnterNotify, &out),
          "parent-to-child crossing did not enter child");
    CHECK(out.xcrossing.detail == NotifyAncestor,
          "parent-to-child EnterNotify did not use NotifyAncestor");

    SDL_Event topLeave;
    SDL_zero(topLeave);
    XC_INIT_WINDOW_EVENT(&topLeave);
    topLeave.window.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(pointerParent)->sdlWindow);
    XC_SET_WINDOW_SUBEVENT(&topLeave, SDL_WINDOWEVENT_LEAVE);
    CHECK(convertEvent(display, &topLeave, &out, True) != 0,
          "top-level leave should queue child leave first");
    XNextEvent(display, &out);
    CHECK(out.type == LeaveNotify && out.xcrossing.window == pointerChild,
          "top-level leave did not deliver nested child LeaveNotify first");
    CHECK(out.xcrossing.x_root == 15 && out.xcrossing.y_root == 18,
          "nested child LeaveNotify did not preserve root coordinates");
    CHECK(out.xcrossing.x == 5 && out.xcrossing.y == 6,
          "nested child LeaveNotify did not preserve child-local coordinates");
    XNextEvent(display, &out);
    CHECK(out.type == LeaveNotify && out.xcrossing.window == pointerParent,
          "top-level leave did not deliver parent LeaveNotify after child");

    XSelectInput(display, pointerParent, NoEventMask);
    XSelectInput(
        display, pointerChild,
        ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask);
    XSetInputFocus(display, pointerParent, RevertToParent, CurrentTime);

    SDL_Event buttonEvent;
    SDL_zero(buttonEvent);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    buttonEvent.button.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(pointerParent)->sdlWindow);
    buttonEvent.button.x = 14;
    buttonEvent.button.y = 17;
    buttonEvent.button.button = SDL_BUTTON_LEFT;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "SDL button did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == pointerChild,
          "SDL button did not target containing child");
    CHECK(out.xbutton.x == 4 && out.xbutton.y == 5 &&
              out.xbutton.x_root == 14 && out.xbutton.y_root == 17,
          "SDL button coordinates were not child-local");
    CHECK((out.xbutton.state & Button1Mask) == 0,
          "ButtonPress state included the newly pressed button");
    Window buttonFocus = None;
    int buttonRevert = 0;
    XGetInputFocus(display, &buttonFocus, &buttonRevert);
    CHECK(buttonFocus == pointerParent,
          "ButtonPress changed keyboard focus without an XSetInputFocus call");

    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "SDL button release did not convert");
    CHECK(out.type == ButtonRelease && out.xbutton.window == pointerChild,
          "SDL button release did not target containing child");
    CHECK((out.xbutton.state & Button1Mask) != 0,
          "ButtonRelease state did not include the released button");

    Window offsetPointerParent =
        XCreateSimpleWindow(display, root, 90, 70, 80, 80, 0, 0, 0);
    CHECK(offsetPointerParent != None, "offset pointer parent creation failed");
    Window offsetPointerChild = XCreateSimpleWindow(
        display, offsetPointerParent, 10, 12, 20, 22, 0, 0, 0);
    CHECK(offsetPointerChild != None, "offset pointer child creation failed");
    XSelectInput(display, offsetPointerChild,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    CHECK(XMapWindow(display, offsetPointerChild),
          "offset pointer child map failed");
    CHECK(XMapWindow(display, offsetPointerParent),
          "offset pointer parent map failed");
    while (XCheckTypedWindowEvent(display, offsetPointerChild, Expose, &out)) {
    }
    SDL_zero(buttonEvent);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    buttonEvent.button.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(offsetPointerParent)->sdlWindow);
    buttonEvent.button.x = 14;
    buttonEvent.button.y = 17;
    buttonEvent.button.button = SDL_BUTTON_LEFT;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "offset SDL button did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == offsetPointerChild,
          "offset SDL button did not target containing child");
    CHECK(out.xbutton.root == SCREEN_WINDOW && out.xbutton.x_root == 104 &&
              out.xbutton.y_root == 87,
          "offset SDL button did not report root coordinates");
    CHECK(out.xbutton.x == 4 && out.xbutton.y == 5,
          "offset SDL button coordinates were not child-local");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "offset SDL button release did not convert");

    Window unmappedCover = XCreateSimpleWindow(display, offsetPointerParent, 0,
                                               0, 80, 80, 0, 0, 0);
    CHECK(unmappedCover != None, "unmapped cover creation failed");
    XSelectInput(display, unmappedCover, ButtonPressMask);
    XSelectInput(display, offsetPointerChild,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "covered SDL button did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == offsetPointerChild,
          "unmapped sibling stole pointer hit-test from visible child");
    CHECK(out.xbutton.subwindow == None,
          "button event reported an unmapped sibling as subwindow");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "covered SDL button release did not convert");

    CHECK(XGrabButton(display, Button1, AnyModifier, offsetPointerChild, False,
                      ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                      GrabModeAsync, None, None) == Success,
          "nested passive button grab failed");
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "nested passive-grab button press did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == offsetPointerChild,
          "nested passive grab did not activate on deepest child");
    CHECK(out.xbutton.subwindow == None,
          "nested passive grab reported unexpected subwindow");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "nested passive-grab button release did not convert");
    CHECK(out.type == ButtonRelease && out.xbutton.window == offsetPointerChild,
          "nested passive grab did not keep release on grab window");
    CHECK(XUngrabButton(display, Button1, AnyModifier, offsetPointerChild) ==
              Success,
          "nested passive button ungrab failed");

    CHECK(XGrabButton(display, Button1, AnyModifier, offsetPointerParent, False,
                      ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                      GrabModeAsync, None, None) == Success,
          "passive button grab failed");
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "passive-grab button press did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == offsetPointerParent,
          "passive button grab did not route press to grab window");
    CHECK(out.xbutton.subwindow == offsetPointerChild,
          "passive button grab did not preserve direct subwindow");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "passive-grab button release did not convert");
    CHECK(
        out.type == ButtonRelease && out.xbutton.window == offsetPointerParent,
        "passive button grab did not keep release on grab window");
    CHECK(XUngrabButton(display, Button1, AnyModifier, offsetPointerParent) ==
              Success,
          "passive button ungrab failed");
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "post-passive-ungrab button press did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == offsetPointerChild,
          "post-passive-ungrab press did not return to normal hit-testing");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "post-passive-ungrab button release did not convert");

    CHECK(XGrabButton(display, Button1, AnyModifier, offsetPointerParent, False,
                      ButtonPressMask | ButtonReleaseMask, GrabModeSync,
                      GrabModeAsync, None, None) == Success,
          "sync passive button grab failed");
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "sync passive-grab button press did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == offsetPointerParent,
          "sync passive button grab did not route press to grab window");
    CHECK(mouseFrozen, "sync passive button grab did not freeze pointer");
    CHECK(XAllowEvents(display, ReplayPointer, CurrentTime),
          "ReplayPointer failed for passive button grab");
    CHECK(!mouseFrozen, "ReplayPointer did not thaw pointer");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "post-ReplayPointer button release did not convert");
    CHECK(out.type == ButtonRelease && out.xbutton.window == offsetPointerChild,
          "ReplayPointer did not release passive button grab routing");
    CHECK(XUngrabButton(display, Button1, AnyModifier, offsetPointerParent) ==
              Success,
          "sync passive button ungrab failed");

    CHECK(XGrabButton(display, Button1, AnyModifier, offsetPointerParent, True,
                      ButtonReleaseMask, GrabModeSync, GrabModeAsync, None,
                      None) == Success,
          "Motif-style passive button grab failed");
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "Motif-style passive-grab button press did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == offsetPointerChild,
          "owner-events passive grab did not deliver press to selected child");
    CHECK(mouseFrozen, "Motif-style sync passive grab did not freeze pointer");
    CHECK(XGrabPointer(display, offsetPointerParent, True,
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                       GrabModeSync, GrabModeAsync, None, None,
                       CurrentTime) == GrabSuccess,
          "XGrabPointer did not upgrade active passive grab");
    CHECK(mouseFrozen, "upgraded menu pointer grab did not preserve sync mode");
    CHECK(XUngrabPointer(display, CurrentTime),
          "upgraded menu pointer grab did not ungrab");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "Motif-style passive-grab button release did not convert");
    CHECK(XUngrabButton(display, Button1, AnyModifier, offsetPointerParent) ==
              Success,
          "Motif-style passive button ungrab failed");

    SDL_zero(hintMotion);
    hintMotion.type = SDL_MOUSEMOTION;
    hintMotion.motion.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(offsetPointerParent)->sdlWindow);
    hintMotion.motion.x = 14;
    hintMotion.motion.y = 17;
    XSelectInput(display, offsetPointerChild, Button1MotionMask);
    CHECK(convertEvent(display, &hintMotion, &out, True) < 0,
          "Button1MotionMask received motion with no held Button1");
    XSelectInput(display, offsetPointerChild,
                 ButtonPressMask | ButtonReleaseMask | Button1MotionMask);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "Button1Motion setup press did not convert");
    CHECK(convertEvent(display, &hintMotion, &out, True) == 0,
          "Button1MotionMask missed drag motion with held Button1");
    CHECK(out.type == MotionNotify && out.xmotion.window == offsetPointerChild,
          "Button1MotionMask drag targeted the wrong window");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "Button1Motion cleanup release did not convert");
    XSelectInput(display, offsetPointerChild,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);

    SDL_zero(buttonEvent);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    buttonEvent.button.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(pointerParent)->sdlWindow);
    buttonEvent.button.x = 14;
    buttonEvent.button.y = 17;
    buttonEvent.button.button = SDL_BUTTON_LEFT;
    CHECK(convertEvent(display, &buttonEvent, &out, False) == 0,
          "SDL button probe did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == pointerChild,
          "SDL button probe targeted the wrong window");
    CHECK((out.xbutton.state & Button1Mask) == 0,
          "ButtonPress probe state included the newly pressed button");
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "SDL button delivery after probe did not convert");
    CHECK((out.xbutton.state & Button1Mask) == 0,
          "ButtonPress delivery observed state mutated by probe");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "SDL button release after probe did not convert");

    Window pointerGrandchild =
        XCreateSimpleWindow(display, pointerChild, 3, 4, 6, 7, 0, 0, 0);
    CHECK(pointerGrandchild != None, "pointer grandchild creation failed");
    CHECK(XMapWindow(display, pointerGrandchild),
          "pointer grandchild map failed");
    SDL_zero(buttonEvent);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    buttonEvent.button.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(pointerParent)->sdlWindow);
    buttonEvent.button.x = 16;
    buttonEvent.button.y = 19;
    buttonEvent.button.button = SDL_BUTTON_LEFT;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "SDL nested-child button did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == pointerChild,
          "SDL nested-child button did not propagate to selected ancestor");
    CHECK(out.xbutton.subwindow == pointerGrandchild,
          "SDL nested-child button did not preserve direct subwindow");
    CHECK(out.xbutton.x == 6 && out.xbutton.y == 7 &&
              out.xbutton.x_root == 16 && out.xbutton.y_root == 19,
          "SDL nested-child button coordinates were not ancestor-local");
    CHECK((out.xbutton.state & Button1Mask) == 0,
          "nested ButtonPress state included the newly pressed button");

    SDL_zero(hintMotion);
    hintMotion.type = SDL_MOUSEMOTION;
    hintMotion.motion.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(pointerParent)->sdlWindow);
    hintMotion.motion.x = 18;
    hintMotion.motion.y = 21;
    CHECK(convertEvent(display, &hintMotion, &out, True) == 0,
          "SDL child motion did not convert");
    CHECK(out.type == MotionNotify && out.xmotion.window == pointerChild,
          "SDL motion did not target containing child");
    CHECK(out.xmotion.x == 8 && out.xmotion.y == 9 &&
              out.xmotion.x_root == 18 && out.xmotion.y_root == 21,
          "SDL motion coordinates were not child-local");
    CHECK((out.xmotion.state & Button1Mask) != 0,
          "SDL drag motion did not include active Button1 state");

    hintMotion.motion.x = 17;
    hintMotion.motion.y = 20;
    CHECK(convertEvent(display, &hintMotion, &out, True) == 0,
          "SDL nested-child motion did not convert");
    CHECK(out.type == MotionNotify && out.xmotion.window == pointerChild,
          "SDL nested-child motion did not propagate to selected ancestor");
    CHECK(out.xmotion.subwindow == pointerGrandchild,
          "SDL nested-child motion did not preserve direct subwindow");
    CHECK(out.xmotion.x == 7 && out.xmotion.y == 8 &&
              out.xmotion.x_root == 17 && out.xmotion.y_root == 20,
          "SDL nested-child motion coordinates were not ancestor-local");

    buttonEvent.type = SDL_MOUSEBUTTONUP;
    buttonEvent.button.x = 70;
    buttonEvent.button.y = 70;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "SDL nested-child button release did not convert");
    CHECK(out.type == ButtonRelease && out.xbutton.window == pointerChild,
          "SDL nested-child release did not stay on active pointer window");
    CHECK(out.xbutton.subwindow == None,
          "SDL outside release incorrectly reported a direct subwindow");
    CHECK(out.xbutton.x == 60 && out.xbutton.y == 58 &&
              out.xbutton.x_root == 70 && out.xbutton.y_root == 70,
          "SDL outside release coordinates were not active-window-local");
    CHECK((out.xbutton.state & Button1Mask) != 0,
          "nested ButtonRelease state did not include released Button1");

    CHECK(convertEvent(display, &hintMotion, &out, True) == 0,
          "SDL post-release motion did not convert");
    CHECK((out.xmotion.state & Button1Mask) == 0,
          "SDL motion kept Button1 state after release");

    Window stalePressWindow =
        XCreateSimpleWindow(display, root, 180, 0, 30, 30, 0, 0, 0);
    CHECK(stalePressWindow != None, "stale press window creation failed");
    XSelectInput(display, stalePressWindow,
                 ButtonPressMask | ButtonReleaseMask);
    CHECK(XMapWindow(display, stalePressWindow),
          "stale press window map failed");
    SDL_zero(buttonEvent);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    buttonEvent.button.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(stalePressWindow)->sdlWindow);
    buttonEvent.button.x = 5;
    buttonEvent.button.y = 5;
    buttonEvent.button.button = SDL_BUTTON_LEFT;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "stale press setup did not convert");
    CHECK(XDestroyWindow(display, stalePressWindow),
          "stale press window destroy failed");
    Window freshPressWindow =
        XCreateSimpleWindow(display, root, 220, 0, 30, 30, 0, 0, 0);
    CHECK(freshPressWindow != None, "fresh press window creation failed");
    XSelectInput(display, freshPressWindow,
                 ButtonPressMask | ButtonReleaseMask);
    CHECK(XMapWindow(display, freshPressWindow),
          "fresh press window map failed");
    SDL_zero(buttonEvent);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    buttonEvent.button.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(freshPressWindow)->sdlWindow);
    buttonEvent.button.x = 5;
    buttonEvent.button.y = 5;
    buttonEvent.button.button = SDL_BUTTON_LEFT;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "fresh press after destroyed active window did not convert");
    CHECK((out.xbutton.state & Button1Mask) == 0,
          "destroyed active window left Button1 stuck down");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "fresh cleanup release did not convert");

    XSelectInput(display, window, EnterWindowMask | LeaveWindowMask);
    SDL_Event crossingEvent;
    SDL_zero(crossingEvent);
    XC_INIT_WINDOW_EVENT(&crossingEvent);
    crossingEvent.window.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    XC_SET_WINDOW_SUBEVENT(&crossingEvent, SDL_WINDOWEVENT_ENTER);
    CHECK(convertEvent(display, &crossingEvent, &out, True) == 0,
          "SDL window enter did not convert");
    CHECK(out.type == EnterNotify && out.xcrossing.mode == NotifyNormal,
          "SDL window enter crossing fields were incorrect");
    XC_SET_WINDOW_SUBEVENT(&crossingEvent, SDL_WINDOWEVENT_LEAVE);
    CHECK(convertEvent(display, &crossingEvent, &out, True) == 0,
          "SDL window leave did not convert");
    CHECK(out.type == LeaveNotify && out.xcrossing.mode == NotifyNormal,
          "SDL window leave crossing fields were incorrect");
    CHECK(XGrabPointer(display, window, False,
                       EnterWindowMask | LeaveWindowMask, GrabModeSync,
                       GrabModeAsync, None, None, CurrentTime) == GrabSuccess,
          "XGrabPointer failed");
    CHECK(mouseFrozen, "XGrabPointer did not freeze sync pointer mode");
    CHECK(XCheckTypedWindowEvent(display, window, EnterNotify, &out),
          "XGrabPointer did not post EnterNotify");
    CHECK(out.xcrossing.mode == NotifyGrab,
          "XGrabPointer EnterNotify used wrong mode");
    /* A second active grab by the same client is a regrab: it updates the
     * active grab instead of reporting AlreadyGrabbed.
     */
    CHECK(XGrabPointer(display, window, False,
                       ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                       GrabModeAsync, None, None, CurrentTime) == GrabSuccess,
          "same-client XGrabPointer regrab did not report GrabSuccess");
    CHECK(!mouseFrozen, "same-client XGrabPointer regrab did not thaw pointer");
    CHECK(getPointerGrabEventMask() == (ButtonPressMask | ButtonReleaseMask),
          "same-client XGrabPointer regrab did not update event mask");
    /* A grab on None or on a non-window must report BadWindow. */
    CHECK(XGrabPointer(display, None, False, 0, GrabModeAsync, GrabModeAsync,
                       None, None, CurrentTime) == BadWindow,
          "XGrabPointer(None) did not report BadWindow");
    CHECK(SDL_GetRelativeMouseMode() == SDL_FALSE,
          "XGrabPointer enabled SDL relative mouse mode");
    CHECK(XAllowEvents(display, SyncPointer, CurrentTime),
          "XAllowEvents(SyncPointer) failed");
    CHECK(!mouseFrozen, "SyncPointer did not thaw pointer freeze");
    CHECK(getGrabbedPointerWindow() == window,
          "SyncPointer released the active pointer grab");
    CHECK(XAllowEvents(display, AsyncPointer, CurrentTime),
          "XAllowEvents failed");
    CHECK(!mouseFrozen, "AsyncPointer did not keep pointer thawed");
    CHECK(XUngrabPointer(display, CurrentTime), "XUngrabPointer failed");
    CHECK(XCheckTypedWindowEvent(display, window, LeaveNotify, &out),
          "XUngrabPointer did not post LeaveNotify");
    CHECK(out.xcrossing.mode == NotifyUngrab,
          "XUngrabPointer LeaveNotify used wrong mode");

    Window viewableRequestedChild =
        XCreateSimpleWindow(display, window, 3, 4, 16, 16, 0, 0, 0);
    CHECK(viewableRequestedChild != None,
          "viewable MapRequested child creation failed");
    CHECK(XMapWindow(display, viewableRequestedChild),
          "viewable MapRequested child map failed");
    GET_WINDOW_STRUCT(viewableRequestedChild)->mapState = MapRequested;
    XWindowAttributes requestedAttrs;
    CHECK(
        XGetWindowAttributes(display, viewableRequestedChild, &requestedAttrs),
        "viewable MapRequested child attributes failed");
    CHECK(requestedAttrs.map_state == IsViewable,
          "test setup did not produce an effectively viewable child");
    CHECK(XGrabPointer(display, viewableRequestedChild, False, ButtonPressMask,
                       GrabModeAsync, GrabModeAsync, None, None,
                       CurrentTime) == GrabSuccess,
          "XGrabPointer rejected effectively viewable child");
    CHECK(XUngrabPointer(display, CurrentTime),
          "effectively viewable child pointer ungrab failed");

    CHECK(XGrabPointer(display, pointerChild, False,
                       ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                       GrabModeAsync, GrabModeAsync, None, None,
                       CurrentTime) == GrabSuccess,
          "explicit pointer grab failed");
    SDL_zero(buttonEvent);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    buttonEvent.button.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(offsetPointerParent)->sdlWindow);
    buttonEvent.button.x = 14;
    buttonEvent.button.y = 17;
    buttonEvent.button.button = SDL_BUTTON_LEFT;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "explicit-grab SDL button did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == pointerChild,
          "explicit pointer grab did not route ButtonPress to grab window");
    CHECK(out.xbutton.x == 94 && out.xbutton.y == 75 &&
              out.xbutton.x_root == 104 && out.xbutton.y_root == 87,
          "explicit pointer grab ButtonPress coordinates were wrong");
    SDL_zero(hintMotion);
    hintMotion.type = SDL_MOUSEMOTION;
    hintMotion.motion.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(offsetPointerParent)->sdlWindow);
    hintMotion.motion.x = 16;
    hintMotion.motion.y = 19;
    CHECK(convertEvent(display, &hintMotion, &out, True) == 0,
          "explicit-grab SDL motion did not convert");
    CHECK(out.type == MotionNotify && out.xmotion.window == pointerChild,
          "explicit pointer grab did not route MotionNotify to grab window");
    CHECK(out.xmotion.x == 96 && out.xmotion.y == 77 &&
              out.xmotion.x_root == 106 && out.xmotion.y_root == 89,
          "explicit pointer grab MotionNotify coordinates were wrong");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    buttonEvent.button.x = 16;
    buttonEvent.button.y = 19;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "explicit-grab SDL button release did not convert");
    CHECK(out.type == ButtonRelease && out.xbutton.window == pointerChild,
          "explicit pointer grab did not route ButtonRelease to grab window");
    CHECK(XUngrabPointer(display, CurrentTime),
          "explicit pointer ungrab failed");
    while (XCheckTypedWindowEvent(display, pointerChild, LeaveNotify, &out)) {
    }

    CHECK(XGrabPointer(display, pointerChild, False,
                       ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                       GrabModeAsync, None, None, CurrentTime) == GrabSuccess,
          "explicit pointer grab for ungrab-release failed");
    SDL_zero(buttonEvent);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    buttonEvent.button.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(offsetPointerParent)->sdlWindow);
    buttonEvent.button.x = 14;
    buttonEvent.button.y = 17;
    buttonEvent.button.button = SDL_BUTTON_LEFT;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "explicit-grab press before ungrab did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == pointerChild,
          "explicit pointer grab before ungrab did not route to grab window");
    CHECK(XUngrabPointer(display, CurrentTime),
          "explicit pointer ungrab during button hold failed");
    while (XCheckTypedWindowEvent(display, pointerChild, LeaveNotify, &out)) {
    }
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "post-ungrab button release did not convert");
    CHECK(out.type == ButtonRelease && out.xbutton.window == offsetPointerChild,
          "post-ungrab release kept routing to stale grab window");
    CHECK((out.xbutton.state & Button1Mask) != 0,
          "post-ungrab release lost the held Button1 state");

    CHECK(XGrabPointer(display, pointerChild, True, ButtonPressMask,
                       GrabModeAsync, GrabModeAsync, None, None,
                       CurrentTime) == GrabSuccess,
          "owner-events pointer grab failed");
    SDL_zero(buttonEvent);
    buttonEvent.type = SDL_MOUSEBUTTONDOWN;
    buttonEvent.button.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(offsetPointerParent)->sdlWindow);
    buttonEvent.button.x = 14;
    buttonEvent.button.y = 17;
    buttonEvent.button.button = SDL_BUTTON_LEFT;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "owner-events grab SDL button did not convert");
    CHECK(out.type == ButtonPress && out.xbutton.window == offsetPointerChild,
          "owner-events pointer grab did not preserve normal owner routing");
    buttonEvent.type = SDL_MOUSEBUTTONUP;
    CHECK(convertEvent(display, &buttonEvent, &out, True) == 0,
          "owner-events grab SDL button release did not convert");
    CHECK(XUngrabPointer(display, CurrentTime),
          "owner-events pointer ungrab failed");
    while (XCheckTypedWindowEvent(display, pointerChild, LeaveNotify, &out)) {
    }

    /* CWOverrideRedirect must round-trip through XGetWindowAttributes. */
    XSetWindowAttributes ovAttrs;
    ovAttrs.override_redirect = True;
    XChangeWindowAttributes(display, window, CWOverrideRedirect, &ovAttrs);
    XWindowAttributes ovQuery;
    CHECK(XGetWindowAttributes(display, window, &ovQuery),
          "XGetWindowAttributes after CWOverrideRedirect failed");
    CHECK(ovQuery.override_redirect == True,
          "CWOverrideRedirect did not stick on the window");

    XSelectInput(display, window, StructureNotifyMask | ExposureMask);
    while (XCheckTypedWindowEvent(display, window, ConfigureNotify, &out)) {
    }
    while (XCheckTypedWindowEvent(display, window, Expose, &out)) {
    }
    unsigned long resizeRequest = XNextRequest(display);
    CHECK(XResizeWindow(display, window, 52, 41),
          "XResizeWindow for serial check failed");
    CHECK(XCheckTypedWindowEvent(display, window, ConfigureNotify, &out),
          "XResizeWindow did not post ConfigureNotify");
    CHECK(out.xconfigure.serial >= resizeRequest,
          "ConfigureNotify serial did not satisfy the triggering request");
    while (XCheckTypedWindowEvent(display, window, Expose, &out)) {
    }

    SDL_Event resizeEvent;
    SDL_zero(resizeEvent);
    XC_INIT_WINDOW_EVENT(&resizeEvent);
    XC_SET_WINDOW_SUBEVENT(&resizeEvent, SDL_WINDOWEVENT_RESIZED);
    resizeEvent.window.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    resizeEvent.window.data1 = 48;
    resizeEvent.window.data2 = 40;
    CHECK(convertEvent(display, &resizeEvent, &out, True) == 0,
          "SDL resize did not convert to ConfigureNotify");
    CHECK(out.type == ConfigureNotify && out.xconfigure.window == window,
          "SDL resize converted to the wrong X event");
    CHECK(out.xconfigure.width == 48 && out.xconfigure.height == 40,
          "ConfigureNotify did not report the SDL resize dimensions");
    CHECK(XCheckTypedWindowEvent(display, window, Expose, &out),
          "SDL resize did not post a full-window Expose");
    CHECK(out.xexpose.x == 0 && out.xexpose.y == 0 && out.xexpose.width == 48 &&
              out.xexpose.height == 40,
          "SDL resize Expose rectangle was incorrect");
    int textureWidth = 0;
    int textureHeight = 0;
    SDL_QueryTexture(GET_WINDOW_STRUCT(window)->sdlTexture, NULL, NULL,
                     &textureWidth, &textureHeight);
    CHECK(textureWidth == 48 && textureHeight == 40,
          "backing texture did not resize to SDL dimensions");
    GC resizeGc = XCreateGC(display, window, 0, NULL);
    CHECK(resizeGc != NULL, "resize backing GC creation failed");
    CHECK(XSetForeground(display, resizeGc, 0xFFFF0000),
          "resize backing red foreground failed");
    CHECK(XFillRectangle(display, window, resizeGc, 0, 0, 12, 12),
          "resize backing stale fill failed");
    CHECK(convertEvent(display, &resizeEvent, &out, True) == 0,
          "second SDL resize did not convert to ConfigureNotify");
    SDL_Renderer *resizeRenderer = NULL;
    GET_RENDERER(window, resizeRenderer);
    CHECK(resizeRenderer != NULL, "resize backing renderer lookup failed");
    SDL_Surface *resizeSurface = getRenderSurface(resizeRenderer);
    CHECK(resizeSurface != NULL, "resize backing readback failed");
    CHECK(!pixel_is_rgb(resizeSurface, 1, 1, 255, 0, 0),
          "top-level resize preserved stale backing pixels");
    SDL_FreeSurface(resizeSurface);
    XFreeGC(display, resizeGc);
    SDL_Event windowEvent;
    SDL_zero(windowEvent);
    XC_INIT_WINDOW_EVENT(&windowEvent);
    windowEvent.window.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_MOVED);
    windowEvent.window.data1 = 11;
    windowEvent.window.data2 = 12;
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL move did not convert to ConfigureNotify");
    XWindowAttributes movedWindowAttrs;
    CHECK(XGetWindowAttributes(display, window, &movedWindowAttrs),
          "XGetWindowAttributes after SDL move failed");
    CHECK(movedWindowAttrs.x == out.xconfigure.x &&
              movedWindowAttrs.y == out.xconfigure.y,
          "SDL move did not update window attributes");

    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_HIDDEN);
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL hidden did not convert to UnmapNotify");
    CHECK(expect_map_state(display, window, IsUnmapped),
          "SDL hidden did not update map state");
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_SHOWN);
    GET_WINDOW_STRUCT(window)->needsPresent = False;
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL shown did not convert to MapNotify");
    CHECK(expect_map_state(display, window, IsViewable),
          "SDL shown did not update map state");
    CHECK(GET_WINDOW_STRUCT(window)->needsPresent,
          "SDL shown did not request repaint of the mapped window");
    GET_WINDOW_STRUCT(window)->needsPresent = False;
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_EXPOSED);
    CHECK(convertEvent(display, &windowEvent, &out, True) < 0,
          "SDL exposed should be consumed internally");
    CHECK(GET_WINDOW_STRUCT(window)->needsPresent,
          "SDL exposed did not request repaint of the existing backing store");
    GET_WINDOW_STRUCT(window)->needsPresent = False;
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_MINIMIZED);
    CHECK(convertEvent(display, &windowEvent, &out, True) < 0,
          "SDL minimized should be consumed internally");
    CHECK(expect_map_state(display, window, IsUnmapped),
          "SDL minimized did not update map state");
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_RESTORED);
    CHECK(convertEvent(display, &windowEvent, &out, True) < 0,
          "SDL restored should be consumed internally");
    CHECK(expect_map_state(display, window, IsViewable),
          "SDL restored did not update map state");
    CHECK(GET_WINDOW_STRUCT(window)->needsPresent,
          "SDL restored did not request repaint of the existing backing store");
    while (XCheckTypedWindowEvent(display, window, Expose, &out)) {
    }

    /* A host focus-out/in cycle must not clobber a client XSetInputFocus that
     * targets a child widget. The SDL focus events name the top-level window;
     * adopting it would mis-route later keys to the parent. Regression for the
     * asymmetric FOCUS_GAINED path that used to overwrite the child focus.
     */
    Window focusKeepChild =
        XCreateSimpleWindow(display, window, 1, 1, 6, 6, 0, 0, 0);
    CHECK(focusKeepChild != None, "focus-keep child creation failed");
    XSetInputFocus(display, focusKeepChild, RevertToParent, CurrentTime);
    Window focusKeepResult = None;
    int focusKeepRevert = 0;
    XGetInputFocus(display, &focusKeepResult, &focusKeepRevert);
    CHECK(focusKeepResult == focusKeepChild,
          "XSetInputFocus did not target child before host focus cycle");
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_FOCUS_LOST);
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL focus lost did not convert to FocusOut");
    XGetInputFocus(display, &focusKeepResult, &focusKeepRevert);
    CHECK(focusKeepResult == focusKeepChild,
          "host focus loss on top-level cleared client child focus");
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_FOCUS_GAINED);
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL focus gained did not convert to FocusIn");
    XGetInputFocus(display, &focusKeepResult, &focusKeepRevert);
    CHECK(focusKeepResult == focusKeepChild,
          "host focus gain on top-level clobbered client child focus");
    CHECK(XDestroyWindow(display, focusKeepChild),
          "focus-keep child destroy failed");

    XSetInputFocus(display, window, RevertToParent, CurrentTime);
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_FOCUS_LOST);
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL focus lost on explicit top-level did not convert to FocusOut");
    XGetInputFocus(display, &focusKeepResult, &focusKeepRevert);
    CHECK(focusKeepResult == window,
          "host focus loss on top-level cleared explicit top-level focus");

    SDL_RaiseWindow(GET_WINDOW_STRUCT(window)->sdlWindow);
    XSetInputFocus(display, None, RevertToParent, CurrentTime);
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_FOCUS_GAINED);
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL focus gain after explicit None did not convert to FocusIn");
    XGetInputFocus(display, &focusKeepResult, &focusKeepRevert);
    CHECK(focusKeepResult == None,
          "host focus gain on top-level clobbered explicit None focus");
    SDL_Event focusNoneKey;
    SDL_zero(focusNoneKey);
    focusNoneKey.type = SDL_KEYDOWN;
    focusNoneKey.key.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    XC_EVENT_SET_KEYSYM(&focusNoneKey, SDLK_a);
    XC_EVENT_SET_KEYMOD(&focusNoneKey, 0);
    XC_EVENT_SET_KEY_PRESSED(&focusNoneKey, True);
    CHECK(convertEvent(display, &focusNoneKey, &out, True) == 0,
          "SDL key after explicit None did not convert");
    XGetInputFocus(display, &focusKeepResult, &focusKeepRevert);
    CHECK(focusKeepResult == None, "SDL key clobbered explicit None focus");

    XSetInputFocus(display, (Window) PointerRoot, RevertToParent, CurrentTime);
    XC_SET_WINDOW_SUBEVENT(&windowEvent, SDL_WINDOWEVENT_FOCUS_GAINED);
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL focus gained after PointerRoot did not convert to FocusIn");
    XGetInputFocus(display, &focusKeepResult, &focusKeepRevert);
    CHECK(focusKeepResult == (Window) PointerRoot,
          "host focus gain on top-level clobbered PointerRoot focus");

    Window gravityChild =
        XCreateSimpleWindow(display, window, 2, 3, 8, 8, 0, 0, 0);
    CHECK(gravityChild != None, "gravity child creation failed");
    XSetWindowAttributes gravityAttrs;
    memset(&gravityAttrs, 0, sizeof(gravityAttrs));
    gravityAttrs.win_gravity = EastGravity;
    CHECK(XChangeWindowAttributes(display, gravityChild, CWWinGravity,
                                  &gravityAttrs),
          "CWWinGravity XChangeWindowAttributes failed");
    XSelectInput(display, gravityChild, StructureNotifyMask);
    CHECK(XMapWindow(display, gravityChild), "XMapWindow gravity child failed");
    while (XCheckTypedWindowEvent(display, gravityChild, GravityNotify, &out)) {
    }
    XWindowAttributes parentBeforeGravity;
    XWindowAttributes childBeforeGravity;
    CHECK(XGetWindowAttributes(display, window, &parentBeforeGravity),
          "XGetWindowAttributes gravity parent failed");
    CHECK(XGetWindowAttributes(display, gravityChild, &childBeforeGravity),
          "XGetWindowAttributes gravity child before resize failed");
    CHECK(XResizeWindow(display, window, 56, 44), "parent resize failed");
    CHECK(XCheckTypedWindowEvent(display, gravityChild, GravityNotify, &out),
          "parent resize did not generate GravityNotify");
    CHECK(out.xgravity.event == gravityChild &&
              out.xgravity.window == gravityChild,
          "GravityNotify fields were incorrect");
    XWindowAttributes movedAttrs;
    CHECK(XGetWindowAttributes(display, gravityChild, &movedAttrs),
          "XGetWindowAttributes gravity child failed");
    int expectedGravityX =
        childBeforeGravity.x + 56 - parentBeforeGravity.width;
    int expectedGravityY =
        childBeforeGravity.y + (44 - parentBeforeGravity.height) / 2;
    CHECK(movedAttrs.x == expectedGravityX && movedAttrs.y == expectedGravityY,
          "window gravity did not move child before GravityNotify");
    /* A gravity-driven move generates GravityNotify only, never a
     * ConfigureNotify: the child's parent-relative geometry changed but it was
     * not reconfigured.
     */
    CHECK(!XCheckTypedWindowEvent(display, gravityChild, ConfigureNotify, &out),
          "gravity-moved child got spurious ConfigureNotify");

    Window gravityGrandchild =
        XCreateSimpleWindow(display, gravityChild, 1, 1, 4, 4, 0, 0, 0);
    CHECK(gravityGrandchild != None, "gravity grandchild creation failed");
    XSelectInput(display, gravityGrandchild,
                 StructureNotifyMask | ExposureMask);
    CHECK(XMapWindow(display, gravityGrandchild),
          "XMapWindow gravity grandchild failed");
    while (XCheckWindowEvent(display, gravityGrandchild,
                             StructureNotifyMask | ExposureMask, &out)) {
    }
    CHECK(XResizeWindow(display, window, 60, 44), "third parent resize failed");
    CHECK(XCheckTypedWindowEvent(display, gravityChild, GravityNotify, &out),
          "second parent resize did not move gravity child");
    /* The grandchild's geometry relative to its parent is unchanged, so it gets
     * neither GravityNotify nor ConfigureNotify. It is still exposed because a
     * top-level resize repaints the whole subtree.
     */
    CHECK(!XCheckTypedWindowEvent(display, gravityGrandchild, ConfigureNotify,
                                  &out),
          "unmoved grandchild got spurious ConfigureNotify");
    CHECK(!XCheckTypedWindowEvent(display, gravityGrandchild, GravityNotify,
                                  &out),
          "unmoved grandchild got spurious GravityNotify");
    CHECK(XCheckTypedWindowEvent(display, gravityGrandchild, Expose, &out),
          "top-level resize did not expose grandchild");

    Window stableChild =
        XCreateSimpleWindow(display, window, 2, 3, 8, 8, 0, 0, 0);
    CHECK(stableChild != None, "stable gravity child creation failed");
    XSelectInput(display, stableChild, StructureNotifyMask);
    CHECK(XMapWindow(display, stableChild), "XMapWindow stable child failed");
    while (XCheckWindowEvent(display, stableChild, StructureNotifyMask, &out)) {
    }
    CHECK(XResizeWindow(display, window, 64, 48),
          "second parent resize failed");
    CHECK(!XCheckTypedWindowEvent(display, stableChild, ConfigureNotify, &out),
          "NorthWestGravity child got spurious ConfigureNotify");
    CHECK(!XCheckTypedWindowEvent(display, stableChild, GravityNotify, &out),
          "NorthWestGravity child got spurious GravityNotify");
    while (XCheckTypedEvent(display, Expose, &out)) {
    }

    /* The XResizeWindow (configureWindow) path must also unmap UnmapGravity
     * children, with UnmapNotify.from_configure == True.
     */
    Window unmapGravityChild =
        XCreateSimpleWindow(display, window, 4, 4, 6, 6, 0, 0, 0);
    CHECK(unmapGravityChild != None, "unmap gravity child creation failed");
    XSetWindowAttributes unmapGravAttrs;
    memset(&unmapGravAttrs, 0, sizeof(unmapGravAttrs));
    unmapGravAttrs.win_gravity = UnmapGravity;
    CHECK(XChangeWindowAttributes(display, unmapGravityChild, CWWinGravity,
                                  &unmapGravAttrs),
          "unmap gravity CWWinGravity failed");
    XSelectInput(display, unmapGravityChild, StructureNotifyMask);
    CHECK(XMapWindow(display, unmapGravityChild),
          "XMapWindow unmap gravity child failed");
    while (XCheckWindowEvent(display, unmapGravityChild, StructureNotifyMask,
                             &out)) {
    }
    CHECK(XResizeWindow(display, window, 68, 48),
          "configure-path parent resize failed");
    CHECK(XCheckTypedWindowEvent(display, unmapGravityChild, UnmapNotify, &out),
          "XResizeWindow did not unmap UnmapGravity child");
    CHECK(out.xunmap.from_configure,
          "configure-path UnmapNotify.from_configure was not True");
    XWindowAttributes ugAttrs;
    CHECK(XGetWindowAttributes(display, unmapGravityChild, &ugAttrs),
          "unmap gravity child attrs failed");
    CHECK(ugAttrs.map_state == IsUnmapped,
          "UnmapGravity child still mapped after XResizeWindow");
    while (XCheckTypedEvent(display, Expose, &out)) {
    }

    Window moveClip =
        XCreateSimpleWindow(display, window, 0, 0, 20, 10, 0, 0, 0);
    CHECK(moveClip != None, "move expose clip window creation failed");
    Window moveChild =
        XCreateSimpleWindow(display, moveClip, 0, 0, 20, 30, 0, 0, 0);
    CHECK(moveChild != None, "move expose child window creation failed");
    XSelectInput(display, moveChild, ExposureMask);
    CHECK(XMapWindow(display, moveChild), "move expose child map failed");
    CHECK(XMapWindow(display, moveClip), "move expose clip map failed");
    while (XCheckTypedWindowEvent(display, moveChild, Expose, &out)) {
    }
    CHECK(XMoveWindow(display, moveChild, 0, -10),
          "move expose child move failed");
    CHECK(XCheckTypedWindowEvent(display, moveChild, Expose, &out),
          "moving clipped child did not expose newly visible area");
    CHECK(out.xexpose.x <= 0 && out.xexpose.y <= 10 &&
              out.xexpose.x + out.xexpose.width >= 20 &&
              out.xexpose.y + out.xexpose.height >= 20,
          "moving clipped child expose did not cover newly visible area");
    while (XCheckTypedEvent(display, Expose, &out)) {
    }

    /* Host-driven resize (the SDL RESIZED path, entered here through
     * postSyntheticWindowResize) must honor child win_gravity: the gravity
     * child gets GravityNotify only, and an unmoved child gets neither
     * GravityNotify nor ConfigureNotify. GravityNotify is never generated at
     * map time, so it cleanly attributes to this resize.
     */
    Window hostResizeWin =
        XCreateSimpleWindow(display, root, 0, 0, 40, 40, 0, 0, 0);
    CHECK(hostResizeWin != None, "host resize window creation failed");
    XSelectInput(display, hostResizeWin, StructureNotifyMask);
    CHECK(XMapWindow(display, hostResizeWin), "host resize window map failed");
    Window hostStillChild =
        XCreateSimpleWindow(display, hostResizeWin, 1, 1, 10, 10, 0, 0, 0);
    CHECK(hostStillChild != None, "host resize still child creation failed");
    XSelectInput(display, hostStillChild, StructureNotifyMask);
    CHECK(XMapWindow(display, hostStillChild),
          "host resize still child map failed");
    Window hostGravityChild =
        XCreateSimpleWindow(display, hostResizeWin, 2, 2, 6, 6, 0, 0, 0);
    CHECK(hostGravityChild != None,
          "host resize gravity child creation failed");
    XSetWindowAttributes hostGravAttrs;
    memset(&hostGravAttrs, 0, sizeof(hostGravAttrs));
    hostGravAttrs.win_gravity = EastGravity;
    CHECK(XChangeWindowAttributes(display, hostGravityChild, CWWinGravity,
                                  &hostGravAttrs),
          "host resize gravity CWWinGravity failed");
    XSelectInput(display, hostGravityChild, StructureNotifyMask);
    CHECK(XMapWindow(display, hostGravityChild),
          "host resize gravity child map failed");
    Window hostUnmapChild =
        XCreateSimpleWindow(display, hostResizeWin, 3, 3, 5, 5, 0, 0, 0);
    CHECK(hostUnmapChild != None, "host resize unmap child creation failed");
    XSetWindowAttributes hostUnmapAttrs;
    memset(&hostUnmapAttrs, 0, sizeof(hostUnmapAttrs));
    hostUnmapAttrs.win_gravity = UnmapGravity;
    CHECK(XChangeWindowAttributes(display, hostUnmapChild, CWWinGravity,
                                  &hostUnmapAttrs),
          "host resize unmap CWWinGravity failed");
    XSelectInput(display, hostUnmapChild, StructureNotifyMask);
    CHECK(XMapWindow(display, hostUnmapChild),
          "host resize unmap child map failed");
    while (XCheckWindowEvent(display, hostGravityChild, StructureNotifyMask,
                             &out)) {
    }
    while (
        XCheckWindowEvent(display, hostStillChild, StructureNotifyMask, &out)) {
    }
    while (
        XCheckWindowEvent(display, hostUnmapChild, StructureNotifyMask, &out)) {
    }
    postSyntheticWindowResize(display, hostResizeWin, 60, 40);
    CHECK(
        XCheckTypedWindowEvent(display, hostGravityChild, GravityNotify, &out),
        "host resize did not move gravity child");
    CHECK(!XCheckTypedWindowEvent(display, hostGravityChild, ConfigureNotify,
                                  &out),
          "host resize gravity child got spurious ConfigureNotify");
    CHECK(!XCheckTypedWindowEvent(display, hostStillChild, GravityNotify, &out),
          "host resize gave unmoved child a spurious GravityNotify");
    CHECK(
        !XCheckTypedWindowEvent(display, hostStillChild, ConfigureNotify, &out),
        "host resize gave unmoved child a spurious ConfigureNotify");
    /* UnmapGravity children are unmapped by the resize, with an UnmapNotify
     * whose from_configure is True (the unmap is a side effect of the resize).
     */
    CHECK(XCheckTypedWindowEvent(display, hostUnmapChild, UnmapNotify, &out),
          "host resize did not unmap UnmapGravity child");
    CHECK(out.xunmap.from_configure,
          "host resize UnmapNotify.from_configure was not True");
    XWindowAttributes unmapChildAttrs;
    CHECK(XGetWindowAttributes(display, hostUnmapChild, &unmapChildAttrs),
          "host resize unmap child attrs failed");
    CHECK(unmapChildAttrs.map_state == IsUnmapped,
          "UnmapGravity child still mapped after parent resize");
    XDestroyWindow(display, hostResizeWin);

    XDestroyWindow(display, window);
    XDestroyWindow(display, resetWindow);
    SDL_zero(resetEvent);
    resetEvent.type = SDL_RENDER_TARGETS_RESET;
    SDL_PushEvent(&resetEvent);
    CHECK(!XCheckTypedEvent(display, Expose, &out),
          "render reset with no top-level children posted Expose");
    XFreePixmap(display, backgroundPixmap);
    return 1;
}

static int test_properties(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));

    /* _XSETTINGS_SETTINGS on the root window publishes a binary payload with at
     * least Gtk/FontName so GTK probes have real defaults.
     */
    Atom xs = XInternAtom(display, "_XSETTINGS_SETTINGS", False);
    CHECK(xs != None, "XInternAtom _XSETTINGS_SETTINGS failed");
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0, bytesAfter = 0;
    unsigned char *data = NULL;
    int rc = XGetWindowProperty(display, root, xs, 0, 1024, False,
                                AnyPropertyType, &actualType, &actualFormat,
                                &nItems, &bytesAfter, &data);
    CHECK(rc == Success, "XGetWindowProperty _XSETTINGS_SETTINGS failed");
    CHECK(actualType == xs && actualFormat == 8 && nItems > 16,
          "_XSETTINGS_SETTINGS payload shape wrong");
    CHECK(data != NULL, "_XSETTINGS_SETTINGS payload missing");
    int found = 0;
    for (unsigned long i = 0; i + 11 < nItems; i++) {
        if (memcmp(data + i, "Gtk/FontName", 12) == 0) {
            found = 1;
            break;
        }
    }
    CHECK(found, "_XSETTINGS_SETTINGS missing Gtk/FontName");
    XFree(data);

    /* _MOTIF_WM_HINTS: synthesize a default reply when unset; return the stored
     * value verbatim once the client writes one.
     */
    Window window = XCreateSimpleWindow(display, root, 0, 0, 32, 32, 0, 0, 0);
    Atom motif = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    CHECK(motif != None, "XInternAtom for _MOTIF_WM_HINTS failed");
    data = NULL;
    nItems = 0;
    bytesAfter = 0;
    rc = XGetWindowProperty(display, window, motif, 0, 5, False,
                            AnyPropertyType, &actualType, &actualFormat,
                            &nItems, &bytesAfter, &data);
    CHECK(rc == Success, "XGetWindowProperty(_MOTIF_WM_HINTS) failed");
    CHECK(actualType == motif && actualFormat == 32 && nItems == 5,
          "synthesized _MOTIF_WM_HINTS has wrong shape");
    CHECK(data != NULL, "synthesized _MOTIF_WM_HINTS data missing");
    long *values = (long *) data;
    CHECK(values[0] != 0, "synthesized flags should be non-zero");
    XFree(data);

    long stored[5] = {0x7, 0x3, 0x5, 0x1, 0x0};
    XChangeProperty(display, window, motif, motif, 32, PropModeReplace,
                    (unsigned char *) stored, 5);
    data = NULL;
    nItems = 0;
    bytesAfter = 0;
    rc = XGetWindowProperty(display, window, motif, 0, 5, False,
                            AnyPropertyType, &actualType, &actualFormat,
                            &nItems, &bytesAfter, &data);
    CHECK(rc == Success, "XGetWindowProperty after store failed");
    CHECK(actualType == motif && actualFormat == 32 && nItems == 5,
          "stored _MOTIF_WM_HINTS lost shape");
    long *back = (long *) data;
    CHECK(back[0] == 0x7 && back[1] == 0x3 && back[2] == 0x5 &&
              back[3] == 0x1 && back[4] == 0x0,
          "stored _MOTIF_WM_HINTS did not round-trip");
    XFree(data);

    char *cmd[] = {"app", "--flag", "value"};
    CHECK(XSetCommand(display, window, cmd, 3), "XSetCommand failed");
    char **cmdBack = NULL;
    int argcBack = 0;
    CHECK(XGetCommand(display, window, &cmdBack, &argcBack),
          "XGetCommand failed");
    CHECK(argcBack == 3 && !strcmp(cmdBack[0], "app") &&
              !strcmp(cmdBack[1], "--flag") && !strcmp(cmdBack[2], "value"),
          "WM_COMMAND did not round-trip");
    for (int i = 0; i < argcBack; i++)
        XFree(cmdBack[i]);
    XFree(cmdBack);

    char longArg[5000];
    memset(longArg, 'c', sizeof(longArg) - 1);
    longArg[sizeof(longArg) - 1] = '\0';
    char *longCmd[] = {"app", longArg, "tail"};
    CHECK(XSetCommand(display, window, longCmd, 3), "long XSetCommand failed");
    cmdBack = NULL;
    argcBack = 0;
    CHECK(XGetCommand(display, window, &cmdBack, &argcBack),
          "long XGetCommand failed");
    CHECK(argcBack == 3 && !strcmp(cmdBack[0], "app") &&
              !strcmp(cmdBack[1], longArg) && !strcmp(cmdBack[2], "tail"),
          "long WM_COMMAND did not round-trip");
    for (int i = 0; i < argcBack; i++)
        XFree(cmdBack[i]);
    XFree(cmdBack);

    XClassHint classHint = {.res_name = "sample", .res_class = "Sample"};
    CHECK(XSetClassHint(display, window, &classHint), "XSetClassHint failed");
    XClassHint classBack;
    CHECK(XGetClassHint(display, window, &classBack), "XGetClassHint failed");
    CHECK(!strcmp(classBack.res_name, "sample") &&
              !strcmp(classBack.res_class, "Sample"),
          "WM_CLASS did not round-trip");
    XFree(classBack.res_name);
    XFree(classBack.res_class);

    char longName[5000];
    char longClass[5000];
    memset(longName, 'n', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    memset(longClass, 'C', sizeof(longClass) - 1);
    longClass[sizeof(longClass) - 1] = '\0';
    XClassHint longClassHint = {.res_name = longName, .res_class = longClass};
    CHECK(XSetClassHint(display, window, &longClassHint),
          "long XSetClassHint failed");
    CHECK(XGetClassHint(display, window, &classBack),
          "long XGetClassHint failed");
    CHECK(!strcmp(classBack.res_name, longName) &&
              !strcmp(classBack.res_class, longClass),
          "long WM_CLASS did not round-trip");
    XFree(classBack.res_name);
    XFree(classBack.res_class);

    Window transientFor =
        XCreateSimpleWindow(display, root, 0, 0, 8, 8, 0, 0, 0);
    CHECK(transientFor != None, "transient parent creation failed");
    CHECK(XSetTransientForHint(display, window, transientFor),
          "XSetTransientForHint failed");
    Window transientBack = None;
    CHECK(XGetTransientForHint(display, window, &transientBack),
          "XGetTransientForHint failed");
    CHECK(transientBack == transientFor, "WM_TRANSIENT_FOR did not round-trip");

    XSelectInput(display, window, PropertyChangeMask);
    Atom notifyProp = XInternAtom(display, "SDL2X11_PROPERTY_NOTIFY", False);
    unsigned long notifyValue = 0x11111111;
    CHECK(XChangeProperty(display, window, notifyProp, XA_CARDINAL, 32,
                          PropModeReplace, (unsigned char *) &notifyValue, 1),
          "PropertyNotify XChangeProperty failed");
    XEvent propEvent;
    CHECK(XCheckTypedWindowEvent(display, window, PropertyNotify, &propEvent),
          "XChangeProperty did not post PropertyNotify");
    CHECK(propEvent.xproperty.atom == notifyProp &&
              propEvent.xproperty.state == PropertyNewValue,
          "XChangeProperty PropertyNotify fields were incorrect");
    CHECK(XDeleteProperty(display, window, notifyProp),
          "PropertyNotify XDeleteProperty failed");
    CHECK(XCheckTypedWindowEvent(display, window, PropertyNotify, &propEvent),
          "XDeleteProperty did not post PropertyNotify");
    CHECK(propEvent.xproperty.atom == notifyProp &&
              propEvent.xproperty.state == PropertyDelete,
          "XDeleteProperty PropertyNotify fields were incorrect");

    Atom rotateA = XInternAtom(display, "SDL2X11_ROTATE_A", False);
    Atom rotateB = XInternAtom(display, "SDL2X11_ROTATE_B", False);
    unsigned long rotateValueA = 0xaaaaaaaa;
    unsigned long rotateValueB = 0xbbbbbbbb;
    CHECK(XChangeProperty(display, window, rotateA, XA_CARDINAL, 32,
                          PropModeReplace, (unsigned char *) &rotateValueA, 1),
          "rotate property A setup failed");
    CHECK(XChangeProperty(display, window, rotateB, XA_CARDINAL, 32,
                          PropModeReplace, (unsigned char *) &rotateValueB, 1),
          "rotate property B setup failed");
    while (
        XCheckTypedWindowEvent(display, window, PropertyNotify, &propEvent)) {
    }
    Atom rotateProps[2] = {rotateA, rotateB};
    CHECK(XRotateWindowProperties(display, window, rotateProps, 2, 1),
          "XRotateWindowProperties failed");
    CHECK(XCheckTypedWindowEvent(display, window, PropertyNotify, &propEvent),
          "XRotateWindowProperties did not post first PropertyNotify");
    CHECK(propEvent.xproperty.state == PropertyNewValue,
          "rotate PropertyNotify state was incorrect");
    CHECK(XCheckTypedWindowEvent(display, window, PropertyNotify, &propEvent),
          "XRotateWindowProperties did not post second PropertyNotify");
    data = NULL;
    CHECK(XGetWindowProperty(display, window, rotateA, 0, 1, False, XA_CARDINAL,
                             &actualType, &actualFormat, &nItems, &bytesAfter,
                             &data) == Success,
          "XGetWindowProperty rotateA failed");
    CHECK(data && ((unsigned long *) data)[0] == rotateValueB,
          "rotateA did not receive rotateB value");
    XFree(data);

    /* XChangeProperty(_NET_WM_NAME, UTF8_STRING) must route through the
     * top-level title path so SDL's window title reflects what Motif/ GTK/Qt
     * published. Regression for the WM_NAME detour added to XChangeProperty.
     */
    {
        Window titleWin =
            XCreateSimpleWindow(display, root, 0, 0, 64, 64, 0, 0, 0);
        CHECK(titleWin != None, "title test window creation failed");
        CHECK(XMapWindow(display, titleWin), "title window map failed");
        Atom netWmName = XInternAtom(display, "_NET_WM_NAME", False);
        Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
        const char *desired = "compat-net-name-detour";
        CHECK(XChangeProperty(display, titleWin, netWmName, utf8, 8,
                              PropModeReplace, (unsigned char *) desired,
                              (int) strlen(desired)),
              "XChangeProperty(_NET_WM_NAME) failed");
        SDL_Window *sdlw = GET_WINDOW_STRUCT(titleWin)->sdlWindow;
        CHECK(sdlw != NULL, "title window has no SDL backing");
        const char *sdlTitle = SDL_GetWindowTitle(sdlw);
        CHECK(sdlTitle && !strcmp(sdlTitle, desired),
              "_NET_WM_NAME XChangeProperty did not update SDL title");

        /* Plain XA_WM_NAME with XA_STRING should detour identically. */
        const char *wmDesired = "compat-wm-name-detour";
        CHECK(XChangeProperty(display, titleWin, XA_WM_NAME, XA_STRING, 8,
                              PropModeReplace, (unsigned char *) wmDesired,
                              (int) strlen(wmDesired)),
              "XChangeProperty(WM_NAME) failed");
        sdlTitle = SDL_GetWindowTitle(sdlw);
        CHECK(sdlTitle && !strcmp(sdlTitle, wmDesired),
              "WM_NAME XChangeProperty did not update SDL title");

        /* Non-text property writes to WM_NAME (atom payload) must NOT touch the
         * title: detour only triggers on XA_STRING / UTF8_STRING /
         * COMPOUND_TEXT format=8 payloads.
         */
        Atom atomPayload = XA_CARDINAL;
        CHECK(
            XChangeProperty(display, titleWin, XA_WM_NAME, XA_ATOM, 32,
                            PropModeReplace, (unsigned char *) &atomPayload, 1),
            "XChangeProperty(WM_NAME, ATOM) failed");
        sdlTitle = SDL_GetWindowTitle(sdlw);
        CHECK(sdlTitle && !strcmp(sdlTitle, wmDesired),
              "non-text WM_NAME write should not change SDL title");
        XDestroyWindow(display, titleWin);
    }

    /* Xutf8TextPropertyToTextList must reject attacker-controlled nitems BEFORE
     * the calloc/memcpy or the +1 NUL terminator wraps SIZE_MAX and the
     * subsequent memcpy writes ULONG_MAX bytes off the heap. Additionally, only
     * UTF8_STRING / XA_STRING encodings are decoded verbatim; COMPOUND_TEXT and
     * unknown atoms must return XConverterNotFound, not silently mis-decode.
     */
    {
        XTextProperty tp;
        memset(&tp, 0, sizeof(tp));
        tp.value = (unsigned char *) "hello";
        tp.encoding = XInternAtom(display, "UTF8_STRING", False);
        tp.format = 8;
        tp.nitems = 5;
        char **listOut = NULL;
        int countOut = -1;
        CHECK(Xutf8TextPropertyToTextList(display, &tp, &listOut, &countOut) ==
                  Success,
              "Xutf8TextPropertyToTextList UTF8_STRING failed");
        CHECK(countOut == 1 && listOut && listOut[0] &&
                  !strcmp(listOut[0], "hello"),
              "UTF8_STRING decode did not round-trip");
        XFreeStringList(listOut);
        listOut = NULL;
        countOut = -1;

        tp.encoding = XA_STRING;
        CHECK(Xutf8TextPropertyToTextList(display, &tp, &listOut, &countOut) ==
                  Success,
              "Xutf8TextPropertyToTextList XA_STRING failed");
        CHECK(countOut == 1 && listOut && listOut[0] &&
                  !strcmp(listOut[0], "hello"),
              "XA_STRING decode did not round-trip");
        XFreeStringList(listOut);
        listOut = NULL;
        countOut = -1;

        unsigned char latin1Value[] = {'c', 'a', 'f', 0xe9};
        char utf8Cafe[] = {'c', 'a', 'f', '\xc3', '\xa9', '\0'};
        tp.value = latin1Value;
        tp.nitems = sizeof(latin1Value);
        CHECK(Xutf8TextPropertyToTextList(display, &tp, &listOut, &countOut) ==
                  Success,
              "Xutf8TextPropertyToTextList Latin-1 XA_STRING failed");
        CHECK(countOut == 1 && listOut && listOut[0] &&
                  !strcmp(listOut[0], utf8Cafe),
              "XA_STRING Latin-1 byte was not transcoded to UTF-8");
        XFreeStringList(listOut);
        listOut = NULL;
        countOut = -1;

        unsigned char multiValue[] = {'o', 'n', 'e', '\0', 't', 'w', 'o'};
        tp.value = multiValue;
        tp.encoding = XInternAtom(display, "UTF8_STRING", False);
        tp.nitems = sizeof(multiValue);
        CHECK(Xutf8TextPropertyToTextList(display, &tp, &listOut, &countOut) ==
                  Success,
              "Xutf8TextPropertyToTextList multi-string UTF8_STRING failed");
        CHECK(countOut == 2 && listOut && listOut[0] && listOut[1] &&
                  !strcmp(listOut[0], "one") && !strcmp(listOut[1], "two"),
              "UTF8_STRING NUL-separated list was not split");
        XFreeStringList(listOut);
        listOut = NULL;
        countOut = -1;

        tp.value = (unsigned char *) "hello";
        tp.nitems = 5;
        tp.encoding = XInternAtom(display, "COMPOUND_TEXT", False);
        CHECK(
            Xutf8TextPropertyToTextList(display, &tp, &listOut, &countOut) ==
                XConverterNotFound,
            "COMPOUND_TEXT must return XConverterNotFound, not silent decode");
        CHECK(listOut == NULL && countOut == 0,
              "unsupported encoding must leave list_ret NULL");

        tp.encoding = XA_STRING;
        tp.format = 16;
        CHECK(Xutf8TextPropertyToTextList(display, &tp, &listOut, &countOut) ==
                  XConverterNotFound,
              "non-8-bit format must return XConverterNotFound");

        tp.encoding = XA_STRING;
        tp.format = 8;
        tp.nitems = (unsigned long) INT_MAX + 1ul;
        CHECK(Xutf8TextPropertyToTextList(display, &tp, &listOut, &countOut) ==
                  XNoMemory,
              "huge nitems must reject before allocation");
        CHECK(listOut == NULL && countOut == 0,
              "overflow path must leave outputs cleared");

        CHECK(Xutf8TextPropertyToTextList(display, NULL, &listOut, &countOut) ==
                  XNoMemory,
              "NULL text_prop must reject");
    }

    /* XwcTextPropertyToTextList shares the NUL-splitting contract with the
     * UTF-8 path. ICCCM list properties (WM_COMMAND, etc.) round through the
     * wide-char surface inside libXaw text widgets, so a collapsed list there
     * cuts off every argv entry past the first.
     */
    {
        XTextProperty tp;
        memset(&tp, 0, sizeof(tp));
        unsigned char multiValue[] = {'o', 'n', 'e', '\0', 't', 'w', 'o'};
        tp.value = multiValue;
        tp.encoding = XInternAtom(display, "UTF8_STRING", False);
        tp.format = 8;
        tp.nitems = sizeof(multiValue);
        wchar_t **wlistOut = NULL;
        int wcountOut = -1;
        CHECK(XwcTextPropertyToTextList(display, &tp, &wlistOut, &wcountOut) ==
                  Success,
              "XwcTextPropertyToTextList multi-string UTF8_STRING failed");
        CHECK(wcountOut == 2 && wlistOut && wlistOut[0] && wlistOut[1] &&
                  wcslen(wlistOut[0]) == 3 && wlistOut[0][0] == L'o' &&
                  wlistOut[0][1] == L'n' && wlistOut[0][2] == L'e' &&
                  wcslen(wlistOut[1]) == 3 && wlistOut[1][0] == L't' &&
                  wlistOut[1][1] == L'w' && wlistOut[1][2] == L'o',
              "Xwc NUL-separated UTF-8 list was not split");
        XwcFreeStringList(wlistOut);
        wlistOut = NULL;
        wcountOut = -1;

        unsigned char latin1Value[] = {'c', 'a', 'f', 0xe9};
        tp.value = latin1Value;
        tp.encoding = XA_STRING;
        tp.nitems = sizeof(latin1Value);
        CHECK(XwcTextPropertyToTextList(display, &tp, &wlistOut, &wcountOut) ==
                  Success,
              "XwcTextPropertyToTextList Latin-1 XA_STRING failed");
        CHECK(wcountOut == 1 && wlistOut && wlistOut[0] &&
                  wlistOut[0][0] == L'c' && wlistOut[0][1] == L'a' &&
                  wlistOut[0][2] == L'f' &&
                  (FcChar32) wlistOut[0][3] == 0xe9u && wlistOut[0][4] == 0,
              "XA_STRING Latin-1 byte was not widened to its codepoint");
        XwcFreeStringList(wlistOut);
        wlistOut = NULL;
        wcountOut = -1;

        tp.nitems = (unsigned long) INT_MAX + 1ul;
        CHECK(XwcTextPropertyToTextList(display, &tp, &wlistOut, &wcountOut) ==
                  XNoMemory,
              "Xwc huge nitems must reject before allocation");
    }

    /* Wide-char text properties must label bytes with the encoding actually
     * emitted. Non-UTF-8 styles use the same 8-bit Latin-1 path that
     * XwcTextPropertyToTextList decodes for XA_STRING, TEXT, and COMPOUND_TEXT.
     */
    {
        wchar_t cafe[] = {L'c', L'a', L'f', (wchar_t) 0xe9, 0};
        wchar_t *wlistIn[] = {cafe};
        XTextProperty tp;
        wchar_t **wlistOut = NULL;
        int wcountOut = -1;

        CHECK(XwcTextListToTextProperty(display, wlistIn, 1, XStringStyle,
                                        &tp) == Success,
              "XwcTextListToTextProperty XStringStyle failed");
        CHECK(tp.encoding == XA_STRING && tp.format == 8 && tp.nitems == 4 &&
                  tp.value && tp.value[0] == 'c' && tp.value[1] == 'a' &&
                  tp.value[2] == 'f' && tp.value[3] == 0xe9,
              "XStringStyle must emit Latin-1 bytes");
        CHECK(XwcTextPropertyToTextList(display, &tp, &wlistOut, &wcountOut) ==
                  Success,
              "XStringStyle round-trip decode failed");
        CHECK(wcountOut == 1 && wlistOut && wlistOut[0] &&
                  wlistOut[0][0] == L'c' && wlistOut[0][1] == L'a' &&
                  wlistOut[0][2] == L'f' &&
                  (FcChar32) wlistOut[0][3] == 0xe9u && wlistOut[0][4] == 0,
              "XStringStyle wide-char round-trip corrupted Latin-1");
        XwcFreeStringList(wlistOut);
        XFree(tp.value);
        wlistOut = NULL;
        wcountOut = -1;

        CHECK(XwcTextListToTextProperty(display, wlistIn, 1, XTextStyle, &tp) ==
                  Success,
              "XwcTextListToTextProperty XTextStyle failed");
        CHECK(tp.encoding == XInternAtom(display, "TEXT", False) &&
                  tp.nitems == 4 && tp.value && tp.value[3] == 0xe9,
              "XTextStyle must emit Latin-1 bytes");
        CHECK(XwcTextPropertyToTextList(display, &tp, &wlistOut, &wcountOut) ==
                  Success,
              "XTextStyle round-trip decode failed");
        CHECK(wcountOut == 1 && wlistOut && wlistOut[0] &&
                  (FcChar32) wlistOut[0][3] == 0xe9u && wlistOut[0][4] == 0,
              "XTextStyle wide-char round-trip corrupted Latin-1");
        XwcFreeStringList(wlistOut);
        XFree(tp.value);
        wlistOut = NULL;
        wcountOut = -1;

        CHECK(XwcTextListToTextProperty(display, wlistIn, 1, XCompoundTextStyle,
                                        &tp) == Success,
              "XwcTextListToTextProperty XCompoundTextStyle failed");
        CHECK(tp.encoding == XInternAtom(display, "COMPOUND_TEXT", False) &&
                  tp.nitems == 4 && tp.value && tp.value[3] == 0xe9,
              "XCompoundTextStyle must emit Latin-1 bytes");
        CHECK(XwcTextPropertyToTextList(display, &tp, &wlistOut, &wcountOut) ==
                  Success,
              "XCompoundTextStyle round-trip decode failed");
        CHECK(wcountOut == 1 && wlistOut && wlistOut[0] &&
                  (FcChar32) wlistOut[0][3] == 0xe9u && wlistOut[0][4] == 0,
              "XCompoundTextStyle wide-char round-trip corrupted Latin-1");
        XwcFreeStringList(wlistOut);
        XFree(tp.value);
        wlistOut = NULL;
        wcountOut = -1;

        CHECK(XwcTextListToTextProperty(display, wlistIn, 1, XUTF8StringStyle,
                                        &tp) == Success,
              "XwcTextListToTextProperty XUTF8StringStyle failed");
        CHECK(tp.encoding == XInternAtom(display, "UTF8_STRING", False) &&
                  tp.nitems == 5 && tp.value && tp.value[0] == 'c' &&
                  tp.value[1] == 'a' && tp.value[2] == 'f' &&
                  tp.value[3] == 0xc3 && tp.value[4] == 0xa9,
              "XUTF8StringStyle must keep UTF-8 bytes");
        CHECK(XwcTextPropertyToTextList(display, &tp, &wlistOut, &wcountOut) ==
                  Success,
              "XUTF8StringStyle round-trip decode failed");
        CHECK(wcountOut == 1 && wlistOut && wlistOut[0] &&
                  (FcChar32) wlistOut[0][3] == 0xe9u && wlistOut[0][4] == 0,
              "XUTF8StringStyle wide-char round-trip corrupted UTF-8");
        XwcFreeStringList(wlistOut);
        XFree(tp.value);
    }

    /* XGetWMProtocols reads the WM_PROTOCOLS atom list. A window with no such
     * property must report zero protocols; once set, the list round-trips.
     */
    {
        Atom *protoBack = NULL;
        int protoCount = -1;
        Window bareWindow =
            XCreateSimpleWindow(display, root, 0, 0, 8, 8, 0, 0, 0);
        CHECK(
            XGetWMProtocols(display, bareWindow, &protoBack, &protoCount) == 0,
            "XGetWMProtocols must report failure when WM_PROTOCOLS is absent");
        CHECK(protoCount == 0 && protoBack == NULL,
              "XGetWMProtocols absent case must yield no protocols");

        Atom wmProtocols = XInternAtom(display, "WM_PROTOCOLS", False);
        Atom wmDelete = XInternAtom(display, "WM_DELETE_WINDOW", False);
        Atom wmTakeFocus = XInternAtom(display, "WM_TAKE_FOCUS", False);
        Atom protoSet[2] = {wmDelete, wmTakeFocus};
        XChangeProperty(display, bareWindow, wmProtocols, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *) protoSet, 2);
        protoBack = NULL;
        protoCount = -1;
        CHECK(XGetWMProtocols(display, bareWindow, &protoBack, &protoCount),
              "XGetWMProtocols must succeed once WM_PROTOCOLS is set");
        CHECK(protoCount == 2 && protoBack && protoBack[0] == wmDelete &&
                  protoBack[1] == wmTakeFocus,
              "WM_PROTOCOLS atom list did not round-trip");
        XFree(protoBack);
        XDestroyWindow(display, bareWindow);
    }

    XDestroyWindow(display, transientFor);
    XDestroyWindow(display, window);
    return 1;
}

/* EWMH _NET_WM_STATE ClientMessage routing: a _NET_WM_STATE ClientMessage
 * targeting the root window must be dispatched to the in-process WM rather than
 * walked up the event-mask propagation chain. The stored _NET_WM_STATE property
 * must mirror the post-action atom set.
 */
static int test_ewmh_wm_state_clientmessage(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 0, 0, 64, 64, 0, 0, 0);
    CHECK(window != None, "ewmh: window creation failed");
    CHECK(XMapWindow(display, window), "ewmh: window map failed");

    Atom netWmState = XInternAtom(display, "_NET_WM_STATE", False);
    Atom netWmStateFullscreen =
        XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    CHECK(netWmState != None && netWmStateFullscreen != None,
          "ewmh: atom intern failed");

    XEvent event = {
        .xclient = {
            .type = ClientMessage,
            .window = window,
            .message_type = netWmState,
            .format = 32,
            .data.l = {1 /* _NET_WM_STATE_ADD */, (long) netWmStateFullscreen},
        }};

    /* SendEvent to root should be consumed by the EWMH dispatch and NOT
     * propagate as a queued ClientMessage to root or to the target.
     */
    CHECK(XSendEvent(display, root, False,
                     SubstructureNotifyMask | SubstructureRedirectMask, &event),
          "ewmh: XSendEvent to root failed");

    XEvent drained;
    CHECK(!XCheckTypedEvent(display, ClientMessage, &drained),
          "ewmh: EWMH ClientMessage to root leaked into the event queue");

    /* Property mirror: stored _NET_WM_STATE must now include the fullscreen
     * atom.
     */
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0, bytesAfter = 0;
    unsigned char *data = NULL;
    int rc = XGetWindowProperty(display, window, netWmState, 0, 16, False,
                                XA_ATOM, &actualType, &actualFormat, &nItems,
                                &bytesAfter, &data);
    CHECK(rc == Success, "ewmh: XGetWindowProperty(_NET_WM_STATE) failed");
    CHECK(actualType == XA_ATOM && actualFormat == 32,
          "ewmh: _NET_WM_STATE has wrong type/format after mirror");
    Bool sawFullscreen = False;
    if (data) {
        Atom *atoms = (Atom *) data;
        for (unsigned long i = 0; i < nItems; i++) {
            if (atoms[i] == netWmStateFullscreen) {
                sawFullscreen = True;
                break;
            }
        }
    }
    XFree(data);
    CHECK(sawFullscreen,
          "ewmh: _NET_WM_STATE property mirror missed FULLSCREEN");

    /* Toggle action removes the atom. */
    event.xclient.data.l[0] = 2; /* _NET_WM_STATE_TOGGLE */
    CHECK(XSendEvent(display, root, False, 0, &event),
          "ewmh: toggle XSendEvent failed");
    data = NULL;
    nItems = 0;
    bytesAfter = 0;
    rc = XGetWindowProperty(display, window, netWmState, 0, 16, False, XA_ATOM,
                            &actualType, &actualFormat, &nItems, &bytesAfter,
                            &data);
    CHECK(rc == Success, "ewmh: XGetWindowProperty after toggle failed");
    Bool stillSawFullscreen = False;
    if (data) {
        Atom *atoms = (Atom *) data;
        for (unsigned long i = 0; i < nItems; i++) {
            if (atoms[i] == netWmStateFullscreen) {
                stillSawFullscreen = True;
                break;
            }
        }
    }
    XFree(data);
    CHECK(!stillSawFullscreen, "ewmh: toggle did not clear FULLSCREEN atom");

    /* Unrecognized message_types fall through. Send a ClientMessage with a
     * private message_type to root with no propagate; the routing should not
     * queue it (no event mask matched on root).
     */
    Atom privateAtom = XInternAtom(display, "SDL2X11_PRIVATE_CLIENT", False);
    event.xclient.message_type = privateAtom;
    CHECK(XSendEvent(display, root, False, NoEventMask, &event),
          "ewmh: private ClientMessage send failed");
    /* NoEventMask should accept on root; either way the dispatch is not
     * involved. Drain any queued events to keep the test isolated.
     */
    while (XCheckTypedEvent(display, ClientMessage, &drained))
        ;

    XDestroyWindow(display, window);
    return 1;
}

static int test_ewmh_wm_state_initial_property(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 0, 0, 64, 64, 0, 0, 0);
    CHECK(window != None, "ewmh-initial: window creation failed");

    Atom netWmState = XInternAtom(display, "_NET_WM_STATE", False);
    Atom netWmStateFullscreen =
        XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False);
    CHECK(netWmState != None && netWmStateFullscreen != None,
          "ewmh-initial: atom intern failed");

    Atom states[1] = {netWmStateFullscreen};
    CHECK(XChangeProperty(display, window, netWmState, XA_ATOM, 32,
                          PropModeReplace, (unsigned char *) states, 1),
          "ewmh-initial: _NET_WM_STATE write failed");
    CHECK(GET_WINDOW_STRUCT(window)->sdlWindow == NULL,
          "ewmh-initial: property write realized the window too early");

    CHECK(XMapWindow(display, window), "ewmh-initial: window map failed");
    SDL_Window *sdlWindow = GET_WINDOW_STRUCT(window)->sdlWindow;
    CHECK(sdlWindow != NULL, "ewmh-initial: window has no SDL backing");
    CHECK(wait_window_flag(sdlWindow, SDL_WINDOW_FULLSCREEN, True),
          "ewmh-initial: initial FULLSCREEN was not applied on map");

    CHECK(XChangeProperty(display, window, netWmState, XA_ATOM, 32,
                          PropModeReplace, NULL, 0),
          "ewmh-initial: empty _NET_WM_STATE rewrite failed");
    CHECK(wait_window_flag(sdlWindow, SDL_WINDOW_FULLSCREEN, False),
          "ewmh-initial: direct _NET_WM_STATE rewrite did not clear SDL state");

    XDestroyWindow(display, window);
    return 1;
}

/* EWMH _NET_ACTIVE_WINDOW ClientMessage routing: a request sent to root is
 * consumed by the in-process WM dispatch and never leaks into the client event
 * queue. Skip target inspection for unmapped or invalid windows.
 */
static int test_ewmh_active_window_clientmessage(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 0, 0, 64, 64, 0, 0, 0);
    CHECK(window != None, "ewmh-active: window creation failed");
    CHECK(XMapWindow(display, window), "ewmh-active: window map failed");

    Atom activeWindow = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    CHECK(activeWindow != None, "ewmh-active: atom intern failed");

    XEvent event = {.xclient = {
                        .type = ClientMessage,
                        .window = window,
                        .message_type = activeWindow,
                        .format = 32,
                        .data.l = {1 /* source: application */, CurrentTime},
                    }};

    CHECK(XSendEvent(display, root, False,
                     SubstructureNotifyMask | SubstructureRedirectMask, &event),
          "ewmh-active: XSendEvent to root failed");

    XEvent drained;
    CHECK(!XCheckTypedEvent(display, ClientMessage, &drained),
          "ewmh-active: ClientMessage leaked into the event queue");

    /* Invalid target window: dispatch must still consume the message silently
     * and not crash on the windowless target lookup.
     */
    event.xclient.window = (Window) 0xdeadbeefUL;
    CHECK(XSendEvent(display, root, False,
                     SubstructureNotifyMask | SubstructureRedirectMask, &event),
          "ewmh-active: invalid-target send failed");
    CHECK(!XCheckTypedEvent(display, ClientMessage, &drained),
          "ewmh-active: invalid-target message leaked");

    XDestroyWindow(display, window);
    return 1;
}

/* EWMH _NET_CLOSE_WINDOW ClientMessage routing: when the target has
 * WM_DELETE_WINDOW listed in WM_PROTOCOLS the dispatcher posts a WM_PROTOCOLS /
 * WM_DELETE_WINDOW ClientMessage to the target rather than unmapping; otherwise
 * it unmaps the window.
 */
static int test_ewmh_close_window_clientmessage(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 0, 0, 64, 64, 0, 0, 0);
    CHECK(window != None, "ewmh-close: window creation failed");
    CHECK(XMapWindow(display, window), "ewmh-close: window map failed");
    XSelectInput(display, window, StructureNotifyMask);

    Atom closeWindow = XInternAtom(display, "_NET_CLOSE_WINDOW", False);
    Atom wmProtocols = XInternAtom(display, "WM_PROTOCOLS", False);
    Atom wmDeleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
    CHECK(closeWindow != None && wmProtocols != None && wmDeleteWindow != None,
          "ewmh-close: atom intern failed");

    /* Path A: target advertises WM_DELETE_WINDOW. Dispatch must post a
     * WM_PROTOCOLS / WM_DELETE_WINDOW ClientMessage to the target and leave the
     * window mapped so the client can decide when to tear it down.
     */
    Atom protocols[1] = {wmDeleteWindow};
    CHECK(XSetWMProtocols(display, window, protocols, 1),
          "ewmh-close: XSetWMProtocols failed");

    /* Verify the property landed so a dispatch mismatch is distinguishable from
     * a missed-post: read WM_PROTOCOLS back and confirm it lists
     * WM_DELETE_WINDOW under XA_ATOM.
     */
    Atom storedType = None;
    int storedFormat = 0;
    unsigned long storedItems = 0, storedAfter = 0;
    unsigned char *storedData = NULL;
    CHECK(XGetWindowProperty(display, window, wmProtocols, 0, 1, False, XA_ATOM,
                             &storedType, &storedFormat, &storedItems,
                             &storedAfter, &storedData) == Success,
          "ewmh-close: XGetWindowProperty(WM_PROTOCOLS) failed");
    CHECK(storedType == XA_ATOM && storedFormat == 32 && storedItems == 1 &&
              storedData && ((Atom *) storedData)[0] == wmDeleteWindow,
          "ewmh-close: WM_PROTOCOLS missing WM_DELETE_WINDOW after store");
    XFree(storedData);
    Atom *readProtocols = NULL;
    int protocolCount = 0;
    CHECK(XGetWMProtocols(display, window, &readProtocols, &protocolCount),
          "ewmh-close: XGetWMProtocols failed");
    CHECK(protocolCount == 1 && readProtocols &&
              readProtocols[0] == wmDeleteWindow,
          "ewmh-close: XGetWMProtocols did not read back WM_DELETE_WINDOW");
    XFree(readProtocols);

    XEvent event = {.xclient = {
                        .type = ClientMessage,
                        .window = window,
                        .message_type = closeWindow,
                        .format = 32,
                    }};

    CHECK(XSendEvent(display, root, False,
                     SubstructureNotifyMask | SubstructureRedirectMask, &event),
          "ewmh-close: XSendEvent(WM_DELETE path) failed");

    XEvent delivered;
    Bool sawWmDelete = False;
    while (XCheckTypedWindowEvent(display, window, ClientMessage, &delivered)) {
        if (delivered.xclient.message_type == wmProtocols &&
            (Atom) delivered.xclient.data.l[0] == wmDeleteWindow) {
            sawWmDelete = True;
            break;
        }
    }
    CHECK(sawWmDelete,
          "ewmh-close: WM_DELETE_WINDOW ClientMessage not delivered to target");
    CHECK(GET_WINDOW_STRUCT(window)->mapState == Mapped,
          "ewmh-close: window unmapped despite WM_DELETE_WINDOW handler");

    SDL_Event closeEvent;
    SDL_zero(closeEvent);
    XC_INIT_WINDOW_EVENT(&closeEvent);
    closeEvent.window.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    XC_SET_WINDOW_SUBEVENT(&closeEvent, SDL_WINDOWEVENT_CLOSE);
    XEvent converted;
    CHECK(convertEvent(display, &closeEvent, &converted, True) < 0,
          "ewmh-close: SDL close event was not consumed internally");
    sawWmDelete = False;
    while (XCheckTypedWindowEvent(display, window, ClientMessage, &delivered)) {
        if (delivered.xclient.message_type == wmProtocols &&
            (Atom) delivered.xclient.data.l[0] == wmDeleteWindow) {
            sawWmDelete = True;
            break;
        }
    }
    CHECK(sawWmDelete,
          "ewmh-close: SDL close did not deliver WM_DELETE_WINDOW");

    XC_SET_WINDOW_SUBEVENT(&closeEvent, SDL_WINDOWEVENT_CLOSE);
    CHECK(SDL_PushEvent(&closeEvent) == 1,
          "ewmh-close: SDL close event was filtered before conversion");
    /* The window selects StructureNotifyMask, so map/configure notifications
     * can sit in the queue ahead of the WM_DELETE_WINDOW ClientMessage the SDL
     * close generates. Drain ClientMessages for this window and keep the one
     * that matches WM_PROTOCOLS / WM_DELETE_WINDOW, so a stale same-window
     * ClientMessage cannot make the assertion stop on the wrong event. Pump
     * before each drain so an event surfaced by the final pump is still seen.
     */
    sawWmDelete = False;
    for (int spins = 0; spins <= 500 && !sawWmDelete; spins++) {
        if (spins > 0) {
            SDL_PumpEvents();
            SDL_Delay(1);
        }
        while (XCheckTypedWindowEvent(display, window, ClientMessage,
                                      &delivered)) {
            if (delivered.xclient.window == window &&
                delivered.xclient.message_type == wmProtocols &&
                (Atom) delivered.xclient.data.l[0] == wmDeleteWindow) {
                sawWmDelete = True;
                break;
            }
        }
    }
    CHECK(sawWmDelete,
          "ewmh-close: XNextEvent did not deliver SDL close WM_DELETE_WINDOW");
    CHECK(
        GET_WINDOW_STRUCT(window)->mapState == Mapped,
        "ewmh-close: SDL close changed map state before client handled delete");

    /* Path B: a window without WM_DELETE_WINDOW in WM_PROTOCOLS is unmapped
     * instead, mirroring SDL_WINDOWEVENT_CLOSE on a client that did not opt in
     * to the delete protocol.
     */
    Window plain = XCreateSimpleWindow(display, root, 0, 0, 48, 48, 0, 0, 0);
    CHECK(plain != None, "ewmh-close: plain window creation failed");
    CHECK(XMapWindow(display, plain), "ewmh-close: plain window map failed");
    CHECK(GET_WINDOW_STRUCT(plain)->mapState == Mapped,
          "ewmh-close: plain window unexpectedly not mapped");

    event.xclient.window = plain;
    CHECK(XSendEvent(display, root, False,
                     SubstructureNotifyMask | SubstructureRedirectMask, &event),
          "ewmh-close: XSendEvent(unmap path) failed");

    CHECK(GET_WINDOW_STRUCT(plain)->mapState == UnMapped,
          "ewmh-close: plain window not unmapped");

    XEvent drained;
    while (XCheckTypedEvent(display, ClientMessage, &drained))
        ;

    XDestroyWindow(display, plain);
    XDestroyWindow(display, window);
    return 1;
}

/* MWM _MOTIF_WM_HINTS write routing: a XChangeProperty write that clears
 * MWM_DECOR_BORDER must drop the SDL window border; clearing MWM_FUNC_RESIZE
 * must drop SDL_WINDOW_RESIZABLE. Under SDL's dummy video driver
 * SDL_SetWindowBordered / SDL_SetWindowResizable are no-ops because the driver
 * has no host window. The test probes for that and falls back to verifying the
 * property storage round-trip when the driver does not honor the flag flip.
 */
static int test_mwm_hints_apply(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 0, 0, 96, 64, 0, 0, 0);
    CHECK(window != None, "motif-hints: window creation failed");
    CHECK(XMapWindow(display, window), "motif-hints: window map failed");

    SDL_Window *sdlWindow = GET_WINDOW_STRUCT(window)->sdlWindow;
    CHECK(sdlWindow != NULL, "motif-hints: window has no SDL backing");

    /* Driver-capability probe. Dummy: SetWindowBordered is unimplemented so
     * SDL_GetWindowFlags never reports SDL_WINDOW_BORDERLESS even after a
     * successful call. Real drivers honor the toggle.
     */
    Uint32 flagsBefore = SDL_GetWindowFlags(sdlWindow);
    SDL_SetWindowBordered(sdlWindow, SDL_FALSE);
    Uint32 flagsAfter = SDL_GetWindowFlags(sdlWindow);
    SDL_SetWindowBordered(sdlWindow, SDL_TRUE); /* restore for the test */
    Bool driverHonorsBorder = (flagsAfter & SDL_WINDOW_BORDERLESS) !=
                              (flagsBefore & SDL_WINDOW_BORDERLESS);
#if SDL_VERSION_ATLEAST(2, 0, 5)
    flagsBefore = SDL_GetWindowFlags(sdlWindow);
    SDL_SetWindowResizable(sdlWindow, SDL_FALSE);
    flagsAfter = SDL_GetWindowFlags(sdlWindow);
    SDL_SetWindowResizable(sdlWindow, SDL_TRUE);
    Bool driverHonorsResizable = (flagsAfter & SDL_WINDOW_RESIZABLE) !=
                                 (flagsBefore & SDL_WINDOW_RESIZABLE);
#else
    Bool driverHonorsResizable = False;
#endif

    Atom motif = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    CHECK(motif != None, "motif-hints: atom intern failed");

    Window premap = XCreateSimpleWindow(display, root, 0, 0, 72, 48, 0, 0, 0);
    CHECK(premap != None, "motif-hints: premap window creation failed");
    long premapHints[5] = {
        0x3 /* MWM_HINTS_FUNCTIONS | MWM_HINTS_DECORATIONS */,
        0x1 /* MWM_FUNC_ALL */,
        0 /* no decorations */,
        0,
        0,
    };
    CHECK(XChangeProperty(display, premap, motif, motif, 32, PropModeReplace,
                          (unsigned char *) premapHints, 5),
          "motif-hints: premap XChangeProperty failed");
    CHECK(GET_WINDOW_STRUCT(premap)->sdlWindow == NULL,
          "motif-hints: premap write realized window too early");
    CHECK(XMapWindow(display, premap), "motif-hints: premap map failed");
    SDL_Window *premapSdl = GET_WINDOW_STRUCT(premap)->sdlWindow;
    CHECK(premapSdl != NULL, "motif-hints: premap window has no SDL backing");
    if (driverHonorsBorder) {
        Uint32 flags = SDL_GetWindowFlags(premapSdl);
        CHECK((flags & SDL_WINDOW_BORDERLESS) != 0,
              "motif-hints: premap decorations did not drop SDL border");
    }
    if (driverHonorsResizable) {
        Uint32 flags = SDL_GetWindowFlags(premapSdl);
        CHECK((flags & SDL_WINDOW_RESIZABLE) != 0,
              "motif-hints: premap FUNC_ALL did not enable resize");
    }
    XDestroyWindow(display, premap);

    Atom reparentNetWmState = XInternAtom(display, "_NET_WM_STATE", False);
    Atom reparentModalAtom = XInternAtom(display, "_NET_WM_STATE_MODAL", False);
    Atom reparentStates[1] = {reparentModalAtom};

    Window shell = XCreateSimpleWindow(display, root, 0, 0, 96, 64, 0, 0, 0);
    Window child = XCreateSimpleWindow(display, shell, 0, 0, 72, 48, 0, 0, 0);
    CHECK(shell != None && child != None, "motif-hints: reparent setup failed");
    CHECK(XChangeProperty(display, child, motif, motif, 32, PropModeReplace,
                          (unsigned char *) premapHints, 5),
          "motif-hints: reparent XChangeProperty failed");
    /* Modal + transient_for on window (already mapped above) so the replay has
     * a driver-independent observable: deferredTransientApplied flips iff
     * applyTransientForRelationship ran after the reparent. The border / resize
     * assertions below stay no-ops on the SDL dummy driver, but this flag
     * survives across drivers.
     */
    CHECK(XChangeProperty(display, child, reparentNetWmState, XA_ATOM, 32,
                          PropModeReplace, (unsigned char *) reparentStates, 1),
          "motif-hints: reparent modal write failed");
    CHECK(XSetTransientForHint(display, child, window),
          "motif-hints: reparent transient_for failed");
    CHECK(XMapWindow(display, shell), "motif-hints: shell map failed");
    CHECK(XMapWindow(display, child), "motif-hints: child map failed");
    CHECK(XReparentWindow(display, child, root, 0, 0),
          "motif-hints: reparent to root failed");
    SDL_Window *reparentedSdl = GET_WINDOW_STRUCT(child)->sdlWindow;
    CHECK(reparentedSdl != NULL,
          "motif-hints: reparented top-level has no SDL backing");
#if SDL_VERSION_ATLEAST(2, 0, 5)
    CHECK(GET_WINDOW_STRUCT(child)->deferredTransientApplied,
          "motif-hints: reparent did not run deferred WM replay");
#endif
    if (driverHonorsBorder) {
        Uint32 flags = SDL_GetWindowFlags(reparentedSdl);
        CHECK((flags & SDL_WINDOW_BORDERLESS) != 0,
              "motif-hints: reparent replay did not drop SDL border");
    }
    if (driverHonorsResizable) {
        Uint32 flags = SDL_GetWindowFlags(reparentedSdl);
        CHECK((flags & SDL_WINDOW_RESIZABLE) != 0,
              "motif-hints: reparent replay did not enable resize");
    }
    XDestroyWindow(display, child);
    XDestroyWindow(display, shell);

    /* MapRequested to Mapped promotion: a child mapped under an unmapped shell
     * sits at MapRequested. Reparenting to root must promote it to Mapped so
     * XGetWindowAttributes does not report IsUnviewable on a now-top-level
     * window, and the new replay site must still fire.
     */
    Window unmappedShell =
        XCreateSimpleWindow(display, root, 0, 0, 96, 64, 0, 0, 0);
    Window mrChild =
        XCreateSimpleWindow(display, unmappedShell, 0, 0, 72, 48, 0, 0, 0);
    CHECK(unmappedShell != None && mrChild != None,
          "motif-hints: map-requested setup failed");
    CHECK(XChangeProperty(display, mrChild, motif, motif, 32, PropModeReplace,
                          (unsigned char *) premapHints, 5),
          "motif-hints: map-requested motif write failed");
    CHECK(XChangeProperty(display, mrChild, reparentNetWmState, XA_ATOM, 32,
                          PropModeReplace, (unsigned char *) reparentStates, 1),
          "motif-hints: map-requested modal write failed");
    CHECK(XSetTransientForHint(display, mrChild, window),
          "motif-hints: map-requested transient_for failed");
    CHECK(XMapWindow(display, mrChild),
          "motif-hints: map-requested child map failed");
    CHECK(GET_WINDOW_STRUCT(mrChild)->mapState == MapRequested,
          "motif-hints: child under unmapped shell did not enter MapRequested");
    CHECK(XReparentWindow(display, mrChild, root, 0, 0),
          "motif-hints: map-requested reparent failed");
    CHECK(GET_WINDOW_STRUCT(mrChild)->sdlWindow != NULL,
          "motif-hints: map-requested reparent did not realize");
    CHECK(GET_WINDOW_STRUCT(mrChild)->mapState == Mapped,
          "motif-hints: reparent-to-root did not promote MapRequested");
#if SDL_VERSION_ATLEAST(2, 0, 5)
    CHECK(GET_WINDOW_STRUCT(mrChild)->deferredTransientApplied,
          "motif-hints: reparent-to-root did not run deferred WM replay");
#endif
    XDestroyWindow(display, mrChild);
    XDestroyWindow(display, unmappedShell);

    /* Decorations: explicit decoration set EXCLUDING border / title bits.
     * Without MWM_DECOR_ALL, only the listed bits are honored, so an empty set
     * means "no decorations" → borderless window.
     */
    long hints[5] = {
        0x2 /* MWM_HINTS_DECORATIONS */, 0, 0 /* no decorations */, 0, 0,
    };
    CHECK(XChangeProperty(display, window, motif, motif, 32, PropModeReplace,
                          (unsigned char *) hints, 5),
          "motif-hints: XChangeProperty failed");
    if (driverHonorsBorder) {
        Uint32 flags = SDL_GetWindowFlags(sdlWindow);
        CHECK((flags & SDL_WINDOW_BORDERLESS) != 0,
              "motif-hints: cleared decorations did not drop SDL border");
    }

    /* Decorations restored to a set INCLUDING border AND title → bordered. */
    hints[2] = 0xa; /* MWM_DECOR_BORDER (1<<1) | MWM_DECOR_TITLE (1<<3) */
    CHECK(XChangeProperty(display, window, motif, motif, 32, PropModeReplace,
                          (unsigned char *) hints, 5),
          "motif-hints: re-enable XChangeProperty failed");
    if (driverHonorsBorder) {
        Uint32 flags = SDL_GetWindowFlags(sdlWindow);
        CHECK((flags & SDL_WINDOW_BORDERLESS) == 0,
              "motif-hints: restoring decor did not re-add border");
    }

    /* Functions: explicit set without MWM_FUNC_ALL excluding RESIZE means
     * resize is NOT allowed.
     */
    hints[0] = 0x1; /* MWM_HINTS_FUNCTIONS */
    hints[1] = 0x4; /* MWM_FUNC_MOVE (1<<2) only, no RESIZE */
    hints[2] = 0;
    CHECK(XChangeProperty(display, window, motif, motif, 32, PropModeReplace,
                          (unsigned char *) hints, 5),
          "motif-hints: functions XChangeProperty failed");
    if (driverHonorsResizable) {
        Uint32 flags = SDL_GetWindowFlags(sdlWindow);
        CHECK((flags & SDL_WINDOW_RESIZABLE) == 0,
              "motif-hints: cleared MWM_FUNC_RESIZE did not drop resizable");
    }

    /* MWM_FUNC_ALL with no listed exclusions → resize back. */
    hints[1] = 0x1; /* MWM_FUNC_ALL only */
    CHECK(XChangeProperty(display, window, motif, motif, 32, PropModeReplace,
                          (unsigned char *) hints, 5),
          "motif-hints: FUNC_ALL-only XChangeProperty failed");
    if (driverHonorsResizable) {
        Uint32 flags = SDL_GetWindowFlags(sdlWindow);
        CHECK((flags & SDL_WINDOW_RESIZABLE) != 0,
              "motif-hints: FUNC_ALL-only did not re-enable resize");
    }

    /* The property itself must round-trip verbatim regardless of driver
     * capability, so a real WM downstream can read what was written.
     */
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0, bytesAfter = 0;
    unsigned char *data = NULL;
    int rc = XGetWindowProperty(display, window, motif, 0, 5, False,
                                AnyPropertyType, &actualType, &actualFormat,
                                &nItems, &bytesAfter, &data);
    CHECK(rc == Success && actualType == motif && actualFormat == 32 &&
              nItems == 5,
          "motif-hints: stored property did not round-trip");
    XFree(data);

    XDestroyWindow(display, window);
    return 1;
}

/* WM_NORMAL_HINTS (XSizeHints) resizability, mirroring a real WM: a top-level
 * is resizable unless the client pinned a fixed size (PMinSize and PMaxSize
 * present with min == max). A Motif client that declares MWM_HINTS_FUNCTIONS
 * keeps the resizable decision, so WM_NORMAL_HINTS must defer to it. Like
 * test_mwm_hints_apply, the SDL dummy driver ignores SDL_SetWindowResizable, so
 * the flag assertions are gated on a driver-capability probe.
 */
static int test_normal_hints_resizable(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));

#if SDL_VERSION_ATLEAST(2, 0, 5)
    Window probe = XCreateSimpleWindow(display, root, 0, 0, 96, 64, 0, 0, 0);
    CHECK(probe != None, "normal-hints: probe window creation failed");
    CHECK(XMapWindow(display, probe), "normal-hints: probe map failed");
    SDL_Window *probeSdl = GET_WINDOW_STRUCT(probe)->sdlWindow;
    CHECK(probeSdl != NULL, "normal-hints: probe has no SDL backing");
    Uint32 flagsBefore = SDL_GetWindowFlags(probeSdl);
    SDL_SetWindowResizable(probeSdl, SDL_FALSE);
    Uint32 flagsAfter = SDL_GetWindowFlags(probeSdl);
    SDL_SetWindowResizable(probeSdl, SDL_TRUE);
    Bool driverHonorsResizable = (flagsAfter & SDL_WINDOW_RESIZABLE) !=
                                 (flagsBefore & SDL_WINDOW_RESIZABLE);
    XDestroyWindow(display, probe);
#else
    Bool driverHonorsResizable = False;
#endif

    /* xwpe-like: PMinSize + PResizeInc + PBaseSize, no PMaxSize cap. A real WM
     * treats this as resizable; this is exactly the case that left xwpe stuck
     * fixed-size before the fix.
     */
    Window resizable =
        XCreateSimpleWindow(display, root, 0, 0, 640, 400, 0, 0, 0);
    CHECK(resizable != None, "normal-hints: resizable window creation failed");
    CHECK(XMapWindow(display, resizable), "normal-hints: resizable map failed");
    SDL_Window *resizableSdl = GET_WINDOW_STRUCT(resizable)->sdlWindow;
    CHECK(resizableSdl != NULL, "normal-hints: resizable has no SDL backing");
    XSizeHints openHints;
    memset(&openHints, 0, sizeof(openHints));
    openHints.flags = PMinSize | PResizeInc | PBaseSize;
    openHints.min_width = 80;
    openHints.min_height = 40;
    openHints.width_inc = 8;
    openHints.height_inc = 16;
    openHints.base_width = 80;
    openHints.base_height = 40;
    XSetWMNormalHints(display, resizable, &openHints);
    /* Driver-independent observable: the pure decoder must say resizable. This
     * is what fails without the fix, even under SDL_VIDEODRIVER=dummy.
     */
    CHECK(topLevelResizableFromNormalHints(resizable),
          "normal-hints: ranged size hints not decoded as resizable");
    if (driverHonorsResizable) {
        Uint32 flags = SDL_GetWindowFlags(resizableSdl);
        CHECK((flags & SDL_WINDOW_RESIZABLE) != 0,
              "normal-hints: ranged size hints did not enable resize");
    }
    XDestroyWindow(display, resizable);

    /* Fixed-size dialog: PMinSize == PMaxSize. Stays non-resizable. */
    Window fixed = XCreateSimpleWindow(display, root, 0, 0, 200, 120, 0, 0, 0);
    CHECK(fixed != None, "normal-hints: fixed window creation failed");
    CHECK(XMapWindow(display, fixed), "normal-hints: fixed map failed");
    SDL_Window *fixedSdl = GET_WINDOW_STRUCT(fixed)->sdlWindow;
    CHECK(fixedSdl != NULL, "normal-hints: fixed has no SDL backing");
#if SDL_VERSION_ATLEAST(2, 0, 5)
    SDL_SetWindowResizable(fixedSdl, SDL_TRUE); /* start resizable to observe */
#endif
    XSizeHints fixedHints;
    memset(&fixedHints, 0, sizeof(fixedHints));
    fixedHints.flags = PMinSize | PMaxSize;
    fixedHints.min_width = fixedHints.max_width = 200;
    fixedHints.min_height = fixedHints.max_height = 120;
    XSetWMNormalHints(display, fixed, &fixedHints);
    CHECK(!topLevelResizableFromNormalHints(fixed),
          "normal-hints: fixed size hints decoded as resizable");
    if (driverHonorsResizable) {
        Uint32 flags = SDL_GetWindowFlags(fixedSdl);
        CHECK((flags & SDL_WINDOW_RESIZABLE) == 0,
              "normal-hints: fixed size hints did not stay non-resizable");
    }

    /* Transition fixed -> ranged: dropping PMaxSize must re-enable resize and
     * clear the stale SDL maximum, otherwise the window stays effectively fixed
     * even once resize is on. The decoder decision is the driver-independent
     * observable; the max-clearing is applied in XSetWMNormalHints.
     */
    XSizeHints rangedHints;
    memset(&rangedHints, 0, sizeof(rangedHints));
    rangedHints.flags = PMinSize | PResizeInc;
    rangedHints.min_width = 100;
    rangedHints.min_height = 60;
    rangedHints.width_inc = 8;
    rangedHints.height_inc = 8;
    XSetWMNormalHints(display, fixed, &rangedHints);
    CHECK(topLevelResizableFromNormalHints(fixed),
          "normal-hints: fixed->ranged not decoded as resizable");
    if (driverHonorsResizable) {
        Uint32 flags = SDL_GetWindowFlags(fixedSdl);
        CHECK((flags & SDL_WINDOW_RESIZABLE) != 0,
              "normal-hints: fixed->ranged did not re-enable resize");
    }
    XDestroyWindow(display, fixed);

    /* Motif precedence: a client that cleared MWM_FUNC_RESIZE owns the
     * decision. A later ranged WM_NORMAL_HINTS must NOT override it back to
     * resizable.
     */
    Atom motif = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    CHECK(motif != None, "normal-hints: motif atom intern failed");
    Window motifOwned =
        XCreateSimpleWindow(display, root, 0, 0, 300, 200, 0, 0, 0);
    CHECK(motifOwned != None, "normal-hints: motif window creation failed");
    long noResize[5] = {
        0x1 /* MWM_HINTS_FUNCTIONS */,
        0x4 /* MWM_FUNC_MOVE, no RESIZE */,
        0,
        0,
        0,
    };
    CHECK(XChangeProperty(display, motifOwned, motif, motif, 32,
                          PropModeReplace, (unsigned char *) noResize, 5),
          "normal-hints: motif write failed");
    CHECK(XMapWindow(display, motifOwned), "normal-hints: motif map failed");
    SDL_Window *motifSdl = GET_WINDOW_STRUCT(motifOwned)->sdlWindow;
    CHECK(motifSdl != NULL, "normal-hints: motif window has no SDL backing");
    XSetWMNormalHints(display, motifOwned, &openHints);
    CHECK(!topLevelResizableFromNormalHints(motifOwned),
          "normal-hints: decoder did not defer to Motif functions");
    if (driverHonorsResizable) {
        Uint32 flags = SDL_GetWindowFlags(motifSdl);
        CHECK((flags & SDL_WINDOW_RESIZABLE) == 0,
              "normal-hints: WM_NORMAL_HINTS overrode Motif no-resize");
    }
    XDestroyWindow(display, motifOwned);
    return 1;
}

/* ICCCM WM_TRANSIENT_FOR + modal hint must wire SDL_SetWindowModalFor; without
 * the modal hint, transient_for alone is a no-op for the modal call.
 */
static int test_icccm_transient_for_modal(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window parent = XCreateSimpleWindow(display, root, 0, 0, 128, 96, 0, 0, 0);
    Window child = XCreateSimpleWindow(display, root, 0, 0, 64, 48, 0, 0, 0);
    CHECK(parent != None && child != None, "transient: window creation failed");
    CHECK(XMapWindow(display, parent), "transient: parent map failed");
    CHECK(XMapWindow(display, child), "transient: child map failed");

    /* Plain transient_for without a modal hint should NOT mark applied; SDL2
     * has no non-modal parent primitive.
     */
    CHECK(XSetTransientForHint(display, child, parent),
          "transient: XSetTransientForHint failed");
    CHECK(!GET_WINDOW_STRUCT(child)->deferredTransientApplied,
          "transient: non-modal transient_for unexpectedly applied modal-for");
    CHECK(GET_WINDOW_STRUCT(child)->deferredTransientParent == parent,
          "transient: deferredTransientParent not recorded");

    Atom netWmState = XInternAtom(display, "_NET_WM_STATE", False);
    Atom modalAtom = XInternAtom(display, "_NET_WM_STATE_MODAL", False);
    Atom states[1] = {modalAtom};
    CHECK(XChangeProperty(display, child, netWmState, XA_CARDINAL, 32,
                          PropModeReplace, (unsigned char *) states, 1),
          "transient: malformed _NET_WM_STATE write failed");
    CHECK(!GET_WINDOW_STRUCT(child)->deferredTransientApplied,
          "transient: non-ATOM _NET_WM_STATE unexpectedly marked modal");

    /* Now add _NET_WM_STATE_MODAL and re-set transient_for: pairing should now
     * apply if SDL supports modal-for.
     */
    CHECK(XChangeProperty(display, child, netWmState, XA_ATOM, 32,
                          PropModeReplace, (unsigned char *) states, 1),
          "transient: _NET_WM_STATE write failed");
#if SDL_VERSION_ATLEAST(2, 0, 5)
    CHECK(GET_WINDOW_STRUCT(child)->deferredTransientApplied,
          "transient: modal hint did not trigger SDL_SetWindowModalFor");
#ifdef LIBX11_COMPAT_SDL3
    bool driverTracksParent =
        SDL_GetWindowParent(GET_WINDOW_STRUCT(child)->sdlWindow) ==
        GET_WINDOW_STRUCT(parent)->sdlWindow;
#endif

    GET_WINDOW_STRUCT(child)->deferredTransientApplied = False;
    XEvent event = {.xclient = {
                        .type = ClientMessage,
                        .window = child,
                        .message_type = netWmState,
                        .format = 32,
                        .data.l = {1 /* _NET_WM_STATE_ADD */, (long) modalAtom},
                    }};
    CHECK(XSendEvent(display, root, False,
                     SubstructureNotifyMask | SubstructureRedirectMask, &event),
          "transient: idempotent modal ClientMessage failed");
    CHECK(GET_WINDOW_STRUCT(child)->deferredTransientApplied,
          "transient: idempotent modal ADD did not reapply modal-for");
#endif

    /* Clear modal: applied flag must drop. */
    CHECK(XDeleteProperty(display, child, netWmState),
          "transient: XDeleteProperty(_NET_WM_STATE) failed");
    CHECK(!GET_WINDOW_STRUCT(child)->deferredTransientApplied,
          "transient: clearing modal did not drop applied flag");
#ifdef LIBX11_COMPAT_SDL3
    CHECK(!driverTracksParent ||
              SDL_GetWindowParent(GET_WINDOW_STRUCT(child)->sdlWindow) == NULL,
          "transient: clearing modal did not drop SDL3 native parent");
#endif

    /* XDeleteProperty(WM_TRANSIENT_FOR) clears the deferred parent. */
    CHECK(XChangeProperty(display, child, netWmState, XA_ATOM, 32,
                          PropModeReplace, (unsigned char *) states, 1),
          "transient: re-arming modal failed");
    CHECK(XDeleteProperty(display, child, XA_WM_TRANSIENT_FOR),
          "transient: XDeleteProperty(WM_TRANSIENT_FOR) failed");
    CHECK(GET_WINDOW_STRUCT(child)->deferredTransientParent == None,
          "transient: delete did not clear deferredTransientParent");

    /* Motif input mode also marks the child modal. Defer the transient pairing
     * here: write the property BEFORE the child has any realized SDL window by
     * destroying and recreating.
     */
    Window deferredChild =
        XCreateSimpleWindow(display, root, 0, 0, 32, 24, 0, 0, 0);
    CHECK(deferredChild != None, "transient: deferred child creation failed");
    Atom motif = XInternAtom(display, "_MOTIF_WM_HINTS", False);
    long mwm[5] = {
        0x4 /* MWM_HINTS_INPUT_MODE */,           0, 0,
        3 /* MWM_INPUT_FULL_APPLICATION_MODAL */, 0,
    };
    CHECK(XChangeProperty(display, deferredChild, motif, motif, 32,
                          PropModeReplace, (unsigned char *) mwm, 5),
          "transient: deferred MWM write failed");
    CHECK(XSetTransientForHint(display, deferredChild, parent),
          "transient: deferred XSetTransientForHint failed");
    /* deferredTransientParent stashed before realize. */
    CHECK(GET_WINDOW_STRUCT(deferredChild)->deferredTransientParent == parent,
          "transient: deferred path lost parent before realize");
    CHECK(XMapWindow(display, deferredChild),
          "transient: deferred child map failed");
#if SDL_VERSION_ATLEAST(2, 0, 5)
    CHECK(GET_WINDOW_STRUCT(deferredChild)->deferredTransientApplied,
          "transient: deferred realize did not apply modal-for");
#endif

    XDestroyWindow(display, deferredChild);

    /* Parent-realized-after-child: a Motif/Xt pattern where the modal child is
     * mapped first and the parent shell is mapped second. realizeTopLevelWindow
     * on the parent must scan top-level siblings for any whose
     * deferredTransientParent points at it and re-resolve the pairing.
     */
    Window lateParent =
        XCreateSimpleWindow(display, root, 0, 0, 144, 96, 0, 0, 0);
    Window earlyChild =
        XCreateSimpleWindow(display, root, 0, 0, 80, 48, 0, 0, 0);
    CHECK(lateParent != None && earlyChild != None,
          "transient: late-parent setup failed");
    Atom netWmState2 = XInternAtom(display, "_NET_WM_STATE", False);
    Atom modalAtom2 = XInternAtom(display, "_NET_WM_STATE_MODAL", False);
    Atom modalStates[1] = {modalAtom2};
    CHECK(XChangeProperty(display, earlyChild, netWmState2, XA_ATOM, 32,
                          PropModeReplace, (unsigned char *) modalStates, 1),
          "transient: early-child modal write failed");
    CHECK(XSetTransientForHint(display, earlyChild, lateParent),
          "transient: early-child transient hint failed");
    CHECK(XMapWindow(display, earlyChild), "transient: early-child map failed");
    /* Parent not yet realized: applyTransientForRelationship bails out. */
    CHECK(!GET_WINDOW_STRUCT(earlyChild)->deferredTransientApplied,
          "transient: child paired before parent existed");
    CHECK(XMapWindow(display, lateParent), "transient: late-parent map failed");
#if SDL_VERSION_ATLEAST(2, 0, 5)
    CHECK(GET_WINDOW_STRUCT(earlyChild)->deferredTransientApplied,
          "transient: late parent did not pick up waiting modal child");
#endif
    XDestroyWindow(display, earlyChild);
    XDestroyWindow(display, lateParent);

    XDestroyWindow(display, child);
    XDestroyWindow(display, parent);
    return 1;
}

static int test_windows(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window parent = XCreateSimpleWindow(display, root, 3, 4, 64, 48, 1, 0, 0);
    Window child1 = XCreateSimpleWindow(display, parent, 1, 2, 16, 16, 0, 0, 0);
    Window child2 = XCreateSimpleWindow(display, parent, 5, 6, 16, 16, 0, 0, 0);
    CHECK(parent != None && child1 != None && child2 != None,
          "window creation failed");

    Window normalTop =
        XCreateSimpleWindow(display, root, 12, 12, 32, 24, 0, 0, 0);
    CHECK(normalTop != None, "normal top-level window creation failed");
    CHECK(XMapWindow(display, normalTop), "normal top-level map failed");
    SDL_Window *normalSdl = GET_WINDOW_STRUCT(normalTop)->sdlWindow;
    CHECK(normalSdl != NULL, "normal top-level did not realize");
    CHECK((SDL_GetWindowFlags(normalSdl) & SDL_WINDOW_BORDERLESS) == 0,
          "normal top-level with X border_width 0 became SDL borderless");

    Window overrideTop =
        XCreateSimpleWindow(display, root, 16, 16, 32, 24, 0, 0, 0);
    CHECK(overrideTop != None, "override-redirect window creation failed");
    XSetWindowAttributes overrideAttrs;
    memset(&overrideAttrs, 0, sizeof(overrideAttrs));
    overrideAttrs.override_redirect = True;
    CHECK(XChangeWindowAttributes(display, overrideTop, CWOverrideRedirect,
                                  &overrideAttrs),
          "CWOverrideRedirect change failed");
    CHECK(XMapWindow(display, overrideTop), "override-redirect map failed");
    SDL_Window *overrideSdl = GET_WINDOW_STRUCT(overrideTop)->sdlWindow;
    CHECK(overrideSdl != NULL, "override-redirect top-level did not realize");
    CHECK((SDL_GetWindowFlags(overrideSdl) & SDL_WINDOW_BORDERLESS) != 0,
          "override-redirect top-level was not SDL borderless");

    Window partialTop =
        XCreateSimpleWindow(display, root, 20, 20, 64, 48, 0, 0, 0);
    Window partialChild =
        XCreateSimpleWindow(display, partialTop, 5, 7, 20, 16, 0, 0, 0);
    CHECK(partialTop != None && partialChild != None,
          "partial present window setup failed");
    CHECK(XMapWindow(display, partialTop), "partial present top map failed");
    CHECK(XMapWindow(display, partialChild),
          "partial present child map failed");
    WindowStruct *partialTopStruct = GET_WINDOW_STRUCT(partialTop);
    partialTopStruct->hasPresented = True;
    partialTopStruct->needsPresent = False;
    partialTopStruct->hasPresentRect = False;
    SDL_Rect partialDamage = {2, 3, 4, 5};
    presentDrawableRectIfVisible(partialChild, &partialDamage);
    CHECK(partialTopStruct->needsPresent,
          "child rectangle damage did not schedule top-level present");
    CHECK(partialTopStruct->hasPresentRect,
          "child rectangle damage did not store present rectangle");
    CHECK(partialTopStruct->presentRect.x == 7 &&
              partialTopStruct->presentRect.y == 10 &&
              partialTopStruct->presentRect.w == 4 &&
              partialTopStruct->presentRect.h == 5,
          "child rectangle damage was not translated to top-level coordinates");
    XDestroyWindow(display, partialTop);

    Window wideRectTop =
        XCreateSimpleWindow(display, root, 24, 24, 64, 48, 0, 0, 0);
    CHECK(wideRectTop != None, "wide rectangle present window setup failed");
    CHECK(XMapWindow(display, wideRectTop),
          "wide rectangle present map failed");
    GC wideRectGc = XCreateGC(display, wideRectTop, 0, NULL);
    CHECK(wideRectGc != NULL, "wide rectangle present GC failed");
    CHECK(XSetLineAttributes(display, wideRectGc, 9, LineSolid, CapButt,
                             JoinMiter),
          "wide rectangle line attributes failed");
    WindowStruct *wideRectTopStruct = GET_WINDOW_STRUCT(wideRectTop);
    wideRectTopStruct->hasPresented = True;
    wideRectTopStruct->needsPresent = False;
    wideRectTopStruct->hasPresentRect = False;
    CHECK(XDrawRectangle(display, wideRectTop, wideRectGc, 16, 16, 8, 8),
          "wide rectangle present draw failed");
    CHECK(wideRectTopStruct->needsPresent,
          "wide rectangle draw did not schedule a present");
    CHECK(wideRectTopStruct->hasPresentRect,
          "wide rectangle draw did not store a present rectangle");
    /* Wide-line XDrawRectangle stores the geometric rect inflated by the stroke
     * pad on each side (arcStrokePad returns lineWidth when > 1), matching what
     * arcDamageRect produces for arcs. For (16, 16, 8, 8) with lineWidth = 9
     * that is (7, 7, 27, 27). The previous "always full window" behavior was
     * over-conservative and defeated the dirty-region read-side coalescing.
     */
    CHECK(wideRectTopStruct->presentRect.x == 7 &&
              wideRectTopStruct->presentRect.y == 7 &&
              wideRectTopStruct->presentRect.w == 27 &&
              wideRectTopStruct->presentRect.h == 27,
          "wide rectangle draw did not store stroke-padded present rect");
    XFreeGC(display, wideRectGc);
    XDestroyWindow(display, wideRectTop);

    Cursor fontCursor = XCreateFontCursor(display, XC_left_ptr);
    CHECK(fontCursor != None, "XCreateFontCursor failed");
    CHECK(XDefineCursor(display, parent, fontCursor), "XDefineCursor failed");
    XColor cursorFg = {.red = 0xffff, .green = 0, .blue = 0};
    XColor cursorBg = {.red = 0, .green = 0, .blue = 0xffff};
    CHECK(XRecolorCursor(display, fontCursor, &cursorFg, &cursorBg),
          "XRecolorCursor failed");
    XSetWindowAttributes cursorAttrs;
    memset(&cursorAttrs, 0, sizeof(cursorAttrs));
    cursorAttrs.cursor = fontCursor;
    CHECK(XChangeWindowAttributes(display, child1, CWCursor, &cursorAttrs),
          "CWCursor XChangeWindowAttributes failed");
    CHECK(XUndefineCursor(display, child1), "XUndefineCursor failed");

    Window gotRoot = None;
    int x = 0, y = 0;
    unsigned int width = 0, height = 0, border = 0, depth = 0;
    CHECK(XGetGeometry(display, parent, &gotRoot, &x, &y, &width, &height,
                       &border, &depth),
          "XGetGeometry failed");
    CHECK(gotRoot == root && x == 3 && y == 4 && width == 64 && height == 48 &&
              border == 1,
          "XGetGeometry returned unexpected parent geometry");

    Window colormapList[2] = {child1, child2};
    CHECK(XSetWMColormapWindows(display, parent, colormapList, 2),
          "XSetWMColormapWindows failed");
    colormapList[0] = parent;
    Window *colormapBack = NULL;
    int colormapCount = 0;
    CHECK(XGetWMColormapWindows(display, parent, &colormapBack, &colormapCount),
          "XGetWMColormapWindows failed");
    CHECK(colormapCount == 2 && colormapBack[0] == child1 &&
              colormapBack[1] == child2,
          "WM_COLORMAP_WINDOWS was not deep-copied");
    XFree(colormapBack);
    int (*oldErrorHandler)(Display *, XErrorEvent *) =
        XSetErrorHandler(ignored_error);
    CHECK(!XSetWMColormapWindows(display, parent, colormapList, -1),
          "XSetWMColormapWindows accepted a negative count");
    Window invalidColormapWindow = (Window) fontCursor;
    CHECK(!XSetWMColormapWindows(display, parent, &invalidColormapWindow, 1),
          "XSetWMColormapWindows accepted a non-window entry");
    XSetErrorHandler(oldErrorHandler);

    CHECK(XSetWindowBorder(display, parent, 0xff00ff00),
          "XSetWindowBorder failed");
    CHECK(XSetWindowBorderWidth(display, parent, 3),
          "XSetWindowBorderWidth failed");
    CHECK(XGetGeometry(display, parent, &gotRoot, &x, &y, &width, &height,
                       &border, &depth),
          "XGetGeometry after border width failed");
    CHECK(border == 3, "XSetWindowBorderWidth did not update geometry");
    Pixmap borderPixmap =
        XCreatePixmap(display, parent, 4, 4, DefaultDepth(display, 0));
    CHECK(borderPixmap != None, "border pixmap creation failed");
    CHECK(XSetWindowBorderPixmap(display, parent, borderPixmap),
          "XSetWindowBorderPixmap failed");
    oldErrorHandler = XSetErrorHandler(ignored_error);
    CHECK(!XSetWindowBorderPixmap(display, parent, child1),
          "XSetWindowBorderPixmap accepted a non-pixmap");
    XSetErrorHandler(oldErrorHandler);
    XFreePixmap(display, borderPixmap);

    Atom lifetimeProperty =
        XInternAtom(display, "SDL2X11_LIFETIME_PROPERTY", False);
    unsigned long firstValue = 0x01020304;
    unsigned long secondValue[2] = {0x05060708, 0x090a0b0c};
    CHECK(XChangeProperty(display, parent, lifetimeProperty, XA_CARDINAL, 32,
                          PropModeReplace, (unsigned char *) &firstValue, 1),
          "initial XChangeProperty failed");
    CHECK(XChangeProperty(display, parent, lifetimeProperty, XA_CARDINAL, 32,
                          PropModeReplace, (unsigned char *) secondValue, 2),
          "replacement XChangeProperty failed");
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char *propertyData = NULL;
    CHECK(XGetWindowProperty(display, parent, lifetimeProperty, 0, 4, False,
                             XA_CARDINAL, &actualType, &actualFormat,
                             &itemCount, &bytesAfter, &propertyData) == Success,
          "XGetWindowProperty failed for replacement property");
    CHECK(actualType == XA_CARDINAL && actualFormat == 32 && itemCount == 2 &&
              bytesAfter == 0,
          "replacement property metadata was incorrect");
    XFree(propertyData);
    CHECK(XDeleteProperty(display, parent, lifetimeProperty),
          "XDeleteProperty failed");
    CHECK(XGetWindowProperty(display, parent, lifetimeProperty, 0, 1, False,
                             XA_CARDINAL, &actualType, &actualFormat,
                             &itemCount, &bytesAfter, &propertyData) == Success,
          "XGetWindowProperty failed after delete");
    CHECK(actualType == None && itemCount == 0 && bytesAfter == 0,
          "deleted property was still visible");

    Window queryRoot = None;
    Window queryParent = None;
    Window *children = NULL;
    unsigned int nchildren = 0;
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree failed");
    CHECK(queryRoot == root && queryParent == root && nchildren == 2 &&
              children[0] == child1 && children[1] == child2,
          "XQueryTree returned unexpected children");
    XFree(children);
    CHECK(!addChildToWindow(parent, child1),
          "addChildToWindow accepted a duplicate child");
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after duplicate insert failed");
    CHECK(nchildren == 2 && children[0] == child1 && children[1] == child2,
          "duplicate child insertion changed child order");
    XFree(children);

    Window reparentOld =
        XCreateSimpleWindow(display, root, 20, 20, 40, 40, 0, 0, 0);
    Window reparentNew =
        XCreateSimpleWindow(display, root, 30, 30, 40, 40, 0, 0, 0);
    Window reparentChild =
        XCreateSimpleWindow(display, reparentOld, 1, 1, 10, 10, 0, 0, 0);
    CHECK(reparentOld != None && reparentNew != None && reparentChild != None,
          "unmapped reparent setup failed");
    XSelectInput(display, reparentChild, StructureNotifyMask | ExposureMask);
    CHECK(XReparentWindow(display, reparentChild, reparentNew, 7, 8),
          "unmapped XReparentWindow failed");
    XEvent reparentEvent;
    CHECK(XCheckTypedWindowEvent(display, reparentChild, ReparentNotify,
                                 &reparentEvent),
          "unmapped reparent did not post ReparentNotify");
    CHECK(reparentEvent.xreparent.parent == reparentNew &&
              reparentEvent.xreparent.x == 7 && reparentEvent.xreparent.y == 8,
          "unmapped ReparentNotify fields were incorrect");
    CHECK(!XCheckTypedWindowEvent(display, reparentChild, ConfigureNotify,
                                  &reparentEvent),
          "unmapped reparent posted a spurious ConfigureNotify");
    CHECK(!XCheckTypedWindowEvent(display, reparentChild, MapNotify,
                                  &reparentEvent),
          "unmapped reparent posted a spurious MapNotify");
    CHECK(!XCheckTypedWindowEvent(display, reparentChild, UnmapNotify,
                                  &reparentEvent),
          "unmapped reparent posted a spurious UnmapNotify");
    CHECK(
        !XCheckTypedWindowEvent(display, reparentChild, Expose, &reparentEvent),
        "unmapped reparent posted a spurious Expose");
    CHECK(XGetGeometry(display, reparentChild, &gotRoot, &x, &y, &width,
                       &height, &border, &depth),
          "XGetGeometry after unmapped reparent failed");
    CHECK(x == 7 && y == 8, "unmapped reparent did not update geometry");
    XDestroyWindow(display, reparentOld);
    XDestroyWindow(display, reparentNew);

    XCirculateSubwindowsUp(display, parent);
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after unmapped circulate-up failed");
    CHECK(nchildren == 2 && children[0] == child1 && children[1] == child2,
          "XCirculateSubwindowsUp changed unmapped children");
    XFree(children);

    XRaiseWindow(display, child1);
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after raise failed");
    CHECK(nchildren == 2 && children[1] == child1,
          "XRaiseWindow did not move child to top");
    XFree(children);

    XLowerWindow(display, child1);
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after lower failed");
    CHECK(nchildren == 2 && children[0] == child1,
          "XLowerWindow did not move child to bottom");
    XFree(children);

    XMapRaised(display, child1);
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after map-raised failed");
    CHECK(nchildren == 2 && children[1] == child1,
          "XMapRaised did not move child to top");
    XFree(children);

    XWindowChanges stackChanges;
    memset(&stackChanges, 0, sizeof(stackChanges));
    stackChanges.sibling = child2;
    stackChanges.stack_mode = Below;
    CHECK(XConfigureWindow(display, child1, CWSibling | CWStackMode,
                           &stackChanges),
          "sibling restack below failed");
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after sibling restack below failed");
    CHECK(nchildren == 2 && children[0] == child1 && children[1] == child2,
          "sibling restack below returned unexpected child order");
    XFree(children);
    stackChanges.stack_mode = Above;
    CHECK(XConfigureWindow(display, child1, CWSibling | CWStackMode,
                           &stackChanges),
          "sibling restack above failed");
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after sibling restack above failed");
    CHECK(nchildren == 2 && children[0] == child2 && children[1] == child1,
          "sibling restack above returned unexpected child order");
    XFree(children);

    XEvent visibility;
    XSelectInput(display, child1, VisibilityChangeMask);
    Window delayedParent =
        XCreateSimpleWindow(display, root, 10, 10, 32, 32, 0, 0, 0);
    Window delayedChild =
        XCreateSimpleWindow(display, delayedParent, 1, 1, 8, 8, 0, 0, 0);
    CHECK(delayedParent != None && delayedChild != None,
          "delayed map window creation failed");
    XSelectInput(display, delayedParent, SubstructureNotifyMask);
    XSelectInput(display, delayedChild, StructureNotifyMask | ExposureMask);
    CHECK(XMapWindow(display, delayedChild),
          "mapping child of unmapped parent failed");
    CHECK(expect_map_state(display, delayedChild, IsUnviewable),
          "child mapped under unmapped parent should be unviewable");
    CHECK(GET_WINDOW_STRUCT(delayedParent)->sdlTexture == NULL,
          "unmapped parent should not merge map-requested child backing");
    CHECK(
        !XCheckTypedWindowEvent(display, delayedParent, MapNotify, &visibility),
        "map-requested child notified parent before ancestor mapped");
    CHECK(
        !XCheckTypedWindowEvent(display, delayedChild, MapNotify, &visibility),
        "map-requested child notified itself before ancestor mapped");
    CHECK(XMapWindow(display, delayedParent), "mapping delayed parent failed");
    CHECK(expect_map_state(display, delayedChild, IsViewable),
          "map-requested child was not viewable after parent map");
    CHECK(
        XCheckTypedWindowEvent(display, delayedParent, MapNotify, &visibility),
        "map-requested child did not notify parent on realization");
    CHECK(visibility.xmap.event == delayedParent &&
              visibility.xmap.window == delayedChild,
          "map-requested child parent MapNotify fields were wrong");
    CHECK(XCheckTypedWindowEvent(display, delayedChild, MapNotify, &visibility),
          "map-requested child did not notify itself on realization");
    CHECK(visibility.xmap.event == delayedChild &&
              visibility.xmap.window == delayedChild,
          "map-requested child self MapNotify fields were wrong");
    CHECK(XCheckTypedWindowEvent(display, delayedChild, Expose, &visibility),
          "map-requested child did not receive realization Expose");
    CHECK(visibility.xexpose.x == 0 && visibility.xexpose.y == 0 &&
              visibility.xexpose.width == 8 && visibility.xexpose.height == 8 &&
              visibility.xexpose.count == 0,
          "map-requested child realization Expose fields were wrong");
    CHECK(XUnmapWindow(display, delayedParent),
          "unmapping delayed parent failed");
    CHECK(expect_map_state(display, delayedChild, IsUnviewable),
          "unmapping ancestor should leave descendant unviewable");
    CHECK(XMapWindow(display, delayedParent),
          "remapping delayed parent failed");
    CHECK(expect_map_state(display, delayedChild, IsViewable),
          "descendant did not become viewable after ancestor remap");
    CHECK(XCheckTypedWindowEvent(display, delayedChild, Expose, &visibility),
          "mapped descendant did not receive Expose after ancestor remap");
    CHECK(visibility.xexpose.x == 0 && visibility.xexpose.y == 0 &&
              visibility.xexpose.width == 8 && visibility.xexpose.height == 8 &&
              visibility.xexpose.count == 0,
          "mapped descendant remap Expose fields were wrong");
    XDestroyWindow(display, delayedParent);

    Window inputParent =
        XCreateSimpleWindow(display, root, 12, 12, 40, 40, 0, 0, 0);
    CHECK(inputParent != None, "InputOnly map parent creation failed");
    CHECK(XMapWindow(display, inputParent), "InputOnly map parent failed");
    XSetWindowAttributes inputAttrs;
    memset(&inputAttrs, 0, sizeof(inputAttrs));
    Window directInput =
        XCreateWindow(display, inputParent, 0, 0, 20, 20, 0, CopyFromParent,
                      InputOnly, CopyFromParent, 0, &inputAttrs);
    CHECK(directInput != None, "direct InputOnly child creation failed");
    XSelectInput(display, directInput, StructureNotifyMask | ExposureMask);
    last_error_code = 0;
    oldErrorHandler = XSetErrorHandler(record_error);
    CHECK(XMapRaised(display, directInput), "direct InputOnly map failed");
    XSetErrorHandler(oldErrorHandler);
    CHECK(last_error_code == 0,
          "mapping direct InputOnly child generated an X error");
    CHECK(expect_map_state(display, directInput, IsViewable),
          "direct InputOnly child was not viewable after map");
    CHECK(XCheckTypedWindowEvent(display, directInput, MapNotify, &visibility),
          "direct InputOnly child did not receive MapNotify");
    CHECK(!XCheckTypedWindowEvent(display, directInput, Expose, &visibility),
          "direct InputOnly child received Expose");

    Window delayedInputParent =
        XCreateSimpleWindow(display, root, 14, 14, 40, 40, 0, 0, 0);
    Window delayedInput = XCreateWindow(display, delayedInputParent, 0, 0, 20,
                                        20, 0, CopyFromParent, InputOnly,
                                        CopyFromParent, 0, &inputAttrs);
    CHECK(delayedInputParent != None && delayedInput != None,
          "delayed InputOnly setup failed");
    XSelectInput(display, delayedInputParent, SubstructureNotifyMask);
    XSelectInput(display, delayedInput, StructureNotifyMask | ExposureMask);
    CHECK(XMapWindow(display, delayedInput),
          "mapping InputOnly child under unmapped parent failed");
    CHECK(expect_map_state(display, delayedInput, IsUnviewable),
          "delayed InputOnly child should be unviewable before parent map");
    last_error_code = 0;
    oldErrorHandler = XSetErrorHandler(record_error);
    CHECK(XMapWindow(display, delayedInputParent),
          "mapping delayed InputOnly parent failed");
    XSetErrorHandler(oldErrorHandler);
    CHECK(last_error_code == 0,
          "realizing delayed InputOnly child generated an X error");
    CHECK(expect_map_state(display, delayedInput, IsViewable),
          "delayed InputOnly child was not viewable after parent map");
    CHECK(XCheckTypedWindowEvent(display, delayedInput, MapNotify, &visibility),
          "delayed InputOnly child did not receive MapNotify");
    CHECK(!XCheckTypedWindowEvent(display, delayedInput, Expose, &visibility),
          "delayed InputOnly child received Expose");
    XDestroyWindow(display, delayedInputParent);
    XDestroyWindow(display, inputParent);
    while (XPending(display)) {
        XNextEvent(display, &visibility);
    }

    Window delayedSubParent =
        XCreateSimpleWindow(display, root, 0, 0, 24, 24, 0, 0, 0);
    Window delayedSubChild =
        XCreateSimpleWindow(display, delayedSubParent, 1, 1, 8, 8, 0, 0, 0);
    CHECK(delayedSubParent != None && delayedSubChild != None,
          "delayed XMapSubwindows setup failed");
    CHECK(XMapWindow(display, delayedSubParent),
          "mapping XMapSubwindows parent failed");
    Window directMapChild =
        XCreateSimpleWindow(display, delayedSubParent, 4, 4, 8, 8, 0, 0, 0);
    CHECK(directMapChild != None, "direct child map setup failed");
    CHECK(GET_WINDOW_STRUCT(directMapChild)->sdlWindow == NULL,
          "child window unexpectedly owns an SDL window");
    CHECK(XMapWindow(display, directMapChild),
          "mapping child of mapped parent failed");
    CHECK(GET_WINDOW_STRUCT(directMapChild)->mapState == Mapped,
          "direct child map did not mark child mapped");
    GET_WINDOW_STRUCT(delayedSubChild)->mapState = MapRequested;
    CHECK(XMapSubwindows(display, delayedSubParent),
          "XMapSubwindows with map-requested child failed");
    CHECK(expect_map_state(display, delayedSubChild, IsViewable),
          "XMapSubwindows skipped map-requested child");
    XDestroyWindow(display, delayedSubParent);

    XMapWindow(display, parent);
    XMapSubwindows(display, parent);
    CHECK(expect_map_state(display, child1, IsViewable),
          "child1 was not viewable");
    CHECK(expect_map_state(display, child2, IsViewable),
          "child2 was not viewable");
    SDL_Renderer *childRenderer = getWindowRenderer(child2);
    SDL_Rect viewport;
    SDL_RenderGetViewport(childRenderer, &viewport);
    CHECK(viewport.x == 5 && viewport.y == 6 && viewport.w == 16 &&
              viewport.h == 16,
          "child renderer viewport did not use top-left X coordinates");
    SDL_Rect negativeViewport = {.x = -5, .y = -6, .w = 16, .h = 16};
    CHECK(SDL_RenderSetViewport(childRenderer, &negativeViewport) == 0,
          "negative viewport setup failed");
    SDL_Rect negativeRead = {.x = 0, .y = 0, .w = 16, .h = 16};
    SDL_Surface *negativeSurface =
        getRenderSurfaceRect(childRenderer, &negativeRead);
    CHECK(negativeSurface != NULL,
          "getRenderSurfaceRect failed for negative viewport");
    SDL_FreeSurface(negativeSurface);
    CHECK(SDL_RenderSetViewport(childRenderer, &viewport) == 0,
          "viewport restore failed");
    GC childClipGc = XCreateGC(display, parent, 0, NULL);
    CHECK(childClipGc != NULL, "ClipByChildren GC creation failed");
    /* Earlier in this test child1 was stacked above child2, so child2's (0, 0,
     * 4, 4) corner now falls under child1 and the sibling occlusion clip would
     * drop the fill before it ever reaches parent's shared backing. Raise
     * child2 back to the top so the ClipByChildren probe is testing what it
     * intends to test: that XClearArea on parent does NOT erase the pixels that
     * map to a mapped child's frame.
     */
    XRaiseWindow(display, child2);
    CHECK(XSetForeground(display, childClipGc, 0xFFFFFFFF),
          "ClipByChildren parent clear setup failed");
    CHECK(XFillRectangle(display, parent, childClipGc, 0, 0, 32, 32),
          "ClipByChildren parent initial fill failed");
    CHECK(XSetForeground(display, childClipGc, 0xFF112233),
          "ClipByChildren child draw setup failed");
    CHECK(XFillRectangle(display, child2, childClipGc, 0, 0, 4, 4),
          "ClipByChildren child fill failed");
    CHECK(XClearArea(display, parent, 0, 0, 32, 32, False),
          "ClipByChildren parent clear failed");
    SDL_Renderer *parentRenderer = getWindowRenderer(parent);
    SDL_Surface *clipSurface = getRenderSurface(parentRenderer);
    CHECK(clipSurface != NULL, "ClipByChildren readback failed");
    CHECK(pixel_is_rgb(clipSurface, 5, 6, 17, 34, 51),
          "parent clear erased mapped child contents");
    SDL_FreeSurface(clipSurface);
    XFreeGC(display, childClipGc);
    /* Restore the pre-probe stacking order so the XCirculateSubwindowsUp test
     * below still sees [child2, child1] and raises child2 to the top as
     * expected.
     */
    XLowerWindow(display, child2);
    CHECK(
        XCheckTypedWindowEvent(display, child1, VisibilityNotify, &visibility),
        "VisibilityNotify was not delivered for mapped child");
    CHECK(visibility.xvisibility.state == VisibilityUnobscured ||
              visibility.xvisibility.state == VisibilityPartiallyObscured ||
              visibility.xvisibility.state == VisibilityFullyObscured,
          "VisibilityNotify returned invalid state");

    XSelectInput(display, parent, ExposureMask);
    XSelectInput(display, child2, StructureNotifyMask);
    while (XCheckTypedWindowEvent(display, parent, Expose, &visibility)) {
    }
    while (
        XCheckTypedWindowEvent(display, child2, ConfigureNotify, &visibility)) {
    }
    CHECK(XMoveWindow(display, child2, 9, 10), "child move configure failed");
    CHECK(XCheckTypedWindowEvent(display, child2, ConfigureNotify, &visibility),
          "child move did not post ConfigureNotify");
    CHECK(visibility.xconfigure.x == 9 && visibility.xconfigure.y == 10,
          "child move ConfigureNotify coordinates were incorrect");
    CHECK(XCheckTypedWindowEvent(display, parent, Expose, &visibility),
          "child move did not expose uncovered parent pixels");
    CHECK(visibility.xexpose.x == 5 && visibility.xexpose.y == 6,
          "parent Expose after child move used unexpected coordinates");

    Window mappedParent =
        XCreateSimpleWindow(display, root, 70, 70, 64, 64, 0, 0, 0);
    Window mappedChild =
        XCreateSimpleWindow(display, mappedParent, 2, 3, 12, 12, 0, 0, 0);
    CHECK(mappedParent != None && mappedChild != None,
          "mapped reparent setup failed");
    XSelectInput(display, mappedChild, StructureNotifyMask | ExposureMask);
    CHECK(XMapWindow(display, mappedParent),
          "mapped reparent parent map failed");
    CHECK(XMapWindow(display, mappedChild), "mapped reparent child map failed");
    while (XCheckWindowEvent(display, mappedChild,
                             StructureNotifyMask | ExposureMask,
                             &reparentEvent)) {
    }
    CHECK(XReparentWindow(display, mappedChild, root, 90, 91),
          "child to top-level XReparentWindow failed");
    CHECK(expect_map_state(display, mappedChild, IsViewable),
          "child to top-level reparent did not keep mapped state");
    CHECK(GET_WINDOW_STRUCT(mappedChild)->sdlWindow != NULL,
          "child to top-level reparent did not realize an SDL window");
    CHECK(GET_WINDOW_STRUCT(mappedChild)->hasPresented,
          "child to top-level reparent did not present before show");
    CHECK(XCheckWindowEvent(display, mappedChild, StructureNotifyMask,
                            &reparentEvent),
          "child to top-level did not post UnmapNotify");
    CHECK(reparentEvent.type == UnmapNotify &&
              !reparentEvent.xunmap.from_configure,
          "child to top-level first structure event was not UnmapNotify");
    CHECK(XCheckWindowEvent(display, mappedChild, StructureNotifyMask,
                            &reparentEvent),
          "child to top-level did not post ReparentNotify");
    CHECK(reparentEvent.type == ReparentNotify &&
              reparentEvent.xreparent.parent == root &&
              reparentEvent.xreparent.x == 90 &&
              reparentEvent.xreparent.y == 91,
          "child to top-level ReparentNotify fields were incorrect");
    CHECK(XCheckWindowEvent(display, mappedChild, StructureNotifyMask,
                            &reparentEvent),
          "child to top-level did not post MapNotify");
    CHECK(reparentEvent.type == MapNotify,
          "child to top-level third structure event was not MapNotify");
    CHECK(!XCheckTypedWindowEvent(display, mappedChild, ConfigureNotify,
                                  &reparentEvent),
          "child to top-level posted a spurious ConfigureNotify");
    CHECK(XReparentWindow(display, mappedChild, mappedParent, 4, 5),
          "top-level to child XReparentWindow failed");
    CHECK(expect_map_state(display, mappedChild, IsViewable),
          "top-level to child reparent did not keep mapped state");
    CHECK(XCheckWindowEvent(display, mappedChild, StructureNotifyMask,
                            &reparentEvent),
          "top-level to child did not post UnmapNotify");
    CHECK(reparentEvent.type == UnmapNotify &&
              !reparentEvent.xunmap.from_configure,
          "top-level to child first structure event was not UnmapNotify");
    CHECK(XCheckWindowEvent(display, mappedChild, StructureNotifyMask,
                            &reparentEvent),
          "top-level to child did not post ReparentNotify");
    CHECK(reparentEvent.type == ReparentNotify &&
              reparentEvent.xreparent.parent == mappedParent &&
              reparentEvent.xreparent.x == 4 && reparentEvent.xreparent.y == 5,
          "top-level to child ReparentNotify fields were incorrect");
    CHECK(XCheckWindowEvent(display, mappedChild, StructureNotifyMask,
                            &reparentEvent),
          "top-level to child did not post MapNotify");
    CHECK(reparentEvent.type == MapNotify,
          "top-level to child third structure event was not MapNotify");
    CHECK(!XCheckTypedWindowEvent(display, mappedChild, ConfigureNotify,
                                  &reparentEvent),
          "top-level to child posted a spurious ConfigureNotify");
    XDestroyWindow(display, mappedParent);

    XCirculateSubwindowsUp(display, parent);
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after circulate-up failed");
    CHECK(nchildren == 2 && children[0] == child1 && children[1] == child2,
          "XCirculateSubwindowsUp did not raise lowest occluded child");
    XFree(children);

    XCirculateSubwindows(display, parent, LowerHighest);
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after circulate-down failed");
    CHECK(nchildren == 2 && children[0] == child2 && children[1] == child1,
          "XCirculateSubwindows Down did not lower highest occluding child");
    XFree(children);

    Window farParent =
        XCreateSimpleWindow(display, root, 0, 0, 80, 24, 0, 0, 0);
    Window farChild1 =
        XCreateSimpleWindow(display, farParent, 1, 2, 16, 16, 0, 0, 0);
    Window farChild2 =
        XCreateSimpleWindow(display, farParent, 40, 2, 16, 16, 0, 0, 0);
    CHECK(farParent != None && farChild1 != None && farChild2 != None,
          "non-overlap window creation failed");
    XMapWindow(display, farParent);
    XMapSubwindows(display, farParent);
    XCirculateSubwindowsUp(display, farParent);
    CHECK(XQueryTree(display, farParent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after non-overlap circulate-up failed");
    CHECK(
        nchildren == 2 && children[0] == farChild1 && children[1] == farChild2,
        "XCirculateSubwindowsUp changed non-overlapping siblings");
    XFree(children);
    XCirculateSubwindowsDown(display, farParent);
    CHECK(XQueryTree(display, farParent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after non-overlap circulate-down failed");
    CHECK(
        nchildren == 2 && children[0] == farChild1 && children[1] == farChild2,
        "XCirculateSubwindowsDown changed non-overlapping siblings");
    XFree(children);
    XDestroyWindow(display, farParent);

    XUnmapSubwindows(display, parent);
    CHECK(expect_map_state(display, child1, IsUnmapped),
          "child1 was not unmapped");
    CHECK(expect_map_state(display, child2, IsUnmapped),
          "child2 was not unmapped");

    XDestroySubwindows(display, parent);
    CHECK(XQueryTree(display, parent, &queryRoot, &queryParent, &children,
                     &nchildren),
          "XQueryTree after destroy-subwindows failed");
    CHECK(nchildren == 0, "XDestroySubwindows did not remove all children");
    XFree(children);

    XDestroyWindow(display, parent);
    XDestroyWindow(display, normalTop);
    XDestroyWindow(display, overrideTop);
    XFreeCursor(display, fontCursor);
    while (XPending(display)) {
        XEvent pending;
        XNextEvent(display, &pending);
    }
    return 1;
}

static int test_fonts(Display *display)
{
    /* The names list passed to XFreeFontInfo must follow the same caller-owned,
     * NULL-terminated convention that XListFonts now returns: each entry is
     * heap-allocated and the array carries a trailing NULL sentinel that
     * XFreeFontNames walks.
     */
    char **names = malloc(sizeof(char *) * 3);
    CHECK(names != NULL, "font names allocation failed");
    names[0] = strdup("font-a");
    names[1] = strdup("font-b");
    names[2] = NULL;
    CHECK(names[0] != NULL && names[1] != NULL, "font names strdup failed");

    XFontStruct *infos = calloc(2, sizeof(XFontStruct));
    CHECK(infos != NULL, "font info allocation failed");
    infos[0].properties = calloc(1, sizeof(XFontProp));
    infos[0].per_char = calloc(1, sizeof(XCharStruct));
    infos[1].per_char = calloc(1, sizeof(XCharStruct));
    CHECK(infos[0].properties != NULL && infos[0].per_char != NULL &&
              infos[1].per_char != NULL,
          "font info sub-allocation failed");
    CHECK(XFreeFontInfo(names, infos, 2) == 1, "XFreeFontInfo failed");

    int fontCount = -1;
    XFontStruct *listedInfo = (XFontStruct *) 1;
    char **listedNames =
        XListFontsWithInfo(display, "*", 1, &fontCount, &listedInfo);
    if (listedNames == NULL) {
        CHECK(fontCount == 0 && listedInfo == NULL,
              "XListFontsWithInfo empty result did not reset outputs");
    } else {
        CHECK(fontCount > 0 && listedInfo != NULL,
              "XListFontsWithInfo returned incomplete font data");
        if (listedNames[0][0] == '-') {
            int dashCount = 0;
            for (const char *p = listedNames[0]; *p; p++) {
                if (*p == '-')
                    dashCount++;
            }
            CHECK(dashCount >= 12 && strstr(listedNames[0], "iso10646-1"),
                  "generated XLFD name missed charset fields");
        }
        CHECK(XFreeFontInfo(listedNames, listedInfo, fontCount) == 1,
              "XFreeFontInfo failed for listed fonts");
    }

    CHECK(exercise_fixed_font_program(display),
          "fixed-font program compatibility failed");

    const char *fontDirs[] = {"fonts", "/System/Library/Fonts",
                              "/Library/Fonts", "/usr/share/fonts",
                              "/usr/local/share/fonts"};
    for (size_t i = 0; i < sizeof(fontDirs) / sizeof(fontDirs[0]); i++) {
        char *path = (char *) fontDirs[i];
        int fdCountBefore = count_open_file_descriptors();
        XSetFontPath(display, &path, 1);
        int fdCountAfter = count_open_file_descriptors();
        CHECK(fdCountBefore < 0 || fdCountAfter <= fdCountBefore + 8,
              "XSetFontPath leaked font file descriptors");
        int count = 0;
        char **availableFonts = XListFonts(display, "*", 1, &count);
        if (availableFonts == NULL || count == 0) {
            XFreeFontNames(availableFonts);
            continue;
        }
        XFreeFontNames(availableFonts);

        int aliasCount = 0;
        char **aliases = XListFonts(display, "9x1?", 4, &aliasCount);
        CHECK(aliases != NULL && aliasCount >= 2,
              "fixed-size aliases were not listed");
        XFreeFontNames(aliases);

        int cappedAliasCount = 0;
        aliases = XListFonts(display, "9x13", 1, &cappedAliasCount);
        CHECK(aliases != NULL && cappedAliasCount == 1,
              "XListFonts alias results exceeded maxnames");
        XFreeFontNames(aliases);

        /* XListFonts must not return the caller's pattern buffer as a list
         * entry. Mutate the buffer after the call and confirm the returned name
         * is unchanged.
         */
        char patternBuf[64];
        snprintf(patternBuf, sizeof(patternBuf), "-*helv*medium-r-*-12-*");
        int aliasResolved = 0;
        char **aliasList = XListFonts(display, patternBuf, 1, &aliasResolved);
        if (aliasList != NULL && aliasResolved > 0) {
            char first[64];
            snprintf(first, sizeof(first), "%s", aliasList[0]);
            memset(patternBuf, 'X', sizeof(patternBuf) - 1);
            patternBuf[sizeof(patternBuf) - 1] = '\0';
            CHECK(!strcmp(aliasList[0], first),
                  "XListFonts aliased the caller's pattern buffer");
            CHECK(!strchr(aliasList[0], '*'),
                  "XListFonts returned a wildcard pattern as a font name");
        }
        XFreeFontNames(aliasList);

        XFontStruct *variable = XLoadQueryFont(display, "variable");
        CHECK(variable != NULL && variable->fid != None,
              "variable alias did not load");
        XFreeFont(display, variable);

        XFontStruct *sevenByThirteenBold = XLoadQueryFont(display, "7x13bold");
        CHECK(sevenByThirteenBold != NULL && sevenByThirteenBold->fid != None,
              "7x13bold alias did not load");
        CHECK(sevenByThirteenBold->ascent == 11 &&
                  sevenByThirteenBold->descent == 2,
              "7x13bold alias did not use native ascent/descent");
        CHECK(XTextWidth(sevenByThirteenBold, "Menu", 4) == 28,
              "7x13bold alias did not use native width");
        XFreeFont(display, sevenByThirteenBold);

        XFontStruct *nineByThirteen = XLoadQueryFont(display, "9x13");
        CHECK(nineByThirteen != NULL && nineByThirteen->fid != None,
              "9x13 alias did not load");
        CHECK(nineByThirteen->ascent + nineByThirteen->descent <= 24,
              "9x13 alias opened with inflated metrics");
        CHECK(XTextWidth(nineByThirteen, "A", 1) > 0,
              "XTextWidth ASCII width failed");
        CHECK(XTextWidth(nineByThirteen, "\xc3\xa9", 2) > 0,
              "XTextWidth UTF-8 width failed");
        XChar2b wideA[] = {{0, 'A'}};
        CHECK(XTextWidth16(nineByThirteen, wideA, 1) ==
                  XTextWidth(nineByThirteen, "A", 1),
              "XTextWidth16 ASCII decoding mismatch");
        XChar2b wideWithNul[] = {{0, 0}, {0, 'A'}};
        CHECK(XTextWidth16(nineByThirteen, wideWithNul, 2) > 0,
              "XTextWidth16 ignored length-counted text after U+0000");
        XFreeFont(display, nineByThirteen);

        XFontStruct *nineByFifteen = XLoadQueryFont(display, "9x15");
        CHECK(nineByFifteen != NULL && nineByFifteen->fid != None,
              "9x15 alias did not load");
        CHECK(XTextWidth(nineByFifteen, "Viola", 5) > 0,
              "9x15 alias selected a font without ASCII metrics");
        CHECK(XTextWidth(nineByFifteen, " ", 1) > 0,
              "9x15 alias did not provide a usable space width");
        XFreeFont(display, nineByFifteen);

        int motifFixedCount = 0;
        char **motifFixedNames =
            XListFonts(display, "-misc-fixed-medium-r-*-*-14-*-*-*-*-*-*-*", 1,
                       &motifFixedCount);
        CHECK(motifFixedNames != NULL && motifFixedCount == 1,
              "Viola fixed XLFD alias was not listed");
        XFontStruct *motifFixed = XLoadQueryFont(display, motifFixedNames[0]);
        CHECK(motifFixed != NULL && motifFixed->fid != None,
              "listed Viola fixed XLFD alias did not load");
        CHECK(font_struct_char_width(motifFixed, ' ') > 0,
              "listed Viola fixed XLFD alias had zero space width");
        XFreeFont(display, motifFixed);
        XFreeFontNames(motifFixedNames);

        int motifHelveticaCount = 0;
        char **motifHelveticaNames = XListFonts(
            display, "-adobe-helvetica-medium-r-*-*-14-*-*-*-p-*-*-*", 1,
            &motifHelveticaCount);
        CHECK(motifHelveticaNames != NULL && motifHelveticaCount == 1,
              "Viola Helvetica XLFD alias was not listed");
        XFontStruct *motifHelvetica =
            XLoadQueryFont(display, motifHelveticaNames[0]);
        CHECK(motifHelvetica != NULL && motifHelvetica->fid != None,
              "listed Viola Helvetica XLFD alias did not load");
        CHECK(font_struct_char_width(motifHelvetica, ' ') > 0,
              "listed Viola Helvetica XLFD alias had zero space width");
        Pixmap motifTextPixmap = XCreatePixmap(
            display, RootWindow(display, DefaultScreen(display)), 160, 32,
            DefaultDepth(display, DefaultScreen(display)));
        CHECK(motifTextPixmap != None,
              "Viola Helvetica alias text pixmap creation failed");
        GC motifTextGc = XCreateGC(display, motifTextPixmap, 0, NULL);
        CHECK(motifTextGc != NULL, "Viola Helvetica alias GC creation failed");
        CHECK(XSetFont(display, motifTextGc, motifHelvetica->fid),
              "Viola Helvetica alias XSetFont failed");
        CHECK(XSetForeground(display, motifTextGc, 0x00FFFFFF),
              "Viola Helvetica alias white foreground failed");
        CHECK(XFillRectangle(display, motifTextPixmap, motifTextGc, 0, 0, 160,
                             32),
              "Viola Helvetica alias background fill failed");
        CHECK(XSetForeground(display, motifTextGc, 0x00000000),
              "Viola Helvetica alias black foreground failed");
        last_error_code = 0;
        int (*oldErrorHandler)(Display *, XErrorEvent *) =
            XSetErrorHandler(record_error);
        CHECK(XDrawString(display, motifTextPixmap, motifTextGc, 4, 18,
                          "World Wide Web", 14),
              "Viola Helvetica alias XDrawString failed");
        XSync(display, False);
        XSetErrorHandler(oldErrorHandler);
        CHECK(last_error_code == 0,
              "Viola Helvetica alias XDrawString raised an X error");
        SDL_Renderer *motifTextRenderer = NULL;
        GET_RENDERER(motifTextPixmap, motifTextRenderer);
        SDL_Surface *motifTextSurface = getRenderSurface(motifTextRenderer);
        CHECK(motifTextSurface,
              "getRenderSurface for anti-aliased Helvetica text failed");
        int sawAntialiasedEdge = 0;
        for (int ty = 0; ty < motifTextSurface->h && !sawAntialiasedEdge;
             ty++) {
            for (int tx = 0; tx < motifTextSurface->w; tx++) {
                if (pixel_is_between_black_and_white(motifTextSurface, tx,
                                                     ty)) {
                    sawAntialiasedEdge = 1;
                    break;
                }
            }
        }
        SDL_FreeSurface(motifTextSurface);
        CHECK(sawAntialiasedEdge,
              "Viola Helvetica alias text did not render anti-aliased edges");
        XFreeGC(display, motifTextGc);
        XFreePixmap(display, motifTextPixmap);
        XFreeFont(display, motifHelvetica);
        XFreeFontNames(motifHelveticaNames);

        XFontStruct *helvetica =
            XLoadQueryFont(display, "*-helvetica-medium-r-normal--14-*");
        if (helvetica) {
            CHECK(helvetica->fid != None,
                  "helvetica XLFD returned invalid font id");
            CHECK(XTextWidth(helvetica, "iiii", 4) <
                      XTextWidth(helvetica, "WWWW", 4),
                  "helvetica XLFD alias did not use proportional metrics");
            XFreeFont(display, helvetica);

            /* Re-probing the same wildcard must not duplicate font cache
             * entries. fontCache is now static, so check it black-box: since
             * XListFonts enumerates the cache, a duplicate entry would show up
             * as an extra or repeated name here.
             */
            const char *helvPattern = "*-helvetica-medium-r-normal--14-*";
            int helvBeforeCount = 0;
            char **helvBefore =
                XListFonts(display, helvPattern, 64, &helvBeforeCount);
            XFreeFontNames(helvBefore);
            XFontStruct *helvReprobe = XLoadQueryFont(display, helvPattern);
            if (helvReprobe)
                XFreeFont(display, helvReprobe);
            int helvAfterCount = 0;
            char **helvAfter =
                XListFonts(display, helvPattern, 64, &helvAfterCount);
            CHECK(helvAfterCount == helvBeforeCount,
                  "repeated helvetica probe duplicated font cache entries");
            for (int a = 0; a < helvAfterCount; a++) {
                for (int b = a + 1; b < helvAfterCount; b++) {
                    CHECK(strcmp(helvAfter[a], helvAfter[b]) != 0,
                          "font cache listed a duplicate name");
                }
            }
            XFreeFontNames(helvAfter);
        }
        XFontStruct *helvetica140 = XLoadQueryFont(
            display, "-*-helvetica-medium-r-*-*-*-140-*-*-*-*-*-*");
        if (helvetica140) {
            CHECK(helvetica140->fid != None,
                  "helvetica 140-decipoint XLFD returned invalid font id");
            CHECK(helvetica140->ascent == 19 && helvetica140->descent == 4,
                  "helvetica 140-decipoint XLFD alias used wrong vertical "
                  "metrics");
            CHECK(XTextWidth(helvetica140, "Motif", 5) > 30,
                  "helvetica 140-decipoint XLFD alias ignored point size");
            CHECK(
                XTextWidth(helvetica140, "iiii", 4) <
                    XTextWidth(helvetica140, "WWWW", 4),
                "helvetica 140-decipoint XLFD alias lost proportional metrics");
            XFreeFont(display, helvetica140);
        }
        XFontStruct *helvBold = XLoadQueryFont(display, "*helv*bold*-r-*-12-*");
        CHECK(helvBold != NULL,
              "Motif helv bold wildcard did not resolve to a fallback font");
        CHECK(helvBold->fid != None,
              "Motif helv bold wildcard returned invalid font id");
        CHECK(helvBold->ascent + helvBold->descent >= 12,
              "Motif helv bold wildcard fell back to the default font size");
        XFreeFont(display, helvBold);

        XFontStruct *xephemFont = XLoadQueryFont(
            display, "*-lucidatypewriter*medium*-10-*-iso8859-1");
        CHECK(xephemFont != NULL,
              "XEphem lucidatypewriter wildcard did not resolve");
        CHECK(xephemFont->ascent + xephemFont->descent >= 13,
              "XEphem lucidatypewriter wildcard used 10 pixels, not 10pt");
        XFreeFont(display, xephemFont);

        XFontStruct *times14 =
            XLoadQueryFont(display, "-*times*medium*-r-*--14-*");
        CHECK(times14 != NULL,
              "Motif Times 14 wildcard did not resolve to a fallback font");
        CHECK(times14->fid != None,
              "Motif Times 14 wildcard returned invalid font id");
        XFreeFont(display, times14);

        int times14ListedCount = 0;
        char **times14Listed = XListFonts(display, "-*times*medium*-r-*--14-*",
                                          1, &times14ListedCount);
        CHECK(times14Listed != NULL && times14ListedCount == 1,
              "Motif Times 14 wildcard was not listed");
        CHECK(
            !strchr(times14Listed[0], '*') && strstr(times14Listed[0], "--14-"),
            "Motif Times 14 listed alias did not preserve requested size");
        times14 = XLoadQueryFont(display, times14Listed[0]);
        CHECK(times14 != NULL && times14->fid != None,
              "listed Motif Times 14 alias did not load");
        CHECK(times14->ascent + times14->descent >= 14,
              "listed Motif Times 14 alias loaded at default size");
        XFreeFont(display, times14);
        XFreeFontNames(times14Listed);

        XFontStruct *timesBoldMenu =
            XLoadQueryFont(display, "*times*bold*-r-*-14-*");
        CHECK(timesBoldMenu != NULL,
              "Motif Times bold menu wildcard did not resolve to a fallback "
              "font");
        CHECK(timesBoldMenu->fid != None,
              "Motif Times bold menu wildcard returned invalid font id");
        CHECK(timesBoldMenu->ascent + timesBoldMenu->descent >= 14,
              "Motif Times bold menu wildcard fell back to the default font "
              "size");
        XFreeFont(display, timesBoldMenu);

        /* One XLFD field tokenizer resolves the pixel size for every name
         * shape, so pin the resolution of representative XLFD / wildcard /
         * compact names. The "*-fixed-13-*" row is the ambiguity the old
         * compact heuristic got wrong (it read 13 as a point size -> 18px); the
         * number is a pixel field with no weight token, so it must stay
         * 13. Point-size rows exercise the decipoint conversion and the
         * Helvetica 120-decipoint -> 16px baseline nudge.
         */
        struct {
            const char *name;
            int expected;
        } xlfdSizeTable[] = {
            {"-misc-fixed-medium-r-*-*-14-*-*-*-*-*-*-*", 14},
            {"-adobe-helvetica-medium-r-*-*-14-*-*-*-p-*-*-*", 14},
            {"-*-helvetica-medium-r-normal--12-*-*-*-p-*-iso8859-1", 12},
            {"-adobe-helvetica-bold-r-normal--24-*-*-*-p-*-iso8859-1", 24},
            {"-*-helvetica-medium-r-*-*-*-140-*-*-*-*-*-*", 19},
            {"-*-helvetica-medium-r-normal-*-*-120-*-*-*-*-iso8859-1", 16},
            {"*-lucidatypewriter*medium*-10-*-iso8859-1", 14},
            {"*helv*bold*-r-*-12-*", 12},
            {"-*times*medium*-r-*--14-*", 14},
            {"*-fixed-13-*", 13},
            {"6x13", 13},
            {"9x15", 15},
        };
        for (size_t t = 0; t < sizeof(xlfdSizeTable) / sizeof(xlfdSizeTable[0]);
             t++) {
            int resolved = x11compat_font_pixel_size(xlfdSizeTable[t].name);
            if (resolved != xlfdSizeTable[t].expected) {
                fprintf(stderr, "XLFD size '%s': expected %d, got %d\n",
                        xlfdSizeTable[t].name, xlfdSizeTable[t].expected,
                        resolved);
            }
            CHECK(resolved == xlfdSizeTable[t].expected,
                  "XLFD pixel-size resolution regressed for a table entry");
        }

        Window root = RootWindow(display, DefaultScreen(display));
        Pixmap pixmap =
            XCreatePixmap(display, root, 96, 32,
                          DefaultDepth(display, DefaultScreen(display)));
        CHECK(pixmap != None, "font pixmap creation failed");
        GC gc = XCreateGC(display, pixmap, 0, NULL);
        CHECK(gc != NULL, "font GC creation failed");
        CHECK(
            XSetState(display, gc, 0xFF112233, 0xFF445566, GXcopy, 0xFFFFFFFF),
            "font GC state setup failed");
        CHECK(XDrawImageString(display, pixmap, gc, 2, 18, "text", 4),
              "XDrawImageString failed");
        for (int draw = 0; draw < 64; draw++) {
            CHECK(XDrawString(display, pixmap, gc, 2, 18, "text", 4),
                  "repeated XDrawString benchmark draw failed");
        }
        Font implicitFont = GET_GC(gc)->font;
        CHECK(implicitFont != None, "text draw did not attach implicit font");
        CHECK(XUnloadFont(display, implicitFont),
              "XUnloadFont rejected implicit fixed font");
        CHECK(XDrawString(display, pixmap, gc, 2, 18, "text", 4),
              "GC-held font stopped drawing after XUnloadFont");
        CHECK(XSetForeground(display, gc, 0x00FFFFFF),
              "font background color setup failed");
        CHECK(XFillRectangle(display, pixmap, gc, 0, 0, 96, 32),
              "font background fill failed");
        CHECK(XSetForeground(display, gc, 0),
              "font X11 black pixel setup failed");
        CHECK(XDrawString(display, pixmap, gc, 2, 18, "text", 4),
              "XDrawString with X11 black pixel failed");
        SDL_Renderer *renderer = NULL;
        GET_RENDERER(pixmap, renderer);
        SDL_Surface *textSurface = getRenderSurface(renderer);
        CHECK(textSurface, "getRenderSurface for X11 black text failed");
        int sawBlackTextPixel = 0;
        for (int ty = 0; ty < textSurface->h && !sawBlackTextPixel; ty++) {
            for (int tx = 0; tx < textSurface->w; tx++) {
                if (pixel_is_rgb(textSurface, tx, ty, 0, 0, 0)) {
                    sawBlackTextPixel = 1;
                    break;
                }
            }
        }
        SDL_FreeSurface(textSurface);
        CHECK(sawBlackTextPixel,
              "XDrawString treated X11 black pixel as transparent");

        /* Text-stamp invalidation regression. The stamp cache only tracks
         * window drawables, and only a TTF (non-fixed) font records a stamp, so
         * draw with a helvetica GC on a mapped window. An identical redraw of a
         * stamped cell is skipped to keep anti-aliased text from accumulating;
         * overwriting the cell must drop the stamp so the redraw actually
         * repaints. A stale stamp would skip the redraw and leave the overwrite
         * showing, which is the missing-text failure mode of the cache.
         */
        Window stampDrawWindow =
            XCreateSimpleWindow(display, root, 0, 0, 96, 32, 0, 0, 0x00FFFFFF);
        CHECK(stampDrawWindow != None,
              "text stamp draw window creation failed");
        XMapWindow(display, stampDrawWindow);
        GC stampDrawGc = XCreateGC(display, stampDrawWindow, 0, NULL);
        CHECK(stampDrawGc != NULL, "text stamp draw GC creation failed");
        XFontStruct *stampDrawFont =
            XLoadQueryFont(display, "*-helvetica-medium-r-normal--14-*");
        CHECK(stampDrawFont && stampDrawFont->fid != None,
              "text stamp draw font load failed");
        CHECK(XSetFont(display, stampDrawGc, stampDrawFont->fid),
              "text stamp draw set font failed");
        CHECK(XSetForeground(display, stampDrawGc, 0),
              "text stamp draw black setup failed");
        CHECK(
            XDrawString(display, stampDrawWindow, stampDrawGc, 2, 18, "MM", 2),
            "text stamp draw first draw failed");
        GET_RENDERER(stampDrawWindow, renderer);
        textSurface = getRenderSurface(renderer);
        CHECK(textSurface, "text stamp draw first readback failed");
        int stampBlackFirst = count_rgb_pixels(textSurface, 0, 0, 0);
        SDL_FreeSurface(textSurface);
        CHECK(stampBlackFirst > 0, "text stamp first draw produced no black");
        /* Overwrite the stamped cell with the background; this must invalidate
         * the stamp recorded by the first draw.
         */
        CHECK(XSetForeground(display, stampDrawGc, 0x00FFFFFF),
              "text stamp draw white setup failed");
        CHECK(
            XFillRectangle(display, stampDrawWindow, stampDrawGc, 0, 0, 96, 32),
            "text stamp draw overwrite failed");
        textSurface = getRenderSurface(renderer);
        CHECK(textSurface, "text stamp draw overwrite readback failed");
        int stampBlackCleared = count_rgb_pixels(textSurface, 0, 0, 0);
        SDL_FreeSurface(textSurface);
        CHECK(stampBlackCleared < stampBlackFirst,
              "text stamp overwrite did not clear the cell");
        /* Redraw the identical label. If the overwrite failed to drop the
         * stamp, the skip leaves the cell cleared and the black never returns.
         */
        CHECK(XSetForeground(display, stampDrawGc, 0),
              "text stamp draw redraw black setup failed");
        CHECK(
            XDrawString(display, stampDrawWindow, stampDrawGc, 2, 18, "MM", 2),
            "text stamp draw redraw failed");
        textSurface = getRenderSurface(renderer);
        CHECK(textSurface, "text stamp draw redraw readback failed");
        int stampBlackRedraw = count_rgb_pixels(textSurface, 0, 0, 0);
        SDL_FreeSurface(textSurface);
        CHECK(stampBlackRedraw > stampBlackCleared,
              "overwriting a stamped cell left a stale skip that dropped the "
              "redraw");
        XFreeFont(display, stampDrawFont);
        XFreeGC(display, stampDrawGc);
        XDestroyWindow(display, stampDrawWindow);

        Window stampWindow =
            XCreateSimpleWindow(display, root, 0, 0, 96, 32, 0, 0, 0x00FFFFFF);
        CHECK(stampWindow != None, "text stamp unmap window creation failed");
        XMapWindow(display, stampWindow);
        Window stampCover = XCreateSimpleWindow(display, stampWindow, 0, 0, 48,
                                                32, 0, 0, 0x00FFFFFF);
        CHECK(stampCover != None, "text stamp unmap cover creation failed");
        XMapWindow(display, stampCover);
        SDL_Rect stampCell = {2, 4, 24, 16};
        textStampRecord(stampWindow, &stampCell, GET_GC(gc)->font, 0, "MM");
        CHECK(
            textStampLookup(stampWindow, &stampCell, GET_GC(gc)->font, 0, "MM"),
            "text stamp unmap setup did not record a stamp");
        CHECK(XUnmapWindow(display, stampCover),
              "text stamp unmap cover unmap failed");
        CHECK(!textStampLookup(stampWindow, &stampCell, GET_GC(gc)->font, 0,
                               "MM"),
              "unmapping an overlapping child left a stale XDrawString stamp");
        XDestroyWindow(display, stampWindow);

        XGCValues values;
        CHECK(XGetGCValues(display, gc, GCForeground | GCBackground, &values),
              "font GC readback failed");
        CHECK(values.foreground == 0 && values.background == 0xFF445566,
              "XDrawImageString mutated GC colors");
        XFreeGC(display, gc);
        XFreePixmap(display, pixmap);

        pixmap = XCreatePixmap(display, root, 96, 32,
                               DefaultDepth(display, DefaultScreen(display)));
        gc = XCreateGC(display, pixmap, 0, NULL);
        Font heldFont = XLoadFont(display, "fixed");
        CHECK(heldFont != None, "explicit fixed font did not load");
        CHECK(XSetFont(display, gc, heldFont), "XSetFont rejected fixed font");
        CHECK(XUnloadFont(display, heldFont), "XUnloadFont rejected held font");
        CHECK(XDrawString(display, pixmap, gc, 2, 18, "held", 4),
              "GC-held explicit font stopped drawing after XUnloadFont");
        int (*fontPreviousErrorHandler)(Display *, XErrorEvent *) =
            XSetErrorHandler(ignored_error);
        CHECK(!XSetFont(display, gc, heldFont),
              "XSetFont accepted a closed client font id");
        XSetErrorHandler(fontPreviousErrorHandler);
        XFreeGC(display, gc);
        XFreePixmap(display, pixmap);

        pixmap = XCreatePixmap(display, root, 96, 32,
                               DefaultDepth(display, DefaultScreen(display)));
        gc = XCreateGC(display, pixmap, 0, NULL);
        Font closedFont = XLoadFont(display, "fixed");
        CHECK(closedFont != None, "stale-id fixed font did not load");
        CHECK(XUnloadFont(display, closedFont),
              "XUnloadFont rejected unreferenced font");
        fontPreviousErrorHandler = XSetErrorHandler(ignored_error);
        CHECK(!XSetFont(display, gc, closedFont),
              "XSetFont accepted an immediately closed font id");
        XSetErrorHandler(fontPreviousErrorHandler);
        XFreeGC(display, gc);
        XFreePixmap(display, pixmap);
        break;
    }
    XSetFontPath(display, NULL, 0);

    int (*previousErrorHandler)(Display *, XErrorEvent *) =
        XSetErrorHandler(ignored_error);
    XFontStruct *invalid = XQueryFont(display, None);
    XSetErrorHandler(previousErrorHandler);
    CHECK(invalid == NULL, "XQueryFont accepted None");

    /* XQueryTextExtents Status contract: nonzero means success. The pre-fix
     * code returned 0 after filling outputs, which Motif/Xt measurement paths
     * interpreted as failure and discarded valid metrics.
     */
    {
        Font measureFont = XLoadFont(display, "fixed");
        if (measureFont != None) {
            int dir = 0;
            int ascent = 0;
            int descent = 0;
            XCharStruct overall;
            memset(&overall, 0, sizeof(overall));
            Status st = XQueryTextExtents(display, measureFont, "hello", 5,
                                          &dir, &ascent, &descent, &overall);
            CHECK(st != 0,
                  "XQueryTextExtents Status must be nonzero on success");
            CHECK(overall.width > 0,
                  "XQueryTextExtents filled overall.width == 0");
            XChar2b wide[] = {{0, 'h'}, {0, 'i'}};
            st = XQueryTextExtents16(display, measureFont, wide, 2, &dir,
                                     &ascent, &descent, &overall);
            CHECK(st != 0,
                  "XQueryTextExtents16 Status must be nonzero on success");
            GC measureGc =
                XCreateGC(display, DefaultRootWindow(display), 0, NULL);
            CHECK(measureGc != NULL,
                  "XQueryTextExtents GC metric GC creation failed");
            /* A GC with no font set must still measure via a temporary fallback
             * font, and must not retain that font afterward.
             */
            memset(&overall, 0, sizeof(overall));
            st = XQueryTextExtents(display, XGContextFromGC(measureGc), "hello",
                                   5, &dir, &ascent, &descent, &overall);
            CHECK(st != 0, "XQueryTextExtents must accept a fontless GC id");
            CHECK(overall.width > 0,
                  "XQueryTextExtents fontless GC filled overall.width == 0");
            XGCValues gcValues;
            memset(&gcValues, 0, sizeof(gcValues));
            CHECK(XGetGCValues(display, measureGc, GCFont, &gcValues),
                  "XQueryTextExtents fontless GC XGetGCValues failed");
            CHECK(gcValues.font == None,
                  "XQueryTextExtents fontless GC retained the temporary font");
            CHECK(XSetFont(display, measureGc, measureFont),
                  "XQueryTextExtents GC metric XSetFont failed");
            CHECK(XUnloadFont(display, measureFont),
                  "XQueryTextExtents GC metric XUnloadFont failed");
            memset(&overall, 0, sizeof(overall));
            st = XQueryTextExtents(display, XGContextFromGC(measureGc), "hello",
                                   5, &dir, &ascent, &descent, &overall);
            CHECK(st != 0,
                  "XQueryTextExtents must accept a GC-retained unloaded font");
            CHECK(overall.width > 0,
                  "XQueryTextExtents GC id filled overall.width == 0");
            memset(&overall, 0, sizeof(overall));
            st = XQueryTextExtents16(display, XGContextFromGC(measureGc), wide,
                                     2, &dir, &ascent, &descent, &overall);
            CHECK(
                st != 0,
                "XQueryTextExtents16 must accept a GC-retained unloaded font");
            XFreeGC(display, measureGc);
        }
    }
    return 1;
}

static int test_contexts(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 0, 0, 16, 16, 0, 0, 0);
    CHECK(window != None, "XCreateSimpleWindow for context test failed");

    int value = 42;
    XPointer found = NULL;
    XContext context = XUniqueContext();
    CHECK(context != 0, "XUniqueContext returned 0");
    CHECK(XSaveContext(display, window, context, (const char *) &value) == 0,
          "XSaveContext failed");
    CHECK(XFindContext(display, window, context, &found) == 0,
          "XFindContext failed");
    CHECK(found == (XPointer) &value, "XFindContext returned wrong value");
    CHECK(XDeleteContext(display, window, context) == 0,
          "XDeleteContext failed");
    CHECK(XFindContext(display, window, context, &found) != 0,
          "XFindContext found deleted context");

    XrmQuark quark = XrmStringToQuark("app.window.title");
    CHECK(quark != NULLQUARK, "XrmStringToQuark returned NULLQUARK");
    CHECK(!strcmp(XrmQuarkToString(quark), "app.window.title"),
          "XrmQuarkToString returned wrong string");

    XrmQuark quarks[4];
    XrmStringToQuarkList("app.window.title", quarks);
    CHECK(!strcmp(XrmQuarkToString(quarks[0]), "app") &&
              !strcmp(XrmQuarkToString(quarks[1]), "window") &&
              !strcmp(XrmQuarkToString(quarks[2]), "title") &&
              quarks[3] == NULLQUARK,
          "XrmStringToQuarkList returned wrong quarks");

    XrmBinding bindings[4];
    XrmStringToBindingQuarkList("app.window*title", bindings, quarks);
    CHECK(bindings[0] == XrmBindTightly && bindings[1] == XrmBindTightly &&
              bindings[2] == XrmBindLoosely && quarks[3] == NULLQUARK,
          "XrmStringToBindingQuarkList returned wrong bindings");

    XrmStringToBindingQuarkList("app*.title", bindings, quarks);
    CHECK(bindings[0] == XrmBindTightly && bindings[1] == XrmBindLoosely &&
              !strcmp(XrmQuarkToString(quarks[1]), "title") &&
              quarks[2] == NULLQUARK,
          "XrmStringToBindingQuarkList tightened loose empty component");

    XDestroyWindow(display, window);
    return 1;
}

static int test_extensions(Display *display)
{
    int major = 99;
    int event = 98;
    int error = 97;
    CHECK(!XQueryExtension(display, "GLX", &major, &event, &error),
          "XQueryExtension reported unsupported GLX");
    CHECK(major == 0 && event == 0 && error == 0,
          "XQueryExtension did not clear unsupported outputs");
    CHECK(XInitExtension(display, "GLX") == NULL,
          "XInitExtension returned codes for unsupported extension");

    XExtCodes *first = XAddExtension(display);
    XExtCodes *second = XAddExtension(display);
    CHECK(first != NULL && second != NULL, "XAddExtension returned NULL");
    CHECK(first->extension != second->extension,
          "extension ids were not unique");
    CHECK(first->major_opcode > 0 && first->first_event > 0 &&
              first->first_error > 0,
          "extension codes were not populated");
    CHECK(XESetCloseDisplay(display, first->extension,
                            counted_extension_close) == NULL,
          "first XESetCloseDisplay did not return NULL previous handler");
    CHECK(XESetCloseDisplay(display, first->extension, NULL) ==
              counted_extension_close,
          "second XESetCloseDisplay did not return previous handler");

    Display *nested = XOpenDisplay(NULL);
    CHECK(nested != NULL, "nested XOpenDisplay failed");
    XExtCodes *nestedCodes = XAddExtension(nested);
    CHECK(nestedCodes != NULL, "nested XAddExtension failed");
    extension_close_count = 0;
    XESetCloseDisplay(nested, nestedCodes->extension, counted_extension_close);
    XCloseDisplay(nested);
    CHECK(extension_close_count == 1,
          "extension close callback was not called");

    /* Safe stubs for XSync and MIT-SHM: probing clients should get stable
     * answers with zeroed outputs where an extension is unsupported.
     */
    int evBase = 99, errBase = 98;
    CHECK(!XSyncQueryExtension(display, &evBase, &errBase),
          "XSyncQueryExtension should report unsupported");
    CHECK(evBase == 0 && errBase == 0,
          "XSyncQueryExtension did not zero outputs");
    int nCounters = 99;
    CHECK(XSyncListSystemCounters(display, &nCounters) == NULL,
          "XSyncListSystemCounters should return NULL");
    CHECK(nCounters == 0, "XSyncListSystemCounters did not zero count");
    XSyncValue dummy = {.hi = 0, .lo = 0};
    CHECK(XSyncCreateCounter(display, dummy) == None,
          "XSyncCreateCounter should return None");

    evBase = 99;
    errBase = 98;
    int shapeOpcode = 99;
    CHECK(XQueryExtension(display, SHAPENAME, &shapeOpcode, &evBase, &errBase),
          "XQueryExtension should report SHAPE");
    CHECK(shapeOpcode > 0 && evBase > 0 && errBase > 0,
          "XQueryExtension returned invalid SHAPE codes");
    evBase = 99;
    errBase = 98;
    CHECK(XShapeQueryExtension(display, &evBase, &errBase),
          "XShapeQueryExtension should report SHAPE");
    CHECK(evBase > 0 && errBase > 0,
          "XShapeQueryExtension returned invalid bases");
    int shapeMajor = 0, shapeMinor = 0;
    CHECK(XShapeQueryVersion(display, &shapeMajor, &shapeMinor),
          "XShapeQueryVersion failed");
    CHECK(
        shapeMajor == SHAPE_MAJOR_VERSION && shapeMinor == SHAPE_MINOR_VERSION,
        "XShapeQueryVersion returned wrong version");
    Bool bShaped = True, cShaped = True;
    int xbs = 99, ybs = 98, xcs = 97, ycs = 96;
    unsigned int wbs = 0, hbs = 0, wcs = 0, hcs = 0;
    CHECK(
        XShapeQueryExtents(display, RootWindow(display, 0), &bShaped, &xbs,
                           &ybs, &wbs, &hbs, &cShaped, &xcs, &ycs, &wcs, &hcs),
        "XShapeQueryExtents failed");
    CHECK(!bShaped && !cShaped,
          "XShapeQueryExtents should report rectangular windows");
    CHECK(xbs == 0 && ybs == 0 && xcs == 0 && ycs == 0,
          "XShapeQueryExtents returned non-zero rectangular origins");
    CHECK(wbs == (unsigned int) DisplayWidth(display, 0) &&
              hbs == (unsigned int) DisplayHeight(display, 0) && wcs == wbs &&
              hcs == hbs,
          "XShapeQueryExtents did not return rectangular extents");
    int xc = 99, oc = 98;
    CHECK(XShapeGetRectangles(display, RootWindow(display, 0), ShapeBounding,
                              &xc, &oc) == NULL,
          "XShapeGetRectangles should return NULL");
    CHECK(xc == 0 && oc == 0, "XShapeGetRectangles did not zero outputs");

    CHECK(XShmQueryExtension(display), "XShmQueryExtension should be true");
    CHECK(XShmGetEventBase(display) != 0,
          "XShmGetEventBase should return a synthetic base");
    int shmMajor = 99, shmMinor = 98;
    Bool sharedPix = True;
    CHECK(XShmQueryVersion(display, &shmMajor, &shmMinor, &sharedPix),
          "XShmQueryVersion failed");
    /* XShmCreatePixmap currently ignores its shared-memory buffer (creates a
     * regular pixmap), so the compat layer reports shared_pixmaps=False to keep
     * callers from relying on semantics the implementation does not honor.
     */
    CHECK(shmMajor == 1 && shmMinor == 2 && sharedPix == False,
          "XShmQueryVersion returned wrong values");

    char shmPixels[16] = {0};
    XShmSegmentInfo shminfo = {.shmaddr = shmPixels};
    XImage *shmImage =
        XShmCreateImage(display, DefaultVisual(display, DefaultScreen(display)),
                        32, ZPixmap, shmPixels, &shminfo, 2, 2);
    CHECK(shmImage, "XShmCreateImage failed");
    CHECK(shmImage->data == shmPixels, "XShmCreateImage did not alias shmaddr");
    Window shmWindow = XCreateSimpleWindow(
        display, RootWindow(display, DefaultScreen(display)), 0, 0, 2, 2, 0, 0,
        0);
    CHECK(shmWindow != None, "XShm completion window creation failed");
    CHECK(XShmPutImage(display, shmWindow,
                       DefaultGC(display, DefaultScreen(display)), shmImage, 0,
                       0, 0, 0, 2, 2, True),
          "XShmPutImage with completion failed");
    XEvent shmEvent;
    CHECK(XCheckTypedEvent(display, XShmGetEventBase(display) + ShmCompletion,
                           &shmEvent),
          "XShmPutImage did not queue completion event");
    XShmCompletionEvent *completion = (XShmCompletionEvent *) &shmEvent;
    CHECK(completion->drawable == shmWindow &&
              completion->major_code == X_ShmPutImage,
          "XShm completion event fields were incorrect");
    XDestroyWindow(display, shmWindow);
    XDestroyImage(shmImage);

    evBase = 99;
    errBase = 98;
    CHECK(
        !XRenderQueryExtension(display, &evBase, &errBase),
        "XRenderQueryExtension should report unsupported while ops are stubs");
    CHECK(evBase == 0 && errBase == 0,
          "XRenderQueryExtension did not zero outputs");

    evBase = 99;
    errBase = 98;
    CHECK(!XFixesQueryExtension(display, &evBase, &errBase),
          "XFixesQueryExtension should report unsupported");
    CHECK(evBase == 0 && errBase == 0,
          "XFixesQueryExtension did not zero outputs");
    evBase = 99;
    errBase = 98;
    CHECK(!XDamageQueryExtension(display, &evBase, &errBase),
          "XDamageQueryExtension should report unsupported");
    CHECK(evBase == 0 && errBase == 0,
          "XDamageQueryExtension did not zero outputs");
    evBase = 99;
    errBase = 98;
    CHECK(!XRRQueryExtension(display, &evBase, &errBase),
          "XRRQueryExtension should report unsupported");
    CHECK(evBase == 0 && errBase == 0,
          "XRRQueryExtension did not zero outputs");
    int opcode = 77, xkbMajor = 1, xkbMinor = 1;
    evBase = 99;
    errBase = 98;
    /* xfreerdp / Motif / GTK probe XkbUseExtension and XkbQueryExtension before
     * any keyboard work and refuse to start when either reports unavailable.
     * The compat layer reports "available" with synthetic opcode / event /
     * error base codes chosen above the core protocol range so "event.type -
     * eventBase" math against the synthetic event base does not collide with X
     * core events. The two probes must agree; the prior inconsistency
     * (UseExtension true, QueryExtension false) was diagnosed by Codex as
     * load-bearing on xfreerdp boot.
     */
    CHECK(XkbQueryExtension(display, &opcode, &evBase, &errBase, &xkbMajor,
                            &xkbMinor),
          "XkbQueryExtension must report supported to match XkbUseExtension");
    CHECK(opcode != 0 && evBase != 0 && errBase != 0,
          "XkbQueryExtension synthetic bases must be nonzero");
    CHECK(evBase > 34,
          "XkbQueryExtension event base must clear the core event range");
    int useMajor = 0;
    int useMinor = 0;
    CHECK(XkbUseExtension(display, &useMajor, &useMinor),
          "XkbUseExtension must report supported");

    /* XDoubleToFixed must clamp out-of-range and NaN inputs instead of invoking
     * undefined behavior on the double->int cast.
     */
    CHECK(XDoubleToFixed(0.0) == 0, "XDoubleToFixed zero failed");
    CHECK(XDoubleToFixed(1.0) == 0x10000, "XDoubleToFixed one failed");
    CHECK(XDoubleToFixed(-1.0) == -0x10000, "XDoubleToFixed minus one failed");
    CHECK(XDoubleToFixed(1.0e100) == 0x7FFFFFFF,
          "XDoubleToFixed positive overflow did not saturate");
    CHECK(XDoubleToFixed(-1.0e100) == (int) (-0x7FFFFFFF - 1),
          "XDoubleToFixed negative overflow did not saturate");
    double nanVal = strtod("NaN", NULL);
    CHECK(XDoubleToFixed(nanVal) == 0, "XDoubleToFixed NaN did not return 0");
    return 1;
}

static int test_selection(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 0, 0, 16, 16, 0, 0, 0);
    XSelectInput(display, window,
                 PropertyChangeMask | SubstructureNotifyMask | NoEventMask);
    Atom clipboard = XInternAtom(display, "CLIPBOARD", False);
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    Atom targets = XInternAtom(display, "TARGETS", False);
    Atom prop = XInternAtom(display, "SDL2X11_SEL_TEST", False);

    /* No owner: XConvertSelection emits SelectionNotify with property=None. */
    SDL_SetClipboardText("");
    XConvertSelection(display, XInternAtom(display, "PRIMARY", False), utf8,
                      prop, window, CurrentTime);
    XEvent ev;
    CHECK(XCheckTypedEvent(display, SelectionNotify, &ev),
          "missing SelectionNotify for no-owner");
    CHECK(ev.xselection.property == None,
          "no-owner SelectionNotify should carry property=None");

    /* Owner replacement: setting a new owner posts SelectionClear to the
     * previous owner.
     */
    Window owner1 = XCreateSimpleWindow(display, root, 0, 0, 8, 8, 0, 0, 0);
    Window owner2 = XCreateSimpleWindow(display, root, 0, 0, 8, 8, 0, 0, 0);
    XSetSelectionOwner(display, clipboard, owner1, CurrentTime);
    CHECK(XGetSelectionOwner(display, clipboard) == owner1,
          "XGetSelectionOwner did not return new owner");
    XSetSelectionOwner(display, clipboard, owner2, CurrentTime);
    CHECK(XCheckTypedEvent(display, SelectionClear, &ev),
          "missing SelectionClear for previous owner");
    CHECK(ev.xselectionclear.window == owner1,
          "SelectionClear delivered to wrong window");

    /* SDL clipboard text bridge for CLIPBOARD targets. */
    XSetSelectionOwner(display, clipboard, None, CurrentTime);
    SDL_SetClipboardText("hello");
    CHECK(XGetSelectionOwner(display, clipboard) == root,
          "SDL-backed clipboard should report root as owner");
    XConvertSelection(display, clipboard, utf8, prop, window, CurrentTime);
    CHECK(XCheckTypedEvent(display, SelectionNotify, &ev),
          "missing SelectionNotify for SDL-backed conversion");
    CHECK(ev.xselection.property == prop,
          "SDL-backed SelectionNotify wrong property");
    XConvertSelection(display, clipboard, utf8, None, window, CurrentTime);
    CHECK(XCheckTypedEvent(display, SelectionNotify, &ev),
          "missing SelectionNotify for default property conversion");
    CHECK(ev.xselection.property == utf8,
          "property=None conversion did not default to target atom");

    XConvertSelection(display, clipboard, targets, prop, window, CurrentTime);
    CHECK(XCheckTypedEvent(display, SelectionNotify, &ev),
          "missing SelectionNotify for SDL-backed TARGETS conversion");
    CHECK(ev.xselection.property == prop,
          "SDL-backed TARGETS SelectionNotify wrong property");
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *data = NULL;
    CHECK(XGetWindowProperty(display, window, prop, 0, 3, False, XA_ATOM,
                             &actualType, &actualFormat, &nItems, &bytesAfter,
                             &data) == Success,
          "XGetWindowProperty for SDL-backed TARGETS failed");
    CHECK(actualType == XA_ATOM && actualFormat == 32 && nItems == 3 &&
              bytesAfter == 0,
          "SDL-backed TARGETS property had wrong shape");
    CHECK(data != NULL, "SDL-backed TARGETS property data missing");
    Atom *atoms = (Atom *) data;
    CHECK(atoms[0] == targets && atoms[1] == utf8 && atoms[2] == XA_STRING,
          "SDL-backed TARGETS atom list was incorrect");
    XFree(data);

    XDestroyWindow(display, owner1);
    XDestroyWindow(display, owner2);
    XDestroyWindow(display, window);
    return 1;
}

static int test_xrm(Display *display)
{
    XrmInitialize();
    const char *src =
        "App.window.title: Hello\n"
        "App*background:  white\n"
        "! a comment line\n"
        "App.window.color: red\n";
    XrmDatabase db = XrmGetStringDatabase(src);
    CHECK(db != NULL, "XrmGetStringDatabase returned NULL");

    XrmValue value = {.size = 0, .addr = NULL};
    char *type = NULL;
    CHECK(XrmGetResource(db, "App.window.title", "Application.Window.Title",
                         &type, &value),
          "XrmGetResource missed exact match");
    CHECK(value.addr != NULL && !strcmp((char *) value.addr, "Hello"),
          "XrmGetResource returned wrong value");

    char longName[301];
    memset(longName, 'a', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    char longSpec[sizeof("App.") + sizeof(longName) + sizeof(": QLong")];
    snprintf(longSpec, sizeof(longSpec), "App.%s: QLong", longName);
    XrmPutLineResource(&db, longSpec);
    XrmName qLongNames[] = {
        XrmStringToQuark("App"),
        XrmStringToQuark(longName),
        NULLQUARK,
    };
    XrmClass qLongClasses[] = {
        XrmStringToQuark("Application"),
        XrmStringToQuark("LongName"),
        NULLQUARK,
    };
    XrmRepresentation qLongType = 0;
    XrmValue qLongValue = {.size = 0, .addr = NULL};
    CHECK(
        XrmQGetResource(db, qLongNames, qLongClasses, &qLongType, &qLongValue),
        "XrmQGetResource missed long quark resource path");
    CHECK(qLongValue.addr != NULL && !strcmp((char *) qLongValue.addr, "QLong"),
          "XrmQGetResource returned wrong value for long path");

    XrmName deepNames[66];
    XrmClass deepClasses[66];
    for (size_t i = 0; i < ARRAY_LENGTH(deepNames) - 1; i++) {
        deepNames[i] = XrmStringToQuark("deepName");
        deepClasses[i] = XrmStringToQuark("DeepClass");
    }
    deepNames[ARRAY_LENGTH(deepNames) - 1] = NULLQUARK;
    deepClasses[ARRAY_LENGTH(deepClasses) - 1] = NULLQUARK;
    /* The prefix-encoding bridge needs 3 + 2 * n slots (db + count + name
     * quarks + class quarks + NULL terminator). A list_length too small for
     * that layout must return False so libXt's _XtDisplayInitialize doubling
     * loop can enlarge the buffer and retry; an adequate buffer encodes the
     * prefix and returns True. Verify both branches so neither alloca- smashes
     * the stack (always-False regression) nor lies about contents (always-True
     * regression).
     */
    XrmHashTable tinySearch[1];
    CHECK(!XrmQGetSearchList(db, deepNames, deepClasses, tinySearch, 1),
          "XrmQGetSearchList should signal buffer-too-small with False");
    XrmHashTable deepSearch[200];
    CHECK(XrmQGetSearchList(db, deepNames, deepClasses, deepSearch, 200),
          "XrmQGetSearchList should encode 65-component prefix in 200 slots");
    CHECK(!XrmQGetSearchResource(deepSearch, XrmStringToQuark("leaf"),
                                 XrmStringToQuark("Leaf"), &qLongType,
                                 &qLongValue),
          "deep no-match search list should not resolve resources");
    XrmDatabase deepDb = NULL;
    XrmPutStringResource(&deepDb, "*labelString", "deep label");
    XrmHashTable deepLabelSearch[200];
    CHECK(
        XrmQGetSearchList(deepDb, deepNames, deepClasses, deepLabelSearch, 200),
        "XrmQGetSearchList should encode deep label prefix");
    CHECK(XrmQGetSearchResource(
              deepLabelSearch, XrmStringToQuark("labelString"),
              XrmStringToQuark("LabelString"), &qLongType, &qLongValue),
          "deep search list missed loose label resource");
    CHECK(!strcmp((char *) qLongValue.addr, "deep label"),
          "deep loose label resource returned wrong value");
    XrmDestroyDatabase(deepDb);

    /* Loose binding via '*'. */
    value.addr = NULL;
    CHECK(XrmGetResource(db, "App.window.background", "App.Window.Background",
                         &type, &value),
          "XrmGetResource did not match through loose '*' binding");
    CHECK(!strcmp((char *) value.addr, "white"),
          "XrmGetResource through '*' returned wrong value");

    XrmPutStringResource(&db, "App.window.?.font", "question-font");
    value.addr = NULL;
    CHECK(XrmGetResource(db, "App.window.label.font",
                         "Application.Window.Label.Font", &type, &value),
          "XrmGetResource did not match '?' component");
    CHECK(!strcmp((char *) value.addr, "question-font"),
          "XrmGetResource '?' component returned wrong value");
    XrmName qQuestionNames[] = {
        XrmStringToQuark("App"),
        XrmStringToQuark("window"),
        XrmStringToQuark("button"),
        XrmStringToQuark("font"),
        NULLQUARK,
    };
    XrmClass qQuestionClasses[] = {
        XrmStringToQuark("Application"),
        XrmStringToQuark("Window"),
        XrmStringToQuark("Button"),
        XrmStringToQuark("Font"),
        NULLQUARK,
    };
    qLongValue.addr = NULL;
    CHECK(XrmQGetResource(db, qQuestionNames, qQuestionClasses, &qLongType,
                          &qLongValue),
          "XrmQGetResource did not match '?' component");
    CHECK(!strcmp((char *) qLongValue.addr, "question-font"),
          "XrmQGetResource '?' component returned wrong value");

    XrmDatabase questionPrec = NULL;
    XrmPutStringResource(&questionPrec, "A.B.foo", "exact");
    XrmPutStringResource(&questionPrec, "?.B.foo", "wildcard");
    value.addr = NULL;
    CHECK(XrmGetResource(questionPrec, "A.B.foo", "A.B.Foo", &type, &value),
          "XrmGetResource exact-vs-'?' lookup failed");
    CHECK(!strcmp((char *) value.addr, "exact"),
          "'?' component should not outrank exact name component");
    XrmDestroyDatabase(questionPrec);

    questionPrec = NULL;
    XrmPutStringResource(&questionPrec, "A.B.foo", "class");
    XrmPutStringResource(&questionPrec, "?.B.foo", "wildcard");
    value.addr = NULL;
    CHECK(XrmGetResource(questionPrec, "Other.B.foo", "A.B.Foo", &type, &value),
          "XrmGetResource class-vs-'?' lookup failed");
    CHECK(!strcmp((char *) value.addr, "class"),
          "'?' component should not outrank exact class component");
    XrmDestroyDatabase(questionPrec);

    /* Specificity: a later, tighter rule must beat an earlier loose one. */
    XrmDatabase prec = NULL;
    XrmPutStringResource(&prec, "App*background", "white");
    XrmPutStringResource(&prec, "App.window.background", "red");
    XrmValue precVal = {.size = 0, .addr = NULL};
    char *precType = NULL;
    CHECK(XrmGetResource(prec, "App.window.background",
                         "Application.Window.Background", &precType, &precVal),
          "specificity lookup failed");
    CHECK(!strcmp((char *) precVal.addr, "red"),
          "tight rule should beat earlier loose rule");
    XrmDestroyDatabase(prec);

    /* XrmParseCommand handles SepArg / StickyArg / IsArg. */
    XrmDatabase parsed = NULL;
    static XrmOptionDescRec opts[] = {
        {.option = "-geometry",
         .specifier = ".geometry",
         .argKind = XrmoptionSepArg,
         .value = NULL},
        {.option = "-fg",
         .specifier = "*foreground",
         .argKind = XrmoptionSepArg,
         .value = NULL},
    };
    char *argv[] = {(char *) "demo", (char *) "-fg", (char *) "yellow",
                    (char *) "extra", NULL};
    int argc = 4;
    XrmParseCommand(&parsed, opts, sizeof(opts) / sizeof(opts[0]), "MyApp",
                    &argc, argv);
    CHECK(parsed != NULL, "XrmParseCommand did not populate database");
    CHECK(argc == 2 && !strcmp(argv[0], "demo") && !strcmp(argv[1], "extra"),
          "XrmParseCommand left wrong argv");
    XrmValue parsedValue = {.size = 0, .addr = NULL};
    char *parsedType = NULL;
    CHECK(XrmGetResource(parsed, "MyApp.foreground", "MyApp.Foreground",
                         &parsedType, &parsedValue),
          "XrmParseCommand did not store -fg");
    CHECK(!strcmp((char *) parsedValue.addr, "yellow"),
          "-fg parsed to wrong value");
    XrmDestroyDatabase(parsed);

    /* XrmGetDatabase / XrmSetDatabase round-trip through display->db. */
    XrmDatabase mine = NULL;
    XrmPutStringResource(&mine, "App.color", "blue");
    CHECK(XrmGetDatabase(display) == NULL,
          "fresh display should report NULL Xrm database");
    XrmSetDatabase(display, mine);
    CHECK(XrmGetDatabase(display) == mine,
          "XrmSetDatabase did not stick on display");
    XrmSetDatabase(display, NULL);
    XrmDestroyDatabase(mine);

    /* Motif-style cascade: '*XmLabel.fontList' must match the widget path
     * 'top.frame.row.col.button.label.fontList' / class
     * 'Top.Frame.Row.Col.Button.XmLabel.FontList'. The leading loose binding
     * skips the four ancestor segments and pins on the 'XmLabel' class quark;
     * the trailing tight binding hits 'fontList' as the leaf. Regression for
     * the bindingQuarkListToPattern bug that dropped the leading '*' from
     * loosely-bound patterns built through XrmQPutStringResource.
     */
    {
        XrmDatabase motifDb = NULL;
        XrmBinding bindings[2] = {XrmBindLoosely, XrmBindTightly};
        XrmQuark quarks[3] = {
            XrmStringToQuark("XmLabel"),
            XrmStringToQuark("fontList"),
            NULLQUARK,
        };
        XrmQPutStringResource(&motifDb, bindings, quarks,
                              "-*-helvetica-medium-r-*-12-*");
        XrmValue mv = {.size = 0, .addr = NULL};
        char *mt = NULL;
        CHECK(XrmGetResource(motifDb, "top.frame.row.col.button.label.fontList",
                             "Top.Frame.Row.Col.Button.XmLabel.FontList", &mt,
                             &mv),
              "Motif *XmLabel.fontList cascade missed");
        CHECK(mv.addr != NULL &&
                  !strcmp((char *) mv.addr, "-*-helvetica-medium-r-*-12-*"),
              "Motif fontList resource returned wrong value");
        XrmDestroyDatabase(motifDb);
    }

    /* Backslash continuation in resource source: '*XmLabel.fontList' lines in
     * Motif app-defaults routinely split across multiple physical lines with
     * trailing '\'. The parser must join them before looking for the ':'.
     */
    {
        const char *multiline =
            "*App.fontList:\\\n"
            "    -*-helvetica-medium-r-normal--12-*-*-*-p-*-iso8859-1\n"
            "*App.foreground: black\n";
        XrmDatabase mlDb = XrmGetStringDatabase(multiline);
        CHECK(mlDb != NULL, "multiline XrmGetStringDatabase returned NULL");
        XrmValue mlv = {.size = 0, .addr = NULL};
        char *mlt = NULL;
        CHECK(XrmGetResource(mlDb, "App.fontList", "App.FontList", &mlt, &mlv),
              "backslash-continued resource missed lookup");
        CHECK(
            mlv.addr != NULL && strstr((char *) mlv.addr, "iso8859-1") != NULL,
            "backslash-continued value missing tail");
        XrmDestroyDatabase(mlDb);
    }

    /* Class-vs-name specificity: a class match scores lower than a name match
     * at the same path depth.
     */
    {
        XrmDatabase cn = NULL;
        XrmPutStringResource(&cn, "*Label.color", "byClass");
        XrmPutStringResource(&cn, "*label.color", "byName");
        XrmValue cv = {.size = 0, .addr = NULL};
        char *ct = NULL;
        CHECK(
            XrmGetResource(cn, "app.label.color", "App.Label.Color", &ct, &cv),
            "class-vs-name lookup missed");
        CHECK(!strcmp((char *) cv.addr, "byName"),
              "name match should beat class match at same depth");
        XrmDestroyDatabase(cn);
    }

    XrmDestroyDatabase(db);
    return 1;
}

static int test_input_methods(Display *display)
{
    XIM im = XOpenIM(display, NULL, NULL, NULL);
    CHECK(im, "XOpenIM failed");
    Window root = RootWindow(display, DefaultScreen(display));
    Window client = XCreateSimpleWindow(display, root, 0, 0, 120, 60, 0, 0, 0);
    Window focus = XCreateSimpleWindow(display, client, 1, 1, 20, 12, 0, 0, 0);
    CHECK(client != None && focus != None, "input method windows failed");

    XRectangle area = {.x = 2, .y = 3, .width = 48, .height = 18};
    XFontSet fontSet = (XFontSet) (uintptr_t) 0x1234;
    unsigned long foreground = 0x112233;
    unsigned long background = 0xffffffff;
    XIMCallback preeditStartCallback = {(XPointer) &preeditStartCount,
                                        test_preedit_start_done_callback};
    XIMCallback preeditDoneCallback = {(XPointer) &preeditDoneCount,
                                       test_preedit_start_done_callback};
    XIMCallback preeditDrawCallback = {NULL, test_preedit_draw_callback};
    XIMCallback preeditCaretCallback = {NULL, test_preedit_caret_callback};
    XVaNestedList preedit = XVaCreateNestedList(
        0, XNArea, &area, XNForeground, (XPointer) (uintptr_t) foreground,
        XNBackground, (XPointer) (uintptr_t) background, XNFontSet, fontSet,
        XNPreeditStartCallback, &preeditStartCallback, XNPreeditDoneCallback,
        &preeditDoneCallback, XNPreeditDrawCallback, &preeditDrawCallback,
        XNPreeditCaretCallback, &preeditCaretCallback, NULL);
    CHECK(preedit, "XVaCreateNestedList for preedit failed");
    XIC ic = XCreateIC(im, XNInputStyle, XIMPreeditArea | XIMStatusNothing,
                       XNClientWindow, client, XNFocusWindow, focus,
                       XNPreeditAttributes, preedit, NULL);
    CHECK(ic, "XCreateIC failed");

    XIMStyle style = 0;
    Window clientBack = None;
    Window focusBack = None;
    XRectangle areaBack = {0, 0, 0, 0};
    unsigned long foregroundBack = 0;
    unsigned long backgroundBack = 0;
    XFontSet fontSetBack = NULL;
    XVaNestedList preeditBack = XVaCreateNestedList(
        0, XNArea, &areaBack, XNForeground, &foregroundBack, XNBackground,
        &backgroundBack, XNFontSet, &fontSetBack, NULL);
    CHECK(preeditBack, "XVaCreateNestedList for preedit return failed");
    unsigned long filterEvents = 0;
    CHECK(!XGetICValues(ic, XNInputStyle, &style, XNClientWindow, &clientBack,
                        XNFocusWindow, &focusBack, XNPreeditAttributes,
                        preeditBack, XNFilterEvents, &filterEvents, NULL),
          "XGetICValues failed");
    CHECK(style == (XIMPreeditArea | XIMStatusNothing),
          "XGetICValues returned wrong style");
    CHECK(clientBack == client && focusBack == focus,
          "XGetICValues returned wrong client/focus windows");
    CHECK(areaBack.x == area.x && areaBack.y == area.y &&
              areaBack.width == area.width && areaBack.height == area.height,
          "XGetICValues returned wrong preedit area");
    CHECK(foregroundBack == foreground && backgroundBack == background,
          "XGetICValues returned wrong preedit colors");
    CHECK(fontSetBack == fontSet, "XGetICValues returned wrong font set");
    CHECK(filterEvents == (KeyPressMask | KeyReleaseMask),
          "XGetICValues returned wrong filter events");

    {
        /* xwpe creates an IC and immediately calls XmbLookupString without an
         * explicit XSetICFocus. Treat the first IC as focused so SDL text input
         * is live for that legacy pattern.
         */
        SDL_Event textEvent;
        set_text_input_event(&textEvent, "u");
        XEvent out;
        CHECK(convertEvent(display, &textEvent, &out, True) == 0 &&
                  out.type == KeyPress && out.xkey.keycode == 0,
              "first XCreateIC did not enable IM text input");
        char buf[4] = {0};
        KeySym keysym = NoSymbol;
        Status status = XLookupNone;
        int n =
            XmbLookupString(ic, &out.xkey, buf, sizeof(buf), &keysym, &status);
        CHECK(
            n == 1 && buf[0] == 'u' && keysym == XK_u && status == XLookupBoth,
            "first XCreateIC auto-focus did not expose committed text");
    }

    GC preeditGc = XCreateGC(display, client, 0, NULL);
    CHECK(preeditGc, "preedit visibility GC creation failed");
    CHECK(XSetForeground(display, preeditGc, 0xffffffff),
          "preedit visibility white foreground failed");
    CHECK(XFillRectangle(display, client, preeditGc, 0, 0, 120, 60),
          "preedit visibility background fill failed");
    XFreeGC(display, preeditGc);

    XIMStyles *styles = NULL;
    CHECK(!XGetIMValues(im, XNQueryInputStyle, &styles, NULL) && styles,
          "XGetIMValues did not return input styles");
    Bool hasCallbackStyle = False;
    Bool hasServerDrawnStyle = False;
    for (unsigned short i = 0; i < styles->count_styles; i++) {
        if (styles->supported_styles[i] ==
            (XIMPreeditCallbacks | XIMStatusNothing)) {
            hasCallbackStyle = True;
        }
        if (styles->supported_styles[i] &
            (XIMPreeditArea | XIMPreeditPosition)) {
            hasServerDrawnStyle = True;
        }
    }
    XFree(styles);
    CHECK(hasCallbackStyle, "XIMPreeditCallbacks was not advertised");
    /* XGetIMValues must advertise the same styles XCreateIC accepts, including
     * the over-the-spot Area/Position styles served by the internal renderer,
     * so clients can actually select them.
     */
    CHECK(hasServerDrawnStyle,
          "over-the-spot (Area/Position) preedit styles were not advertised");

    XSetInputFocus(display, focus, RevertToNone, CurrentTime);
    XSelectInput(display, focus, KeyPressMask);
    {
        unsigned long blackBackground = 0;
        unsigned long whiteForeground = 0xffffffff;
        void *blackPreedit[] = {
            XNArea,       &area,
            XNForeground, (XPointer) (uintptr_t) whiteForeground,
            XNBackground, (XPointer) (uintptr_t) blackBackground,
            NULL};
        XIC blackBgIC =
            XCreateIC(im, XNInputStyle, XIMPreeditArea | XIMStatusNothing,
                      XNClientWindow, client, XNFocusWindow, focus,
                      XNPreeditAttributes, blackPreedit, NULL);
        CHECK(blackBgIC, "XCreateIC failed for black preedit background");
        preeditGc = XCreateGC(display, client, 0, NULL);
        CHECK(preeditGc, "black preedit GC creation failed");
        CHECK(XSetForeground(display, preeditGc, 0xffffffff),
              "black preedit white foreground failed");
        CHECK(XFillRectangle(display, client, preeditGc, 0, 0, 120, 60),
              "black preedit background fill failed");
        XFreeGC(display, preeditGc);
        XSetICFocus(blackBgIC);
        SDL_Event blackEdit;
        SDL_zero(blackEdit);
        blackEdit.type = SDL_TEXTEDITING;
        XC_SET_EDITING_EVENT(blackEdit, "bg");
        XEvent blackIgnored;
        CHECK(convertEvent(display, &blackEdit, &blackIgnored, True) == -1,
              "black-background SDL_TEXTEDITING produced a committed X event");
        XImage *blackImage =
            XGetImage(display, client, area.x, area.y, area.width, area.height,
                      AllPlanes, ZPixmap);
        CHECK(blackImage, "black preedit XGetImage failed");
        unsigned long blackPixel = XGetPixel(blackImage, 1, 1);
        CHECK((blackPixel & 0x00ffffff) == 0,
              "black preedit background pixel was not honored");
        XDestroyImage(blackImage);
        XUnsetICFocus(blackBgIC);
        XDestroyIC(blackBgIC);
    }
    XSetICFocus(ic);
    preeditStartCount = 0;
    preeditDoneCount = 0;
    preeditDrawCount = 0;
    preeditCaretCount = 0;
    preeditCaretLastPos = -1;
    preeditDrawText[0] = '\0';
    SDL_Event editingEvent;
    SDL_zero(editingEvent);
    editingEvent.type = SDL_TEXTEDITING;
    XC_SET_EDITING_EVENT(editingEvent, "zh");
    /* Caret sits between the two composed characters; it must be reported as
     * such, not snapped to the end of the preedit.
     */
    editingEvent.edit.start = 1;
    XEvent ignored;
    CHECK(convertEvent(display, &editingEvent, &ignored, True) == -1,
          "SDL_TEXTEDITING should not produce a committed X event");
    CHECK(preeditDrawCount == 1 && strcmp(preeditDrawText, "zh") == 0,
          "SDL_TEXTEDITING did not reach XNPreeditDrawCallback");
    CHECK(preeditStartCount == 1 && preeditDoneCount == 0 &&
              preeditCaretCount == 1,
          "SDL_TEXTEDITING did not deliver preedit lifecycle callbacks");
    CHECK(preeditCaretLastPos == 1,
          "SDL_TEXTEDITING caret position not threaded from edit.start");
    XImage *preeditImage =
        XGetImage(display, client, area.x, area.y, area.width, area.height,
                  AllPlanes, ZPixmap);
    CHECK(preeditImage, "preedit visibility XGetImage failed");
    int darkPixels = 0;
    for (int py = 0; py < area.height; py++) {
        for (int px = 0; px < area.width; px++) {
            if ((XGetPixel(preeditImage, px, py) & 0x00ffffff) != 0x00ffffff)
                darkPixels++;
        }
    }
    XDestroyImage(preeditImage);
    CHECK(darkPixels > 0, "area-style preedit was not visibly drawn");
    {
        SDL_Event returnDuringPreedit;
        SDL_zero(returnDuringPreedit);
        returnDuringPreedit.type = SDL_KEYDOWN;
        returnDuringPreedit.key.windowID =
            SDL_GetWindowID(GET_WINDOW_STRUCT(client)->sdlWindow);
        XC_EVENT_SET_KEYSYM(&returnDuringPreedit, SDLK_RETURN);
        XC_EVENT_SET_KEYMOD(&returnDuringPreedit, 0);
        CHECK(convertEvent(display, &returnDuringPreedit, &ignored, True) == -1,
              "Return keydown escaped while preedit was active");

        SDL_Event commitAfterReturn;
        set_text_input_event(&commitAfterReturn, "z");
        XEvent committed;
        CHECK(
            convertEvent(display, &commitAfterReturn, &committed, True) == 0 &&
                committed.type == KeyPress && committed.xkey.keycode == 0,
            "preedit Return commit did not produce IM KeyPress");
        char commitBuf[4] = {0};
        KeySym commitKeysym = NoSymbol;
        Status commitStatus = XLookupNone;
        int commitLen =
            XmbLookupString(ic, &committed.xkey, commitBuf, sizeof(commitBuf),
                            &commitKeysym, &commitStatus);
        CHECK(commitLen == 1 && commitBuf[0] == 'z' && commitKeysym == XK_z &&
                  commitStatus == XLookupBoth,
              "preedit Return commit was not readable through XmbLookupString");
    }

    XIC switchIC =
        XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                  XNClientWindow, client, XNFocusWindow, focus, NULL);
    CHECK(switchIC, "XCreateIC failed for focus-switch IC");
    XSetICFocus(switchIC);
    XImage *switched = XGetImage(display, client, area.x, area.y, area.width,
                                 area.height, AllPlanes, ZPixmap);
    CHECK(switched, "focus-switch preedit clear XGetImage failed");
    int switchedNonWhite = 0;
    for (int py = 0; py < area.height; py++) {
        for (int px = 0; px < area.width; px++) {
            if ((XGetPixel(switched, px, py) & 0x00ffffff) != 0x00ffffff)
                switchedNonWhite++;
        }
    }
    XDestroyImage(switched);
    CHECK(switchedNonWhite == 0, "focus switch did not clear old preedit");
    CHECK(preeditDoneCount == 1, "focus switch did not finish old preedit");
    XDestroyIC(switchIC);
    XSetICFocus(ic);
    Window replacementClient =
        XCreateSimpleWindow(display, root, 0, 0, 120, 60, 0, 0, 0);
    CHECK(replacementClient != None, "replacement IM client creation failed");
    CHECK(XMapWindow(display, replacementClient),
          "replacement IM client map failed");
    CHECK(!XSetICValues(ic, XNClientWindow, replacementClient, NULL),
          "XSetICValues failed for replacement IM client");
    {
        XIC ic2 =
            XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                      XNClientWindow, client, XNFocusWindow, focus, NULL);
        CHECK(ic2, "XCreateIC failed for second IC");
        /* Modifying a non-focused IC must not disable text input for the
         * currently focused IC
         */
        CHECK(!XSetICValues(ic2, XNClientWindow, replacementClient, NULL),
              "XSetICValues failed for second IC client window");
        /* Verify that text input still works on the focused IC: ASCII keydown
         * is delivered raw, and the paired SDL_TEXTINPUT is suppressed.
         */
        SDL_Event keyEvent;
        SDL_zero(keyEvent);
        keyEvent.type = SDL_KEYDOWN;
        keyEvent.key.windowID =
            SDL_GetWindowID(GET_WINDOW_STRUCT(client)->sdlWindow);
        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_a);
        XC_EVENT_SET_KEYMOD(&keyEvent, 0);
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == 0,
              "setting client window on a non-focused IC disabled active IM "
              "text input");
        SDL_Event duplicateText;
        set_text_input_event(&duplicateText, "a");
        CHECK(convertEvent(display, &duplicateText, &ignored, True) == -1,
              "duplicate ASCII SDL_TEXTINPUT survived non-focused IC change");
        XDestroyIC(ic2);
    }
    {
        SDL_Event keyEvent;
        SDL_zero(keyEvent);
        keyEvent.type = SDL_KEYDOWN;
        keyEvent.key.windowID =
            SDL_GetWindowID(GET_WINDOW_STRUCT(client)->sdlWindow);
        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_a);
        XC_EVENT_SET_KEYMOD(&keyEvent, 0);
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == 0 &&
                  ignored.type == KeyPress && ignored.xkey.keycode == SDLK_a,
              "plain ASCII SDL_KEYDOWN under IC focus was not delivered raw");
        SDL_Event duplicateText;
        set_text_input_event(&duplicateText, "a");
        CHECK(convertEvent(display, &duplicateText, &ignored, True) == -1,
              "duplicate ASCII SDL_TEXTINPUT was not suppressed after raw key");
        set_text_input_event(&duplicateText, "a");
        CHECK(convertEvent(display, &duplicateText, &ignored, True) == 0 &&
                  ignored.type == KeyPress && ignored.xkey.keycode == 0,
              "repeated ASCII SDL_TEXTINPUT was suppressed after duplicate");
        char repeatedBuf[4] = {0};
        KeySym repeatedKeysym = NoSymbol;
        Status repeatedStatus = XLookupNone;
        CHECK(
            XmbLookupString(ic, &ignored.xkey, repeatedBuf, sizeof(repeatedBuf),
                            &repeatedKeysym, &repeatedStatus) == 1 &&
                repeatedBuf[0] == 'a' && repeatedKeysym == XK_a &&
                repeatedStatus == XLookupBoth,
            "repeated ASCII SDL_TEXTINPUT did not commit text");

        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_1);
        XC_EVENT_SET_KEYMOD(&keyEvent, KMOD_SHIFT);
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == 0 &&
                  ignored.type == KeyPress && ignored.xkey.keycode == SDLK_1,
              "shifted ASCII SDL_KEYDOWN under IC focus was not delivered raw");
        set_text_input_event(&duplicateText, "!");
        CHECK(convertEvent(display, &duplicateText, &ignored, True) == -1,
              "duplicate shifted ASCII SDL_TEXTINPUT was not suppressed");

        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_a);
        XC_EVENT_SET_KEYMOD(&keyEvent, KMOD_CAPS);
        CHECK(
            convertEvent(display, &keyEvent, &ignored, True) == 0 &&
                ignored.type == KeyPress && ignored.xkey.keycode == SDLK_a,
            "CapsLock ASCII SDL_KEYDOWN under IC focus was not delivered raw");
        set_text_input_event(&duplicateText, "A");
        CHECK(convertEvent(display, &duplicateText, &ignored, True) == -1,
              "duplicate CapsLock ASCII SDL_TEXTINPUT was not suppressed");

        XC_EVENT_SET_KEYMOD(&keyEvent, 0);

        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_KP_1);
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == -1,
              "keypad SDL_KEYDOWN under IC focus produced duplicate KeyPress");

        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_KP_LEFTPAREN);
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == -1,
              "keypad punctuation under IC focus produced duplicate KeyPress");

        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_a);
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == 0,
              "ASCII keydown before suppressed key was not delivered raw");
        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_KP_1);
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == -1,
              "keypad key after raw ASCII was not suppressed");
        set_text_input_event(&duplicateText, "a");
        CHECK(convertEvent(display, &duplicateText, &ignored, True) == -1,
              "raw ASCII duplicate marker was suppressed from the ring");

        /* Non-ASCII printable keys also arrive as SDL_TEXTINPUT, so their raw
         * keydown must be suppressed too (Latin-1 and arbitrary Unicode).
         */
        XC_EVENT_SET_KEYSYM(&keyEvent, 0xe9); /* Latin-1 e-acute */
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == -1,
              "Latin-1 SDL_KEYDOWN under IC focus produced duplicate KeyPress");

        XC_EVENT_SET_KEYSYM(&keyEvent, 0x4e2d); /* CJK codepoint */
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == -1,
              "Unicode SDL_KEYDOWN under IC focus produced duplicate KeyPress");

        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_RETURN);
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == 0,
              "non-text SDL_KEYDOWN under IC focus was suppressed");

        XC_EVENT_SET_KEYSYM(&keyEvent, SDLK_KP_ENTER);
        CHECK(convertEvent(display, &keyEvent, &ignored, True) == 0,
              "keypad Enter under IC focus was suppressed");
    }
    XSelectInput(display, focus, KeyPressMask);
    {
        SDL_Event selectedKeyEvent;
        SDL_zero(selectedKeyEvent);
        selectedKeyEvent.type = SDL_KEYDOWN;
        selectedKeyEvent.key.windowID =
            SDL_GetWindowID(GET_WINDOW_STRUCT(client)->sdlWindow);
        XC_EVENT_SET_KEYSYM(&selectedKeyEvent, SDLK_a);
        XC_EVENT_SET_KEYMOD(&selectedKeyEvent, 0);
        CHECK(convertEvent(display, &selectedKeyEvent, &ignored, True) == 0,
              "XSelectInput disabled active IM text input");
        SDL_Event duplicateText;
        set_text_input_event(&duplicateText, "a");
        CHECK(convertEvent(display, &duplicateText, &ignored, True) == -1,
              "XSelectInput left duplicate ASCII text input active");
    }
    XSetICFocus(ic);
    while (XCheckTypedEvent(display, KeyRelease, &ignored))
        ;
    for (int i = 0; i < 4; i++) {
        SDL_Event textEvent;
        set_text_input_event(&textEvent, "z");
        SDL_PushEvent(&textEvent);
        XEvent out;
        CHECK(
            next_typed_event(display, KeyPress, &out) && out.xkey.keycode == 0,
            "SDL_TEXTINPUT under IC focus did not produce IM KeyPress");
        if (i == 0) {
            XImage *cleared =
                XGetImage(display, client, area.x, area.y, area.width,
                          area.height, AllPlanes, ZPixmap);
            CHECK(cleared, "preedit clear XGetImage failed");
            int nonWhite = 0;
            for (int py = 0; py < area.height; py++) {
                for (int px = 0; px < area.width; px++) {
                    if ((XGetPixel(cleared, px, py) & 0x00ffffff) != 0x00ffffff)
                        nonWhite++;
                }
            }
            XDestroyImage(cleared);
            CHECK(nonWhite == 0, "commit did not clear visible preedit area");
        }
        Status lookupStatus = XLookupNone;
        KeySym keysym = NoSymbol;
        if (i == 0) {
            char buf[4] = {0};
            int n = Xutf8LookupString(ic, &out.xkey, buf, sizeof(buf), &keysym,
                                      &lookupStatus);
            CHECK(n == 1 && buf[0] == 'z' && keysym == XK_z &&
                      lookupStatus == XLookupBoth,
                  "Xutf8LookupString did not return focused SDL text");
        } else if (i == 1) {
            char buf[4] = {0};
            int n = XmbLookupString(ic, &out.xkey, buf, sizeof(buf), &keysym,
                                    &lookupStatus);
            CHECK(n == 1 && buf[0] == 'z' && keysym == XK_z &&
                      lookupStatus == XLookupBoth,
                  "XmbLookupString did not return focused SDL text");
        } else if (i == 2) {
            wchar_t buf[4] = {0};
            int n =
                XwcLookupString(ic, &out.xkey, buf, 4, &keysym, &lookupStatus);
            CHECK(n == 1 && buf[0] == L'z' && keysym == XK_z &&
                      lookupStatus == XLookupBoth,
                  "XwcLookupString did not return focused SDL text");
        } else {
            /* Clients reading the commit with the simple 8-bit XLookupString
             * (as Mosaic's URL field does) must also get the text.
             */
            char buf[4] = {0};
            int n = XLookupString(&out.xkey, buf, sizeof(buf), &keysym, NULL);
            CHECK(n == 1 && buf[0] == 'z' && keysym == XK_z,
                  "XLookupString did not return focused SDL text");
        }
    }
    {
        /* XLookupString into a buffer too small for the commit must copy the
         * prefix without consuming, so a retry with a larger buffer recovers
         * the whole text instead of dropping the remainder.
         */
        SDL_Event textEvent;
        set_text_input_event(&textEvent, "ab");
        SDL_PushEvent(&textEvent);
        XEvent out;
        CHECK(
            next_typed_event(display, KeyPress, &out) && out.xkey.keycode == 0,
            "two-char SDL_TEXTINPUT did not produce IM KeyPress");
        char small[1];
        KeySym ks = NoSymbol;
        int n = XLookupString(&out.xkey, small, 1, &ks, NULL);
        CHECK(n == 1 && small[0] == 'a',
              "XLookupString did not return the truncated prefix");
        char full[4] = {0};
        n = XLookupString(&out.xkey, full, sizeof(full), &ks, NULL);
        CHECK(n == 2 && full[0] == 'a' && full[1] == 'b',
              "XLookupString lost committed text after a truncated read");
    }
    {
        SDL_Event textEvent;
        set_text_input_event(&textEvent, "\xe4\xb8\xad");
        SDL_PushEvent(&textEvent);
        XEvent out;
        CHECK(
            next_typed_event(display, KeyPress, &out) && out.xkey.keycode == 0,
            "wide-overflow SDL_TEXTINPUT did not produce IM KeyPress");
        wchar_t small[1] = {0};
        Status overflowStatus = XLookupNone;
        KeySym overflowKeysym = NoSymbol;
        int needed = XwcLookupString(ic, &out.xkey, small, 0, &overflowKeysym,
                                     &overflowStatus);
        CHECK(needed == 1 && overflowStatus == XBufferOverflow,
              "XwcLookupString did not report wide overflow");
        wchar_t retry[2] = {0};
        int got = XwcLookupString(ic, &out.xkey, retry, 2, &overflowKeysym,
                                  &overflowStatus);
        CHECK(got == 1 && retry[0] == (wchar_t) 0x4e2d &&
                  overflowKeysym == NoSymbol && overflowStatus == XLookupChars,
              "XwcLookupString lost pending text after overflow");
    }
    for (int i = 0; i < 2; i++) {
        SDL_Event textEvent;
        set_text_input_event(&textEvent, "\xe4\xb8\xad");
        SDL_PushEvent(&textEvent);
        XEvent out;
        CHECK(
            next_typed_event(display, KeyPress, &out) && out.xkey.keycode == 0,
            "CJK SDL_TEXTINPUT under IC focus did not produce IM KeyPress");
        Status lookupStatus = XLookupNone;
        KeySym keysym = NoSymbol;
        if (i == 0) {
            char buf[8] = {0};
            int n = Xutf8LookupString(ic, &out.xkey, buf, sizeof(buf), &keysym,
                                      &lookupStatus);
            CHECK(n == 3 && memcmp(buf, "\xe4\xb8\xad", 3) == 0 &&
                      keysym == NoSymbol && lookupStatus == XLookupChars,
                  "Xutf8LookupString did not return focused CJK SDL text");
        } else {
            wchar_t buf[4] = {0};
            int n =
                XwcLookupString(ic, &out.xkey, buf, 4, &keysym, &lookupStatus);
            CHECK(n == 1 && buf[0] == (wchar_t) 0x4e2d && keysym == NoSymbol &&
                      lookupStatus == XLookupChars,
                  "XwcLookupString did not return focused CJK SDL text");
        }
    }
    {
        /* drainSdlEventsToPutBack converts a whole SDL batch into put-back
         * KeyPress events before the client looks any commit up. Each commit is
         * bound to its event by id, so a batch must not collapse into the last
         * commit; reproduce that batch conversion here.
         */
        SDL_Event firstText, secondText;
        set_text_input_event(&firstText, "a");
        set_text_input_event(&secondText, "b");
        XEvent firstOut, secondOut;
        CHECK(convertEvent(display, &firstText, &firstOut, True) == 0 &&
                  convertEvent(display, &secondText, &secondOut, True) == 0,
              "batched SDL_TEXTINPUT conversion failed");
        char buf[4] = {0};
        Status batchStatus = XLookupNone;
        KeySym batchKeysym = NoSymbol;
        int n = Xutf8LookupString(ic, &firstOut.xkey, buf, sizeof(buf),
                                  &batchKeysym, &batchStatus);
        CHECK(n == 1 && buf[0] == 'a',
              "batched IM commits collapsed: first commit lost");
        buf[0] = '\0';
        n = Xutf8LookupString(ic, &secondOut.xkey, buf, sizeof(buf),
                              &batchKeysym, &batchStatus);
        CHECK(n == 1 && buf[0] == 'b',
              "batched IM commits collapsed: second commit wrong");
    }
    {
        /* Event scans (XCheckWindowEvent and friends) can hand the client a
         * later IM commit before an earlier one. Each commit is bound to its
         * event by id, so an out-of-order lookup must still fetch its own text;
         * a plain FIFO would return them swapped.
         */
        SDL_Event firstText, secondText;
        set_text_input_event(&firstText, "p");
        set_text_input_event(&secondText, "q");
        XEvent firstOut, secondOut;
        CHECK(convertEvent(display, &firstText, &firstOut, True) == 0 &&
                  convertEvent(display, &secondText, &secondOut, True) == 0,
              "out-of-order IM conversion failed");
        char buf[4] = {0};
        Status st = XLookupNone;
        KeySym ks = NoSymbol;
        int n =
            Xutf8LookupString(ic, &secondOut.xkey, buf, sizeof(buf), &ks, &st);
        CHECK(n == 1 && buf[0] == 'q',
              "out-of-order IM lookup fetched the wrong commit");
        buf[0] = '\0';
        n = Xutf8LookupString(ic, &firstOut.xkey, buf, sizeof(buf), &ks, &st);
        CHECK(n == 1 && buf[0] == 'p',
              "out-of-order IM lookup lost the earlier commit");
    }
    XUnsetICFocus(ic);

    XIC callbackIC =
        XCreateIC(im, XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
                  XNClientWindow, client, XNFocusWindow, focus,
                  XNPreeditAttributes, preedit, NULL);
    CHECK(callbackIC, "XCreateIC failed for XIMPreeditCallbacks");
    XSetICFocus(callbackIC);
    preeditStartCount = 0;
    preeditDoneCount = 0;
    preeditDrawCount = 0;
    preeditCaretCount = 0;
    preeditDrawText[0] = '\0';
    SDL_zero(editingEvent);
    editingEvent.type = SDL_TEXTEDITING;
    XC_SET_EDITING_EVENT(editingEvent, "xy");
    CHECK(convertEvent(display, &editingEvent, &ignored, True) == -1,
          "callback-style SDL_TEXTEDITING produced a committed X event");
    CHECK(preeditStartCount == 1 && preeditDrawCount == 1 &&
              preeditCaretCount == 1 && strcmp(preeditDrawText, "xy") == 0,
          "callback-style IC did not receive visible preedit callbacks");
    SDL_Event commitEvent;
    set_text_input_event(&commitEvent, "x");
    CHECK(convertEvent(display, &commitEvent, &ignored, True) != -1 &&
              ignored.type == KeyPress,
          "callback-style SDL_TEXTINPUT did not commit through XIM");
    CHECK(preeditDoneCount == 1, "commit did not finish active preedit");
    Status callbackStatus = XLookupNone;
    KeySym callbackKeysym = NoSymbol;
    char callbackBuf[4] = {0};
    int callbackLen = Xutf8LookupString(callbackIC, &ignored.xkey, callbackBuf,
                                        sizeof(callbackBuf), &callbackKeysym,
                                        &callbackStatus);
    CHECK(callbackLen == 1 && callbackBuf[0] == 'x' && callbackKeysym == XK_x &&
              callbackStatus == XLookupBoth,
          "callback-style committed text was not available to lookup");
    /* A plain commit with no preedit in progress (events.c still clears preedit
     * with an empty string first) must not emit a spurious empty draw callback.
     */
    int drawBeforePlainCommit = preeditDrawCount;
    SDL_Event plainCommit;
    set_text_input_event(&plainCommit, "y");
    CHECK(convertEvent(display, &plainCommit, &ignored, True) != -1 &&
              ignored.type == KeyPress,
          "plain callback-style SDL_TEXTINPUT did not commit through XIM");
    CHECK(preeditDrawCount == drawBeforePlainCommit,
          "plain commit fired a spurious empty preedit draw callback");
    callbackStatus = XLookupNone;
    callbackBuf[0] = '\0';
    callbackLen = Xutf8LookupString(callbackIC, &ignored.xkey, callbackBuf,
                                    sizeof(callbackBuf), &callbackKeysym,
                                    &callbackStatus);
    CHECK(callbackLen == 1 && callbackBuf[0] == 'y',
          "plain callback-style commit text was not available to lookup");
    XUnsetICFocus(callbackIC);
    XDestroyIC(callbackIC);

    {
        /* A preedit callback that destroys its own IC must not recurse, double
         * free, or fire later callbacks. With the reentrancy and focus guards
         * the draw callback runs exactly once and the caret callback, which
         * would deref the freed IC, never runs.
         */
        reentrantDrawCount = 0;
        reentrantCaretCount = 0;
        reentrantDestroyOnDraw = True;
        XIMCallback reDraw = {NULL, test_preedit_reentrant_draw_callback};
        XIMCallback reCaret = {NULL, test_preedit_reentrant_caret_callback};
        XVaNestedList rePreedit =
            XVaCreateNestedList(0, XNPreeditDrawCallback, &reDraw,
                                XNPreeditCaretCallback, &reCaret, NULL);
        CHECK(rePreedit, "XVaCreateNestedList for reentrant preedit failed");
        XIC reIC =
            XCreateIC(im, XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
                      XNClientWindow, client, XNFocusWindow, focus,
                      XNPreeditAttributes, rePreedit, NULL);
        CHECK(reIC, "XCreateIC failed for reentrant IC");
        XFree(rePreedit);
        reentrantIC = reIC;
        XSetICFocus(reIC);
        SDL_zero(editingEvent);
        editingEvent.type = SDL_TEXTEDITING;
        XC_SET_EDITING_EVENT(editingEvent, "z");
        CHECK(convertEvent(display, &editingEvent, &ignored, True) == -1,
              "reentrant SDL_TEXTEDITING produced a committed X event");
        CHECK(reentrantDrawCount == 1,
              "reentrant draw callback did not fire exactly once");
        CHECK(reentrantCaretCount == 0,
              "caret callback ran after the IC was destroyed mid-draw");
        reentrantIC = NULL;
        reentrantDestroyOnDraw = False;
    }

    {
        /* Same reentrant destruction, but while XDestroyIC itself clears active
         * preedit through inputMethodUnsetFocus.
         */
        reentrantDrawCount = 0;
        reentrantCaretCount = 0;
        reentrantDestroyOnDraw = False;
        XIMCallback reDraw = {NULL, test_preedit_reentrant_draw_callback};
        XIMCallback reCaret = {NULL, test_preedit_reentrant_caret_callback};
        XVaNestedList rePreedit =
            XVaCreateNestedList(0, XNPreeditDrawCallback, &reDraw,
                                XNPreeditCaretCallback, &reCaret, NULL);
        CHECK(rePreedit,
              "XVaCreateNestedList for teardown reentrant preedit failed");
        XIC reIC =
            XCreateIC(im, XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
                      XNClientWindow, client, XNFocusWindow, focus,
                      XNPreeditAttributes, rePreedit, NULL);
        CHECK(reIC, "XCreateIC failed for teardown reentrant IC");
        XFree(rePreedit);
        reentrantIC = reIC;
        XSetICFocus(reIC);
        SDL_zero(editingEvent);
        editingEvent.type = SDL_TEXTEDITING;
        XC_SET_EDITING_EVENT(editingEvent, "w");
        CHECK(
            convertEvent(display, &editingEvent, &ignored, True) == -1,
            "teardown reentrant SDL_TEXTEDITING produced a committed X event");
        CHECK(reentrantDrawCount == 1 && reentrantCaretCount == 1,
              "teardown reentrant IC did not enter active preedit");
        reentrantDestroyOnDraw = True;
        XDestroyIC(reIC);
        CHECK(reentrantDrawCount == 2,
              "teardown reentrant draw callback did not fire exactly once");
        CHECK(reentrantCaretCount == 1,
              "caret callback ran after the IC was destroyed during teardown");
        reentrantIC = NULL;
        reentrantDestroyOnDraw = False;
    }

    {
        /* Destroying an IC with active preedit must still fire the done
         * callback so a callback client's start/done stay balanced; the caret
         * is skipped during teardown.
         */
        int balanceStart = 0;
        int balanceDone = 0;
        XIMCallback balanceStartCb = {(XPointer) &balanceStart,
                                      test_preedit_start_done_callback};
        XIMCallback balanceDoneCb = {(XPointer) &balanceDone,
                                     test_preedit_start_done_callback};
        XIMCallback balanceDrawCb = {NULL, test_preedit_draw_callback};
        XVaNestedList balancePreedit = XVaCreateNestedList(
            0, XNPreeditStartCallback, &balanceStartCb, XNPreeditDoneCallback,
            &balanceDoneCb, XNPreeditDrawCallback, &balanceDrawCb, NULL);
        CHECK(balancePreedit, "XVaCreateNestedList for balance preedit failed");
        XIC balanceIC =
            XCreateIC(im, XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
                      XNClientWindow, client, XNFocusWindow, focus,
                      XNPreeditAttributes, balancePreedit, NULL);
        CHECK(balanceIC, "XCreateIC failed for balance IC");
        XFree(balancePreedit);
        XSetICFocus(balanceIC);
        SDL_zero(editingEvent);
        editingEvent.type = SDL_TEXTEDITING;
        XC_SET_EDITING_EVENT(editingEvent, "ab");
        CHECK(convertEvent(display, &editingEvent, &ignored, True) == -1,
              "balance SDL_TEXTEDITING produced a committed X event");
        CHECK(balanceStart == 1 && balanceDone == 0,
              "balance preedit start did not fire or done fired early");
        XDestroyIC(balanceIC);
        CHECK(balanceDone == 1,
              "destroying an IC mid-preedit did not fire the done callback");
    }

    {
        /* Focus switch where the outgoing IC's done callback destroys the
         * incoming IC. XSetICFocus must detect the freed target and bail rather
         * than installing and dereferencing it.
         */
        focusSwitchDoneCount = 0;
        XIMCallback outDone = {NULL, test_focus_switch_done_callback};
        XIMCallback outDraw = {NULL, test_preedit_draw_callback};
        XVaNestedList outPreedit =
            XVaCreateNestedList(0, XNPreeditDoneCallback, &outDone,
                                XNPreeditDrawCallback, &outDraw, NULL);
        CHECK(outPreedit, "XVaCreateNestedList for focus-switch out IC failed");
        XIC outIC =
            XCreateIC(im, XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
                      XNClientWindow, client, XNFocusWindow, focus,
                      XNPreeditAttributes, outPreedit, NULL);
        CHECK(outIC, "XCreateIC failed for focus-switch out IC");
        XFree(outPreedit);
        XIC inIC =
            XCreateIC(im, XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
                      XNClientWindow, client, XNFocusWindow, focus, NULL);
        CHECK(inIC, "XCreateIC failed for focus-switch in IC");
        XSetICFocus(outIC);
        SDL_zero(editingEvent);
        editingEvent.type = SDL_TEXTEDITING;
        XC_SET_EDITING_EVENT(editingEvent, "q");
        CHECK(convertEvent(display, &editingEvent, &ignored, True) == -1,
              "focus-switch SDL_TEXTEDITING produced a committed X event");
        focusSwitchVictim = inIC;
        XSetICFocus(inIC);
        CHECK(focusSwitchDoneCount == 1,
              "outgoing IC done callback did not fire during focus switch");
        CHECK(focusSwitchVictim == NULL,
              "incoming IC was not destroyed by the focus-switch callback");
        /* Focus must remain on the live outgoing IC, not the freed incoming
         * one: a further edit still reaches outIC's draw callback. If the freed
         * IC had been installed this would dereference it.
         */
        preeditDrawCount = 0;
        preeditDrawText[0] = '\0';
        SDL_zero(editingEvent);
        editingEvent.type = SDL_TEXTEDITING;
        XC_SET_EDITING_EVENT(editingEvent, "r");
        CHECK(convertEvent(display, &editingEvent, &ignored, True) == -1,
              "post-switch SDL_TEXTEDITING produced a committed X event");
        CHECK(preeditDrawCount >= 1 && strcmp(preeditDrawText, "r") == 0,
              "focus did not stay on the live IC after the aborted switch");
        XDestroyIC(outIC);
    }

    XIC rootStyleIC =
        XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                  XNClientWindow, client, XNFocusWindow, client, NULL);
    CHECK(rootStyleIC, "XCreateIC failed for XIMPreeditNothing");
    preeditGc = XCreateGC(display, client, 0, NULL);
    CHECK(preeditGc, "root-style preedit GC creation failed");
    CHECK(XSetForeground(display, preeditGc, 0xffffffff),
          "root-style preedit white foreground failed");
    CHECK(XFillRectangle(display, client, preeditGc, 0, 0, 120, 60),
          "root-style preedit background fill failed");
    XFreeGC(display, preeditGc);
    XSetICFocus(rootStyleIC);
    SDL_zero(editingEvent);
    editingEvent.type = SDL_TEXTEDITING;
    XC_SET_EDITING_EVENT(editingEvent, "rt");
    CHECK(convertEvent(display, &editingEvent, &ignored, True) == -1,
          "root-style SDL_TEXTEDITING produced a committed X event");
    preeditImage = XGetImage(display, client, 2, 2, 24, 18, AllPlanes, ZPixmap);
    CHECK(preeditImage, "root-style preedit XGetImage failed");
    darkPixels = 0;
    for (int py = 0; py < 18; py++) {
        for (int px = 0; px < 24; px++) {
            if ((XGetPixel(preeditImage, px, py) & 0x00ffffff) != 0x00ffffff)
                darkPixels++;
        }
    }
    XDestroyImage(preeditImage);
    CHECK(darkPixels > 0, "root-style preedit was not visibly drawn");
    XUnsetICFocus(rootStyleIC);
    XDestroyIC(rootStyleIC);

    XIC positionIC =
        XCreateIC(im, XNInputStyle, XIMPreeditPosition | XIMStatusNothing,
                  XNClientWindow, client, XNFocusWindow, client, NULL);
    CHECK(positionIC, "XCreateIC failed for XIMPreeditPosition");
    XPoint spot = {.x = 32, .y = 22};
    XVaNestedList spotAttrs =
        XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
    CHECK(spotAttrs, "XVaCreateNestedList for spot location failed");
    CHECK(!XSetICValues(positionIC, XNPreeditAttributes, spotAttrs, NULL),
          "XSetICValues failed for spot location");
    XFree(spotAttrs);
    preeditGc = XCreateGC(display, client, 0, NULL);
    CHECK(preeditGc, "position preedit GC creation failed");
    CHECK(XSetForeground(display, preeditGc, 0xffffffff),
          "position preedit white foreground failed");
    CHECK(XFillRectangle(display, client, preeditGc, 0, 0, 120, 60),
          "position preedit background fill failed");
    XFreeGC(display, preeditGc);
    XSetICFocus(positionIC);
    SDL_zero(editingEvent);
    editingEvent.type = SDL_TEXTEDITING;
    XC_SET_EDITING_EVENT(editingEvent, "os");
    CHECK(convertEvent(display, &editingEvent, &ignored, True) == -1,
          "position SDL_TEXTEDITING produced a committed X event");
    preeditImage =
        XGetImage(display, client, spot.x, spot.y, 24, 18, AllPlanes, ZPixmap);
    CHECK(preeditImage, "position preedit XGetImage failed");
    darkPixels = 0;
    for (int py = 0; py < 18; py++) {
        for (int px = 0; px < 24; px++) {
            if ((XGetPixel(preeditImage, px, py) & 0x00ffffff) != 0x00ffffff)
                darkPixels++;
        }
    }
    XDestroyImage(preeditImage);
    CHECK(darkPixels > 0, "position preedit was not drawn at spot location");
    XUnsetICFocus(positionIC);
    XDestroyIC(positionIC);

    XFree(preedit);
    XFree(preeditBack);
    XDestroyIC(ic);

    XVaNestedList nestedIC = XVaCreateNestedList(
        0, XNInputStyle,
        (XPointer) (uintptr_t) (XIMPreeditNothing | XIMStatusNothing),
        XNClientWindow, client, XNFocusWindow, focus, NULL);
    CHECK(nestedIC, "XVaCreateNestedList for IC failed");
    ic = XCreateIC(im, XNVaNestedList, nestedIC, NULL);
    CHECK(ic, "XCreateIC failed for XNVaNestedList");
    style = 0;
    CHECK(!XGetICValues(ic, XNInputStyle, &style, NULL),
          "XGetICValues failed for nested-list IC");
    CHECK(style == (XIMPreeditNothing | XIMStatusNothing),
          "XCreateIC did not apply nested-list input style");
    XFree(nestedIC);
    XDestroyIC(ic);

    char **missingCharsets = NULL;
    int missingCharsetCount = -1;
    char *defaultString = NULL;
    XFontSet fixed14 =
        XCreateFontSet(display, "*medium*-r-*--14*", &missingCharsets,
                       &missingCharsetCount, &defaultString);
    CHECK(fixed14 != NULL, "14-pixel medium fontset did not load");
    CHECK(missingCharsetCount == 0, "fontset reported missing charsets");
    XFontStruct **fontStructs = NULL;
    char **fontNames = NULL;
    CHECK(XFontsOfFontSet(fixed14, &fontStructs, &fontNames) == 1,
          "fontset did not expose one compatibility font");
    CHECK(fontStructs[0]->ascent == 12 && fontStructs[0]->descent == 2 &&
              fontStructs[0]->max_bounds.width == 7,
          "14-pixel medium fontset did not use native fixed metrics");
    CHECK(XmbTextEscapement(fixed14, "Hello", 5) == 35,
          "XmbTextEscapement ignored fontset fixed width");
    XFreeFontSet(display, fixed14);

    XFontSet fixed18 =
        XCreateFontSet(display, "*medium*-r-*--18*", NULL, NULL, NULL);
    CHECK(fixed18 != NULL, "18-pixel medium fontset did not load");
    CHECK(XmbTextEscapement(fixed18, "Hello", 5) == 45,
          "18-pixel fontset escapement did not use native fixed width");
    XFreeFontSet(display, fixed18);

    XDestroyWindow(display, client);
    XCloseIM(im);
    return 1;
}

static int test_defaults(Display *display)
{
    int screenNumber = DefaultScreen(display);
    Screen *screen = DefaultScreenOfDisplay(display);
    Window root = RootWindow(display, screenNumber);
    CHECK(XBitmapUnit(display) == 32, "XBitmapUnit returned unexpected value");
    CHECK(XBitmapPad(display) == 32, "XBitmapPad returned unexpected value");
    CHECK(XBitmapBitOrder(display) == ImageByteOrder(display),
          "XBitmapBitOrder did not match image byte order");
    CHECK(XRootWindowOfScreen(screen) == root, "XRootWindowOfScreen mismatch");
    CHECK(XBlackPixelOfScreen(screen) == BlackPixel(display, screenNumber),
          "XBlackPixelOfScreen mismatch");
    CHECK(XWhitePixelOfScreen(screen) == WhitePixel(display, screenNumber),
          "XWhitePixelOfScreen mismatch");
    unsigned long defaultPixelLimit =
        1ul << (unsigned int) DefaultDepth(display, screenNumber);
    CHECK(BlackPixel(display, screenNumber) < defaultPixelLimit &&
              WhitePixel(display, screenNumber) < defaultPixelLimit,
          "default black/white pixels exceeded default depth range");
    CHECK(XDefaultColormapOfScreen(screen) ==
              DefaultColormap(display, screenNumber),
          "XDefaultColormapOfScreen mismatch");
    CHECK(XDefaultDepthOfScreen(screen) == DefaultDepth(display, screenNumber),
          "XDefaultDepthOfScreen mismatch");
    CHECK(XDefaultGCOfScreen(screen) == DefaultGC(display, screenNumber),
          "XDefaultGCOfScreen mismatch");
    CHECK(XWidthOfScreen(screen) == DisplayWidth(display, screenNumber),
          "XWidthOfScreen mismatch");
    CHECK(XHeightOfScreen(screen) == DisplayHeight(display, screenNumber),
          "XHeightOfScreen mismatch");
    CHECK(XWidthMMOfScreen(screen) == DisplayWidthMM(display, screenNumber),
          "XWidthMMOfScreen mismatch");
    CHECK(XHeightMMOfScreen(screen) == DisplayHeightMM(display, screenNumber),
          "XHeightMMOfScreen mismatch");
    int displayWidthMM = DisplayWidthMM(display, screenNumber);
    int displayHeightMM = DisplayHeightMM(display, screenNumber);
    int reportedXDpi =
        (DisplayWidth(display, screenNumber) * 254 + displayWidthMM * 5) /
        (displayWidthMM * 10);
    int reportedYDpi =
        (DisplayHeight(display, screenNumber) * 254 + displayHeightMM * 5) /
        (displayHeightMM * 10);
    CHECK(reportedXDpi >= 95 && reportedXDpi <= 97,
          "DisplayWidthMM did not report 96 DPI logical width");
    CHECK(reportedYDpi >= 95 && reportedYDpi <= 97,
          "DisplayHeightMM did not report 96 DPI logical height");
    CHECK(XPlanesOfScreen(screen) == DisplayPlanes(display, screenNumber),
          "XPlanesOfScreen mismatch");
    CHECK(XCellsOfScreen(screen) == DisplayCells(display, screenNumber),
          "XCellsOfScreen mismatch");
    CHECK(XMinCmapsOfScreen(screen) == 1 && XMaxCmapsOfScreen(screen) == 1,
          "screen colormap limits mismatch");
    CHECK(!XDoesSaveUnders(screen), "XDoesSaveUnders should be false");
    CHECK(XDoesBackingStore(screen) == NotUseful, "XDoesBackingStore mismatch");
    CHECK(XEventMaskOfScreen(screen) == NoEventMask,
          "XEventMaskOfScreen mismatch");
    CHECK(XScreenNumberOfScreen(screen) == screenNumber,
          "XScreenNumberOfScreen mismatch");
    CHECK(ServerVendor(display), "ServerVendor returned no string");
    CHECK(strncmp(ServerVendor(display), "SDL ", 4) == 0,
          "ServerVendor did not report compiled SDL policy");
    CHECK(DefaultScreen(display) == 0, "DefaultScreen policy changed");
    CHECK(DefaultScreen(display) < ScreenCount(display),
          "DefaultScreen outside screen bounds");
    CHECK(XBell(display, 0) == 1 && XBell(display, -100) == 1 &&
              XBell(display, 100) == 1,
          "XBell rejected a valid percent");
    last_error_code = 0;
    int (*oldErrorHandler)(Display *, XErrorEvent *) =
        XSetErrorHandler(record_error);
    CHECK(XBell(display, 101) == 1, "XBell invalid percent call failed");
    XSetErrorHandler(oldErrorHandler);
    CHECK(last_error_code == BadValue,
          "XBell invalid percent did not report BadValue");
    CHECK(matchWildcard("*ab??", "bla-abab--"),
          "matchWildcard failed backtracking case");
    CHECK(matchWildcard("demo.*.font", "demo.ui.font"),
          "matchWildcard failed interior star case");
    CHECK(matchWildcard("demo.??", "demo.ui"),
          "matchWildcard failed question mark case");
    CHECK(!matchWildcard("demo.??", "demo.u"),
          "matchWildcard accepted too-short question match");
    CHECK(matchWildcard("demo*", "demo"), "matchWildcard failed trailing star");
    CHECK(matchWildcard("", ""), "matchWildcard failed empty pattern");
    CHECK(!matchWildcard(NULL, "demo"), "matchWildcard accepted NULL pattern");
    CHECK(!matchWildcard("demo", NULL), "matchWildcard accepted NULL string");

    int depthCount = 0;
    int *depths = XListDepths(display, screenNumber, &depthCount);
    CHECK(depths != NULL && depthCount >= 4,
          "XListDepths returned too few depths");
    int hasDepth1 = 0;
    int hasDepth16 = 0;
    int hasDepth24 = 0;
    int hasDepth32 = 0;
    for (int i = 0; i < depthCount; i++) {
        if (depths[i] == 1)
            hasDepth1 = 1;
        if (depths[i] == 16)
            hasDepth16 = 1;
        if (depths[i] == 24)
            hasDepth24 = 1;
        if (depths[i] == 32)
            hasDepth32 = 1;
    }
    XFree(depths);
    CHECK(hasDepth1 && hasDepth16 && hasDepth24 && hasDepth32,
          "XListDepths missing common depths");

    Visual *defaultVisual = DefaultVisual(display, screenNumber);
    CHECK(defaultVisual, "DefaultVisual returned no visual");
    XVisualInfo visualTemplate;
    memset(&visualTemplate, 0, sizeof(visualTemplate));
    visualTemplate.screen = screenNumber;
    visualTemplate.depth = DefaultDepth(display, screenNumber);
    int visualCount = 0;
    XVisualInfo *visuals =
        XGetVisualInfo(display, VisualScreenMask | VisualDepthMask,
                       &visualTemplate, &visualCount);
    CHECK(visuals && visualCount > 0,
          "XGetVisualInfo did not match default screen/depth");
    CHECK(visuals[0].screen == screenNumber &&
              visuals[0].depth == DefaultDepth(display, screenNumber),
          "XGetVisualInfo returned incorrect screen/depth");
    XFree(visuals);
    CHECK(XMatchVisualInfo(display, screenNumber,
                           DefaultDepth(display, screenNumber), TrueColor,
                           &visualTemplate),
          "XMatchVisualInfo did not match the default TrueColor visual");
    CHECK(visualTemplate.visual == defaultVisual,
          "XMatchVisualInfo returned a non-default visual");
    CHECK(!XMatchVisualInfo(display, screenNumber, 64, TrueColor,
                            &visualTemplate),
          "XMatchVisualInfo accepted an unsupported depth");

    char path[] = "/tmp/libx11-compat-defaults-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp failed for defaults test");
    const char defaults[] =
        "demo.font: 9x13\n"
        "*dpi: 144\n"
        "other.theme: ignored\n";
    CHECK(write(fd, defaults, sizeof(defaults) - 1) ==
              (ssize_t) sizeof(defaults) - 1,
          "writing defaults failed");
    close(fd);

    setenv("XENVIRONMENT", path, 1);
    CHECK(!strcmp(XGetDefault(display, "/usr/bin/demo", "font"), "9x13"),
          "XGetDefault did not read program-specific value");
    CHECK(!strcmp(XGetDefault(display, "other", "dpi"), "144"),
          "XGetDefault did not read wildcard value");
    unsetenv("XENVIRONMENT");
    unlink(path);

    char *oldDefaults = display->xdefaults;
    display->xdefaults =
        "demo.font: 10x20\n"
        "*foreground: navy\n";
    CHECK(XResourceManagerString(display) == display->xdefaults,
          "XResourceManagerString did not expose display defaults");
    CHECK(XScreenResourceString(screen) == display->xdefaults,
          "XScreenResourceString did not expose screen defaults");
    CHECK(!strcmp(XGetDefault(display, "demo", "font"), "10x20"),
          "XGetDefault did not read display resource manager string");
    display->xdefaults = oldDefaults;

    CHECK(!strcmp(XGetDefault(display, "demo", "font"), "fixed"),
          "XGetDefault built-in font default failed");
    CHECK(!strcmp(XGetDefault(display, "demo", "Xft.dpi"), "96"),
          "XGetDefault built-in DPI default failed");
    CHECK(XGetDefault(display, "demo", "unknownOption") == NULL,
          "XGetDefault returned value for unknown option");
    return 1;
}

/* ICCCM WM_HINTS / WM_NORMAL_HINTS / WM_NAME round-trip plus pass-through for
 * _NET_WM_ICON edge inputs (empty / truncated).
 */
static int test_icccm_wm_hints(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 0, 0, 16, 16, 0, 0, 0);
    CHECK(window != None, "XCreateSimpleWindow for WM hint test failed");

    XWMHints *hints = XAllocWMHints();
    CHECK(hints != NULL, "XAllocWMHints failed");
    hints->flags = InputHint | StateHint | IconPositionHint;
    hints->input = True;
    hints->initial_state = NormalState;
    hints->icon_x = 7;
    hints->icon_y = 9;
    CHECK(XSetWMHints(display, window, hints), "XSetWMHints failed");
    XWMHints *readHints = XGetWMHints(display, window);
    CHECK(readHints != NULL, "XGetWMHints failed");
    CHECK(readHints->flags == hints->flags && readHints->input == True &&
              readHints->initial_state == NormalState &&
              readHints->icon_x == 7 && readHints->icon_y == 9,
          "WM hints did not round-trip");
    XFree(hints);
    XFree(readHints);

    XSizeHints *sizeHints = XAllocSizeHints();
    CHECK(sizeHints != NULL, "XAllocSizeHints failed");
    sizeHints->flags = PMinSize | PMaxSize | PBaseSize | PWinGravity;
    sizeHints->min_width = 10;
    sizeHints->min_height = 11;
    sizeHints->max_width = 100;
    sizeHints->max_height = 101;
    sizeHints->base_width = 8;
    sizeHints->base_height = 9;
    sizeHints->win_gravity = StaticGravity;
    XSetWMNormalHints(display, window, sizeHints);

    XSizeHints readSizeHints;
    long supplied = 0;
    CHECK(XGetWMNormalHints(display, window, &readSizeHints, &supplied),
          "XGetWMNormalHints failed");
    CHECK(supplied == sizeHints->flags && readSizeHints.min_width == 10 &&
              readSizeHints.max_height == 101 &&
              readSizeHints.base_width == 8 &&
              readSizeHints.win_gravity == StaticGravity,
          "WM normal hints did not round-trip");
    XFree(sizeHints);

    char *nameList[] = {"libx11-compat"};
    XTextProperty text;
    CHECK(XStringListToTextProperty(nameList, 1, &text),
          "XStringListToTextProperty failed");
    XSetTextProperty(display, window, &text, XA_WM_NAME);
    XTextProperty readText;
    CHECK(XGetTextProperty(display, window, &readText, XA_WM_NAME),
          "XGetTextProperty failed");
    CHECK(readText.encoding == XA_STRING && readText.format == 8 &&
              !strcmp((char *) readText.value, "libx11-compat"),
          "text property did not round-trip");
    XFree(text.value);
    XFree(readText.value);

    Atom netWmIcon = XInternAtom(display, "_NET_WM_ICON", False);
    CHECK(XChangeProperty(display, window, netWmIcon, XA_CARDINAL, 32,
                          PropModeReplace, NULL, 0),
          "empty _NET_WM_ICON property failed");
    unsigned long badIcon[] = {64, 64, 0xFFFFFFFF};
    CHECK(XChangeProperty(display, window, netWmIcon, XA_CARDINAL, 32,
                          PropModeReplace, (unsigned char *) badIcon, 3),
          "truncated _NET_WM_ICON property failed");

    XDestroyWindow(display, window);
    return 1;
}

/* An active grab is released when its grab window becomes unviewable. Without
 * this a Motif menu's pointer grab outlives the unmapped menu shell and
 * swallows all later input (the Help-menu freeze). Exercise unmap, destroy, the
 * ancestor-unmap path, and reparent-under-an-unmapped-parent.
 */
static int test_grab_release_on_unviewable(Display *display)
{
    Window root = DefaultRootWindow(display);

    /* Pointer grab dropped when the grab window itself unmaps. */
    Window win = XCreateSimpleWindow(display, root, 0, 0, 32, 32, 0, 0, 0);
    CHECK(win != None, "grab-release: window creation failed");
    XMapWindow(display, win);
    CHECK(XGrabPointer(display, win, False, ButtonPressMask, GrabModeAsync,
                       GrabModeAsync, None, None, CurrentTime) == GrabSuccess,
          "grab-release: XGrabPointer failed");
    CHECK(getGrabbedPointerWindow() == win,
          "grab-release: pointer grab not active on the window");
    CHECK(XUnmapWindow(display, win), "grab-release: unmap failed");
    CHECK(getGrabbedPointerWindow() == None,
          "unmapping the grab window left the pointer grab active");
    XDestroyWindow(display, win);

    /* Keyboard grab dropped on unmap as well. */
    win = XCreateSimpleWindow(display, root, 0, 0, 32, 32, 0, 0, 0);
    CHECK(win != None, "grab-release: keyboard window creation failed");
    XMapWindow(display, win);
    CHECK(XGrabKeyboard(display, win, False, GrabModeAsync, GrabModeAsync,
                        CurrentTime) == GrabSuccess,
          "grab-release: XGrabKeyboard failed");
    CHECK(getGrabbedKeyboardWindow() == win,
          "grab-release: keyboard grab not active on the window");
    CHECK(XUnmapWindow(display, win), "grab-release: keyboard unmap failed");
    CHECK(getGrabbedKeyboardWindow() == None,
          "unmapping the grab window left the keyboard grab active");
    XDestroyWindow(display, win);

    /* Pointer grab dropped when the grab window is destroyed. */
    win = XCreateSimpleWindow(display, root, 0, 0, 32, 32, 0, 0, 0);
    CHECK(win != None, "grab-release: destroy window creation failed");
    XMapWindow(display, win);
    CHECK(XGrabPointer(display, win, False, ButtonPressMask, GrabModeAsync,
                       GrabModeAsync, None, None, CurrentTime) == GrabSuccess,
          "grab-release: XGrabPointer (destroy) failed");
    CHECK(getGrabbedPointerWindow() == win,
          "grab-release: pointer grab not active before destroy");
    XDestroyWindow(display, win);
    CHECK(getGrabbedPointerWindow() == None,
          "destroying the grab window left the pointer grab active");

    /* Grab on a child dropped when an ancestor unmaps (isParent path). */
    Window parent = XCreateSimpleWindow(display, root, 0, 0, 64, 64, 0, 0, 0);
    CHECK(parent != None, "grab-release: ancestor parent creation failed");
    XMapWindow(display, parent);
    Window child = XCreateSimpleWindow(display, parent, 0, 0, 32, 32, 0, 0, 0);
    CHECK(child != None, "grab-release: ancestor child creation failed");
    XMapWindow(display, child);
    CHECK(XGrabPointer(display, child, False, ButtonPressMask, GrabModeAsync,
                       GrabModeAsync, None, None, CurrentTime) == GrabSuccess,
          "grab-release: XGrabPointer on child failed");
    CHECK(getGrabbedPointerWindow() == child,
          "grab-release: child grab not active");
    CHECK(XUnmapWindow(display, parent), "grab-release: ancestor unmap failed");
    CHECK(getGrabbedPointerWindow() == None,
          "unmapping an ancestor of the grab window left the grab active");
    XDestroyWindow(display, parent);

    /* Grab dropped when a mapped grab window is reparented under an unmapped
     * parent, which makes it unviewable without an XUnmapWindow.
     */
    Window hiddenParent =
        XCreateSimpleWindow(display, root, 0, 0, 64, 64, 0, 0, 0);
    CHECK(hiddenParent != None, "grab-release: hidden parent creation failed");
    win = XCreateSimpleWindow(display, root, 0, 0, 32, 32, 0, 0, 0);
    CHECK(win != None, "grab-release: reparent window creation failed");
    XMapWindow(display, win);
    CHECK(XGrabPointer(display, win, False, ButtonPressMask, GrabModeAsync,
                       GrabModeAsync, None, None, CurrentTime) == GrabSuccess,
          "grab-release: XGrabPointer (reparent) failed");
    CHECK(getGrabbedPointerWindow() == win,
          "grab-release: grab not active before reparent");
    XReparentWindow(display, win, hiddenParent, 0, 0);
    CHECK(getGrabbedPointerWindow() == None,
          "reparenting the grab window under an unmapped parent left the grab "
          "active");
    XDestroyWindow(display, hiddenParent);

    return 1;
}

static int run_test(const char *name, int (*test)(Display *))
{
    Display *display = XOpenDisplay(NULL);
    if (display == NULL) {
        fprintf(stderr, "%s: XOpenDisplay returned NULL\n", name);
        failures++;
        return 0;
    }
    int ok = test(display);
    XCloseDisplay(display);
    if (ok) {
        printf("ok %s\n", name);
    }
    return ok;
}

/* XmbTextPropertyToTextList used to be a stub that returned Success while
 * leaving its output pointers untouched, so Motif callers (the DataField text
 * widget, the XmString-to-text-property converter, and the file-selection path)
 * read garbage - a selected filename like "spectrum.agr" came back as random
 * bytes and could not be opened. Guard the round-trip: poison the outputs, run
 * the conversion, and require the real string back, for both an XA_STRING
 * (Latin-1) and a UTF8_STRING property, single and multi item.
 */
static int test_text_property_list(Display *display)
{
    Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    struct {
        const char *bytes;
        unsigned long nitems;
        Atom encoding;
        int expect_count;
        const char *expect0;
        const char *expect1;
    } cases[] = {
        {.bytes = "spectrum.agr",
         .nitems = 12,
         .encoding = XA_STRING,
         .expect_count = 1,
         .expect0 = "spectrum.agr"},
        {.bytes = "spectrum.agr",
         .nitems = 12,
         .encoding = 0 /* filled below */,
         .expect_count = 1,
         .expect0 = "spectrum.agr"},
        {.bytes = "a.agr\0b.agr",
         .nitems = 11,
         .encoding = XA_STRING,
         .expect_count = 2,
         .expect0 = "a.agr",
         .expect1 = "b.agr"},
    };
    cases[1].encoding = utf8;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        XTextProperty tp = {
            .value = (unsigned char *) cases[i].bytes,
            .encoding = cases[i].encoding,
            .format = 8,
            .nitems = cases[i].nitems,
        };
        char **list = (char **) (size_t) 0xdeadbeef;
        int count = -12345;
        int rc = XmbTextPropertyToTextList(display, &tp, &list, &count);
        CHECK(rc == Success, "text_property_list: conversion did not succeed");
        CHECK(count == cases[i].expect_count,
              "text_property_list: wrong item count");
        CHECK(list != NULL && list != (char **) (size_t) 0xdeadbeef,
              "text_property_list: output list left unset");
        CHECK(list[0] && !strcmp(list[0], cases[i].expect0),
              "text_property_list: first string garbled");
        if (cases[i].expect1)
            CHECK(list[1] && !strcmp(list[1], cases[i].expect1),
                  "text_property_list: second string garbled");
        XFreeStringList(list);
    }

    const char *currentLocale = setlocale(LC_CTYPE, NULL);
    char *oldLocale = strdup(currentLocale ? currentLocale : "C");
    CHECK(oldLocale != NULL, "text_property_list: could not save locale");
    const char *isoLocales[] = {"fr_FR.ISO8859-1", "en_IE.ISO8859-1",
                                "pt_PT.ISO8859-1"};
    const char *isoLocale = NULL;
    for (size_t i = 0; i < sizeof(isoLocales) / sizeof(isoLocales[0]); i++) {
        if (setlocale(LC_CTYPE, isoLocales[i])) {
            isoLocale = isoLocales[i];
            break;
        }
    }
    if (isoLocale) {
        unsigned char cafe[] = {'c', 'a', 'f', 0xe9};
        XTextProperty tp = {
            .value = cafe,
            .encoding = XA_STRING,
            .format = 8,
            .nitems = sizeof(cafe),
        };
        char **list = NULL;
        int count = 0;
        int rc = XmbTextPropertyToTextList(display, &tp, &list, &count);
        int ok = rc == Success && count == 1 && list && list[0] &&
                 !memcmp(list[0], "caf\xe9", 5);
        XFreeStringList(list);
        setlocale(LC_CTYPE, oldLocale);
        free(oldLocale);
        CHECK(ok, "text_property_list: Xmb did not return locale bytes");
    } else {
        setlocale(LC_CTYPE, oldLocale);
        free(oldLocale);
    }

    /* COMPOUND_TEXT is refused (no iconv), but it must fail cleanly with the
     * outputs cleared, not the old stub's Success-with-garbage.
     */
    XTextProperty ct = {
        .value = (unsigned char *) "spectrum.agr",
        .encoding = XInternAtom(display, "COMPOUND_TEXT", False),
        .format = 8,
        .nitems = 12,
    };
    char **ctlist = (char **) (size_t) 0xdeadbeef;
    int ctcount = -12345;
    int ctrc = XmbTextPropertyToTextList(display, &ct, &ctlist, &ctcount);
    CHECK(ctrc != Success,
          "text_property_list: COMPOUND_TEXT should be refused");
    CHECK(ctlist == NULL && ctcount == 0,
          "text_property_list: refused conversion left outputs unset");
    return 1;
}

/* End-to-end check that an installed shape mask carves a hole in subsequent
 * draw primitives. The mask is opaque-white except for a BLACK_HOLE rect in the
 * middle, which the spec says should clip the shape; pixels in that hole must
 * NOT receive the foreground fill applied afterwards.
 */
static int test_shape_mask(Display *display)
{
    enum { W = 16, H = 16, HOLE_X = 6, HOLE_Y = 6, HOLE_W = 4, HOLE_H = 4 };
    Window root = DefaultRootWindow(display);
    int screen = DefaultScreen(display);
    int depth = DefaultDepth(display, screen);

    Window window = XCreateSimpleWindow(display, root, 0, 0, W, H, 0, 0, 0);
    CHECK(window != None, "shape: window creation failed");
    CHECK(XMapWindow(display, window), "shape: window map failed");

    GC gc = XCreateGC(display, window, 0, NULL);
    CHECK(gc, "shape: GC creation failed");

    /* Build the mask pixmap first (white everywhere, black inside HOLE) and
     * install it. XShapeCombineMask paints visual indicators for the masked-out
     * region as part of its install, so the "baseline" the test preserves
     * across later draws is the state AFTER that install.
     */
    Pixmap mask = XCreatePixmap(display, window, W, H, depth);
    CHECK(mask != None, "shape: mask pixmap creation failed");
    GC maskGC = XCreateGC(display, mask, 0, NULL);
    CHECK(maskGC, "shape: mask GC creation failed");
    XSetForeground(display, maskGC, 0xFFFFFFFF);
    CHECK(XFillRectangle(display, mask, maskGC, 0, 0, W, H),
          "shape: mask white fill failed");
    XSetForeground(display, maskGC, 0xFF000000);
    CHECK(XFillRectangle(display, mask, maskGC, HOLE_X, HOLE_Y, HOLE_W, HOLE_H),
          "shape: mask hole fill failed");
    XFreeGC(display, maskGC);

    /* Seed the entire window with a baseline color before installing the mask
     * so the mask-install indicator + the surrounding region give the
     * post-install snapshot something stable to capture.
     */
    unsigned long blue = 0xFF0000FF;
    unsigned long red = 0xFFFF0000;
    XSetForeground(display, gc, blue);
    CHECK(XFillRectangle(display, window, gc, 0, 0, W, H),
          "shape: baseline fill failed");

    Bool bShaped = False;
    Bool cShaped = False;
    int xbs = 0, ybs = 0, xcs = 0, ycs = 0;
    unsigned int wbs = 0, hbs = 0, wcs = 0, hcs = 0;

    XShapeCombineMask(display, window, ShapeClip, 1, 2, mask, ShapeSet);
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "shape: query after clip-only mask failed");
    CHECK(!bShaped && cShaped && xbs == 0 && ybs == 0 && wbs == W && hbs == H &&
              xcs == 1 && ycs == 2 && wcs == W && hcs == H,
          "shape: clip-only mask polluted bounding extents");
    XShapeCombineMask(display, window, ShapeClip, 0, 0, None, ShapeSet);

    XShapeCombineMask(display, window, ShapeBounding, 0, 0, mask, ShapeSet);
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "shape: query after bounding mask failed");
    CHECK(bShaped && !cShaped && xbs == 0 && ybs == 0 && wbs == W && hbs == H &&
              xcs == 0 && ycs == 0 && wcs == W && hcs == H,
          "shape: bounding mask polluted clip extents");

    XShapeCombineMask(display, window, ShapeClip, 1, 2, mask, ShapeSet);
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "shape: query after clip mask failed");
    CHECK(bShaped && cShaped && xbs == 0 && ybs == 0 && wbs == W && hbs == H &&
              xcs == 1 && ycs == 2 && wcs == W && hcs == H,
          "shape: clip mask overwrote bounding extents");

    XShapeCombineMask(display, window, ShapeClip, 0, 0, None, ShapeSet);
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "shape: query after clearing clip mask failed");
    CHECK(bShaped && !cShaped && xbs == 0 && ybs == 0 && wbs == W && hbs == H,
          "shape: clearing clip mask cleared bounding mask");

    Pixmap sparseMask = XCreatePixmap(display, window, 8, 8, depth);
    CHECK(sparseMask != None, "shape: sparse pixmap creation failed");
    GC sparseGC = XCreateGC(display, sparseMask, 0, NULL);
    CHECK(sparseGC, "shape: sparse GC creation failed");
    XSetForeground(display, sparseGC, 0xFF000000);
    CHECK(XFillRectangle(display, sparseMask, sparseGC, 0, 0, 8, 8),
          "shape: sparse black fill failed");
    XSetForeground(display, sparseGC, 0xFFFFFFFF);
    CHECK(XFillRectangle(display, sparseMask, sparseGC, 2, 3, 3, 2),
          "shape: sparse active fill failed");
    XFreeGC(display, sparseGC);
    XShapeCombineMask(display, window, ShapeClip, 4, 5, sparseMask, ShapeSet);
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "shape: query after sparse clip mask failed");
    CHECK(cShaped && xcs == 6 && ycs == 8 && wcs == 3 && hcs == 2,
          "shape: sparse clip extents ignored active pixels");
    XShapeCombineMask(display, window, ShapeClip, 0, 0, None, ShapeSet);

    Pixmap smallMask = XCreatePixmap(display, window, 4, 4, depth);
    CHECK(smallMask != None, "shape: small pixmap creation failed");
    GC smallGC = XCreateGC(display, smallMask, 0, NULL);
    CHECK(smallGC, "shape: small GC creation failed");
    XSetForeground(display, smallGC, 0xFFFFFFFF);
    CHECK(XFillRectangle(display, smallMask, smallGC, 0, 0, 4, 4),
          "shape: small mask fill failed");
    XFreeGC(display, smallGC);
    XImage *beforeSmall =
        XGetImage(display, window, 0, 0, W, H, AllPlanes, ZPixmap);
    CHECK(beforeSmall, "shape: small mask baseline XGetImage failed");
    unsigned long beforeOutsideSmall = XGetPixel(beforeSmall, 10, 10);
    XDestroyImage(beforeSmall);
    XShapeCombineMask(display, window, ShapeBounding, 2, 2, smallMask,
                      ShapeSet);
    XImage *smallReadback =
        XGetImage(display, window, 0, 0, W, H, AllPlanes, ZPixmap);
    CHECK(smallReadback, "shape: small mask XGetImage failed");
    unsigned long outsideSmall = XGetPixel(smallReadback, 10, 10);
    XDestroyImage(smallReadback);
    CHECK(outsideSmall != beforeOutsideSmall,
          "shape: smaller bounding mask left stale outside pixel visible");
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "shape: query after small bounding mask failed");
    CHECK(bShaped && xbs == 2 && ybs == 2 && wbs == 4 && hbs == 4,
          "shape: small bounding extents ignored mask offset");
    XShapeCombineMask(display, window, ShapeBounding, 0, 0, mask, ShapeSet);

    /* Capture the post-install state and compare later readbacks against this
     * stored baseline byte-for-byte instead of guessing a channel layout.
     */
    XImage *baseline =
        XGetImage(display, window, 0, 0, W, H, AllPlanes, ZPixmap);
    CHECK(baseline, "shape: baseline XGetImage failed");
    unsigned long holeBaseline = XGetPixel(baseline, HOLE_X + 1, HOLE_Y + 1);
    unsigned long outsideBaseline = XGetPixel(baseline, 1, 1);
    XDestroyImage(baseline);

    /* Now paint red over the whole window. The shape mask should keep the HOLE
     * pixels at their preserved baseline and turn everything else red.
     */
    XSetForeground(display, gc, red);
    CHECK(XFillRectangle(display, window, gc, 0, 0, W, H),
          "shape: post-mask fill failed");

    XImage *readback =
        XGetImage(display, window, 0, 0, W, H, AllPlanes, ZPixmap);
    CHECK(readback, "shape: XGetImage failed");
    unsigned long inside = XGetPixel(readback, HOLE_X + 1, HOLE_Y + 1);
    unsigned long outside = XGetPixel(readback, 1, 1);
    CHECK(inside == holeBaseline,
          "shape: inside-hole pixel lost its preserved baseline");
    CHECK(outside != outsideBaseline,
          "shape: outside-hole pixel did not receive the red fill");
    XDestroyImage(readback);

    /* Remove the mask and verify that a fresh fill now reaches the hole. */
    XShapeCombineMask(display, window, ShapeBounding, 0, 0, None, ShapeSet);
    XSetForeground(display, gc, red);
    CHECK(XFillRectangle(display, window, gc, 0, 0, W, H),
          "shape: post-uninstall fill failed");
    XImage *readback2 =
        XGetImage(display, window, 0, 0, W, H, AllPlanes, ZPixmap);
    CHECK(readback2, "shape: second XGetImage failed");
    unsigned long after = XGetPixel(readback2, HOLE_X + 1, HOLE_Y + 1);
    CHECK(after != holeBaseline,
          "shape: hole still preserved after mask removed");
    XDestroyImage(readback2);

    XFreePixmap(display, smallMask);
    XFreePixmap(display, sparseMask);
    XFreePixmap(display, mask);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    return 1;
}

/* Bounding and clip masks must compose by intersection: a pixel is preserved
 * when EITHER mask excludes it. Install two disjoint holes, one via
 * ShapeBounding and one via ShapeClip, and verify that pixels in either hole
 * survive the post-mask fill while pixels admitted by both masks are
 * overwritten.
 */
static int test_shape_mask_intersection(Display *display)
{
    enum { W = 20, H = 20 };
    /* The bounding hole sits in the top-left quadrant, the clip hole in the
     * bottom-right; they don't overlap so each verifies one mask independently.
     */
    enum { B_X = 4, B_Y = 4, B_W = 4, B_H = 4 };
    enum { C_X = 12, C_Y = 12, C_W = 4, C_H = 4 };

    Window root = DefaultRootWindow(display);
    int screen = DefaultScreen(display);
    int depth = DefaultDepth(display, screen);

    Window window = XCreateSimpleWindow(display, root, 0, 0, W, H, 0, 0, 0);
    CHECK(window != None, "intersect: window creation failed");
    CHECK(XMapWindow(display, window), "intersect: window map failed");
    GC gc = XCreateGC(display, window, 0, NULL);
    CHECK(gc, "intersect: GC creation failed");

    Pixmap boundingMask = XCreatePixmap(display, window, W, H, depth);
    CHECK(boundingMask != None, "intersect: bounding pixmap failed");
    Pixmap clipMask = XCreatePixmap(display, window, W, H, depth);
    CHECK(clipMask != None, "intersect: clip pixmap failed");
    GC maskGC = XCreateGC(display, boundingMask, 0, NULL);
    CHECK(maskGC, "intersect: mask GC failed");
    XSetForeground(display, maskGC, 0xFFFFFFFF);
    CHECK(XFillRectangle(display, boundingMask, maskGC, 0, 0, W, H),
          "intersect: bounding white failed");
    CHECK(XFillRectangle(display, clipMask, maskGC, 0, 0, W, H),
          "intersect: clip white failed");
    XSetForeground(display, maskGC, 0xFF000000);
    CHECK(XFillRectangle(display, boundingMask, maskGC, B_X, B_Y, B_W, B_H),
          "intersect: bounding hole failed");
    CHECK(XFillRectangle(display, clipMask, maskGC, C_X, C_Y, C_W, C_H),
          "intersect: clip hole failed");
    XFreeGC(display, maskGC);

    XSetForeground(display, gc, 0xFF0000FF);
    CHECK(XFillRectangle(display, window, gc, 0, 0, W, H),
          "intersect: baseline fill failed");

    XShapeCombineMask(display, window, ShapeBounding, 0, 0, boundingMask,
                      ShapeSet);
    XShapeCombineMask(display, window, ShapeClip, 0, 0, clipMask, ShapeSet);

    XImage *baseline =
        XGetImage(display, window, 0, 0, W, H, AllPlanes, ZPixmap);
    CHECK(baseline, "intersect: baseline XGetImage failed");
    unsigned long bHoleBaseline = XGetPixel(baseline, B_X + 1, B_Y + 1);
    unsigned long cHoleBaseline = XGetPixel(baseline, C_X + 1, C_Y + 1);
    unsigned long openBaseline = XGetPixel(baseline, W / 2, 1);
    XDestroyImage(baseline);

    XSetForeground(display, gc, 0xFFFF0000);
    CHECK(XFillRectangle(display, window, gc, 0, 0, W, H),
          "intersect: post-mask fill failed");

    XImage *readback =
        XGetImage(display, window, 0, 0, W, H, AllPlanes, ZPixmap);
    CHECK(readback, "intersect: XGetImage failed");
    unsigned long bHole = XGetPixel(readback, B_X + 1, B_Y + 1);
    unsigned long cHole = XGetPixel(readback, C_X + 1, C_Y + 1);
    unsigned long openPixel = XGetPixel(readback, W / 2, 1);
    CHECK(bHole == bHoleBaseline,
          "intersect: bounding-only hole was overwritten");
    CHECK(cHole == cHoleBaseline, "intersect: clip-only hole was overwritten");
    CHECK(openPixel != openBaseline,
          "intersect: pixel admitted by both masks was not drawn");
    XDestroyImage(readback);

    XFreePixmap(display, boundingMask);
    XFreePixmap(display, clipMask);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    return 1;
}

static int test_shape_combine_ops(Display *display)
{
    enum { W = 16, H = 10, MW = 8, MH = 6 };
    Window root = DefaultRootWindow(display);
    int screen = DefaultScreen(display);
    int depth = DefaultDepth(display, screen);
    Window window = XCreateSimpleWindow(display, root, 0, 0, W, H, 0, 0, 0);
    CHECK(window != None, "combine: window creation failed");

    Pixmap base = XCreatePixmap(display, window, MW, MH, depth);
    Pixmap src = XCreatePixmap(display, window, MW, MH, depth);
    CHECK(base != None && src != None, "combine: pixmap creation failed");
    GC gc = XCreateGC(display, base, 0, NULL);
    CHECK(gc, "combine: GC creation failed");
    XSetForeground(display, gc, 0xFFFFFFFF);
    CHECK(XFillRectangle(display, base, gc, 0, 0, MW, MH),
          "combine: base fill failed");
    CHECK(XFillRectangle(display, src, gc, 0, 0, MW, MH),
          "combine: source fill failed");

    Bool bShaped = False;
    Bool cShaped = False;
    int xbs = 0, ybs = 0, xcs = 0, ycs = 0;
    unsigned int wbs = 0, hbs = 0, wcs = 0, hcs = 0;

    XShapeCombineMask(display, window, ShapeBounding, 0, 0, base, ShapeSet);
    XShapeCombineMask(display, window, ShapeBounding, 4, 0, src, ShapeUnion);
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "combine: union query failed");
    CHECK(bShaped && xbs == 0 && ybs == 0 && wbs == 12 && hbs == 6,
          "combine: ShapeUnion did not expand extents");

    XShapeCombineMask(display, window, ShapeBounding, 0, 0, base, ShapeSet);
    XShapeCombineMask(display, window, ShapeBounding, 4, 0, src,
                      ShapeIntersect);
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "combine: intersect query failed");
    CHECK(bShaped && xbs == 4 && ybs == 0 && wbs == 4 && hbs == 6,
          "combine: ShapeIntersect did not shrink extents");

    XShapeCombineMask(display, window, ShapeBounding, 0, 0, base, ShapeSet);
    XShapeCombineMask(display, window, ShapeBounding, 4, 0, src, ShapeSubtract);
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "combine: subtract query failed");
    CHECK(bShaped && xbs == 0 && ybs == 0 && wbs == 4 && hbs == 6,
          "combine: ShapeSubtract did not remove source region");

    XShapeCombineMask(display, window, ShapeBounding, 0, 0, base, ShapeSet);
    XShapeCombineMask(display, window, ShapeBounding, 4, 0, src, ShapeInvert);
    CHECK(XShapeQueryExtents(display, window, &bShaped, &xbs, &ybs, &wbs, &hbs,
                             &cShaped, &xcs, &ycs, &wcs, &hcs),
          "combine: invert query failed");
    CHECK(bShaped && xbs == 8 && ybs == 0 && wbs == 4 && hbs == 6,
          "combine: ShapeInvert did not compute source-minus-dest");

    XFreeGC(display, gc);
    XFreePixmap(display, src);
    XFreePixmap(display, base);
    XDestroyWindow(display, window);
    return 1;
}

/* Sibling-occlusion clipping regression. Two child windows overlap on the same
 * parent; we raise one above the other, then fill the lower window's full area
 * and assert that pixels mapping to the upper sibling's frame were NOT touched.
 * Without the cached visibleRegion clip, the lower window's draw would "punch
 * through" the upper sibling on the shared backing texture; with it, those
 * pixels stay at their pre-draw value.
 */
static int test_sibling_occlusion_clip(Display *display)
{
    enum { W = 64, H = 48 };
    enum { LOWER_X = 4, LOWER_Y = 4, LOWER_W = 32, LOWER_H = 32 };
    enum { UPPER_X = 20, UPPER_Y = 12, UPPER_W = 24, UPPER_H = 24 };
    Window root = RootWindow(display, DefaultScreen(display));
    Window parent = XCreateSimpleWindow(display, root, 0, 0, W, H, 0, 0, 0);
    CHECK(parent != None, "sibling: parent creation failed");
    Window lower = XCreateSimpleWindow(display, parent, LOWER_X, LOWER_Y,
                                       LOWER_W, LOWER_H, 0, 0, 0);
    Window upper = XCreateSimpleWindow(display, parent, UPPER_X, UPPER_Y,
                                       UPPER_W, UPPER_H, 0, 0, 0);
    CHECK(lower != None && upper != None, "sibling: children creation failed");
    CHECK(XMapWindow(display, parent), "sibling: parent map failed");
    CHECK(XMapWindow(display, lower), "sibling: lower map failed");
    CHECK(XMapWindow(display, upper), "sibling: upper map failed");

    GC gc = XCreateGC(display, parent, 0, NULL);
    CHECK(gc != NULL, "sibling: GC creation failed");

    unsigned long warmColor = 0xFF778899;
    CHECK(XSetForeground(display, gc, warmColor),
          "sibling: warm foreground setup failed");
    CHECK(XFillRectangle(display, lower, gc, 0, 0, LOWER_W, LOWER_H),
          "sibling: warm fill failed");

    CHECK(XRaiseWindow(display, upper),
          "sibling: raise upper above lower failed");

    unsigned long baselineColor = 0xFF445566;
    unsigned long lowerColor = 0xFF112233;
    CHECK(XSetForeground(display, gc, baselineColor),
          "sibling: baseline foreground setup failed");
    /* Pre-fill the entire lower window to give every pixel inside it a stable
     * baseline. With sibling occlusion clipping, the pixels that fall under
     * "upper" do not receive the baseline either, so the test captures the
     * actual pre-test pixel at the probe coordinate and uses it as the
     * "untouched" reference for both assertions below.
     */
    CHECK(XFillRectangle(display, lower, gc, 0, 0, LOWER_W, LOWER_H),
          "sibling: baseline fill failed");

    /* The probe point sits in the overlap: lower-local (LOWER overlaps UPPER
     * starting at upperX - lowerX, upperY - lowerY). Pick a pixel inside the
     * overlap and a pixel outside it, both expressed in lower-local coords.
     */
    int probeInsideX = (UPPER_X - LOWER_X) + 4;
    int probeInsideY = (UPPER_Y - LOWER_Y) + 4;
    int probeOutsideX = 2, probeOutsideY = 2;

    XImage *pre =
        XGetImage(display, lower, 0, 0, LOWER_W, LOWER_H, AllPlanes, ZPixmap);
    CHECK(pre != NULL, "sibling: pre XGetImage failed");
    unsigned long preInside = XGetPixel(pre, probeInsideX, probeInsideY);
    unsigned long preOutside = XGetPixel(pre, probeOutsideX, probeOutsideY);
    XDestroyImage(pre);

    CHECK(XSetForeground(display, gc, lowerColor),
          "sibling: lower color setup failed");
    CHECK(XFillRectangle(display, lower, gc, 0, 0, LOWER_W, LOWER_H),
          "sibling: post-baseline fill failed");

    XImage *post =
        XGetImage(display, lower, 0, 0, LOWER_W, LOWER_H, AllPlanes, ZPixmap);
    CHECK(post != NULL, "sibling: post XGetImage failed");
    unsigned long postInside = XGetPixel(post, probeInsideX, probeInsideY);
    unsigned long postOutside = XGetPixel(post, probeOutsideX, probeOutsideY);
    XDestroyImage(post);

    /* The non-occluded outside pixel must have received the new fill; the
     * occluded inside pixel must have kept its pre-fill value.
     */
    CHECK(postOutside != preOutside,
          "sibling: outside-overlap pixel did not receive the fill");
    CHECK(postInside == preInside,
          "sibling: fill punched through the upper sibling");

    XFreeGC(display, gc);
    XDestroyWindow(display, upper);
    XDestroyWindow(display, lower);
    XDestroyWindow(display, parent);
    return 1;
}

static int test_sibling_occlusion_respects_shape(Display *display)
{
    enum { W = 64, H = 48 };
    enum { LOWER_X = 4, LOWER_Y = 4, LOWER_W = 32, LOWER_H = 32 };
    enum { UPPER_X = 20, UPPER_Y = 12, UPPER_W = 24, UPPER_H = 24 };
    Window root = RootWindow(display, DefaultScreen(display));
    int screen = DefaultScreen(display);
    int depth = DefaultDepth(display, screen);
    Window parent = XCreateSimpleWindow(display, root, 0, 0, W, H, 0, 0, 0);
    CHECK(parent != None, "shape sibling: parent creation failed");
    Window lower = XCreateSimpleWindow(display, parent, LOWER_X, LOWER_Y,
                                       LOWER_W, LOWER_H, 0, 0, 0);
    Window upper = XCreateSimpleWindow(display, parent, UPPER_X, UPPER_Y,
                                       UPPER_W, UPPER_H, 0, 0, 0);
    CHECK(lower != None && upper != None,
          "shape sibling: children creation failed");

    Pixmap mask = XCreatePixmap(display, upper, UPPER_W, UPPER_H, depth);
    CHECK(mask != None, "shape sibling: mask creation failed");
    GC maskGC = XCreateGC(display, mask, 0, NULL);
    CHECK(maskGC != NULL, "shape sibling: mask GC creation failed");
    XSetForeground(display, maskGC, 0x00000000);
    CHECK(XFillRectangle(display, mask, maskGC, 0, 0, UPPER_W, UPPER_H),
          "shape sibling: mask black fill failed");
    XSetForeground(display, maskGC, 0xFFFFFFFF);
    CHECK(XFillRectangle(display, mask, maskGC, 0, 0, UPPER_W / 2, UPPER_H),
          "shape sibling: mask white fill failed");
    XShapeCombineMask(display, upper, ShapeBounding, 0, 0, mask, ShapeSet);

    CHECK(XMapWindow(display, parent), "shape sibling: parent map failed");
    CHECK(XMapWindow(display, lower), "shape sibling: lower map failed");
    CHECK(XMapWindow(display, upper), "shape sibling: upper map failed");
    CHECK(XRaiseWindow(display, upper),
          "shape sibling: raise upper above lower failed");

    GC gc = XCreateGC(display, parent, 0, NULL);
    CHECK(gc != NULL, "shape sibling: GC creation failed");
    unsigned long baselineColor = 0xFF102030;
    unsigned long lowerColor = 0xFF405060;
    CHECK(XSetForeground(display, gc, baselineColor),
          "shape sibling: baseline foreground setup failed");
    CHECK(XFillRectangle(display, lower, gc, 0, 0, LOWER_W, LOWER_H),
          "shape sibling: baseline fill failed");

    int activeX = UPPER_X - LOWER_X + 2;
    int transparentX = UPPER_X - LOWER_X + UPPER_W / 2 + 2;
    int probeY = UPPER_Y - LOWER_Y + 4;

    XImage *pre =
        XGetImage(display, lower, 0, 0, LOWER_W, LOWER_H, AllPlanes, ZPixmap);
    CHECK(pre != NULL, "shape sibling: pre XGetImage failed");
    unsigned long preActive = XGetPixel(pre, activeX, probeY);
    unsigned long preTransparent = XGetPixel(pre, transparentX, probeY);
    XDestroyImage(pre);

    CHECK(XSetForeground(display, gc, lowerColor),
          "shape sibling: lower color setup failed");
    CHECK(XFillRectangle(display, lower, gc, 0, 0, LOWER_W, LOWER_H),
          "shape sibling: lower fill failed");

    XImage *post =
        XGetImage(display, lower, 0, 0, LOWER_W, LOWER_H, AllPlanes, ZPixmap);
    CHECK(post != NULL, "shape sibling: post XGetImage failed");
    unsigned long postActive = XGetPixel(post, activeX, probeY);
    unsigned long postTransparent = XGetPixel(post, transparentX, probeY);
    XDestroyImage(post);

    CHECK(postActive == preActive,
          "shape sibling: fill punched through active shape");
    CHECK(postTransparent != preTransparent,
          "shape sibling: transparent shape area stayed clipped");

    XShapeCombineMask(display, upper, ShapeBounding, 0, 0, None, ShapeSet);
    XImage *preClear =
        XGetImage(display, lower, 0, 0, LOWER_W, LOWER_H, AllPlanes, ZPixmap);
    CHECK(preClear != NULL, "shape sibling: pre-clear XGetImage failed");
    unsigned long preClearTransparent =
        XGetPixel(preClear, transparentX, probeY);
    XDestroyImage(preClear);

    CHECK(XSetForeground(display, gc, 0xFF708090),
          "shape sibling: cleared lower color setup failed");
    CHECK(XFillRectangle(display, lower, gc, 0, 0, LOWER_W, LOWER_H),
          "shape sibling: cleared lower fill failed");

    XImage *postClear =
        XGetImage(display, lower, 0, 0, LOWER_W, LOWER_H, AllPlanes, ZPixmap);
    CHECK(postClear != NULL, "shape sibling: post-clear XGetImage failed");
    unsigned long postClearTransparent =
        XGetPixel(postClear, transparentX, probeY);
    XDestroyImage(postClear);

    CHECK(postClearTransparent == preClearTransparent,
          "shape sibling: cleared shape did not restore rectangle occlusion");

    XFreeGC(display, gc);
    XFreeGC(display, maskGC);
    XFreePixmap(display, mask);
    XDestroyWindow(display, upper);
    XDestroyWindow(display, lower);
    XDestroyWindow(display, parent);
    return 1;
}

static int test_sibling_occlusion_shape_extends_outside_frame(Display *display)
{
    enum { W = 72, H = 48 };
    enum { LOWER_X = 4, LOWER_Y = 8, LOWER_W = 32, LOWER_H = 24 };
    enum { UPPER_X = 44, UPPER_Y = 8, UPPER_W = 8, UPPER_H = 16 };
    enum { MASK_W = 32, MASK_H = 16, MASK_OFFSET_X = -24 };
    Window root = RootWindow(display, DefaultScreen(display));
    int screen = DefaultScreen(display);
    int depth = DefaultDepth(display, screen);
    Window parent = XCreateSimpleWindow(display, root, 0, 0, W, H, 0, 0, 0);
    CHECK(parent != None, "shape extent: parent creation failed");
    Window lower = XCreateSimpleWindow(display, parent, LOWER_X, LOWER_Y,
                                       LOWER_W, LOWER_H, 0, 0, 0);
    Window upper = XCreateSimpleWindow(display, parent, UPPER_X, UPPER_Y,
                                       UPPER_W, UPPER_H, 0, 0, 0);
    CHECK(lower != None && upper != None,
          "shape extent: children creation failed");

    Pixmap mask = XCreatePixmap(display, upper, MASK_W, MASK_H, depth);
    CHECK(mask != None, "shape extent: mask creation failed");
    GC maskGC = XCreateGC(display, mask, 0, NULL);
    CHECK(maskGC != NULL, "shape extent: mask GC creation failed");
    XSetForeground(display, maskGC, 0xFFFFFFFF);
    CHECK(XFillRectangle(display, mask, maskGC, 0, 0, MASK_W, MASK_H),
          "shape extent: mask fill failed");
    XShapeCombineMask(display, upper, ShapeBounding, MASK_OFFSET_X, 0, mask,
                      ShapeSet);

    CHECK(XMapWindow(display, parent), "shape extent: parent map failed");
    CHECK(XMapWindow(display, lower), "shape extent: lower map failed");
    CHECK(XMapWindow(display, upper), "shape extent: upper map failed");
    CHECK(XRaiseWindow(display, upper),
          "shape extent: raise upper above lower failed");

    GC gc = XCreateGC(display, parent, 0, NULL);
    CHECK(gc != NULL, "shape extent: GC creation failed");
    CHECK(XSetForeground(display, gc, 0xFF203040),
          "shape extent: baseline foreground setup failed");
    CHECK(XFillRectangle(display, lower, gc, 0, 0, LOWER_W, LOWER_H),
          "shape extent: baseline fill failed");

    int occludedX = UPPER_X + MASK_OFFSET_X - LOWER_X + 2;
    int occludedY = UPPER_Y - LOWER_Y + 2;
    int openX = 2, openY = 2;

    XImage *pre =
        XGetImage(display, lower, 0, 0, LOWER_W, LOWER_H, AllPlanes, ZPixmap);
    CHECK(pre != NULL, "shape extent: pre XGetImage failed");
    unsigned long preOccluded = XGetPixel(pre, occludedX, occludedY);
    unsigned long preOpen = XGetPixel(pre, openX, openY);
    XDestroyImage(pre);

    CHECK(XSetForeground(display, gc, 0xFF506070),
          "shape extent: lower color setup failed");
    CHECK(XFillRectangle(display, lower, gc, 0, 0, LOWER_W, LOWER_H),
          "shape extent: lower fill failed");

    XImage *post =
        XGetImage(display, lower, 0, 0, LOWER_W, LOWER_H, AllPlanes, ZPixmap);
    CHECK(post != NULL, "shape extent: post XGetImage failed");
    unsigned long postOccluded = XGetPixel(post, occludedX, occludedY);
    unsigned long postOpen = XGetPixel(post, openX, openY);
    XDestroyImage(post);

    CHECK(postOccluded == preOccluded,
          "shape extent: extended shape was culled before clipping");
    CHECK(postOpen != preOpen,
          "shape extent: non-occluded lower pixel did not receive fill");

    XFreeGC(display, gc);
    XFreeGC(display, maskGC);
    XFreePixmap(display, mask);
    XDestroyWindow(display, upper);
    XDestroyWindow(display, lower);
    XDestroyWindow(display, parent);
    return 1;
}

static int test_timeline_counters(Display *display)
{
    /* Counters always increment regardless of LIBX11_COMPAT_TIMELINE so Phase
     * 3's wait-converge works in an untapped build.
     */
    uint64_t baseExpose = timelineCounter(TIMELINE_KIND_EXPOSE);
    uint64_t baseConfigure = timelineCounter(TIMELINE_KIND_CONFIGURE);
    uint64_t baseDestroy = timelineCounter(TIMELINE_KIND_DESTROY);

    Window root = RootWindow(display, DefaultScreen(display));
    Window window =
        XCreateSimpleWindow(display, root, 0, 0, 64, 64, 0,
                            BlackPixel(display, 0), WhitePixel(display, 0));
    CHECK(window != None, "XCreateSimpleWindow returned None");
    XSelectInput(display, window, ExposureMask | StructureNotifyMask);
    XMapWindow(display, window);

    /* configureWindow taps emit on geometry mutation; resize twice. */
    XResizeWindow(display, window, 96, 96);
    XResizeWindow(display, window, 128, 128);

    XDestroyWindow(display, window);

    CHECK(timelineCounter(TIMELINE_KIND_CONFIGURE) > baseConfigure,
          "configure counter did not advance");
    CHECK(timelineCounter(TIMELINE_KIND_DESTROY) > baseDestroy,
          "destroy counter did not advance");
    /* Don't assert Expose: dummy SDL driver may not always deliver one in the
     * time check_test gives, and the unit-test path runs synchronously without
     * an event pump iteration.
     */
    (void) baseExpose;
    return 1;
}

static int test_timeline_wait_converge_idle(Display *display)
{
    (void) display;

    int rc = timelineWaitConverge(timelineNowMs(), 1, 2, 16, 32, 50);
    CHECK(rc == 1, "idle wait-converge did not settle");
    return 1;
}

static int test_state_snapshot(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));
    Window window =
        XCreateSimpleWindow(display, root, 0, 0, 100, 80, 0,
                            BlackPixel(display, 0), WhitePixel(display, 0));
    CHECK(window != None, "XCreateSimpleWindow returned None");
    Atom wm_class_atom = XInternAtom(display, "WM_CLASS", False);
    XChangeProperty(display, window, wm_class_atom, XA_STRING, 8,
                    PropModeReplace, (const unsigned char *) "TestProbe", 9);
    XMapWindow(display, window);

    /* mkstemp under $TMPDIR (falls back to /tmp) so parallel test runs and
     * symlink-attack vectors on a shared multi-user /tmp do not collide. The
     * temp file gets unlinked after readback.
     */
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir)
        tmpdir = "/tmp";
    char path[256];
    int written =
        snprintf(path, sizeof(path),
                 "%s/libx11-compat-state-snapshot-check.XXXXXX", tmpdir);
    CHECK(written > 0 && (size_t) written < sizeof(path),
          "snapshot path template truncated");
    int fd = mkstemp(path);
    CHECK(fd >= 0, "mkstemp failed");
    /* stateSnapshotRequestAndWait reopens the path with fopen("w") on the main
     * thread; close our fd so the open does not race the kernel handle.
     */
    close(fd);

    int rc = stateSnapshotRequestAndWait(path);
    /* In the unit-test environment the SDL event pump may not drain the
     * SDL_USEREVENT round-trip; treat -4 (timed-out wait) as a soft skip but
     * still verify that the request was at least dispatched without a hard
     * error.
     */
    int hard_error = !(rc == 0 || rc == -4);
    if (rc == 0) {
        FILE *fp = fopen(path, "r");
        if (fp) {
            char header[64];
            size_t r = fread(header, 1, sizeof(header) - 1, fp);
            header[r] = '\0';
            fclose(fp);
            if (!strstr(header, "\"focused_window\"")) {
                fprintf(stderr, "%s:%d: snapshot JSON missing focused_window\n",
                        __FILE__, __LINE__);
                failures++;
                unlink(path);
                XDestroyWindow(display, window);
                return 0;
            }
        } else {
            fprintf(stderr, "%s:%d: snapshot JSON file missing\n", __FILE__,
                    __LINE__);
            failures++;
            unlink(path);
            XDestroyWindow(display, window);
            return 0;
        }
    }
    unlink(path);
    CHECK(!hard_error, "stateSnapshotRequestAndWait hard error");

    XDestroyWindow(display, window);
    return 1;
}

/* A pointer event must route to the top-level SDL reports it for, even when a
 * later-created top-level overlaps the click point in the window model. This
 * guards the GIMP regression where opening an image created a canvas window
 * that overlapped the toolbox in the model, so toolbox menu clicks were
 * misrouted to the canvas and menus stopped posting.
 */
static int test_overlap_pointer_routing(Display *display)
{
    Window root = RootWindow(display, DefaultScreen(display));

    Window lower = XCreateSimpleWindow(display, root, 0, 0, 60, 60, 0, 0, 0);
    CHECK(lower != None, "overlap lower-window creation failed");
    XSelectInput(display, lower, ButtonPressMask | ButtonReleaseMask);
    CHECK(XMapWindow(display, lower), "overlap lower-window map failed");

    /* Created after lower and covering the same point, so it sits above lower
     * in the model and getContainingWindow alone would pick it.
     */
    Window upper = XCreateSimpleWindow(display, root, 0, 0, 120, 120, 0, 0, 0);
    CHECK(upper != None, "overlap upper-window creation failed");
    XSetWindowAttributes upperAttrs;
    memset(&upperAttrs, 0, sizeof(upperAttrs));
    upperAttrs.override_redirect = True;
    CHECK(XChangeWindowAttributes(display, upper, CWOverrideRedirect,
                                  &upperAttrs),
          "overlap upper-window override_redirect setup failed");
    XSelectInput(display, upper, ButtonPressMask | ButtonReleaseMask);
    CHECK(XMapWindow(display, upper), "overlap upper-window map failed");
    XSync(display, False);

    XEvent out;
    while (XPending(display))
        XNextEvent(display, &out);

    SDL_Window *lowerSdl = GET_WINDOW_STRUCT(lower)->sdlWindow;
    CHECK(lowerSdl, "overlap lower-window has no SDL window");

    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_MOUSEBUTTONDOWN;
    ev.button.windowID = SDL_GetWindowID(lowerSdl);
    ev.button.button = SDL_BUTTON_LEFT;
    ev.button.x = 20;
    ev.button.y = 20;
    SDL_PushEvent(&ev);

    CHECK(XCheckTypedEvent(display, ButtonPress, &out),
          "overlap click produced no ButtonPress");
    CHECK(out.xbutton.window == lower,
          "click on SDL-reported window was misrouted to the overlapping "
          "top-level");

    ev.type = SDL_MOUSEBUTTONUP;
    SDL_PushEvent(&ev);
    CHECK(XCheckTypedEvent(display, ButtonRelease, &out),
          "overlap button release produced no ButtonRelease");

    CHECK(XGrabPointer(display, lower, True, ButtonPressMask, GrabModeAsync,
                       GrabModeAsync, None, None, CurrentTime) == GrabSuccess,
          "overlap owner-events pointer grab failed");
    ev.type = SDL_MOUSEBUTTONDOWN;
    SDL_PushEvent(&ev);

    CHECK(XCheckTypedEvent(display, ButtonPress, &out),
          "overlap owner-events grab click produced no ButtonPress");
    CHECK(out.xbutton.window == lower,
          "owner-events grab click on SDL-reported window was misrouted to the "
          "overlapping top-level");
    ev.type = SDL_MOUSEBUTTONUP;
    SDL_PushEvent(&ev);
    CHECK(XCheckTypedEvent(display, ButtonRelease, &out),
          "overlap owner-events grab release produced no ButtonRelease");
    CHECK(XUngrabPointer(display, CurrentTime),
          "overlap owner-events pointer ungrab failed");

    XDestroyWindow(display, upper);
    XDestroyWindow(display, lower);
    return 1;
}

int main(void)
{
    run_test("smoke", test_smoke);
    run_test("timeline_counters", test_timeline_counters);
    run_test("timeline_wait_converge_idle", test_timeline_wait_converge_idle);
    run_test("state_snapshot", test_state_snapshot);
    run_test("atoms", test_atoms);
    run_test("keyboard", test_keyboard);
    run_test("xtest_modifiers", test_xtest_modifiers);
    run_test("gc", test_gc);
    run_test("compat_stubs", test_compat_stubs);
    run_test("colors", test_colors);
    run_test("pixmaps", test_pixmaps);
    run_test("drawables_and_gcs", test_drawables_and_gcs);
    run_test("drawing_coverage", test_drawing_coverage);
    run_test("gxxor_self_inverse", test_gxxor_self_inverse);
    run_test("pixmap_readback_cache", test_pixmap_readback_cache);
    run_test("pixmap_readback_uncached", test_pixmap_readback_uncached);
    run_test("xgetimage_color_roundtrip", test_xgetimage_color_roundtrip);
    run_test("images", test_images);
    run_test("path_accelerator", test_path_accelerator);
    run_test("regions", test_regions);
    run_test("events", test_events);
    run_test("grab_release_on_unviewable", test_grab_release_on_unviewable);
    run_test("windows", test_windows);
    run_test("fonts", test_fonts);
    run_test("contexts", test_contexts);
    run_test("extensions", test_extensions);
    run_test("selection", test_selection);
    run_test("properties", test_properties);
    run_test("text_property_list", test_text_property_list);
    /* ICCCM / EWMH / MWM compliance block. ICCCM covers the WM_* core (hints,
     * transient_for); EWMH covers _NET_WM_* state and root ClientMessage
     * routing; MWM covers Motif-specific _MOTIF_WM_HINTS decoding. Grouped here
     * so the suite output reads as a window-manager-convention pass.
     */
    run_test("icccm_wm_hints", test_icccm_wm_hints);
    run_test("icccm_transient_for_modal", test_icccm_transient_for_modal);
    run_test("ewmh_wm_state_clientmessage", test_ewmh_wm_state_clientmessage);
    run_test("ewmh_wm_state_initial_property",
             test_ewmh_wm_state_initial_property);
    run_test("ewmh_active_window_clientmessage",
             test_ewmh_active_window_clientmessage);
    run_test("ewmh_close_window_clientmessage",
             test_ewmh_close_window_clientmessage);
    run_test("mwm_hints_apply", test_mwm_hints_apply);
    run_test("normal_hints_resizable", test_normal_hints_resizable);
    run_test("xrm", test_xrm);
    run_test("input_methods", test_input_methods);
    run_test("defaults", test_defaults);
    run_test("shape_mask", test_shape_mask);
    run_test("shape_mask_intersection", test_shape_mask_intersection);
    run_test("shape_combine_ops", test_shape_combine_ops);
    run_test("sibling_occlusion_clip", test_sibling_occlusion_clip);
    run_test("sibling_occlusion_respects_shape",
             test_sibling_occlusion_respects_shape);
    run_test("sibling_occlusion_shape_extends_outside_frame",
             test_sibling_occlusion_shape_extends_outside_frame);
    run_test("overlap_pointer_routing", test_overlap_pointer_routing);
    return failures == 0 ? 0 : 1;
}
