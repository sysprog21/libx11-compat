#include <math.h>
#include <X11/Xlibint.h>
#include "X11/Xutil.h"
#include "sdl-compat.h"
#include "sdl-ttf-compat.h"
#include "window.h"
#include "errors.h"
#include "events.h"
#include "replay.h"
#include "colors.h"
#include "drawing.h"
#include "display.h"
#include "gc.h"
#include "atoms.h"
#include "visual.h"
#include "font.h"
#include "extension.h"
#include "selection.h"
#include "image.h"
#include "input-method.h"
#include <X11/X.h>
#include <X11/Xutil.h>
#include <limits.h>
#include <stdlib.h>

#ifndef SDL_HINT_VIDEO_X11_XKB
#define SDL_HINT_VIDEO_X11_XKB "SDL_VIDEO_X11_XKB"
#endif

/* Lock hooks installed by libX11's locking.c when XInitThreads runs.
 * libx11-compat holds the function-pointer storage so display open/close can
 * invoke them without dragging in upstream XlibInt.c. The storage is
 * LIBX11_COMPAT_HIDDEN so a system libX11.so.6 loaded alongside libx11-compat
 * cannot reach in and overwrite it; see util.h for the rationale.
 */
#include "locking.h"
LIBX11_COMPAT_HIDDEN int (*_XInitDisplayLock_fn)(Display *dpy) = NULL;
LIBX11_COMPAT_HIDDEN void (*_XFreeDisplayLock_fn)(Display *dpy) = NULL;

#define InitDisplayLock(d) \
    (_XInitDisplayLock_fn ? (*_XInitDisplayLock_fn)(d) : Success)
#define FreeDisplayLock(d)    \
    if (_XFreeDisplayLock_fn) \
    (*_XFreeDisplayLock_fn)(d)

// void __attribute__((constructor)) _init() {
//     setenv("DISPLAY", ":0", 0);
// }

int numDisplaysOpen = 0;
/* Vendor reports the SDL version this library was compiled against. The active
 * video driver can vary by environment at runtime, while this string is stable
 * and useful for compatibility probes.
 */
static char *vendor = "SDL " TO_STRING(SDL_MAJOR_VERSION) "." TO_STRING(
    SDL_MINOR_VERSION) "." TO_STRING(SDL_PATCHLEVEL);
static const int releaseVersion = 1;
static const int supportedDepths[] = {1, 16, 24, 32};
#define COMPAT_LOGICAL_DPI 96.0f

static void applyScreenGeometryOverride(int *width, int *height)
{
    const char *value = getenv("LIBX11_COMPAT_SCREEN_GEOMETRY");
    if (!value || !*value)
        return;

    int parsedWidth = 0;
    int parsedHeight = 0;
    char tail = '\0';
    if (sscanf(value, "%dx%d%c", &parsedWidth, &parsedHeight, &tail) == 2 &&
        parsedWidth > 0 && parsedHeight > 0) {
        *width = parsedWidth;
        *height = parsedHeight;
    }
}

int XCloseDisplay(Display *display)
{
    // https://tronche.com/gui/x/xlib/display/XCloseDisplay.html
    freeExtensionStorage(display);
    freeSelectionStorage(display);
    int screenIndex;

    /* XOpenDisplay can call XCloseDisplay for cleanup after
     * SDL_GetNumVideoDisplays succeeds but the screens calloc fails: nscreens
     * is then >= 0 while screens is NULL. Guard the GC teardown so partial-init
     * paths do not segfault.
     */
    if (display->screens) {
        for (screenIndex = 0; screenIndex < display->nscreens; screenIndex++) {
            Screen *screen = &display->screens[screenIndex];
            if (screen->default_gc) {
                XFreeGC(display, screen->default_gc);
                screen->default_gc = NULL;
            }
        }
    }

    /* Teardown order before SDL_Quit:
     *   releaseLastRequestCode -> replayStop -> destroyScreenWindow
     *   -> closeEventPipe -> SDL_Quit.
     * All four touch SDL primitives (mutexes, timers, the event filter) that
     * must still exist. replayStop precedes closeEventPipe so the worker cannot
     * push XTest events through a queue with no filter behind it.
     * destroyScreenWindow precedes closeEventPipe because destroyWindow
     * recurses into discardQueuedEventsForWindow() and
     * postEvent(DestroyNotify), both of which take per-display event mutexes
     * that closeEventPipe destroys. The numDisplaysOpen guard scopes
     * replay/screen teardown to the last close so secondary displays opened by
     * Motif/Xt probes do not tear down shared state.
     */
    releaseLastRequestCode(display);
    if (numDisplaysOpen == 1) {
        inputMethodReset();
        replayStop();
        freeImageStorage();
        destroyScreenWindow(display);
    }
    closeEventPipe(display);
    if (numDisplaysOpen == 1) {
        freeAtomStorage();
        resetSelectionAtomCache();
        freeFontStorage();
        freeColorStorage();
        freeVisuals();
        releaseMainEventThread();
        freeLastRequestStorage();
        TTF_Quit();
        SDL_Quit();
    }
    if (numDisplaysOpen > 0)
        numDisplaysOpen--;

    if (GET_DISPLAY(display)->screens) {
        for (screenIndex = 0; screenIndex < GET_DISPLAY(display)->nscreens;
             screenIndex++) {
            free(GET_DISPLAY(display)->screens[screenIndex].depths);
            GET_DISPLAY(display)->screens[screenIndex].depths = NULL;
        }
        free(GET_DISPLAY(display)->screens);
    }
    free(display);
    return 0;
}

/* Set only while this library drives SDL_Init(VIDEO) from XOpenDisplay. When a
 * real X server is present, host SDL's x11 video driver opens the display
 * through XOpenDisplay, which this library interposes; without this guard that
 * re-enters XOpenDisplay and recurses into SDL_Init until the stack overflows.
 * This must be thread-local: concurrent client XOpenDisplay calls are valid,
 * only same-thread recursion from SDL's probe should be refused.
 */
static __thread Bool sdlVideoInitInProgress = False;

Display *XOpenDisplay(_Xconst char *display_name)
{
    /* Refuse the re-entrant open from SDL's x11 driver so SDL reports x11
     * unavailable and falls back to a headless video driver instead of
     * recursing. A genuine second client XOpenDisplay never nests inside our
     * own SDL_Init, so this only ever rejects the driver's recursion.
     */
    if (sdlVideoInitInProgress)
        return NULL;

    setenv("DISPLAY", ":0", 0);

    // https://tronche.com/gui/x/xlib/display/opening.html
    /* calloc zeroes every field, which matches the explicit reset of the many
     * _Xprivate Display members the protocol expects to start at NULL/0
     * (lock_meaning == NoSymbol == 0, cursor_font == None == 0, etc.).
     */
    Display *display = calloc(1, sizeof(Display));
    if (!display) {
        fprintf(stderr, "libX11-compat: failed to allocate Display struct\n");
        return NULL;
    }

    /* Track whether the open path owns the SDL/TTF init so the failure paths
     * only tear down what this call brought up. An embedding application that
     * pre-initialized SDL would otherwise lose its video subsystem.
     */
    Bool sdlOwned = False;
    Bool ttfOwned = False;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
#ifndef LIBX11_COMPAT_SDL3
        /* SDL3's SDL_SetMainReady lives in the separate SDL3_main helper, not
         * libSDL3; this library drives SDL_Init itself and does not need it.
         */
        SDL_SetMainReady();
#endif
        SDL_SetHint(SDL_HINT_VIDEO_X11_XKB, "0");
        SDL_SetHint("SDL_IME_SHOW_UI", "1");

        /* On macOS, the click that activates a background window is consumed by
         * activation and never seen by the app. Click-through routes that first
         * click to the app as a real button event, matching X11 semantics. Has
         * no effect on Accessibility/TCC gating of synthetic input (cliclick,
         * CGEvent).
         */
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        sdlVideoInitInProgress = True;
        int sdlInitResult = SDL_Init(SDL_INIT_VIDEO);
        sdlVideoInitInProgress = False;
        if (sdlInitResult == -1) {
            /* Diagnose intermittent XOpenDisplay -> NULL failures observed
             * under load on the remote Xvfb differential test. The Xt error
             * "Can't open display" is generic; without this line the actual SDL
             * backend error is lost when DEBUG_LIBX11_COMPAT is off. Cache
             * getenv() results in locals: passing them through ternaries inline
             * evaluates getenv() twice per arg, and a NULL result the second
             * time around would be undefined behavior in fprintf %s.
             */
            const char *displayEnv = getenv("DISPLAY");
            const char *driverEnv = getenv("SDL_VIDEODRIVER");
            fprintf(stderr,
                    "libX11-compat: SDL_Init(VIDEO) failed: %s "
                    "(DISPLAY=%s SDL_VIDEODRIVER=%s)\n",
                    SDL_GetError(), displayEnv ? displayEnv : "",
                    driverEnv ? driverEnv : "");
            free(display);
            return NULL;
        }
        sdlOwned = True;
    }
    if (!TTF_WasInit()) {
        if (TTF_Init() == -1) {
            fprintf(stderr, "libX11-compat: TTF_Init failed: %s\n",
                    TTF_GetError());
            if (sdlOwned)
                SDL_Quit();
            free(display);
            return NULL;
        }
        ttfOwned = True;
    }
    if (numDisplaysOpen == 0) {
        if (!(initVisuals() && initColorStorage() && initFontStorage())) {
            fprintf(stderr,
                    "libX11-compat: visuals/colors/fonts init failed\n");
            freeFontStorage();
            freeColorStorage();
            freeVisuals();
            if (ttfOwned)
                TTF_Quit();
            if (sdlOwned)
                SDL_Quit();
            free(display);
            return NULL;
        }
    }
    numDisplaysOpen++;

    display->next_event_serial_num = 1;
    int eventFd = initEventPipe(display);
    if (eventFd < 0) {
        fprintf(stderr, "libX11-compat: initEventPipe failed\n");
        display->nscreens = 0;
        XCloseDisplay(display);
        return NULL;
    }
    display->fd = eventFd;
    display->proto_major_version = X_PROTOCOL;
    display->proto_minor_version = X_PROTOCOL_REVISION;
    display->vendor = vendor;
    display->release = releaseVersion;
    display->request = 0;
    display->last_request_read = 0;
    setLastRequestCode(display, X_NoOperation);
    display->min_keycode = 8;
    display->max_keycode = 255;
    display->display_name = (char *) display_name;
    display->byte_order = SDL_BYTEORDER == SDL_BIG_ENDIAN ? MSBFirst : LSBFirst;
    display->bitmap_unit = 32;
    display->bitmap_pad = 32;
    display->bitmap_bit_order = display->byte_order;
    /* Keep the Xlib default screen stable at 0. SDL_GetCurrentVideoDisplay is
     * window-relative and can change after windows move between displays.
     */
    display->default_screen = 0;
    display->nscreens = SDL_GetNumVideoDisplays();
    if (display->nscreens < 0) {
        fprintf(stderr, "libX11-compat: SDL_GetNumVideoDisplays failed: %s\n",
                SDL_GetError());
        XCloseDisplay(display);
        return NULL;
    }
    display->screens = calloc((size_t) display->nscreens, sizeof(Screen));
    if (!display->screens) {
        fprintf(stderr, "libX11-compat: failed to allocate %d screen records\n",
                display->nscreens);
        XCloseDisplay(display);
        return NULL;
    }

    /* Initialize the display lock */
    if (InitDisplayLock(display) != 0) {
        fprintf(stderr, "libX11-compat: InitDisplayLock failed\n");
        XCloseDisplay(display);
        return NULL;
    }

    if (!(display->free_funcs = Xcalloc(1, sizeof(_XFreeFuncRec)))) {
        fprintf(stderr, "libX11-compat: failed to allocate free_funcs\n");
        XCloseDisplay(display);
        return NULL;
    }

    int screenIndex;
    for (screenIndex = 0; screenIndex < display->nscreens; screenIndex++) {
        Screen *screen = &display->screens[screenIndex];
        SDL_DisplayMode displayMode;
        if (SDL_GetDesktopDisplayMode(screenIndex, &displayMode) != 0) {
            fprintf(stderr,
                    "libX11-compat: SDL_GetDesktopDisplayMode(%d) failed: %s\n",
                    screenIndex, SDL_GetError());
            XCloseDisplay(display);
            return NULL;
        }
        screen->display = display;
        applyScreenGeometryOverride(&displayMode.w, &displayMode.h);
        screen->width = displayMode.w;
        screen->height = displayMode.h;
        /* Motif converts resolution-independent dimensions from the screen's
         * reported physical size. Host physical DPI, especially Retina/HiDPI,
         * makes those resources expand by the monitor scale factor while
         * XGetDefault/_XSETTINGS still advertise 96 DPI. Report a stable
         * logical X server size so toolkit layout and font defaults agree.
         */
        screen->mwidth =
            (int) roundf(((float) displayMode.w * 25.4f) / COMPAT_LOGICAL_DPI);
        screen->mheight =
            (int) roundf(((float) displayMode.h * 25.4f) / COMPAT_LOGICAL_DPI);
        screen->root = SCREEN_WINDOW;
        screen->root_visual = getDefaultVisual(screenIndex);
        /* RGB32 visuals have 24 significant bits with the high byte ignored for
         * alpha. Matches the (depth 24, bpp 32) entry from XListPixmapFormats.
         */
        screen->root_depth = 24;
        screen->ndepths = ARRAY_LENGTH(supportedDepths);
        screen->depths = calloc(ARRAY_LENGTH(supportedDepths), sizeof(Depth));
        if (!screen->depths) {
            fprintf(stderr,
                    "libX11-compat: failed to allocate depths for screen %d\n",
                    screenIndex);
            XCloseDisplay(display);
            return NULL;
        }
        for (size_t depthIndex = 0; depthIndex < ARRAY_LENGTH(supportedDepths);
             depthIndex++) {
            screen->depths[depthIndex].depth = supportedDepths[depthIndex];
        }
        /* Default pixels are 24-bit TrueColor values, not full internal ARGB
         * draw colors. Some legacy clients index tables sized by 1 <<
         * DefaultDepth using BlackPixel/WhitePixel.
         */
        screen->white_pixel = 0x00FFFFFF;
        screen->black_pixel = 0x00000000;
        screen->cmap = REAL_COLOR_COLORMAP;
        screen->min_maps = 1;
        screen->max_maps = 1;
        screen->backing_store = NotUseful;
        screen->save_unders = False;
        screen->root_input_mask = NoEventMask;
    }
    if (SCREEN_WINDOW == None) {
        if (initScreenWindow(display) != True) {
            fprintf(stderr, "libX11-compat: initScreenWindow failed: %s\n",
                    SDL_GetError());
            XCloseDisplay(display);
            return NULL;
        }
    }
    for (screenIndex = 0; screenIndex < display->nscreens; screenIndex++) {
        display->screens[screenIndex].root = SCREEN_WINDOW;
        display->screens[screenIndex].default_gc =
            XCreateGC(display, RootWindow(display, 0), 0, 0);
        if (!display->screens[screenIndex].default_gc) {
            fprintf(stderr, "libX11-compat: XCreateGC failed for screen %d\n",
                    screenIndex);
            XCloseDisplay(display);
            return NULL;
        }
        GraphicContext *defaultGc =
            GET_GC(display->screens[screenIndex].default_gc);
        defaultGc->foreground = display->screens[screenIndex].black_pixel;
        defaultGc->background = display->screens[screenIndex].white_pixel;
    }

    if (numDisplaysOpen == 1) {
        /* Init the font search path */
        XSetFontPath(display, NULL, 0);
    }
    return display;
}

int XBell(Display *display, int percent)
{
    // https://tronche.com/gui/x/xlib/input/XBell.html
    SET_X_SERVER_REQUEST(display, X_Bell);
    if (-100 > percent || 100 < percent) {
        handleError(0, display, None, 0, BadValue, 0);
    } else {
        /* Terminal bell only: portable SDL audio/haptic feedback would need
         * device setup and user-visible side effects beyond XBell's hint.
         */
        printf("\a");
        fflush(stdout);
    }
    return 1;
}

int XSync(Display *display, Bool discard)
{
    // https://tronche.com/gui/x/xlib/event-handling/XSync.html
    // WARN_UNIMPLEMENTED;
    drawWindowDataToScreen();
    return 1;
}

/* XConvertSelection / XSetSelectionOwner live in src/selection.c. */

int XNoOp(Display *display)
{
    // https://tronche.com/gui/x/xlib/display/XNoOp.html
    SET_X_SERVER_REQUEST(display, X_NoOperation);
    return 1;
}

int XGrabServer(Display *display)
{
    // https://tronche.com/gui/x/xlib/window-and-session-manager/XGrabServer.html
    SET_X_SERVER_REQUEST(display, X_GrabServer);
    /* No-op: this backend has no separate X server or competing clients to
     * exclude while a client updates global state.
     */
    return 1;
}

int XUngrabServer(Display *display)
{
    // https://tronche.com/gui/x/xlib/window-and-session-manager/XUngrabServer.html
    SET_X_SERVER_REQUEST(display, X_UngrabServer);
    return 1;
}

XHostAddress *XListHosts(Display *display,
                         int *nhosts_return,
                         Bool *state_return)
{
    // https://tronche.com/gui/x/xlib/window-and-session-manager/controlling-host-access/XListHosts.html
    SET_X_SERVER_REQUEST(display, X_ListHosts);
    const static char *LOCAL_HOST = "127.0.0.1";
    *state_return = True;
    XHostAddress *host = malloc(sizeof(XHostAddress));
    if (!host) {
        *nhosts_return = 0;
        return NULL;
    }
    *nhosts_return = 1;
    host->address = (char *) LOCAL_HOST;
    host->length = strlen(LOCAL_HOST);
    host->family = FamilyInternet;
    return host;
}

#define BOOL long
#define SIGNEDINT long
#define UNSIGNEDINT unsigned long
#define RESOURCEID unsigned long

/* this structure may be extended, but do not change the order */
typedef struct {
    UNSIGNEDINT flags;
    BOOL input;             /* need to convert */
    SIGNEDINT initialState; /* need to cvt */
    RESOURCEID iconPixmap;
    RESOURCEID iconWindow;
    SIGNEDINT iconX; /* need to cvt */
    SIGNEDINT iconY; /* need to cvt */
    RESOURCEID iconMask;
    UNSIGNEDINT windowGroup;
} xPropWMHints;
#define NumPropWMHintsElements 9 /* number of elements in this structure */

#undef BOOL
#undef SIGNEDINT
#undef UNSIGNEDINT
#undef RESOURCEID

int XSetWMHints(Display *dpy, Window w, XWMHints *wmhints)
{
    xPropWMHints prop;
    memset(&prop, 0, sizeof(prop));
    prop.flags = wmhints->flags;
    if (wmhints->flags & InputHint)
        prop.input = (wmhints->input == True ? 1 : 0);
    if (wmhints->flags & StateHint)
        prop.initialState = wmhints->initial_state;
    if (wmhints->flags & IconPixmapHint)
        prop.iconPixmap = wmhints->icon_pixmap;
    if (wmhints->flags & IconWindowHint)
        prop.iconWindow = wmhints->icon_window;
    if (wmhints->flags & IconPositionHint) {
        prop.iconX = wmhints->icon_x;
        prop.iconY = wmhints->icon_y;
    }
    if (wmhints->flags & IconMaskHint)
        prop.iconMask = wmhints->icon_mask;
    if (wmhints->flags & WindowGroupHint)
        prop.windowGroup = wmhints->window_group;
    return XChangeProperty(dpy, w, XA_WM_HINTS, XA_WM_HINTS, 32,
                           PropModeReplace, (unsigned char *) &prop,
                           NumPropWMHintsElements);
}

XWMHints *XGetWMHints(Display *dpy, Window w)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *property = NULL;
    long propertyLength = NumPropWMHintsElements * (long) sizeof(long) / 4;
    if (XGetWindowProperty(dpy, w, XA_WM_HINTS, 0, propertyLength, False,
                           XA_WM_HINTS, &actualType, &actualFormat, &nitems,
                           &bytesAfter, &property) != Success ||
        actualType != XA_WM_HINTS || actualFormat != 32 ||
        nitems < NumPropWMHintsElements || !property) {
        if (property)
            XFree(property);
        return NULL;
    }

    xPropWMHints *prop = (xPropWMHints *) property;
    XWMHints *hints = XAllocWMHints();
    if (!hints) {
        XFree(property);
        return NULL;
    }
    hints->flags = prop->flags;
    hints->input = prop->input ? True : False;
    hints->initial_state = prop->initialState;
    hints->icon_pixmap = prop->iconPixmap;
    hints->icon_window = prop->iconWindow;
    hints->icon_x = prop->iconX;
    hints->icon_y = prop->iconY;
    hints->icon_mask = prop->iconMask;
    hints->window_group = prop->windowGroup;
    XFree(property);
    return hints;
}

int XSetCommand(Display *display, Window w, char **argv, int argc)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-session-manager/XSetCommand.html
    if (argc < 0) {
        handleError(0, display, None, 0, BadValue, 0);
        return 0;
    }

    /* XChangeProperty takes the element count as int, and the ICCCM payload
     * size is bounded by the X protocol; cap aggregate WM_COMMAND bytes at
     * INT_MAX. Check bytes first so the subtraction in the second clause cannot
     * wrap when bytes is already at the cap.
     */
    size_t bytes = 0;
    for (int i = 0; i < argc; i++) {
        size_t len = argv && argv[i] ? strlen(argv[i]) : 0;
        if (bytes >= (size_t) INT_MAX || len > (size_t) INT_MAX - 1 - bytes) {
            handleError(0, display, None, 0, BadAlloc, 0);
            return 0;
        }
        bytes += len + 1;
    }
    unsigned char *data = bytes ? malloc(bytes) : NULL;
    if (bytes && !data) {
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    unsigned char *p = data;
    for (int i = 0; i < argc; i++) {
        size_t len = argv && argv[i] ? strlen(argv[i]) : 0;
        if (len)
            memcpy(p, argv[i], len);
        p[len] = '\0';
        p += len + 1;
    }
    int ok = XChangeProperty(display, w, XA_WM_COMMAND, XA_STRING, 8,
                             PropModeReplace, data, (int) bytes);
    free(data);
    return ok;
}

#define BOOL long
#define SIGNEDINT long
#define UNSIGNEDINT unsigned long
#define RESOURCEID unsigned long

/* this structure may be extended, but do not change the order */
typedef struct {
    UNSIGNEDINT flags;
    SIGNEDINT x, y, width, height;    /* need to cvt; only for pre-ICCCM */
    SIGNEDINT minWidth, minHeight;    /* need to cvt */
    SIGNEDINT maxWidth, maxHeight;    /* need to cvt */
    SIGNEDINT widthInc, heightInc;    /* need to cvt */
    SIGNEDINT minAspectX, minAspectY; /* need to cvt */
    SIGNEDINT maxAspectX, maxAspectY; /* need to cvt */
    SIGNEDINT baseWidth, baseHeight;  /* need to cvt; ICCCM version 1 */
    SIGNEDINT winGravity;             /* need to cvt; ICCCM version 1 */
} xPropSizeHints;
#define OldNumPropSizeElements 15 /* pre-ICCCM */
#define NumPropSizeElements 18    /* ICCCM version 1 */

#undef BOOL
#undef SIGNEDINT
#undef UNSIGNEDINT
#undef RESOURCEID

void XSetWMSizeHints(Display *dpy, Window w, XSizeHints *hints, Atom prop)
{
    xPropSizeHints data;

    memset(&data, 0, sizeof(data));
    data.flags = (hints->flags &
                  (USPosition | USSize | PPosition | PSize | PMinSize |
                   PMaxSize | PResizeInc | PAspect | PBaseSize | PWinGravity));

    /* The x, y, width, and height fields are obsolete; but, applications that
     * want to work with old window managers might set them.
     */
    if (hints->flags & (USPosition | PPosition)) {
        data.x = hints->x;
        data.y = hints->y;
    }
    if (hints->flags & (USSize | PSize)) {
        data.width = hints->width;
        data.height = hints->height;
    }

    if (hints->flags & PMinSize) {
        data.minWidth = hints->min_width;
        data.minHeight = hints->min_height;
    }
    if (hints->flags & PMaxSize) {
        data.maxWidth = hints->max_width;
        data.maxHeight = hints->max_height;
    }
    if (hints->flags & PResizeInc) {
        data.widthInc = hints->width_inc;
        data.heightInc = hints->height_inc;
    }
    if (hints->flags & PAspect) {
        data.minAspectX = hints->min_aspect.x;
        data.minAspectY = hints->min_aspect.y;
        data.maxAspectX = hints->max_aspect.x;
        data.maxAspectY = hints->max_aspect.y;
    }
    if (hints->flags & PBaseSize) {
        data.baseWidth = hints->base_width;
        data.baseHeight = hints->base_height;
    }
    if (hints->flags & PWinGravity)
        data.winGravity = hints->win_gravity;

    XChangeProperty(dpy, w, prop, XA_WM_SIZE_HINTS, 32, PropModeReplace,
                    (unsigned char *) &data, NumPropSizeElements);
}


void XSetWMNormalHints(Display *dpy, Window w, XSizeHints *hints)
{
    XSetWMSizeHints(dpy, w, hints, XA_WM_NORMAL_HINTS);
    /* Apply size hints to the SDL window so user resizes are clamped. X11
     * 'border_width' lives inside the window's geometry, so SDL minimum /
     * maximum can use min/max_width/height directly without adding border to
     * either side.
     */
    if (!hints || !IS_MAPPED_TOP_LEVEL_WINDOW(w))
        return;

    SDL_Window *sdlWindow = GET_WINDOW_STRUCT(w)->sdlWindow;
    if (hints->flags & PMinSize) {
        SDL_SetWindowMinimumSize(sdlWindow, hints->min_width,
                                 hints->min_height);
    }
    if (hints->flags & PMaxSize) {
        SDL_SetWindowMaximumSize(sdlWindow, hints->max_width,
                                 hints->max_height);
    }
}

Status XGetWMSizeHints(Display *dpy,
                       Window w,
                       XSizeHints *hints,
                       long *supplied,
                       Atom property)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *propertyData = NULL;
    long propertyLength = NumPropSizeElements * (long) sizeof(long) / 4;
    if (XGetWindowProperty(dpy, w, property, 0, propertyLength, False,
                           XA_WM_SIZE_HINTS, &actualType, &actualFormat,
                           &nitems, &bytesAfter, &propertyData) != Success ||
        actualType != XA_WM_SIZE_HINTS || actualFormat != 32 ||
        nitems < OldNumPropSizeElements || !propertyData) {
        if (propertyData)
            XFree(propertyData);
        return 0;
    }

    xPropSizeHints *prop = (xPropSizeHints *) propertyData;
    memset(hints, 0, sizeof(*hints));
    hints->flags = prop->flags;
    hints->x = prop->x;
    hints->y = prop->y;
    hints->width = prop->width;
    hints->height = prop->height;
    hints->min_width = prop->minWidth;
    hints->min_height = prop->minHeight;
    hints->max_width = prop->maxWidth;
    hints->max_height = prop->maxHeight;
    hints->width_inc = prop->widthInc;
    hints->height_inc = prop->heightInc;
    hints->min_aspect.x = prop->minAspectX;
    hints->min_aspect.y = prop->minAspectY;
    hints->max_aspect.x = prop->maxAspectX;
    hints->max_aspect.y = prop->maxAspectY;
    if (nitems >= NumPropSizeElements) {
        hints->base_width = prop->baseWidth;
        hints->base_height = prop->baseHeight;
        hints->win_gravity = prop->winGravity;
    }
    if (supplied)
        *supplied = hints->flags;
    XFree(propertyData);
    return 1;
}

Status XGetWMNormalHints(Display *dpy,
                         Window w,
                         XSizeHints *hints,
                         long *supplied)
{
    return XGetWMSizeHints(dpy, w, hints, supplied, XA_WM_NORMAL_HINTS);
}

int XSetClassHint(Display *display, Window w, XClassHint *class_hints)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XSetClassHint.html
    if (!class_hints)
        return 0;
    const char *resName = class_hints->res_name ? class_hints->res_name : "";
    const char *resClass = class_hints->res_class ? class_hints->res_class : "";
    size_t nameLen = strlen(resName);
    size_t classLen = strlen(resClass);

    /* XChangeProperty takes int element count; both NUL terminators must fit
     * along with nameLen + classLen, so cap the combined size below INT_MAX.
     * Check nameLen first so the subtraction in the second clause cannot wrap
     * when nameLen is already at the cap.
     */
    if (nameLen > (size_t) INT_MAX - 2 ||
        classLen > (size_t) INT_MAX - 2 - nameLen) {
        handleError(0, display, None, 0, BadAlloc, 0);
        return 0;
    }
    size_t bytes = nameLen + 1 + classLen + 1;
    unsigned char *data = malloc(bytes);
    if (!data) {
        handleOutOfMemory(0, display, 0, 0);
        return 0;
    }
    memcpy(data, resName, nameLen + 1);
    memcpy(data + nameLen + 1, resClass, classLen + 1);
    int ok = XChangeProperty(display, w, XA_WM_CLASS, XA_STRING, 8,
                             PropModeReplace, data, (int) bytes);
    free(data);
    return ok;
}

Status XStringListToTextProperty(char **list,
                                 int count,
                                 XTextProperty *text_prop_return)
{
    // https://tronche.com/gui/x/xlib/ICC/client-to-window-manager/XStringListToTextProperty.html
    /* Per ICCCM: value holds NUL-separated strings, nitems is the byte count
     * excluding the trailing NUL. NULL entries become empty strings.
     */
    text_prop_return->encoding = XA_STRING;
    text_prop_return->format = 8;
    text_prop_return->value = NULL;
    text_prop_return->nitems = 0;

    size_t nbytes = 0;
    for (int i = 0; i < count; i++) {
        size_t len = list[i] ? strlen(list[i]) : 0;
        /* Guard against size_t wrap from a malicious or absurdly large list. */
        if (len + 1 > SIZE_MAX - nbytes)
            return 0;
        nbytes += len + 1;
    }

    if (nbytes == 0) {
        text_prop_return->value = malloc(1);
        if (!text_prop_return->value)
            return 0;
        text_prop_return->value[0] = '\0';
        return 1;
    }

    text_prop_return->value = malloc(nbytes);
    if (!text_prop_return->value)
        return 0;

    char *buf = (char *) text_prop_return->value;
    for (int i = 0; i < count; i++) {
        if (list[i]) {
            size_t n = strlen(list[i]) + 1;
            memcpy(buf, list[i], n);
            buf += n;
        } else {
            *buf++ = '\0';
        }
    }
    text_prop_return->nitems = nbytes - 1;
    return 1;
}

Status XGetTextProperty(Display *display,
                        Window window,
                        XTextProperty *tp,
                        Atom property)
{
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0;
    unsigned long bytesAfter = 0;
    unsigned char *value = NULL;
    if (XGetWindowProperty(display, window, property, 0, 1000000, False,
                           AnyPropertyType, &actualType, &actualFormat, &nitems,
                           &bytesAfter, &value) != Success ||
        actualType == None) {
        if (value)
            XFree(value);
        tp->value = NULL;
        tp->encoding = None;
        tp->format = 0;
        tp->nitems = 0;
        return 0;
    }
    tp->value = value;
    tp->encoding = actualType;
    tp->format = actualFormat;
    tp->nitems = nitems;
    return 1;
}

void XSetWMClientMachine(Display *dpy, Window w, XTextProperty *tp)
{
    XSetTextProperty(dpy, w, tp, XA_WM_CLIENT_MACHINE);
}

#define safestrlen(s) ((s) ? strlen(s) : 0)

int XSetSizeHints(/* old routine */
                  Display *dpy,
                  Window w,
                  XSizeHints *hints,
                  Atom property)
{
    xPropSizeHints prop;
    memset(&prop, 0, sizeof(prop));
    prop.flags = (hints->flags & (USPosition | USSize | PAllHints));
    if (hints->flags & (USPosition | PPosition)) {
        prop.x = hints->x;
        prop.y = hints->y;
    }
    if (hints->flags & (USSize | PSize)) {
        prop.width = hints->width;
        prop.height = hints->height;
    }
    if (hints->flags & PMinSize) {
        prop.minWidth = hints->min_width;
        prop.minHeight = hints->min_height;
    }
    if (hints->flags & PMaxSize) {
        prop.maxWidth = hints->max_width;
        prop.maxHeight = hints->max_height;
    }
    if (hints->flags & PResizeInc) {
        prop.widthInc = hints->width_inc;
        prop.heightInc = hints->height_inc;
    }
    if (hints->flags & PAspect) {
        prop.minAspectX = hints->min_aspect.x;
        prop.minAspectY = hints->min_aspect.y;
        prop.maxAspectX = hints->max_aspect.x;
        prop.maxAspectY = hints->max_aspect.y;
    }
    return XChangeProperty(dpy, w, property, XA_WM_SIZE_HINTS, 32,
                           PropModeReplace, (unsigned char *) &prop,
                           OldNumPropSizeElements);
}

Status XGetSizeHints(Display *dpy, Window w, XSizeHints *hints, Atom property)
{
    long supplied = 0;
    return XGetWMSizeHints(dpy, w, hints, &supplied, property);
}

/* XSetNormalHints sets the property
 * 	WM_NORMAL_HINTS 	type: WM_SIZE_HINTS format: 32
 */

int XSetNormalHints(/* old routine */
                    Display *dpy,
                    Window w,
                    XSizeHints *hints)
{
    return XSetSizeHints(dpy, w, hints, XA_WM_NORMAL_HINTS);
}

Status XGetNormalHints(Display *dpy, Window w, XSizeHints *hints)
{
    long supplied = 0;
    return XGetWMNormalHints(dpy, w, hints, &supplied);
}

/* XSetStandardProperties sets the following properties:
 * 	WM_NAME		  type: STRING		format: 8
 * 	WM_ICON_NAME	  type: STRING		format: 8
 * 	WM_HINTS	  type: WM_HINTS	format: 32
 * 	WM_COMMAND	  type: STRING
 * 	WM_NORMAL_HINTS	  type: WM_SIZE_HINTS 	format: 32
 */

int XSetStandardProperties(
    Display *dpy,
    Window w,                  /* window to decorate */
    _Xconst char *name,        /* name of application */
    _Xconst char *icon_string, /* name string for icon */
    Pixmap icon_pixmap,        /* pixmap to use as icon, or None */
    char **argv,               /* command to be used to restart application */
    int argc,                  /* count of arguments */
    XSizeHints *hints)         /* size hints for window in its normal state */
{
    XWMHints phints;
    phints.flags = 0;

    if (name)
        XStoreName(dpy, w, name);

    if (safestrlen(icon_string) >= USHRT_MAX)
        return 1;
    if (icon_string) {
        XChangeProperty(dpy, w, XA_WM_ICON_NAME, XA_STRING, 8, PropModeReplace,
                        (_Xconst unsigned char *) icon_string,
                        (int) safestrlen(icon_string));
    }

    if (icon_pixmap != None) {
        phints.icon_pixmap = icon_pixmap;
        phints.flags |= IconPixmapHint;
    }
    if (argv)
        XSetCommand(dpy, w, argv, argc);

    if (hints)
        XSetNormalHints(dpy, w, hints);

    if (phints.flags != 0)
        XSetWMHints(dpy, w, &phints);

    return 1;
}
