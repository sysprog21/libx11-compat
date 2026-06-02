#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XKB.h>
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
#include <SDL2/SDL.h>

#include "drawing.h"
#include "gc.h"
#include "image.h"
#include "path/compose.h"
#include "path/edges.h"
#include "path/path.h"
#include "util.h"

int convertEvent(Display *display,
                 SDL_Event *sdlEvent,
                 XEvent *xEvent,
                 Bool freeInternalEvents);
extern Bool mouseFrozen;

#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
        if (strcmp(entry->d_name, ".") != 0 &&
            strcmp(entry->d_name, "..") != 0) {
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
    CHECK(strcmp(predefinedName, "WM_NAME") == 0,
          "unexpected predefined atom name");
    XFree(predefinedName);

    Atom netWmIcon = XInternAtom(display, "_NET_WM_ICON", True);
    CHECK(netWmIcon != None, "_NET_WM_ICON predefined atom was not found");
    char *netName = XGetAtomName(display, netWmIcon);
    CHECK(netName != NULL, "XGetAtomName failed for _NET_WM_ICON");
    CHECK(strcmp(netName, "_NET_WM_ICON") == 0, "unexpected _NET atom name");
    XFree(netName);

    Atom dynamic = XInternAtom(display, "SDL2X11_DYNAMIC_ATOM", False);
    CHECK(dynamic != None, "dynamic atom intern failed");
    CHECK(dynamic <= UINT32_MAX, "dynamic atom exceeded 32 bits");
    CHECK(XInternAtom(display, "SDL2X11_DYNAMIC_ATOM", True) == dynamic,
          "dynamic atom lookup did not return the existing atom");

    char *dynamicName = XGetAtomName(display, dynamic);
    CHECK(dynamicName != NULL, "XGetAtomName failed for dynamic atom");
    CHECK(strcmp(dynamicName, "SDL2X11_DYNAMIC_ATOM") == 0,
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
    CHECK(strcmp(returnedNames[0], names[0]) == 0,
          "first batch atom name mismatch");
    CHECK(strcmp(returnedNames[1], names[1]) == 0,
          "second batch atom name mismatch");
    XFree(returnedNames[0]);
    XFree(returnedNames[1]);
    return 1;
}

static int test_keyboard(Display *display)
{
    KeyCode aCode = XKeysymToKeycode(display, XK_a);
    CHECK(aCode != 0, "XKeysymToKeycode(XK_a) returned 0");

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

    unsigned int consumedModifiers = ShiftMask;
    KeySym lookupSym = NoSymbol;
    CHECK(XkbLookupKeySym(display, aCode, ShiftMask, &consumedModifiers,
                          &lookupSym),
          "XkbLookupKeySym failed");
    CHECK(lookupSym == XK_A, "XkbLookupKeySym did not return XK_A for Shift+a");
    CHECK(consumedModifiers == ShiftMask,
          "XkbLookupKeySym did not consume ShiftMask");
    CHECK(XkbKeysymToModifiers(display, XK_A) == ShiftMask,
          "XkbKeysymToModifiers did not report ShiftMask for XK_A");
    CHECK(XkbKeysymToModifiers(display, XK_Control_L) == ControlMask,
          "XkbKeysymToModifiers did not report ControlMask for XK_Control_L");
    CHECK(XkbKeysymToModifiers(display, XK_Alt_L) == Mod1Mask,
          "XkbKeysymToModifiers did not report Mod1Mask for XK_Alt_L");
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
    CHECK(modifier_slot_has(modmap, Mod1MapIndex,
                            XKeysymToKeycode(display, XK_Alt_L)),
          "modifier map missing left alt");
    CHECK(modifier_slot_has(modmap, Mod2MapIndex,
                            XKeysymToKeycode(display, XK_Num_Lock)),
          "modifier map missing num lock");
    CHECK(modifier_slot_has(modmap, Mod4MapIndex,
                            XKeysymToKeycode(display, XK_Alt_R)),
          "modifier map missing right alt");
    CHECK(XFreeModifiermap(modmap) == 1, "XFreeModifiermap failed");

    /* XmbLookupString decodes ASCII keysyms to UTF-8 with XLookupBoth. */
    XKeyEvent ev = {0};
    ev.type = KeyPress;
    ev.display = display;
    ev.keycode = (KeyCode) (XK_a & 0xFF);
    char buf[8] = {0};
    KeySym lookedUp = NoSymbol;
    Status lookupStatus = 0;
    int n =
        XmbLookupString(NULL, &ev, buf, sizeof(buf), &lookedUp, &lookupStatus);
    CHECK(n == 1 && buf[0] == 'a', "XmbLookupString did not return 'a'");
    CHECK(lookedUp == XK_a, "XmbLookupString returned wrong keysym");
    CHECK(lookupStatus == XLookupBoth,
          "XmbLookupString status should be XLookupBoth for ASCII");
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

    /* Generation counter: every GC mutator bumps it so a future
     * per-renderer state cache can skip redundant SDL pushes. */
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
    CHECK(strcmp(errorText, "fallback") == 0,
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
    CHECK(exact.red == 0x1212 && exact.green == 0x3434 && exact.blue == 0x5656,
          "XParseColor #rrggbb returned wrong components");
    CHECK((exact.flags & (DoRed | DoGreen | DoBlue)) ==
              (DoRed | DoGreen | DoBlue),
          "XParseColor did not set component flags");

    memset(&exact, 0, sizeof(exact));
    CHECK(XParseColor(display, colormap, "#abc", &exact),
          "XParseColor #rgb failed");
    CHECK(exact.red == 0xaaaa && exact.green == 0xbbbb && exact.blue == 0xcccc,
          "XParseColor #rgb returned wrong components");

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
    CHECK(!XLookupColor(display, colormap, "not-a-real-color", &exact, &screen),
          "XLookupColor accepted invalid color");
    CHECK(!XAllocNamedColor(display, colormap, "not-a-real-color", &screen,
                            &exact),
          "XAllocNamedColor accepted invalid color");
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
                surface->format, &gotRed, &gotGreen, &gotBlue, &gotAlpha);
    return gotRed == red && gotGreen == green && gotBlue == blue &&
           gotAlpha == alpha;
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

    /* XCreateBitmapFromData expects ceil(width/8) * height packed bytes.
     * For a 2x2 bitmap that is two bytes (one per row); supplying a single
     * byte tripped AddressSanitizer's stack-buffer-overflow check. */
    char bits[] = {0x01, 0x00};
    Pixmap bitmap = XCreateBitmapFromData(display, root, bits, 2, 2);
    CHECK(bitmap != None, "XCreateBitmapFromData failed");
    SDL_Renderer *renderer = NULL;
    GET_RENDERER(bitmap, renderer);
    SDL_Surface *surface = getRenderSurface(renderer);
    CHECK(surface != NULL, "getRenderSurface for bitmap failed");
    CHECK(pixel_is_rgb(surface, 0, 0, 255, 255, 255),
          "bitmap set bit did not become white");
    CHECK(pixel_is_rgb(surface, 1, 0, 0, 0, 0),
          "bitmap clear bit did not become black");
    SDL_FreeSurface(surface);

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
    GET_RENDERER(colorPixmap, renderer);
    surface = getRenderSurface(renderer);
    CHECK(surface != NULL, "getRenderSurface for color pixmap failed");
    CHECK(pixel_is_rgb(surface, 0, 0, 0, 0, 0),
          "pixmap clear bit did not use background");
    CHECK(pixel_is_rgb(surface, 1, 0, 255, 0, 0),
          "pixmap set bit did not use foreground");
    SDL_FreeSurface(surface);
    XFreePixmap(display, colorPixmap);

    /* XBM file round-trip: parse a hand-written file, then read back a
     * file produced by XWriteBitmapFile. */
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
 * primitives, non-GXcopy line batches, and dashed small arcs. */
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
    /* Width 10 keeps us under the legacy 16-pixel cutoff. Before the fix,
     * the small-arc fallback always rendered as ArcPieSlice and would have
     * filled the center pixel for both modes. After the fix the path
     * accelerator handles ArcChord regardless of size, so the center pixel
     * stays unfilled for chord but stays filled for pie. */
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
     * triangle but on the center side of the chord, i.e. inside pie and
     * outside chord. Chord arc is at (0,1); pie arc is at (12,1). */
    CHECK(pixel_is_rgb(arcSurface, 7, 5, 0, 0, 0),
          "small ArcChord still filled the triangle (legacy pie fallback?)");
    CHECK(pixel_is_rgb(arcSurface, 19, 5, 255, 0, 0),
          "small ArcPieSlice failed to fill the wedge triangle");
    SDL_FreeSurface(arcSurface);
    XFreeGC(display, arcGc);
    XFreePixmap(display, arcPx);

    /* XDrawPoints / XDrawRectangles were previously WARN_UNIMPLEMENTED
     * stubs in missing.c. Exercise both batched forms and verify the
     * pixels actually land. */
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
    /* XDrawRectangles per X11 spec outlines (w+1)x(h+1): rect {5,8,3,3}
     * has corners (5,8) and (8,11); rect {10,10,4,4} has corners
     * (10,10) and (14,14). The far corner check would fail under the
     * old SDL w-by-h behavior, which only reached (13,13). */
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

    /* Non-GXcopy XDrawSegments and XDrawLines: before the fix, batched
     * line primitives fell through to plain SDL blending and silently
     * dropped the GC function. With the software walker the XOR pattern
     * applies to every span pixel. */
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
     * trivially even if dashes were ignored. */
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
    /* Segment 2 must restart the dash pattern — same gap at the same
     * relative offset confirms the per-segment phase reset. */
    CHECK(pixel_is_rgb(dashSegSurface, 0, 14, 255, 255, 255),
          "dashed XDrawSegments did not reset dash phase");
    CHECK(pixel_is_rgb(dashSegSurface, 3, 14, 0, 0, 0),
          "dashed XDrawSegments reset broke off-dash on segment 2");
    SDL_FreeSurface(dashSegSurface);
    XFreeGC(display, dashSegGc);
    XFreePixmap(display, dashSegPx);

    /* Dashed small arcs: before shouldUsePathArc gated on lineStyle, a
     * sub-16 arc with LineOnOffDash routed to the legacy point spray
     * which ignores the dash list entirely. Now the dashed arc must
     * leave visible gaps. We compare against the same arc drawn solid
     * and require the dashed pixel count to be strictly smaller. */
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

static int test_images(Display *display)
{
    CHECK(XImageByteOrder(display) == ImageByteOrder(display),
          "XImageByteOrder did not match display byte order");

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
    /* Renderer-unification: mapped top-level windows share the SCREEN
     * renderer, so XDestroyWindow no longer triggers a renderer
     * destruction or a corresponding cache invalidation. The load-
     * bearing safety property is that the cached renderer stays valid
     * across the destroy boundary; if a future change reverts to per-
     * window renderers, the invalidate hook must still fire and this
     * assertion must be tightened back to NULL. */
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
    while (XCheckTypedWindowEvent(display, window, Expose, &out)) {
    }
    Pixmap backgroundPixmap = XCreatePixmap(
        display, window, 4, 4, DefaultDepth(display, DefaultScreen(display)));
    CHECK(backgroundPixmap != None, "XCreatePixmap for background failed");
    CHECK(XSetWindowBackgroundPixmap(display, window, backgroundPixmap),
          "XSetWindowBackgroundPixmap failed");
    CHECK(XSetWindowBackgroundPixmap(display, window, (Pixmap) ParentRelative),
          "XSetWindowBackgroundPixmap ParentRelative failed");
    CHECK(XSetWindowBackgroundPixmap(display, window, None),
          "XSetWindowBackgroundPixmap None failed");

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
    CHECK(XPutBackEvent(display, &client) == 0, "XPutBackEvent failed");
    CHECK(XEventsQueued(display, QueuedAlready) > 0,
          "QueuedAlready did not see put-back event");
    CHECK(XPending(display) > 0, "XPending did not see put-back event");
    XNextEvent(display, &out);
    CHECK(out.type == ClientMessage && out.xany.window == window,
          "XPutBackEvent did not place event at the head");
    XNextEvent(display, &out);
    CHECK(out.type == Expose && out.xany.window == window,
          "XPutBackEvent disturbed queued event order");

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
    SDL_zero(textEvent);
    textEvent.type = SDL_TEXTINPUT;
    strcpy(textEvent.text.text, "a");
    SDL_PushEvent(&textEvent);
    CHECK(!XCheckTypedEvent(display, KeyPress, &out),
          "SDL_TEXTINPUT was converted into a duplicate KeyPress");

    /* Wheel up -> ButtonPress Button4 with current modifier state. */
    SDL_Event wheelEvent;
    SDL_zero(wheelEvent);
    wheelEvent.type = SDL_MOUSEWHEEL;
    wheelEvent.wheel.y = 1;
    wheelEvent.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&wheelEvent);
    CHECK(XCheckTypedEvent(display, ButtonPress, &out),
          "SDL_MOUSEWHEEL did not produce ButtonPress");
    CHECK(out.xbutton.button == Button4, "wheel-up did not map to Button4");

    SDL_zero(wheelEvent);
    wheelEvent.type = SDL_MOUSEWHEEL;
    wheelEvent.wheel.y = -1;
    wheelEvent.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    SDL_PushEvent(&wheelEvent);
    CHECK(XCheckTypedEvent(display, ButtonPress, &out),
          "wheel-down did not produce ButtonPress");
    CHECK(out.xbutton.button == Button5, "wheel-down did not map to Button5");

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

    XSelectInput(display, window, EnterWindowMask | LeaveWindowMask);
    SDL_Event crossingEvent;
    SDL_zero(crossingEvent);
    crossingEvent.type = SDL_WINDOWEVENT;
    crossingEvent.window.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    crossingEvent.window.event = SDL_WINDOWEVENT_ENTER;
    CHECK(convertEvent(display, &crossingEvent, &out, True) == 0,
          "SDL window enter did not convert");
    CHECK(out.type == EnterNotify && out.xcrossing.mode == NotifyNormal,
          "SDL window enter crossing fields were incorrect");
    crossingEvent.window.event = SDL_WINDOWEVENT_LEAVE;
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
    CHECK(XAllowEvents(display, AsyncPointer, CurrentTime),
          "XAllowEvents failed");
    CHECK(!mouseFrozen, "XAllowEvents did not release pointer freeze");
    CHECK(XUngrabPointer(display, CurrentTime), "XUngrabPointer failed");
    CHECK(XCheckTypedWindowEvent(display, window, LeaveNotify, &out),
          "XUngrabPointer did not post LeaveNotify");
    CHECK(out.xcrossing.mode == NotifyUngrab,
          "XUngrabPointer LeaveNotify used wrong mode");

    /* CWOverrideRedirect must round-trip through XGetWindowAttributes. */
    XSetWindowAttributes ovAttrs;
    ovAttrs.override_redirect = True;
    XChangeWindowAttributes(display, window, CWOverrideRedirect, &ovAttrs);
    XWindowAttributes ovQuery;
    CHECK(XGetWindowAttributes(display, window, &ovQuery),
          "XGetWindowAttributes after CWOverrideRedirect failed");
    CHECK(ovQuery.override_redirect == True,
          "CWOverrideRedirect did not stick on the window");

    XSelectInput(display, window, StructureNotifyMask);
    SDL_Event resizeEvent;
    SDL_zero(resizeEvent);
    resizeEvent.type = SDL_WINDOWEVENT;
    resizeEvent.window.event = SDL_WINDOWEVENT_RESIZED;
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
    int textureWidth = 0;
    int textureHeight = 0;
    SDL_QueryTexture(GET_WINDOW_STRUCT(window)->sdlTexture, NULL, NULL,
                     &textureWidth, &textureHeight);
    CHECK(textureWidth == 48 && textureHeight == 40,
          "backing texture did not resize to SDL dimensions");
    SDL_Event windowEvent;
    SDL_zero(windowEvent);
    windowEvent.type = SDL_WINDOWEVENT;
    windowEvent.window.windowID =
        SDL_GetWindowID(GET_WINDOW_STRUCT(window)->sdlWindow);
    windowEvent.window.event = SDL_WINDOWEVENT_MOVED;
    windowEvent.window.data1 = 11;
    windowEvent.window.data2 = 12;
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL move did not convert to ConfigureNotify");
    XWindowAttributes movedWindowAttrs;
    CHECK(XGetWindowAttributes(display, window, &movedWindowAttrs),
          "XGetWindowAttributes after SDL move failed");
    CHECK(movedWindowAttrs.x == 11 && movedWindowAttrs.y == 12,
          "SDL move did not update window attributes");

    windowEvent.window.event = SDL_WINDOWEVENT_HIDDEN;
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL hidden did not convert to UnmapNotify");
    CHECK(expect_map_state(display, window, IsUnmapped),
          "SDL hidden did not update map state");
    windowEvent.window.event = SDL_WINDOWEVENT_SHOWN;
    CHECK(convertEvent(display, &windowEvent, &out, True) == 0,
          "SDL shown did not convert to MapNotify");
    CHECK(expect_map_state(display, window, IsViewable),
          "SDL shown did not update map state");
    windowEvent.window.event = SDL_WINDOWEVENT_MINIMIZED;
    CHECK(convertEvent(display, &windowEvent, &out, True) < 0,
          "SDL minimized should be consumed internally");
    CHECK(expect_map_state(display, window, IsUnmapped),
          "SDL minimized did not update map state");
    windowEvent.window.event = SDL_WINDOWEVENT_RESTORED;
    CHECK(convertEvent(display, &windowEvent, &out, True) < 0,
          "SDL restored should be consumed internally");
    CHECK(expect_map_state(display, window, IsViewable),
          "SDL restored did not update map state");
    while (XCheckTypedWindowEvent(display, window, Expose, &out)) {
    }

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

    Window stableChild =
        XCreateSimpleWindow(display, window, 2, 3, 8, 8, 0, 0, 0);
    CHECK(stableChild != None, "stable gravity child creation failed");
    XSelectInput(display, stableChild, StructureNotifyMask);
    CHECK(XMapWindow(display, stableChild), "XMapWindow stable child failed");
    while (XCheckTypedWindowEvent(display, stableChild, GravityNotify, &out)) {
    }
    CHECK(XResizeWindow(display, window, 64, 48),
          "second parent resize failed");
    CHECK(!XCheckTypedWindowEvent(display, stableChild, GravityNotify, &out),
          "NorthWestGravity child got spurious GravityNotify");
    while (XCheckTypedEvent(display, Expose, &out)) {
    }

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

    /* _XSETTINGS_SETTINGS on the root window publishes a binary payload
     * with at least Gtk/FontName so GTK probes have real defaults. */
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

    /* _MOTIF_WM_HINTS: synthesize a default reply when unset; return the
     * stored value verbatim once the client writes one. */
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
    CHECK(argcBack == 3 && strcmp(cmdBack[0], "app") == 0 &&
              strcmp(cmdBack[1], "--flag") == 0 &&
              strcmp(cmdBack[2], "value") == 0,
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
    CHECK(argcBack == 3 && strcmp(cmdBack[0], "app") == 0 &&
              strcmp(cmdBack[1], longArg) == 0 &&
              strcmp(cmdBack[2], "tail") == 0,
          "long WM_COMMAND did not round-trip");
    for (int i = 0; i < argcBack; i++)
        XFree(cmdBack[i]);
    XFree(cmdBack);

    XClassHint classHint = {.res_name = "sample", .res_class = "Sample"};
    CHECK(XSetClassHint(display, window, &classHint), "XSetClassHint failed");
    XClassHint classBack;
    CHECK(XGetClassHint(display, window, &classBack), "XGetClassHint failed");
    CHECK(strcmp(classBack.res_name, "sample") == 0 &&
              strcmp(classBack.res_class, "Sample") == 0,
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
    CHECK(strcmp(classBack.res_name, longName) == 0 &&
              strcmp(classBack.res_class, longClass) == 0,
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

    XDestroyWindow(display, transientFor);
    XDestroyWindow(display, window);
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

    XSelectInput(display, child1, VisibilityChangeMask);
    Window delayedParent =
        XCreateSimpleWindow(display, root, 10, 10, 32, 32, 0, 0, 0);
    Window delayedChild =
        XCreateSimpleWindow(display, delayedParent, 1, 1, 8, 8, 0, 0, 0);
    CHECK(delayedParent != None && delayedChild != None,
          "delayed map window creation failed");
    CHECK(XMapWindow(display, delayedChild),
          "mapping child of unmapped parent failed");
    CHECK(expect_map_state(display, delayedChild, IsUnviewable),
          "child mapped under unmapped parent should be unviewable");
    CHECK(XMapWindow(display, delayedParent), "mapping delayed parent failed");
    CHECK(expect_map_state(display, delayedChild, IsViewable),
          "map-requested child was not viewable after parent map");
    CHECK(XUnmapWindow(display, delayedParent),
          "unmapping delayed parent failed");
    CHECK(expect_map_state(display, delayedChild, IsUnviewable),
          "unmapping ancestor should leave descendant unviewable");
    CHECK(XMapWindow(display, delayedParent),
          "remapping delayed parent failed");
    CHECK(expect_map_state(display, delayedChild, IsViewable),
          "descendant did not become viewable after ancestor remap");
    XDestroyWindow(display, delayedParent);

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
    XEvent visibility;
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
    XFreeCursor(display, fontCursor);
    return 1;
}

static int test_fonts(Display *display)
{
    char **names = malloc(sizeof(char *) * 2);
    CHECK(names != NULL, "font names allocation failed");
    names[0] = "font-a";
    names[1] = "font-b";

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

        XFontStruct *fixed = XLoadQueryFont(display, "fixed");
        CHECK(fixed != NULL && fixed->fid != None, "fixed alias did not load");
        XFreeFont(display, fixed);

        XFontStruct *nineByThirteen = XLoadQueryFont(display, "9x13");
        CHECK(nineByThirteen != NULL && nineByThirteen->fid != None,
              "9x13 alias did not load");
        CHECK(XTextWidth(nineByThirteen, "A", 1) > 0,
              "XTextWidth ASCII width failed");
        CHECK(XTextWidth(nineByThirteen, "\xc3\xa9", 2) > 0,
              "XTextWidth UTF-8 width failed");
        XChar2b wideA[] = {{0, 'A'}};
        CHECK(XTextWidth16(nineByThirteen, wideA, 1) ==
                  XTextWidth(nineByThirteen, "A", 1),
              "XTextWidth16 ASCII decoding mismatch");
        XFreeFont(display, nineByThirteen);

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
        XGCValues values;
        CHECK(XGetGCValues(display, gc, GCForeground | GCBackground, &values),
              "font GC readback failed");
        CHECK(
            values.foreground == 0xFF112233 && values.background == 0xFF445566,
            "XDrawImageString mutated GC colors");
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
    CHECK(strcmp(XrmQuarkToString(quark), "app.window.title") == 0,
          "XrmQuarkToString returned wrong string");

    XrmQuark quarks[4];
    XrmStringToQuarkList("app.window.title", quarks);
    CHECK(strcmp(XrmQuarkToString(quarks[0]), "app") == 0 &&
              strcmp(XrmQuarkToString(quarks[1]), "window") == 0 &&
              strcmp(XrmQuarkToString(quarks[2]), "title") == 0 &&
              quarks[3] == NULLQUARK,
          "XrmStringToQuarkList returned wrong quarks");

    XrmBinding bindings[4];
    XrmStringToBindingQuarkList("app.window*title", bindings, quarks);
    CHECK(bindings[0] == XrmBindTightly && bindings[1] == XrmBindTightly &&
              bindings[2] == XrmBindLoosely && quarks[3] == NULLQUARK,
          "XrmStringToBindingQuarkList returned wrong bindings");

    XrmStringToBindingQuarkList("app*.title", bindings, quarks);
    CHECK(bindings[0] == XrmBindTightly && bindings[1] == XrmBindLoosely &&
              strcmp(XrmQuarkToString(quarks[1]), "title") == 0 &&
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

    /* Safe stubs for XSync, XShape, MIT-SHM: probing clients should get
     * stable "unsupported" answers with zeroed outputs. */
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
    CHECK(!XShapeQueryExtension(display, &evBase, &errBase),
          "XShapeQueryExtension should report unsupported");
    CHECK(evBase == 0 && errBase == 0,
          "XShapeQueryExtension did not zero outputs");
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
     * regular pixmap), so we report shared_pixmaps=False to keep callers from
     * relying on semantics the implementation does not honor. */
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
    CHECK(!XkbQueryExtension(display, &opcode, &evBase, &errBase, &xkbMajor,
                             &xkbMinor),
          "XkbQueryExtension should report unsupported");
    CHECK(opcode == 0 && evBase == 0 && errBase == 0,
          "XkbQueryExtension did not zero opcode/event/error outputs");
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
     * previous owner. */
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
    CHECK(value.addr != NULL && strcmp((char *) value.addr, "Hello") == 0,
          "XrmGetResource returned wrong value");

    /* Loose binding via '*'. */
    value.addr = NULL;
    CHECK(XrmGetResource(db, "App.window.background", "App.Window.Background",
                         &type, &value),
          "XrmGetResource did not match through loose '*' binding");
    CHECK(strcmp((char *) value.addr, "white") == 0,
          "XrmGetResource through '*' returned wrong value");

    /* Specificity: a later, tighter rule must beat an earlier loose one. */
    XrmDatabase prec = NULL;
    XrmPutStringResource(&prec, "App*background", "white");
    XrmPutStringResource(&prec, "App.window.background", "red");
    XrmValue precVal = {.size = 0, .addr = NULL};
    char *precType = NULL;
    CHECK(XrmGetResource(prec, "App.window.background",
                         "Application.Window.Background", &precType, &precVal),
          "specificity lookup failed");
    CHECK(strcmp((char *) precVal.addr, "red") == 0,
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
    CHECK(argc == 2 && strcmp(argv[0], "demo") == 0 &&
              strcmp(argv[1], "extra") == 0,
          "XrmParseCommand left wrong argv");
    XrmValue parsedValue = {.size = 0, .addr = NULL};
    char *parsedType = NULL;
    CHECK(XrmGetResource(parsed, "MyApp.foreground", "MyApp.Foreground",
                         &parsedType, &parsedValue),
          "XrmParseCommand did not store -fg");
    CHECK(strcmp((char *) parsedValue.addr, "yellow") == 0,
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

    XrmDestroyDatabase(db);
    return 1;
}

static int test_input_methods(Display *display)
{
    XIM im = XOpenIM(display, NULL, NULL, NULL);
    CHECK(im, "XOpenIM failed");
    Window root = RootWindow(display, DefaultScreen(display));
    Window client = XCreateSimpleWindow(display, root, 0, 0, 16, 16, 0, 0, 0);
    Window focus = XCreateSimpleWindow(display, client, 1, 1, 8, 8, 0, 0, 0);
    CHECK(client != None && focus != None, "input method windows failed");

    XRectangle area = {.x = 2, .y = 3, .width = 4, .height = 5};
    XFontSet fontSet = (XFontSet) (uintptr_t) 0x1234;
    XVaNestedList preedit =
        XVaCreateNestedList(0, XNArea, &area, XNFontSet, fontSet, NULL);
    CHECK(preedit, "XVaCreateNestedList for preedit failed");
    XIC ic = XCreateIC(im, XNInputStyle, XIMPreeditArea | XIMStatusNothing,
                       XNClientWindow, client, XNFocusWindow, focus,
                       XNPreeditAttributes, preedit, NULL);
    CHECK(ic, "XCreateIC failed");

    XIMStyle style = 0;
    Window clientBack = None;
    Window focusBack = None;
    XRectangle areaBack = {0, 0, 0, 0};
    XFontSet fontSetBack = NULL;
    XVaNestedList preeditBack = XVaCreateNestedList(
        0, XNArea, &areaBack, XNFontSet, &fontSetBack, NULL);
    CHECK(preeditBack, "XVaCreateNestedList for preedit return failed");
    CHECK(!XGetICValues(ic, XNInputStyle, &style, XNClientWindow, &clientBack,
                        XNFocusWindow, &focusBack, XNPreeditAttributes,
                        preeditBack, NULL),
          "XGetICValues failed");
    CHECK(style == (XIMPreeditArea | XIMStatusNothing),
          "XGetICValues returned wrong style");
    CHECK(clientBack == client && focusBack == focus,
          "XGetICValues returned wrong client/focus windows");
    CHECK(areaBack.x == area.x && areaBack.y == area.y &&
              areaBack.width == area.width && areaBack.height == area.height,
          "XGetICValues returned wrong preedit area");
    CHECK(fontSetBack == fontSet, "XGetICValues returned wrong font set");

    XFree(preedit);
    XFree(preeditBack);
    XDestroyIC(ic);
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
    CHECK(strcmp(XGetDefault(display, "/usr/bin/demo", "font"), "9x13") == 0,
          "XGetDefault did not read program-specific value");
    CHECK(strcmp(XGetDefault(display, "other", "dpi"), "144") == 0,
          "XGetDefault did not read wildcard value");
    unsetenv("XENVIRONMENT");
    unlink(path);

    CHECK(strcmp(XGetDefault(display, "demo", "font"), "fixed") == 0,
          "XGetDefault built-in font default failed");
    CHECK(strcmp(XGetDefault(display, "demo", "Xft.dpi"), "96") == 0,
          "XGetDefault built-in DPI default failed");
    CHECK(XGetDefault(display, "demo", "unknownOption") == NULL,
          "XGetDefault returned value for unknown option");
    return 1;
}

static int test_wm_hints(Display *display)
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
              strcmp((char *) readText.value, "libx11-compat") == 0,
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

int main(void)
{
    run_test("smoke", test_smoke);
    run_test("atoms", test_atoms);
    run_test("keyboard", test_keyboard);
    run_test("gc", test_gc);
    run_test("compat_stubs", test_compat_stubs);
    run_test("colors", test_colors);
    run_test("pixmaps", test_pixmaps);
    run_test("drawables_and_gcs", test_drawables_and_gcs);
    run_test("drawing_coverage", test_drawing_coverage);
    run_test("images", test_images);
    run_test("path_accelerator", test_path_accelerator);
    run_test("regions", test_regions);
    run_test("events", test_events);
    run_test("windows", test_windows);
    run_test("fonts", test_fonts);
    run_test("contexts", test_contexts);
    run_test("extensions", test_extensions);
    run_test("selection", test_selection);
    run_test("properties", test_properties);
    run_test("xrm", test_xrm);
    run_test("input_methods", test_input_methods);
    run_test("defaults", test_defaults);
    run_test("wm_hints", test_wm_hints);
    return failures == 0 ? 0 : 1;
}
