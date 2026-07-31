#include <limits.h>
#include <math.h>
#include <stdint.h>

#include "window-internal.h"
#include "drawing.h"
#include "events.h"
#include "main-dispatch.h"
#include "display.h"
#include "font.h"
#include "image.h"
#include "input.h"
#include "colors.h"
#include "replay-target.h"
#include "timeline.h"
#include "util.h"

Window SCREEN_WINDOW = None;

/* Optional GLX layer teardown hook. src/glx.c registers a callback here from a
 * constructor when GLX is built (GLX=0 compiles it out); the pointer stays NULL
 * and the call is skipped otherwise. A registered pointer is portable where a
 * weak reference to a possibly-absent symbol is not (Mach-O rejects it).
 */
void (*glxDrawableDestroyedHook)(Window drawable) = NULL;

static void ensureMappingListLock(void);

struct windowOpArgs {
    void (*fn)(Window);
    Window window;
};

static void windowOpOnMain(void *p)
{
    struct windowOpArgs *a = (struct windowOpArgs *) p;
    a->fn(a->window);
}

void runWindowOpOnMain(void (*fn)(Window), Window window)
{
    struct windowOpArgs a = {fn, window};
    runOnMainThread(windowOpOnMain, &a);
}

unsigned long resolvedWindowBackgroundColor(Window window)
{
    unsigned long color = 0;
    Window current = window;
    while (current != None && IS_TYPE(current, WINDOW)) {
        WindowStruct *windowStruct = GET_WINDOW_STRUCT(current);
        color = windowStruct->backgroundColor;
        if (windowStruct->background != (Pixmap) ParentRelative)
            break;

        current = GET_PARENT(current);
    }

    if ((color & (0xFFul << ALPHA_SHIFT)) == 0)
        color |= 0xFFul << ALPHA_SHIFT;
    return color;
}

void initWindowStruct(WindowStruct *windowStruct,
                      int x,
                      int y,
                      unsigned int width,
                      unsigned int height,
                      Visual *visual,
                      Colormap colormap,
                      Bool inputOnly,
                      unsigned long backgroundColor,
                      Pixmap backgroundPixmap)
{
    windowStruct->parent = None;
    initArray(&windowStruct->children, 0);
    windowStruct->x = x;
    windowStruct->y = y;
    windowStruct->w = width;
    windowStruct->h = height;
    windowStruct->hiDpiScaleX = 1.0;
    windowStruct->hiDpiScaleY = 1.0;
    windowStruct->hiDpiPromoted = False;
    windowStruct->hasResizeInc = False;
    windowStruct->widthInc = 0;
    windowStruct->heightInc = 0;
    windowStruct->baseWidth = 0;
    windowStruct->baseHeight = 0;
    windowStruct->minWidth = 0;
    windowStruct->minHeight = 0;
    windowStruct->liveResizeLastSeenW = 0;
    windowStruct->liveResizeLastSeenH = 0;
    windowStruct->liveResizeSettleTicks = 0;
    SDL_AtomicSet(&windowStruct->suppressSdlResizeEcho, 0);
    SDL_AtomicSet(&windowStruct->moveEchoValid, 0);
    windowStruct->moveEchoHostX = 0;
    windowStruct->moveEchoHostY = 0;
    SDL_AtomicSet(&windowStruct->suppressSdlShowMap, 0);
    windowStruct->snapEchoW = 0;
    windowStruct->snapEchoH = 0;
    windowStruct->inputOnly = inputOnly;
    windowStruct->colormap = colormap;
    windowStruct->visual = visual;
    windowStruct->sdlTexture = NULL;
    windowStruct->sdlWindow = NULL;
    windowStruct->glxMetalView = NULL;
    windowStruct->glxCompositeTexture = NULL;
    windowStruct->glxCompositeFlip = NULL;
    windowStruct->glxCompositeW = 0;
    windowStruct->glxCompositeH = 0;
    windowStruct->glxReadbackBuf = NULL;
    windowStruct->glxReadbackCap = 0;
    windowStruct->needsPresent = False;
    windowStruct->hasPresentRect = False;
    windowStruct->presentRect = (SDL_Rect) {0, 0, 0, 0};

    /* Region must be init'd before any union/clear; matches the
     * pixman_region32_init contract for visibleRegion above.
     */
    pixman_region32_init(&windowStruct->dirty);
    windowStruct->fullyDirty = False;
    windowStruct->hasPresented = False;
    windowStruct->contentsMergedToParent = False;
    windowStruct->sdlRenderer = NULL;
    windowStruct->presentRenderer = NULL;
    windowStruct->presentTexture = NULL;
    windowStruct->presentTexW = 0;
    windowStruct->presentTexH = 0;
    windowStruct->presentReadback = NULL;
    windowStruct->presentReadbackCap = 0;
    windowStruct->presentUsesSoftware = False;
    windowStruct->backgroundColor = backgroundColor;
    windowStruct->background = backgroundPixmap;
    windowStruct->colormapWindowsCount = -1;
    windowStruct->colormapWindows = NULL;
    initArray(&windowStruct->properties, 0);
    windowStruct->windowName = NULL;
    windowStruct->icon = NULL;
    windowStruct->cursor = None;
    windowStruct->borderWidth = 0;
    windowStruct->borderPixel = 0;
    windowStruct->borderPixmap = None;
    windowStruct->depth = 0;
    windowStruct->bitGravity = ForgetGravity;
    windowStruct->winGravity = NorthWestGravity;
    windowStruct->mapState = UnMapped;
    windowStruct->eventMask = NoEventMask;
    windowStruct->overrideRedirect = False;
    windowStruct->internal = False;
    windowStruct->shapeBoundingMask = NULL;
    windowStruct->shapeBoundingOffsetX = 0;
    windowStruct->shapeBoundingOffsetY = 0;
    windowStruct->shapeClipMask = NULL;
    windowStruct->shapeClipOffsetX = 0;
    windowStruct->shapeClipOffsetY = 0;
    windowStruct->deferredTransientParent = None;
    windowStruct->deferredTransientApplied = False;
    windowStruct->popupParent = None;
    windowStruct->popupAnchorX = x;
    windowStruct->popupAnchorY = y;

    /* pixman_region32_init is the only safe zero state for a region; a
     * memset/bare zero would crash on the first fini. The valid flag stays
     * False so the next consumer recomputes from the real frame dimensions and
     * parent chain.
     */
    pixman_region32_init(&windowStruct->visibleRegion);
    windowStruct->visibleRegionValid = False;
#ifdef DEBUG_WINDOWS
    windowStruct->debugId = ((unsigned long) rand() << 16) | rand();
#endif /* DEBUG_WINDOWS */
}

void destroyWindowRenderer(SDL_Renderer *renderer)
{
    if (!renderer)
        return;
    invalidatePutImageStagingTexture(renderer);
    invalidateTextCacheForRenderer(renderer);
    invalidateStippleStampsForRenderer(renderer);
    SDL_DestroyRenderer(renderer);
}

/* Screen window handles */

Bool initScreenWindow(Display *display)
{
    ensureMappingListLock();
    if (SCREEN_WINDOW == None) {
        SCREEN_WINDOW = ALLOC_XID();
        if (SCREEN_WINDOW == None) {
            LOG("Out of memory: Failed to allocate SCREEN_WINDOW in "
                "initScreenWindow!\n");
            return False;
        }
        SET_XID_TYPE(SCREEN_WINDOW, WINDOW);
        WindowStruct *window = malloc(sizeof(WindowStruct));
        if (!window) {
            FREE_XID(SCREEN_WINDOW);
            SCREEN_WINDOW = None;
            LOG("Out of memory: Failed to allocate SCREEN_WINDOW in "
                "initScreenWindow!\n");
            return False;
        }
        initWindowStruct(window, 0, 0, GET_DISPLAY(display)->screens[0].width,
                         GET_DISPLAY(display)->screens[0].height, NULL, None,
                         False, 0, None);
        SET_XID_VALUE(SCREEN_WINDOW, window);
        SDL_Surface *sdlSurface = SDL_CreateRGBSurface(
            0, GET_DISPLAY(display)->screens[0].width,
            GET_DISPLAY(display)->screens[0].height, SDL_SURFACE_DEPTH,
            DEFAULT_RED_MASK, DEFAULT_GREEN_MASK, DEFAULT_BLUE_MASK,
            DEFAULT_ALPHA_MASK);
        window->sdlRenderer = SDL_CreateSoftwareRenderer(sdlSurface);
        if (!window->sdlRenderer) {
            LOG("Creating the main renderer failed: %s\n", SDL_GetError());
            SDL_DestroyWindow(window->sdlWindow);
            window->sdlWindow = NULL;
            FREE_XID(SCREEN_WINDOW);
            SCREEN_WINDOW = None;
            return False;
        }
        window->mapState = Mapped;
    }
    return True;
}

static void destroyScreenWindowImpl(Display *display)
{
    requireMainEventThread("destroyScreenWindow");
    if (SCREEN_WINDOW != None) {
        size_t i;
        Window *children = GET_CHILDREN(SCREEN_WINDOW);
        WindowStruct *windowStruct = GET_WINDOW_STRUCT(SCREEN_WINDOW);
        for (i = 0; i < windowStruct->children.length; i++)
            destroyWindow(display, children[i], False);

        /* A grab taken directly on the root survives the child teardown above,
         * so drop it before SCREEN_WINDOW goes away to avoid stale grab state
         * across a display close/reopen.
         */
        releaseActiveGrabsForUnviewableWindow(display, SCREEN_WINDOW);
        destroyWindowRenderer(windowStruct->sdlRenderer);
        windowStruct->sdlRenderer = NULL;
        SDL_DestroyWindow(windowStruct->sdlWindow);
        freeArray(&windowStruct->children);
        pixman_region32_fini(&windowStruct->visibleRegion);
        pixman_region32_fini(&windowStruct->dirty);
        free(windowStruct);
        FREE_XID(SCREEN_WINDOW);
        SCREEN_WINDOW = None;
    }
}

static void destroyScreenWindowOnMain(void *p)
{
    destroyScreenWindowImpl((Display *) p);
}

void destroyScreenWindow(Display *display)
{
    if (!libx11CompatOnMainEventThread()) {
        runOnMainThread(destroyScreenWindowOnMain, display);
        return;
    }
    destroyScreenWindowImpl(display);
}

static WindowSdlIdMapper *mappingListStart = NULL;

/* The mapping list is touched both by client threads (X{Create,Destroy}Window)
 * and by the SDL event-pump thread which translates SDL_WINDOW events back to X
 * Windows. Without a lock the linked list insert/unlink races, and a concurrent
 * malloc/free corrupts the chain.
 */
static SDL_mutex *mappingListLock = NULL;

/* initScreenWindow primes the mutex on the single-threaded XOpenDisplay path
 * before SDL_Window / event-pump threads can reach the register / lookup
 * helpers; subsequent callers are harmless no-ops. If SDL_CreateMutex fails the
 * lock stays NULL and the lock/unlock wrappers below skip the SDL call. That is
 * better than crashing on an unrecoverable allocator fail.
 */
static void ensureMappingListLock(void)
{
    if (!mappingListLock) {
        mappingListLock = SDL_CreateMutex();
        if (!mappingListLock)
            LOG("%s: SDL_CreateMutex failed; mapping list will run "
                "unsynchronized: %s\n",
                __func__, SDL_GetError());
    }
}

static void lockMappingList(void)
{
    if (mappingListLock)
        SDL_LockMutex(mappingListLock);
}

static void unlockMappingList(void)
{
    if (mappingListLock)
        SDL_UnlockMutex(mappingListLock);
}

static WindowSdlIdMapper *findMapperByIdLocked(Uint32 sdlWindowId)
{
    for (WindowSdlIdMapper *mapper = mappingListStart; mapper;
         mapper = mapper->next) {
        if (mapper->sdlWindowId == sdlWindowId)
            return mapper;
    }
    return NULL;
}

void deleteWindowMapping(Window window)
{
    ensureMappingListLock();
    lockMappingList();

    /* Indirect pointer walk: link points at the slot that references the
     * current node, so unlinking the head needs no special case.
     */
    WindowSdlIdMapper **link = &mappingListStart;
    while (*link) {
        if ((*link)->window == window) {
            WindowSdlIdMapper *victim = *link;
            *link = victim->next;
            free(victim);
            break;
        }
        link = &(*link)->next;
    }
    unlockMappingList();
}

void registerWindowMapping(Window window, Uint32 sdlWindowId)
{
    ensureMappingListLock();
    lockMappingList();
    WindowSdlIdMapper *mapper = findMapperByIdLocked(sdlWindowId);
    if (!mapper) {
        mapper = malloc(sizeof(WindowSdlIdMapper));
        if (!mapper) {
            unlockMappingList();
            LOG("Failed to allocate mapping object to map xWindow to SDL "
                "window ID!\n");
            return;
        }
        mapper->next = mappingListStart;
        mappingListStart = mapper;
        mapper->sdlWindowId = sdlWindowId;
    }
    mapper->window = window;
    unlockMappingList();
}

int clampToIntRange(int64_t v)
{
    if (v > INT_MAX)
        return INT_MAX;
    if (v < INT_MIN)
        return INT_MIN;
    return (int) v;
}

/* Accumulate in int64 so a deeply nested tree or a hostile window position
 * cannot wrap before the result is clamped, matching this file's overflow
 * discipline for hostile geometry (see rectDistanceSq).
 */
static void windowAbsoluteOrigin64(Window window,
                                   int64_t *xReturn,
                                   int64_t *yReturn)
{
    int64_t x = 0, y = 0;
    while (window != None && window != SCREEN_WINDOW) {
        int wx = 0, wy = 0;
        GET_WINDOW_POS(window, wx, wy);
        x += wx;
        y += wy;
        window = GET_PARENT(window);
    }
    *xReturn = x;
    *yReturn = y;
}

void windowAbsoluteOrigin(Window window, int *xReturn, int *yReturn)
{
    int64_t x = 0, y = 0;
    windowAbsoluteOrigin64(window, &x, &y);
    *xReturn = clampToIntRange(x);
    *yReturn = clampToIntRange(y);
}

void translateWindowPoint(Window sourceWindow,
                          Window destinationWindow,
                          int sourceX,
                          int sourceY,
                          int *destinationXReturn,
                          int *destinationYReturn)
{
    int64_t sourceAbsX = 0, sourceAbsY = 0;
    int64_t destAbsX = 0, destAbsY = 0;
    windowAbsoluteOrigin64(sourceWindow, &sourceAbsX, &sourceAbsY);
    windowAbsoluteOrigin64(destinationWindow, &destAbsX, &destAbsY);
    *destinationXReturn =
        clampToIntRange(sourceAbsX + (int64_t) sourceX - destAbsX);
    *destinationYReturn =
        clampToIntRange(sourceAbsY + (int64_t) sourceY - destAbsY);
}

/* Decoration offset of the decorated top-level whose logical rectangle contains
 * (logicalX, logicalY). Each native window is placed independently by the host
 * WM, so the gap between where X thinks a window sits and where the host drew
 * its content differs per window (a window near the top of the screen is pushed
 * down further than one lower down). A posted menu shell's logical origin is
 * its cascade button, which lies inside its owning window, so matching by
 * containment applies that specific window's offset, and the nearest window is
 * used for the rare menu whose origin lands just outside every rectangle.
 * Squared distance from a point to a rectangle, 0 when inside. int64 throughout
 * to match this file's overflow discipline for hostile geometry.
 */
static int64_t rectDistanceSq(int px, int py, int rx, int ry, int rw, int rh)
{
    int64_t right = (int64_t) rx + rw, bottom = (int64_t) ry + rh;
    int64_t dx =
        px < rx ? (int64_t) rx - px : (px > right ? (int64_t) px - right : 0);
    int64_t dy =
        py < ry ? (int64_t) ry - py : (py > bottom ? (int64_t) py - bottom : 0);
    return dx * dx + dy * dy;
}

static Bool decoratedWindowOffsetAt(int logicalX,
                                    int logicalY,
                                    int *offsetX,
                                    int *offsetY)
{
    *offsetX = 0;
    *offsetY = 0;
    if (SCREEN_WINDOW == None)
        return False;

    WindowStruct *screenStruct = GET_WINDOW_STRUCT(SCREEN_WINDOW);
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    Window nearest = None;
    int64_t nearestDistSq = 0;

    /* Top-down (children run bottom-to-top) so an overlapping stack resolves to
     * the top-most owner the menu was posted from.
     */
    for (size_t i = screenStruct->children.length; i-- > 0;) {
        Window child = children[i];
        WindowStruct *childStruct = GET_WINDOW_STRUCT(child);
        if (!childStruct->sdlWindow || childStruct->overrideRedirect)
            continue;
        int rx = childStruct->x, ry = childStruct->y;
        int rw = (int) childStruct->w, rh = (int) childStruct->h;
        if ((int64_t) logicalX >= rx &&
            (int64_t) logicalX < (int64_t) rx + rw &&
            (int64_t) logicalY >= ry &&
            (int64_t) logicalY < (int64_t) ry + rh) {
            int hostX = 0, hostY = 0;
            SDL_GetWindowPosition(childStruct->sdlWindow, &hostX, &hostY);
            *offsetX = hostX - rx;
            *offsetY = hostY - ry;
            return True;
        }

        /* No rectangle contains the point when a menu drops below or beside its
         * owner. Track the nearest decorated window instead of falling back to
         * an arbitrary one, so a dropdown just past its owner still picks that
         * owner's offset.
         */
        int64_t distSq = rectDistanceSq(logicalX, logicalY, rx, ry, rw, rh);
        if (nearest == None || distSq < nearestDistSq) {
            nearest = child;
            nearestDistSq = distSq;
        }
    }
    if (nearest != None) {
        WindowStruct *nearestStruct = GET_WINDOW_STRUCT(nearest);
        int hostX = 0, hostY = 0;
        SDL_GetWindowPosition(nearestStruct->sdlWindow, &hostX, &hostY);
        *offsetX = hostX - nearestStruct->x;
        *offsetY = hostY - nearestStruct->y;
        return True;
    }
    return False;
}

static Bool decoratedWindowHostAnchorAt(int logicalX,
                                        int logicalY,
                                        int *rootX,
                                        int *rootY,
                                        int *hostX,
                                        int *hostY)
{
    if (!decoratedWindowOffsetAt(logicalX, logicalY, hostX, hostY))
        return False;

    *rootX = logicalX;
    *rootY = logicalY;
    WindowStruct *screenStruct = GET_WINDOW_STRUCT(SCREEN_WINDOW);
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    Window nearest = None;
    int64_t nearestDistSq = 0;
    for (size_t i = screenStruct->children.length; i-- > 0;) {
        Window child = children[i];
        WindowStruct *childStruct = GET_WINDOW_STRUCT(child);
        if (!childStruct->sdlWindow || childStruct->overrideRedirect)
            continue;
        int rx = childStruct->x, ry = childStruct->y;
        int rw = (int) childStruct->w, rh = (int) childStruct->h;
        if ((int64_t) logicalX >= rx &&
            (int64_t) logicalX < (int64_t) rx + rw &&
            (int64_t) logicalY >= ry &&
            (int64_t) logicalY < (int64_t) ry + rh) {
            *rootX = rx;
            *rootY = ry;
            SDL_GetWindowPosition(childStruct->sdlWindow, hostX, hostY);
            return True;
        }
        int64_t distSq = rectDistanceSq(logicalX, logicalY, rx, ry, rw, rh);
        if (nearest == None || distSq < nearestDistSq) {
            nearest = child;
            nearestDistSq = distSq;
        }
    }
    if (nearest != None) {
        WindowStruct *nearestStruct = GET_WINDOW_STRUCT(nearest);
        *rootX = nearestStruct->x;
        *rootY = nearestStruct->y;
        SDL_GetWindowPosition(nearestStruct->sdlWindow, hostX, hostY);
        return True;
    }
    return False;
}

void topLevelWindowHostPosition(Window window,
                                int logicalX,
                                int logicalY,
                                int *hostX,
                                int *hostY)
{
    *hostX = logicalX;
    *hostY = logicalY;
    if (!IS_TOP_LEVEL(window) || !GET_WINDOW_STRUCT(window)->overrideRedirect)
        return;

    WindowStruct *ws = GET_WINDOW_STRUCT(window);

    /* Only promoted windows carry physical-pixel geometry; a self-scaling
     * toolkit records a >1 backing ratio but keeps X11 geometry logical, so
     * effectiveHiDpiScale collapses it to 1.0 and this transform is skipped.
     */
    double sx = effectiveHiDpiScaleX(ws);
    double sy = effectiveHiDpiScaleY(ws);
    if (sx > 1.0 || sy > 1.0) {
        int rootX = 0, rootY = 0, anchorHostX = 0, anchorHostY = 0;
        if (decoratedWindowHostAnchorAt(logicalX, logicalY, &rootX, &rootY,
                                        &anchorHostX, &anchorHostY)) {
            *hostX = clampToIntRange(
                (int64_t) anchorHostX +
                (int64_t) lround(((double) logicalX - rootX) / sx));
            *hostY = clampToIntRange(
                (int64_t) anchorHostY +
                (int64_t) lround(((double) logicalY - rootY) / sy));
            return;
        }
    }

    int offsetX = 0, offsetY = 0;
    if (decoratedWindowOffsetAt(logicalX, logicalY, &offsetX, &offsetY)) {
        *hostX += offsetX;
        *hostY += offsetY;
    }
}

void topLevelWindowLogicalPosition(Window window,
                                   int hostX,
                                   int hostY,
                                   int *logicalX,
                                   int *logicalY)
{
    *logicalX = hostX;
    *logicalY = hostY;
    if (!IS_TOP_LEVEL(window) || !GET_WINDOW_STRUCT(window)->overrideRedirect)
        return;

    /* Undo the same per-window offset the forward mapping applied. The
     * override- redirect window's stored logical origin identifies its owning
     * decorated window by containment, so the two directions stay consistent
     * even when several native windows sit at different host offsets.
     */
    int wx = 0, wy = 0;
    GET_WINDOW_POS(window, wx, wy);
    WindowStruct *ws = GET_WINDOW_STRUCT(window);
    double sx = effectiveHiDpiScaleX(ws);
    double sy = effectiveHiDpiScaleY(ws);
    if (sx > 1.0 || sy > 1.0) {
        int rootX = 0, rootY = 0, anchorHostX = 0, anchorHostY = 0;
        if (decoratedWindowHostAnchorAt(wx, wy, &rootX, &rootY, &anchorHostX,
                                        &anchorHostY)) {
            *logicalX = clampToIntRange(
                (int64_t) rootX +
                (int64_t) lround(((double) hostX - anchorHostX) * sx));
            *logicalY = clampToIntRange(
                (int64_t) rootY +
                (int64_t) lround(((double) hostY - anchorHostY) * sy));
            return;
        }
    }
    int offsetX = 0, offsetY = 0;
    if (decoratedWindowOffsetAt(wx, wy, &offsetX, &offsetY)) {
        *logicalX -= offsetX;
        *logicalY -= offsetY;
    }
}

/* Collect mapped override-redirect top-levels that own an SDL window. The
 * snapshot compositor uses this to fold popup menus and dialog shells into an
 * in-process capture. Sourcing from the realized-window mapping list rather
 * than SCREEN_WINDOW's children array is deliberate: a freshly posted Motif
 * menu shell is realized and mapped here (with a valid SDL window) before it
 * appears in the root's children array, so a children-only scan misses exactly
 * the popup a snapshot is trying to capture. Fills out[0..max) and returns the
 * count written; entries are in most-recently-realized-first order.
 */
size_t collectMappedOverrideRedirectWindows(Window *out, size_t max)
{
    size_t written = 0;
    ensureMappingListLock();
    lockMappingList();
    for (WindowSdlIdMapper *m = mappingListStart; m && written < max;
         m = m->next) {
        if (!IS_TYPE(m->window, WINDOW))
            continue;

        WindowStruct *ws = GET_WINDOW_STRUCT(m->window);
        if (ws && ws->overrideRedirect && ws->mapState == Mapped &&
            ws->sdlWindow)
            out[written++] = m->window;
    }
    unlockMappingList();
    return written;
}

/* Find the parent surface to anchor an override-redirect popup to, for the SDL3
 * xdg_popup path. A Wayland client cannot self-position a top-level, so a menu
 * must be born as a popup positioned relative to an already-mapped parent. Pick
 * the innermost mapped realized window whose rectangle contains the popup's
 * top-left anchor: for a menu dropping off a menu bar that is the application
 * shell, and for a cascade submenu it is the parent menu popup, a smaller rect
 * that also contains the anchor, so smallest-area-wins yields the correct
 * grab-chain parent.
 *
 * Returns None when nothing contains the anchor, in which case the caller keeps
 * the absolute-position top-level path. The offset is computed in the same X11
 * logical coordinates both windows were positioned in, so their difference
 * recovers exactly the offset the toolkit intended regardless of where the host
 * actually placed the parent. Stacking position of a window among the root's
 * children, higher meaning closer to the top; -1 when it is not a direct root
 * child. Root children run bottom-to-top, so the index is the stacking rank
 * directly.
 */
static ptrdiff_t rootStackIndex(Window window)
{
    if (SCREEN_WINDOW == None)
        return -1;
    WindowStruct *screenStruct = GET_WINDOW_STRUCT(SCREEN_WINDOW);
    if (!screenStruct)
        return -1;
    Window *children = GET_CHILDREN(SCREEN_WINDOW);
    for (size_t i = 0; i < screenStruct->children.length; i++)
        if (children[i] == window)
            return (ptrdiff_t) i;
    return -1;
}

Window findPopupParentToplevel(Window popup, int *offsetX, int *offsetY)
{
    WindowStruct *pop = GET_WINDOW_STRUCT(popup);
    if (!pop)
        return None;
    long long anchorX = pop->x;
    long long anchorY = pop->y;
    Window best = None;
    long long bestArea = 0;
    ptrdiff_t bestStack = -1;
    ensureMappingListLock();
    lockMappingList();
    for (WindowSdlIdMapper *m = mappingListStart; m; m = m->next) {
        if (m->window == popup || !IS_TYPE(m->window, WINDOW))
            continue;
        WindowStruct *ws = GET_WINDOW_STRUCT(m->window);
        if (!ws || !ws->sdlWindow || ws->mapState != Mapped)
            continue;

        /* w/h may be HiDPI-promoted to physical pixels while x/y and the popup
         * anchor stay in logical points, so normalize the extent back to
         * logical before the test. effectiveHiDpiScale is 1.0 for self-scaling
         * toolkits such as Motif, leaving this a no-op there. All math is
         * widened to 64 bits so the extent and area cannot overflow. Adjacent
         * cascade/menu-bar popups commonly anchor exactly on the right or
         * bottom edge, so accept those edges as containing the anchor for
         * parent selection.
         */
        long long left = popupAnchorOriginX(ws);
        long long top = popupAnchorOriginY(ws);
        long long extentW =
            (long long) ((double) ws->w / effectiveHiDpiScaleX(ws) + 0.5);
        long long extentH =
            (long long) ((double) ws->h / effectiveHiDpiScaleY(ws) + 0.5);
        long long right = left + extentW;
        long long bottom = top + extentH;
        if (anchorX < left || anchorY < top || anchorX > right ||
            anchorY > bottom)
            continue;

        /* Skip a degenerate zero-area parent: a mapped window transiently sized
         * to zero in a dimension (Motif does this mid-setup) would otherwise
         * win as the "smallest" containing rect and misanchor the popup onto an
         * invisible artifact.
         */
        if (extentW <= 0 || extentH <= 0)
            continue;

        /* Clamp each extent to int range before multiplying so a hostile
         * dimension cannot overflow the signed area (INT_MAX squared stays
         * below LLONG_MAX), matching this file's overflow discipline. Real
         * windows sit far below the cap, so selection ordering is unchanged.
         */
        if (extentW > INT_MAX)
            extentW = INT_MAX;
        if (extentH > INT_MAX)
            extentH = INT_MAX;
        long long area = extentW * extentH;

        /* Smallest containing rect wins (the menu shell over the app shell, the
         * parent popup over its toplevel). On an exact area tie between two
         * overlapping toplevels, prefer the one higher in the root stacking:
         * that is the window whose menu bar was just activated and raised, so
         * the popup anchors to it instead of jumping to an equal-sized sibling
         * behind it.
         */
        if (best == None || area < bestArea) {
            best = m->window;
            bestArea = area;
            bestStack = rootStackIndex(m->window);
        } else if (area == bestArea) {
            ptrdiff_t stack = rootStackIndex(m->window);
            if (stack > bestStack) {
                best = m->window;
                bestStack = stack;
            }
        }
    }
    if (best != None) {
        WindowStruct *pws = GET_WINDOW_STRUCT(best);

        /* Same parent origin the containment test above and
         * repositionAnchoredPopup use, so the create-time offset cannot drift
         * from the later reposition. Clamp to int range like the rest of this
         * file's geometry math so a hostile anchor cannot wrap the offset.
         */
        if (offsetX)
            *offsetX = clampToIntRange(anchorX - popupAnchorOriginX(pws));
        if (offsetY)
            *offsetY = clampToIntRange(anchorY - popupAnchorOriginY(pws));
    }
    unlockMappingList();
    return best;
}

/* Collect realized override-redirect popups anchored to parent, so the caller
 * can tear them down before it destroys parent's SDL window. On SDL3
 * SDL_DestroyWindow recursively destroys owned popup children, which would
 * leave their WindowStruct pointing at a freed SDL_Window; unrealizing them
 * first through our own path frees their present resources and clears
 * sdlWindow. Fills out[0..max) and returns the count written.
 */
size_t collectPopupChildren(Window parent, Window *out, size_t max)
{
    size_t written = 0;
    ensureMappingListLock();
    lockMappingList();
    for (WindowSdlIdMapper *m = mappingListStart; m && written < max;
         m = m->next) {
        if (!IS_TYPE(m->window, WINDOW))
            continue;
        WindowStruct *ws = GET_WINDOW_STRUCT(m->window);
        if (ws && ws->popupParent == parent && ws->sdlWindow)
            out[written++] = m->window;
    }
    unlockMappingList();
    return written;
}

Window getWindowFromId(Uint32 sdlWindowId)
{
    /* Read the Window value while holding the lock; returning the node pointer
     * would expose a use-after-free window if another thread unlinked and freed
     * the node between unlock and dereference.
     */
    ensureMappingListLock();
    lockMappingList();
    WindowSdlIdMapper *mapper = findMapperByIdLocked(sdlWindowId);
    Window result = mapper ? mapper->window : None;
    unlockMappingList();
    LOG("Got window %lu for id %u\n", result, sdlWindowId);
    return result;
}

static void applyChildResizeGravity(Window child,
                                    int oldParentWidth,
                                    int oldParentHeight,
                                    int newParentWidth,
                                    int newParentHeight,
                                    int *dx,
                                    int *dy)
{
    WindowStruct *childStruct = GET_WINDOW_STRUCT(child);
    int widthDelta = newParentWidth - oldParentWidth;
    int heightDelta = newParentHeight - oldParentHeight;
    *dx = 0;
    *dy = 0;

    switch (childStruct->winGravity) {
    case NorthGravity:
    case CenterGravity:
    case SouthGravity:
        *dx = widthDelta / 2;
        break;
    case NorthEastGravity:
    case EastGravity:
    case SouthEastGravity:
        *dx = widthDelta;
        break;
    default:
        break;
    }

    switch (childStruct->winGravity) {
    case WestGravity:
    case CenterGravity:
    case EastGravity:
        *dy = heightDelta / 2;
        break;
    case SouthWestGravity:
    case SouthGravity:
    case SouthEastGravity:
        *dy = heightDelta;
        break;
    default:
        break;
    }

    childStruct->x += *dx;
    childStruct->y += *dy;
}

/* When a parent is resized, children are repositioned according to their
 * win_gravity. The X protocol reports these implicit moves with GravityNotify
 * only: the child's geometry relative to its parent changed, but it was not
 * reconfigured, so no ConfigureNotify is generated. Descendants deeper than the
 * moved child keep their parent-relative geometry and receive nothing. Contents
 * move with the window, so no Expose is owed here either; callers that wiped
 * the subtree first are responsible for re-exposing it.
 *
 * UnmapGravity children (win_gravity value 0) are unmapped on parent resize;
 * unmapWindowInternal drives the normal UnmapNotify (from_configure=True) and
 * parent-exposure flow without bumping the request counter, and the client is
 * expected to remap.
 */
void postResizeConfigureForMappedChildren(Display *display,
                                          Window window,
                                          int oldWidth,
                                          int oldHeight,
                                          int newWidth,
                                          int newHeight)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    Window *children = GET_CHILDREN(window);
    for (size_t i = 0; i < windowStruct->children.length; i++) {
        Window child = children[i];
        WindowStruct *childStruct = GET_WINDOW_STRUCT(child);
        if (childStruct->mapState != Mapped)
            continue;

        if (childStruct->winGravity == UnmapGravity) {
            unmapWindowInternal(display, child, True);
            continue;
        }

        int dx = 0, dy = 0;
        applyChildResizeGravity(child, oldWidth, oldHeight, newWidth, newHeight,
                                &dx, &dy);
        if (dx == 0 && dy == 0)
            continue;

        postEvent(display, child, GravityNotify);
    }
}

Window getContainingWindow(Window window, int x, int y)
{
    WindowStruct *windowStruct = (window != None && IS_TYPE(window, WINDOW))
                                     ? GET_WINDOW_STRUCT(window)
                                     : NULL;
    if (!windowPointInsideBoundingShape(windowStruct, x, y))
        return None;

    Window child = getDirectChildContainingPoint(window, x, y);
    if (child != None) {
        int childX = 0, childY = 0;
        GET_WINDOW_POS(child, childX, childY);
        return getContainingWindow(child, x - childX, y - childY);
    }
    return window;
}

Bool windowPointInsideBoundingShape(WindowStruct *windowStruct, int x, int y)
{
    if (!windowStruct || !windowStruct->shapeBoundingMask)
        return True;

    return shapeSurfaceContains(windowStruct->shapeBoundingMask,
                                windowStruct->shapeBoundingOffsetX,
                                windowStruct->shapeBoundingOffsetY, x, y);
}

Window getDirectChildContainingPoint(Window window, int x, int y)
{
    if (window == None || !IS_TYPE(window, WINDOW))
        return None;

    Window *children = GET_CHILDREN(window);
    for (size_t i = GET_WINDOW_STRUCT(window)->children.length; i > 0; i--) {
        Window child = children[i - 1];
        if (!isWindowEffectivelyViewable(child))
            continue;
        WindowStruct *childStruct = GET_WINDOW_STRUCT(child);

        int childX = 0, childY = 0;
        int childW = 0, childH = 0;
        GET_WINDOW_POS(child, childX, childY);
        GET_WINDOW_DIMS(child, childW, childH);
        if (x >= childX && x < childX + childW && y >= childY &&
            y < childY + childH &&
            windowPointInsideBoundingShape(childStruct, x - childX,
                                           y - childY)) {
            return child;
        }
    }
    return None;
}

void removeChildFromParent(Window child)
{
    if (child == SCREEN_WINDOW)
        return;

    Window parent = GET_PARENT(child);
    if (parent != None) {
        /* Tear-down may have already invalidated this top-level subtree via the
         * map-state change; calling again is a cheap no-op walk but keeps the
         * contract that any sibling-list edit leaves cached visible regions
         * stale.
         */
        invalidateVisibleRegionForTopLevel(child);
        ssize_t childIndex =
            findInArray(&GET_WINDOW_STRUCT(parent)->children, (void *) child);
        if (childIndex != -1) {
            removeArray(&GET_WINDOW_STRUCT(parent)->children,
                        (size_t) childIndex, True);
        }
    }
}

/* X releases an active pointer or keyboard grab when its grab window becomes
 * unviewable. Mirror that on unmap and destroy: a Motif menu grabs the pointer
 * on its override-redirect shell, and when the shell is unmapped on selection
 * the grab must drop, or every later pointer event routes to the now-invisible
 * shell and the application appears frozen. The grab window is unviewable when
 * the window losing viewability is the grab window itself or one of its
 * ancestors.
 */
void releaseActiveGrabsForUnviewableWindow(Display *display, Window window)
{
    Window pointerGrabWindow = getGrabbedPointerWindow();
    if (pointerGrabWindow != None &&
        (pointerGrabWindow == window || isParent(window, pointerGrabWindow)))
        XUngrabPointer(display, CurrentTime);
    Window keyboardGrabWindow = getGrabbedKeyboardWindow();
    if (keyboardGrabWindow != None &&
        (keyboardGrabWindow == window || isParent(window, keyboardGrabWindow)))
        XUngrabKeyboard(display, CurrentTime);
}

void destroyWindow(Display *display, Window window, Bool freeParentData)
{
    /* Tears down the SDL window/renderer (SDL_DestroyWindow /
     * SDL_DestroyRenderer via unrealizeTopLevelWindow), main-thread-only on
     * macOS. Reached only via the routed XDestroyWindow/XDestroySubwindows
     * entry points and internal main-thread teardown/recursion, so assert the
     * invariant at the SDL source.
     */
    requireMainEventThread("destroyWindow");
    flushTextStampsForWindow(window);

    /* Drain pre-cascade stale events for this window FIRST, before any
     * recursion. A focused descendant whose revert lands on this window would
     * otherwise queue FocusIn(window) during the recursion, and a naive discard
     * at the end of this function would erase it. The order is: drain own stale
     * events -> recurse (children's reverts may queue events for this window,
     * all survive) -> own revert -> teardown -> DestroyNotify.
     */
    timelineTapDestroy(window);
    discardQueuedEventsForWindow(display, window);

    size_t i;
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    Window *children = GET_CHILDREN(window);
    for (i = 0; i < windowStruct->children.length; i++)
        destroyWindow(display, children[i], False);

    /* Unmap any SDL3 xdg_popup anchored to this window before its SDL window is
     * destroyed below. An override-redirect popup is a sibling under the root,
     * not an X child, so the recursion above never reaches it; yet SDL owns it
     * as a popup child and SDL_DestroyWindow would free it here, leaving the
     * popup's WindowStruct with a dangling sdlWindow that later
     * configure/teardown paths would touch. Only a realized window can be a
     * popup parent, so the sdlWindow gate skips the mapping-list walk for the
     * unrealized child widgets that make up the bulk of a destroyed tree.
     */
    if (windowStruct->sdlWindow)
        drainAnchoredPopups(display, window);

    /* Auto-revert per Xlib spec; post-order recursion means each ancestor's
     * check sees the updated focus as the cascade unwinds.
     */
    revertKeyboardFocusForDestroyedWindow(display, window);

    /* Drop any passive button grabs on this window so a later XID reuse cannot
     * route events through a stale grab entry.
     */
    releaseButtonGrabsForWindow(window);

    /* A destroyed window is unviewable, so release any active pointer/keyboard
     * grab it held for the same reason XUnmapWindow does.
     */
    releaseActiveGrabsForUnviewableWindow(display, window);

    /* Clear cached pointer-target XIDs so a queued SDL motion event does not
     * drive postPointerCrossingEvents -> buildWindowPathToRoot into this
     * window's freed WindowStruct. ASan caught this on the test-xtest path
     * where XDestroyWindow ran before the SDL motion queue drained.
     */
    clearPointerStateForWindow(window);

    /* Tear down any GLX/EGL surface bound to this drawable. The GLX layer is
     * optional (GLX=0 compiles src/glx.c out), so this is a registered
     * function-pointer hook: src/glx.c sets it from a constructor when built,
     * and it stays NULL (call skipped) otherwise.
     */
    if (glxDrawableDestroyedHook)
        glxDrawableDestroyedHook(window);

    freeArray(&windowStruct->children);
    XFreeColormap(display, GET_COLORMAP(window));
    for (i = 0; i < windowStruct->properties.length; i++)
        freeWindowProperty(windowStruct->properties.array[i]);
    freeArray(&windowStruct->properties);
    if (windowStruct->windowName)
        free(windowStruct->windowName);
    if (windowStruct->icon)
        SDL_FreeSurface(windowStruct->icon);
    if (windowStruct->shapeBoundingMask)
        SDL_FreeSurface(windowStruct->shapeBoundingMask);
    if (windowStruct->shapeClipMask)
        SDL_FreeSurface(windowStruct->shapeClipMask);
    free(windowStruct->colormapWindows);

    /* Per-window accelerated present resources. The streaming texture belongs
     * to presentRenderer, so destroy it first, then the renderer; both must go
     * before SDL_DestroyWindow below since the renderer is bound to sdlWindow.
     * The SCREEN-owned backing sdlTexture (destroyed further down) is unrelated
     * to presentRenderer and must not be torn down through it.
     */
    if (windowStruct->presentTexture)
        SDL_DestroyTexture(windowStruct->presentTexture);

    /* presentRenderer is destroyed directly, not through destroyWindowRenderer:
     * the put-image, text, and stipple caches only ever build textures on the
     * window sdlRenderer (getWindowRenderer) or the SCREEN renderer, never on
     * presentRenderer, so there is nothing to invalidate here. Route it through
     * destroyWindowRenderer if a cache ever starts keying on presentRenderer.
     */
    if (windowStruct->presentRenderer)
        SDL_DestroyRenderer(windowStruct->presentRenderer);
    free(windowStruct->presentReadback);
    destroyWindowRenderer(windowStruct->sdlRenderer);
    if (windowStruct->sdlTexture)
        SDL_DestroyTexture(windowStruct->sdlTexture);

    /* GLX offscreen-composite scratch (destroyed before the top-level renderer
     * that owns the staging texture, since children tear down before parents).
     */
    if (windowStruct->glxCompositeTexture)
        SDL_DestroyTexture(windowStruct->glxCompositeTexture);
    free(windowStruct->glxCompositeFlip);
    free(windowStruct->glxReadbackBuf);

    if (windowStruct->sdlWindow) {
#ifndef LIBX11_COMPAT_SDL3
        SDL_Rect closedRect = {windowStruct->x, windowStruct->y,
                               clampToIntRange((int64_t) windowStruct->w),
                               clampToIntRange((int64_t) windowStruct->h)};
#endif
#if SDL_VERSION_ATLEAST(2, 0, 5)

        /* Detach any live modal-for binding so the host WM does not keep
         * blocking the parent shell once this child is gone. SDL_DestroyWindow
         * cleans up child resources but parent state on some platforms
         * (Wayland, X11/xdg-popup) is observably stickier.
         */
        if (windowStruct->deferredTransientApplied) {
            SDL_SetWindowModalFor(windowStruct->sdlWindow, NULL);
            windowStruct->deferredTransientApplied = False;
        }
#endif
        replayTargetForgetWindow(SDL_GetWindowID(windowStruct->sdlWindow));
        SDL_DestroyWindow(windowStruct->sdlWindow);
        windowStruct->sdlWindow = NULL;
#ifndef LIBX11_COMPAT_SDL3

        /* This top-level's SDL window is gone; repaint whatever it covered so
         * its pixels do not linger on the siblings below, matching the
         * XUnmapWindow teardown. XDestroyWindow (Motif closes an editor window
         * this way) never routes through unmapWindowInternal, and Xvnc delivers
         * the uncovered siblings no Expose, so without this the closed window's
         * pixels stay as residue. sdlWindow is nulled above so the present pass
         * skips this now-torn-down window while it briefly stays in the screen
         * child list. Gate on freeParentData, which is set only for a genuine
         * client destroy (XDestroyWindow, XDestroySubwindows): the recursion
         * into child widgets and the XCloseDisplay screen teardown pass False,
         * and repainting there would present siblings that are themselves about
         * to be destroyed, once per closing window (O(n^2) over the teardown).
         */
        if (freeParentData) {
            repaintTopLevelsOverlappingRect(window, closedRect);
            drawWindowDataToScreen();
        }
#endif
    }
    deleteWindowMapping(window);
    if (!windowStruct->internal)
        postEvent(display, window, DestroyNotify);
    if (freeParentData)
        removeChildFromParent(window);

    /* Hold visibleRegion live through removeChildFromParent's invalidation
     * walk, then fini it immediately before the matching free so the lifecycle
     * is obvious and a future reorder cannot wedge a use after free between the
     * two.
     */
    pixman_region32_fini(&windowStruct->visibleRegion);
    pixman_region32_fini(&windowStruct->dirty);
    free(windowStruct);
    SET_XID_VALUE(window, NULL);
    SET_XID_TYPE(window, CLOSED_WINDOW);
}

/* Neither addChildToWindow nor insertChildIntoWindow invalidates the
 * sibling-occlusion cache. Current callers are safe: XCreateWindow attaches a
 * brand-new Unmapped child (no occlusion contribution until XMapWindow runs),
 * and XReparentWindow's failure-rollback re-adds to the old parent which
 * removeChildFromParent already invalidated. A future caller that attaches a
 * Mapped child must call invalidateVisibleRegionForTopLevel(child) after the
 * attach succeeds, or the new siblings' cached visible regions will be stale.
 */
Bool addChildToWindow(Window parent, Window child)
{
    if (findInArray(&GET_WINDOW_STRUCT(parent)->children, (void *) child) >=
        0) {
        return False;
    }
    if (insertArray(&GET_WINDOW_STRUCT(parent)->children, (void *) child)) {
        GET_WINDOW_STRUCT(child)->parent = parent;
        return True;
    }
    return False;
}

Bool insertChildIntoWindow(Window parent, Window child, size_t index)
{
    Array *children = &GET_WINDOW_STRUCT(parent)->children;
    if (findInArray(children, (void *) child) >= 0)
        return False;
    if (!insertArray(children, (void *) child))
        return False;
    if (index >= children->length)
        index = children->length - 1;
    if (index + 1 < children->length) {
        memmove(&children->array[index + 1], &children->array[index],
                sizeof(void *) * (children->length - index - 1));
        children->array[index] = (void *) child;
    }
    GET_WINDOW_STRUCT(child)->parent = parent;
    return True;
}

Bool isParent(Window window1, Window window2)
{
    Window parent = GET_PARENT(window2);
    while (parent != None) {
        if (parent == window1)
            return True;
        parent = GET_PARENT(parent);
    }
    return False;
}

Bool isWindowEffectivelyViewable(Window window)
{
    if (window == None || !IS_TYPE(window, WINDOW))
        return False;
    if (GET_WINDOW_STRUCT(window)->mapState == UnMapped)
        return False;
    Window parent = GET_PARENT(window);
    while (parent != None) {
        if (GET_WINDOW_STRUCT(parent)->mapState == UnMapped)
            return False;
        parent = GET_PARENT(parent);
    }
    return True;
}

WindowProperty *findProperty(Array *properties, Atom property, size_t *index)
{
    size_t i;
    WindowProperty **windowProperties = (WindowProperty **) properties->array;
    for (i = 0; i < properties->length; i++) {
        if (windowProperties[i]->property == property) {
            if (index)
                *index = i;
            return windowProperties[i];
        }
    }
    return NULL;
}

Bool moveChildToIndex(Window window, size_t targetIndex)
{
    if (window == SCREEN_WINDOW)
        return False;
    Window parent = GET_PARENT(window);
    if (parent == None)
        return False;
    Array *children = &GET_WINDOW_STRUCT(parent)->children;
    ssize_t index = findInArray(children, (void *) window);
    if (index < 0)
        return False;
    if (targetIndex >= children->length)
        targetIndex = children->length - 1;
    if ((size_t) index == targetIndex)
        return True;
    removeArray(children, (size_t) index, True);
    if (targetIndex > children->length)
        targetIndex = children->length;
    if (targetIndex < children->length) {
        memmove(&children->array[targetIndex + 1],
                &children->array[targetIndex],
                sizeof(void *) * (children->length - targetIndex));
    }
    children->array[targetIndex] = (void *) window;
    children->length++;
    invalidateVisibleRegionForTopLevel(window);
    return True;
}

typedef struct {
    Window window;
    pixman_region32_t before;
} RestackExposureSnapshot;

static Window getTopLevelForWindow(Window window)
{
    Window top = window;
    while (top != None && top != SCREEN_WINDOW && !IS_TOP_LEVEL(top))
        top = GET_PARENT(top);
    if (top == None || top == SCREEN_WINDOW)
        return None;
    return top;
}

static void freeRestackExposureSnapshots(Array *snapshots)
{
    for (size_t i = 0; i < snapshots->length; i++) {
        RestackExposureSnapshot *snapshot = snapshots->array[i];
        pixman_region32_fini(&snapshot->before);
        free(snapshot);
    }
    freeArray(snapshots);
}

static Bool appendRestackExposureSnapshot(Array *snapshots, Window window)
{
    RestackExposureSnapshot *snapshot = malloc(sizeof(*snapshot));
    if (!snapshot)
        return False;
    snapshot->window = window;
    computeVisibleRegion(window, &snapshot->before);
    if (!insertArray(snapshots, snapshot)) {
        pixman_region32_fini(&snapshot->before);
        free(snapshot);
        return False;
    }
    return True;
}

static Bool collectRestackExposureSnapshots(Array *snapshots, Window window)
{
    if (window == None || !IS_TYPE(window, WINDOW))
        return True;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!windowStruct->inputOnly && isWindowEffectivelyViewable(window) &&
        !appendRestackExposureSnapshot(snapshots, window)) {
        return False;
    }

    Window *children = GET_CHILDREN(window);
    for (size_t i = 0; i < windowStruct->children.length; i++) {
        if (!collectRestackExposureSnapshots(snapshots, children[i]))
            return False;
    }
    return True;
}

static void postFullExposeForMappedSubtree(Display *display, Window window)
{
    if (window == None || !IS_TYPE(window, WINDOW))
        return;

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!windowStruct->inputOnly && isWindowEffectivelyViewable(window)) {
        SDL_Rect full = {0, 0, (int) windowStruct->w, (int) windowStruct->h};
        postEvent(display, window, Expose, &full, (size_t) 0);
    }

    Window *children = GET_CHILDREN(window);
    for (size_t i = 0; i < windowStruct->children.length; i++)
        postFullExposeForMappedSubtree(display, children[i]);
}

static void postRestackExposureDiffs(Display *display, Array *snapshots)
{
    for (size_t i = 0; i < snapshots->length; i++) {
        RestackExposureSnapshot *snapshot = snapshots->array[i];
        pixman_region32_t after;
        pixman_region32_t exposed;
        computeVisibleRegion(snapshot->window, &after);
        pixman_region32_init(&exposed);
        pixman_region32_subtract(&exposed, &after, &snapshot->before);

        int rectCount = 0;
        pixman_box32_t *boxes =
            pixman_region32_rectangles(&exposed, &rectCount);
        for (int j = rectCount; j-- > 0;) {
            SDL_Rect rect = {
                boxes[j].x1,
                boxes[j].y1,
                boxes[j].x2 - boxes[j].x1,
                boxes[j].y2 - boxes[j].y1,
            };
            if (rect.w > 0 && rect.h > 0)
                postEvent(display, snapshot->window, Expose, &rect, (size_t) j);
        }

        pixman_region32_fini(&exposed);
        pixman_region32_fini(&after);
    }
}

Bool moveChildToIndexAndExpose(Display *display,
                               Window window,
                               size_t targetIndex)
{
    if (window == SCREEN_WINDOW)
        return False;
    Window parent = GET_PARENT(window);
    if (parent == None)
        return False;

    Array *children = &GET_WINDOW_STRUCT(parent)->children;
    ssize_t index = findInArray(children, (void *) window);
    if (index < 0)
        return False;
    if (targetIndex >= children->length)
        targetIndex = children->length - 1;
    if ((size_t) index == targetIndex)
        return True;

    Window top = getTopLevelForWindow(window);
    Array snapshots;
    Bool haveSnapshots = False;
    if (display && top != None && initArray(&snapshots, 0)) {
        haveSnapshots = collectRestackExposureSnapshots(&snapshots, top);
        if (!haveSnapshots)
            freeRestackExposureSnapshots(&snapshots);
    }

    Bool moved = moveChildToIndex(window, targetIndex);
    if (!moved) {
        if (haveSnapshots)
            freeRestackExposureSnapshots(&snapshots);
        return False;
    }

    if (display && top != None) {
        if (haveSnapshots) {
            postRestackExposureDiffs(display, &snapshots);
            freeRestackExposureSnapshots(&snapshots);
        } else {
            postFullExposeForMappedSubtree(display, top);
        }
    }
    return True;
}

/* Returns True when the geometric rectangles of a and b overlap. Endpoints are
 * computed in int64_t so x + w cannot overflow when the window struct holds
 * extreme coordinates or dimensions.
 */
Bool windowsOverlap(Window a, Window b)
{
    WindowStruct *sa = GET_WINDOW_STRUCT(a);
    WindowStruct *sb = GET_WINDOW_STRUCT(b);
    int64_t ax2 = (int64_t) sa->x + (int64_t) sa->w;
    int64_t ay2 = (int64_t) sa->y + (int64_t) sa->h;
    int64_t bx2 = (int64_t) sb->x + (int64_t) sb->w;
    int64_t by2 = (int64_t) sb->y + (int64_t) sb->h;
    return (int64_t) sa->x < bx2 && (int64_t) sb->x < ax2 &&
           (int64_t) sa->y < by2 && (int64_t) sb->y < ay2;
}

static Bool shapeMaskPixelActive(SDL_Surface *mask, int x, int y)
{
    if (!mask || x < 0 || y < 0 || x >= mask->w || y >= mask->h)
        return False;
    Uint32 pixel = getPixel(mask, (unsigned int) x, (unsigned int) y);
    Uint8 r = 0, g = 0, b = 0;
    SDL_GetRGB(pixel, XC_SURFACE_FORMAT(mask), &r, &g, &b);
    return r || g || b;
}

/* Build the occluder region in the target window's local frame. rx64/ry64 are
 * the sibling's origin already expressed in the target's coordinate system;
 * winW/winH are the target's frame. All geometry math runs in int64_t so a huge
 * or far-away sibling cannot overflow the int32 inputs pixman expects: the rect
 * is pre-clipped to (0,0,winW,winH) before any cast to int. The shape-mask path
 * applies the same clip per span.
 */
static void initSiblingOccluderRegion(WindowStruct *siblingStruct,
                                      int64_t rx64,
                                      int64_t ry64,
                                      unsigned int winW,
                                      unsigned int winH,
                                      pixman_region32_t *occluder)
{
    int64_t winWi = (int64_t) winW, winHi = (int64_t) winH;
    SDL_Surface *mask = siblingStruct->shapeBoundingMask;
    if (!mask) {
        int64_t x1 = rx64 < 0 ? 0 : rx64, y1 = ry64 < 0 ? 0 : ry64;
        int64_t x2 = rx64 + (int64_t) siblingStruct->w;
        int64_t y2 = ry64 + (int64_t) siblingStruct->h;
        if (x2 > winWi)
            x2 = winWi;
        if (y2 > winHi)
            y2 = winHi;
        if (x2 <= x1 || y2 <= y1) {
            pixman_region32_init(occluder);
            return;
        }
        pixman_region32_init_rect(occluder, (int) x1, (int) y1,
                                  (unsigned int) (x2 - x1),
                                  (unsigned int) (y2 - y1));
        return;
    }

    pixman_region32_init(occluder);
    int64_t baseX = rx64 + (int64_t) siblingStruct->shapeBoundingOffsetX;
    int64_t baseY = ry64 + (int64_t) siblingStruct->shapeBoundingOffsetY;
    for (int y = 0; y < mask->h; y++) {
        int64_t pixY = baseY + (int64_t) y;
        if (pixY < 0 || pixY >= winHi)
            continue;
        int x = 0;
        while (x < mask->w) {
            while (x < mask->w && !shapeMaskPixelActive(mask, x, y))
                x++;
            int start = x;
            while (x < mask->w && shapeMaskPixelActive(mask, x, y))
                x++;
            if (start == x)
                continue;
            int64_t spanX1 = baseX + (int64_t) start;
            int64_t spanX2 = baseX + (int64_t) x;
            if (spanX1 < 0)
                spanX1 = 0;
            if (spanX2 > winWi)
                spanX2 = winWi;
            if (spanX2 <= spanX1)
                continue;
            pixman_region32_t span;
            pixman_region32_init_rect(&span, (int) spanX1, (int) pixY,
                                      (unsigned int) (spanX2 - spanX1), 1);
            pixman_region32_union(occluder, occluder, &span);
            pixman_region32_fini(&span);
        }
    }
}

static Bool siblingOccluderIntersectsWindow(WindowStruct *siblingStruct,
                                            int64_t rx64,
                                            int64_t ry64,
                                            unsigned int winW,
                                            unsigned int winH)
{
    int64_t x1 = rx64, y1 = ry64;
    int64_t x2 = rx64 + (int64_t) siblingStruct->w;
    int64_t y2 = ry64 + (int64_t) siblingStruct->h;
    SDL_Surface *mask = siblingStruct->shapeBoundingMask;
    if (mask) {
        x1 = rx64 + (int64_t) siblingStruct->shapeBoundingOffsetX;
        y1 = ry64 + (int64_t) siblingStruct->shapeBoundingOffsetY;
        x2 = x1 + (int64_t) mask->w;
        y2 = y1 + (int64_t) mask->h;
    }
    return x1 < (int64_t) winW && y1 < (int64_t) winH && x2 > 0 && y2 > 0;
}

/* See the declaration in window-internal.h for why the range guard exists. */
void regionInitRectSafe(pixman_region32_t *region,
                        int x,
                        int y,
                        unsigned int w,
                        unsigned int h)
{
    if (w == 0 || h == 0 || (int64_t) x + (int64_t) w > INT_MAX ||
        (int64_t) y + (int64_t) h > INT_MAX) {
        pixman_region32_init(region);
        return;
    }
    pixman_region32_init_rect(region, x, y, w, h);
}

/* Build the sibling-occlusion region for "window" in window-local coords.
 * Starts from the full (0,0,w,h) frame and subtracts each higher sibling
 * encountered while walking up the ancestor chain, translated into the window's
 * coordinate frame. Walks stop at SCREEN_WINDOW: sibling top-levels are
 * arbitrated by the host compositor, not by X clipping. The caller must NOT
 * have previously initialized *out: this function calls
 * pixman_region32_init_rect on it.
 */
void computeVisibleRegion(Window window, pixman_region32_t *out)
{
    if (window == None || window == SCREEN_WINDOW || !IS_TYPE(window, WINDOW)) {
        pixman_region32_init(out);
        return;
    }
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    regionInitRectSafe(out, 0, 0, windowStruct->w, windowStruct->h);

    /* (winOX, winOY) holds the window's origin expressed in the current
     * ancestor's parent's coordinate frame. Initially that's just (window.x,
     * window.y) because the first level processed is window's own parent. Each
     * step up adds the just-visited ancestor's parent-relative origin so the
     * invariant holds for the grandparent's frame next. Coordinates accumulate
     * in int64_t so a deep ancestor chain or hostile XConfigureWindow inputs
     * cannot trip signed-int overflow before pixman sees the rect.
     */
    int64_t winOX = (int64_t) windowStruct->x,
            winOY = (int64_t) windowStruct->y;
    Window cur = window;
    while (cur != SCREEN_WINDOW) {
        Window parent = GET_PARENT(cur);
        if (parent == None || parent == SCREEN_WINDOW)
            break;
        WindowStruct *parentStruct = GET_WINDOW_STRUCT(parent);
        Window *children = (Window *) parentStruct->children.array;
        ssize_t curIndex = findInArray(&parentStruct->children, (void *) cur);
        if (curIndex < 0)
            break;

        for (size_t i = (size_t) (curIndex + 1);
             i < parentStruct->children.length; i++) {
            Window sibling = children[i];
            if (sibling == None || !IS_TYPE(sibling, WINDOW))
                continue;

            WindowStruct *siblingStruct = GET_WINDOW_STRUCT(sibling);
            if (siblingStruct->mapState == UnMapped)
                continue;
            if (siblingStruct->inputOnly)
                continue;
            if (siblingStruct->w == 0 || siblingStruct->h == 0)
                continue;

            int64_t rx64 = (int64_t) siblingStruct->x - winOX;
            int64_t ry64 = (int64_t) siblingStruct->y - winOY;

            /* Cull occluders that cannot intersect the (0,0,w,h) frame before
             * they reach pixman. The helper would clip to an empty rect anyway,
             * but the explicit cull keeps the math and the iterations under it
             * cheap. Shaped windows use the bounding-mask extents because the
             * active shape can extend outside the default window frame.
             */
            if (!siblingOccluderIntersectsWindow(siblingStruct, rx64, ry64,
                                                 windowStruct->w,
                                                 windowStruct->h))
                continue;

            pixman_region32_t occluder;
            initSiblingOccluderRegion(siblingStruct, rx64, ry64,
                                      windowStruct->w, windowStruct->h,
                                      &occluder);
            pixman_region32_subtract(out, out, &occluder);
            pixman_region32_fini(&occluder);
        }

        winOX += (int64_t) parentStruct->x;
        winOY += (int64_t) parentStruct->y;
        cur = parent;
    }
}

const pixman_region32_t *ensureVisibleRegion(Window window)
{
    if (window == None || !IS_TYPE(window, WINDOW))
        return NULL;
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (windowStruct->visibleRegionValid)
        return &windowStruct->visibleRegion;
    pixman_region32_fini(&windowStruct->visibleRegion);
    computeVisibleRegion(window, &windowStruct->visibleRegion);
    windowStruct->visibleRegionValid = True;
    return &windowStruct->visibleRegion;
}

static void invalidateVisibleRegionSubtree(Window window)
{
    if (window == None || !IS_TYPE(window, WINDOW))
        return;
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    windowStruct->visibleRegionValid = False;

    /* The per-primitive drawing-side cache may hold a pointer to the just-
     * invalidated region; flush it so the next draw recomputes from fresh data
     * instead of reading freed pixman rect storage.
     */
    invalidatePrimitiveClipCache();
    Window *children = (Window *) windowStruct->children.array;
    for (size_t i = 0; i < windowStruct->children.length; i++)
        invalidateVisibleRegionSubtree(children[i]);
}

/* A move, resize, restack, or map-state change on "window" can shift the
 * visible region of anything beneath the nearest top-level ancestor: lower
 * siblings change occlusion, descendants inherit the cut, and ancestors only
 * matter if "window" was itself a covering sibling on one of them. Invalidating
 * the full top-level subtree keeps the rule trivial; the recompute on next
 * consult is bounded by sibling depth and runs lazily on demand.
 */
void invalidateVisibleRegionForTopLevel(Window window)
{
    if (window == None || !IS_TYPE(window, WINDOW))
        return;
    Window top = window;
    while (top != None && top != SCREEN_WINDOW && !IS_TOP_LEVEL(top))
        top = GET_PARENT(top);
    if (top == None || top == SCREEN_WINDOW)
        return;
    invalidateVisibleRegionSubtree(top);
}

/* TopIf/BottomIf/Opposite are conditional restacks. "Occludes" means an upper
 * sibling overlaps window; "occluded by" means a lower sibling overlaps window.
 * An unconditional raise/lower would violate the Xlib spec when no overlap is
 * present.
 */
static Bool hasOccludingSiblingAbove(Array *children, size_t index)
{
    Window window = (Window) children->array[index];
    for (size_t i = index + 1; i < children->length; i++) {
        if (windowsOverlap(window, (Window) children->array[i]))
            return True;
    }
    return False;
}

static Bool hasOccludedSiblingBelow(Array *children, size_t index)
{
    Window window = (Window) children->array[index];
    for (size_t i = 0; i < index; i++) {
        if (windowsOverlap(window, (Window) children->array[i]))
            return True;
    }
    return False;
}

static Bool restackWindow(Display *display,
                          Window window,
                          unsigned long value_mask,
                          XWindowChanges *values)
{
    (void) display;
    if (!HAS_VALUE(value_mask, CWStackMode))
        return True;
    Window parent = GET_PARENT(window);
    if (parent == None)
        return True;

    Array *children = &GET_WINDOW_STRUCT(parent)->children;
    ssize_t index = findInArray(children, (void *) window);
    if (index < 0)
        return False;

    int mode = values->stack_mode;
    if (HAS_VALUE(value_mask, CWSibling)) {
        TYPE_CHECK(values->sibling, WINDOW, display, False);
        if (GET_PARENT(values->sibling) != parent ||
            values->sibling == window) {
            handleError(0, display, window, 0, BadMatch, 0);
            return False;
        }
        ssize_t siblingIndex = findInArray(children, (void *) values->sibling);
        if (siblingIndex < 0)
            return False;
        size_t target = (size_t) siblingIndex;
        if (mode == Above) {
            target++;
        } else if (mode != Below) {
            handleError(0, display, window, 0, BadValue, 0);
            return False;
        }
        if ((size_t) index < target)
            target--;
        return moveChildToIndexAndExpose(display, window, target);
    }

    size_t idx = (size_t) index;
    switch (mode) {
    case Above:
        return moveChildToIndexAndExpose(display, window, children->length - 1);
    case Below:
        return moveChildToIndexAndExpose(display, window, 0);
    case TopIf:
        if (hasOccludingSiblingAbove(children, idx))
            return moveChildToIndexAndExpose(display, window,
                                             children->length - 1);
        return True;
    case BottomIf:
        if (hasOccludedSiblingBelow(children, idx))
            return moveChildToIndexAndExpose(display, window, 0);
        return True;
    case Opposite:
        if (hasOccludingSiblingAbove(children, idx))
            return moveChildToIndexAndExpose(display, window,
                                             children->length - 1);
        if (hasOccludedSiblingBelow(children, idx))
            return moveChildToIndexAndExpose(display, window, 0);
        return True;
    default:
        handleError(0, display, window, 0, BadValue, 0);
        return False;
    }
}

typedef struct {
    SDL_Surface *surface;
    SDL_Rect dst;
} MovedWindowSnapshot;

static Bool parentRectToTop(Window parent, SDL_Rect *rect, Window *top)
{
    Window window = parent;
    while (window != None && window != SCREEN_WINDOW) {
        if (IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
            *top = window;
            return True;
        }
        int x = 0, y = 0;
        GET_WINDOW_POS(window, x, y);
        rect->x += x;
        rect->y += y;
        window = GET_PARENT(window);
    }
    return False;
}

static MovedWindowSnapshot snapshotMovedWindow(Window window,
                                               int oldX,
                                               int oldY,
                                               int oldWidth,
                                               int oldHeight)
{
    MovedWindowSnapshot snapshot = {0};
    Window parent = GET_PARENT(window);
    if (parent == None || IS_INPUT_ONLY(window) ||
        GET_WINDOW_STRUCT(window)->mapState == UnMapped ||
        GET_WINDOW_STRUCT(parent)->mapState == UnMapped)
        return snapshot;

    SDL_Rect oldRect = {oldX, oldY, oldWidth, oldHeight};
    SDL_Rect parentBounds = {0, 0, 0, 0};
    GET_WINDOW_DIMS(parent, parentBounds.w, parentBounds.h);
    SDL_Rect visible;
    if (!SDL_IntersectRect(&oldRect, &parentBounds, &visible))
        return snapshot;

    /* Only relocate pixels when the child sits fully inside its parent at the
     * old position. A clip against parentBounds means the snapshot would miss
     * the off-parent slice, and because the caller skips the post-move backing
     * clear whenever a snapshot is restored, that newly-visible slice would
     * keep stale pixels until the client repaints. Fall back to clear+expose
     * there.
     */
    if (visible.w != oldWidth || visible.h != oldHeight)
        return snapshot;

    snapshot.dst =
        (SDL_Rect) {visible.x - oldX, visible.y - oldY, visible.w, visible.h};
    const pixman_region32_t *visibleRegion = ensureVisibleRegion(window);
    pixman_box32_t dstBox = {
        snapshot.dst.x,
        snapshot.dst.y,
        snapshot.dst.x + snapshot.dst.w,
        snapshot.dst.y + snapshot.dst.h,
    };
    if (pixman_region32_contains_rectangle(visibleRegion, &dstBox) !=
        PIXMAN_REGION_IN)
        return (MovedWindowSnapshot) {0};

    Window top = None;
    if (!parentRectToTop(parent, &visible, &top))
        return snapshot;

    SDL_Renderer *topRenderer = getWindowRenderer(top);
    snapshot.surface = getRenderSurfaceRect(topRenderer, &visible);
    return snapshot;
}

static Bool restoreMovedWindowSnapshot(Window window,
                                       MovedWindowSnapshot *snapshot)
{
    if (!snapshot->surface)
        return False;

    SDL_Renderer *renderer = getWindowRenderer(window);
    SDL_Texture *texture =
        renderer ? SDL_CreateTextureFromSurface(renderer, snapshot->surface)
                 : NULL;
    SDL_FreeSurface(snapshot->surface);
    snapshot->surface = NULL;
    if (!texture)
        return False;

    Bool restored = False;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE);
    if (SDL_RenderCopy(renderer, texture, NULL, &snapshot->dst) == 0) {
        invalidateSdlDrawStateCache();
        presentDrawableRectIfVisible(window, &snapshot->dst);
        restored = True;
    }
    SDL_DestroyTexture(texture);
    return restored;
}

/* exceptChild is the window whose old footprint is being cleared when it MOVED:
 * it shares the parent backing, so its pre-move pixels linger there, and when
 * the new footprint overlaps the old one an ordinary XClearArea would protect
 * the child and leave that overlap as a ghost. The caller snapshots the child's
 * old pixels before this clear and restores them into the new footprint after
 * it. Pass None for a shrink-in-place, where the origin is unchanged: there the
 * smaller footprint is top-left aligned inside the old one and still holds
 * valid pixels, so the child must stay clipped and only the vacated L-shape is
 * cleared.
 */
static void postParentExposureForOldArea(Display *display,
                                         Window window,
                                         int oldX,
                                         int oldY,
                                         int oldWidth,
                                         int oldHeight,
                                         Window exceptChild)
{
    Window parent = GET_PARENT(window);
    if (parent == None || IS_INPUT_ONLY(parent) ||
        GET_WINDOW_STRUCT(parent)->mapState == UnMapped)
        return;

    compatClearWindowAreaExceptChild(
        display, parent, oldX, oldY, (unsigned int) oldWidth,
        (unsigned int) oldHeight, False, exceptChild);
    SDL_Rect exposed = {oldX, oldY, oldWidth, oldHeight};
    postExposeEvent(display, parent, &exposed, 1);
}

static void postMovedWindowExposure(Display *display,
                                    Window window,
                                    Bool clearBacking)
{
    Window parent = GET_PARENT(window);
    if (parent == None || IS_INPUT_ONLY(window) ||
        GET_WINDOW_STRUCT(window)->mapState == UnMapped ||
        GET_WINDOW_STRUCT(parent)->mapState == UnMapped) {
        return;
    }

    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    int x, y;
    GET_WINDOW_POS(window, x, y);
    SDL_Rect parentBounds = {0, 0, 0, 0};
    GET_WINDOW_DIMS(parent, parentBounds.w, parentBounds.h);
    SDL_Rect windowBounds = {
        .x = x,
        .y = y,
        .w = (int) windowStruct->w,
        .h = (int) windowStruct->h,
    };
    SDL_Rect visible;
    if (!SDL_IntersectRect(&windowBounds, &parentBounds, &visible))
        return;

    SDL_Rect expose = {
        .x = visible.x - x,
        .y = visible.y - y,
        .w = visible.w,
        .h = visible.h,
    };
    if (clearBacking) {
        XClearArea(display, window, expose.x, expose.y, (unsigned int) expose.w,
                   (unsigned int) expose.h, False);
    }
    postExposeEvent(display, window, &expose, 1);
}

Bool bitGravityIsTopLeftAnchored(int bitGravity)
{
    return bitGravity == NorthWestGravity || bitGravity == StaticGravity;
}

/* Returns True when it has already posted a full-window Expose for a mapped
 * top-level window, so the caller can skip its own redundant full-window
 * Expose. False otherwise (non-top-level, or the band-only top-left path where
 * the caller's full-window Expose is still wanted).
 */
static Bool postResizeExpose(Display *display,
                             Window window,
                             int oldWidth,
                             int oldHeight)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (windowStruct->inputOnly)
        return False;

    /* With ForgetGravity (the X default) the server discards all window
     * contents on a resize, so wipe the whole non-top-level window to
     * background before re-exposing it. A window that requested a retaining bit
     * gravity (e.g. StaticGravity, which xwpe sets on its cells) keeps its old
     * pixels: only the bands that newly appeared because a dimension grew need
     * a clean background, and the previously visible region must be left
     * intact. Wiping that retained region would blank content the client has
     * not yet repainted; during the macOS modal resize loop the client's
     * repaint lags a few frames, so that blank shows as a transient black
     * flash. Preserving it until the reflow lands both removes the flash and
     * matches real X11, which never erases the retained part of such a window
     * on resize.
     *
     * The band optimization below models a top-left anchor only: it assumes the
     * retained pixels stay pinned at (0,0) and the growth appears on the right
     * and bottom edges. That holds for NorthWestGravity and StaticGravity (the
     * only retaining gravity xwpe uses). Other anchors move the retained region
     * to a different corner/edge (NorthEastGravity keeps the top-right, so the
     * new band is on the left; CenterGravity splits both sides), and clearing
     * the right/bottom bands there would erase live pixels and leave the real
     * new band dirty. Rather than model every anchor, fall back to a
     * full-window clear for any bit gravity this path does not model: always
     * correct, just without the flash-avoidance optimization.
     */
    Bool clearWholeWindow =
        !bitGravityIsTopLeftAnchored(windowStruct->bitGravity);

    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
        SDL_Rect fullWindow = {
            0,
            0,
            (int) windowStruct->w,
            (int) windowStruct->h,
        };
        if (clearWholeWindow) {
            XClearArea(display, window, 0, 0, (unsigned int) fullWindow.w,
                       (unsigned int) fullWindow.h, False);
        } else {
            if ((int) windowStruct->w > oldWidth) {
                XClearArea(display, window, oldWidth, 0,
                           (unsigned int) ((int) windowStruct->w - oldWidth),
                           windowStruct->h, False);
            }
            if ((int) windowStruct->h > oldHeight) {
                int width = MIN((int) windowStruct->w, oldWidth);
                XClearArea(display, window, 0, oldHeight, (unsigned int) width,
                           (unsigned int) ((int) windowStruct->h - oldHeight),
                           False);
            }
        }
        postExposeEvent(display, window, &fullWindow, 1);
        return False;
    }

    /* Mapped top-level: the growth-band exposure has the same top-left anchor
     * assumption as the band clear above, so for a bit gravity this path does
     * not model, expose the whole window instead of the right/bottom bands.
     */
    if (clearWholeWindow) {
        SDL_Rect fullWindow = {0, 0, (int) windowStruct->w,
                               (int) windowStruct->h};
        postExposeEvent(display, window, &fullWindow, 1);
        return True;
    }
    SDL_Rect rects[2];
    size_t count = 0;
    if ((int) windowStruct->w > oldWidth) {
        rects[count++] =
            (SDL_Rect) {oldWidth, 0, (int) windowStruct->w - oldWidth,
                        (int) windowStruct->h};
    }
    if ((int) windowStruct->h > oldHeight) {
        int width = MIN((int) windowStruct->w, oldWidth);
        rects[count++] =
            (SDL_Rect) {0, oldHeight, width, (int) windowStruct->h - oldHeight};
    }
    if (count > 0)
        postExposeEvent(display, window, rects, count);
    return False;
}

void freeWindowProperty(WindowProperty *property)
{
    if (!property)
        return;
    free(property->data);
    free(property);
}

void resizeWindowTexture(Window window)
{
    /* The backing is about to be recreated and re-cleared, dropping every
     * painted cell, so discard the stamps that tracked them.
     */
    flushTextStampsForWindow(window);
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!windowStruct->sdlTexture)
        return;
    SDL_Renderer *windowRenderer =
        GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
    if (!windowRenderer)
        return;

    /* Caller must record the new dimensions in windowStruct->w/h before
     * calling. The SDL window may not actually be the target size yet (resize
     * events may be faked in tests), so this function cannot re-query via
     * SDL_GetWindowSize here.
     */
    SDL_Texture *oldTexture = windowStruct->sdlTexture;
    SDL_Texture *newTexture = SDL_CreateTexture(
        windowRenderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        (int) windowStruct->w, (int) windowStruct->h);
    if (!newTexture) {
        LOG("SDL_CreateTexture failed in %s for window 0x%08lx: %s\n", __func__,
            window, SDL_GetError());
        return;
    }
    SDL_Texture *prevTarget = SDL_GetRenderTarget(windowRenderer);
    if (SDL_SetRenderTarget(windowRenderer, newTexture) != 0) {
        LOG("SDL_SetRenderTarget failed in %s: %s\n", __func__, SDL_GetError());
        SDL_DestroyTexture(newTexture);
        return;
    }
    unsigned long bg = resolvedWindowBackgroundColor(window);
    SDL_SetRenderDrawColor(windowRenderer, GET_RED_FROM_COLOR(bg),
                           GET_GREEN_FROM_COLOR(bg), GET_BLUE_FROM_COLOR(bg),
                           GET_ALPHA_FROM_COLOR(bg));
    SDL_RenderClear(windowRenderer);

    /* Carry the overlapping region of the previous backing into the new one
     * before the client repaints. On a live resize the client needs a few
     * frames to reflow and redraw at the new size, and during the macOS modal
     * resize loop it gets no events at all until the drag ends; without this
     * copy the freshly-cleared backing shows the background colour for those
     * frames, which reads as a flash during the drag and, if the client only
     * partially repaints afterward (e.g. an empty editor pane it never
     * redraws), a persistent grey area. Copying the common top-left region
     * keeps the last-good content on screen until the repaint lands.
     *
     * Skip the copy only for a bit gravity that RETAINS at a non-top-left
     * anchor (NorthEastGravity/CenterGravity/etc): there the top-left overlap
     * is semantically the wrong pixels, so discard and let the full Expose
     * repaint. ForgetGravity keeps no retained pixels by definition, so the
     * top-left copy is a harmless flash-avoidance (stale until repaint), not a
     * wrong-anchor artifact, and NorthWestGravity/StaticGravity pin at top-left
     * where it is exactly correct.
     */
    Bool retainingNonTopLeft =
        windowStruct->bitGravity != ForgetGravity &&
        !bitGravityIsTopLeftAnchored(windowStruct->bitGravity);
    int oldW = 0, oldH = 0;
    if (!retainingNonTopLeft &&
        SDL_QueryTexture(oldTexture, NULL, NULL, &oldW, &oldH) == 0 &&
        oldW > 0 && oldH > 0) {
        int copyW = oldW < (int) windowStruct->w ? oldW : (int) windowStruct->w;
        int copyH = oldH < (int) windowStruct->h ? oldH : (int) windowStruct->h;
        if (copyW > 0 && copyH > 0) {
            SDL_Rect overlap = {0, 0, copyW, copyH};
            SDL_RenderCopy(windowRenderer, oldTexture, &overlap, &overlap);
        }
    }
    windowStruct->sdlTexture = newTexture;
    windowStruct->needsPresent = True;
    windowStruct->hasPresentRect = False;

    /* The old rects are stale relative to the new texture geometry. Drop them
     * and let fullyDirty force a single full-window readback on the next
     * present; that is cheaper than re-marking, and the present-success path in
     * drawWindowDataToScreen clears both fields together.
     */
    pixman_region32_clear(&windowStruct->dirty);
    windowStruct->fullyDirty = True;
    markWindowNeedsPresent(window);
    SDL_SetRenderTarget(windowRenderer,
                        prevTarget == oldTexture ? newTexture : prevTarget);
    SDL_DestroyTexture(oldTexture);
    invalidateSdlDrawStateCache();
}

Bool mergeWindowDrawables(Window parent, Window child)
{
    WindowStruct *childWindowStruct = GET_WINDOW_STRUCT(child);
    if (!childWindowStruct->sdlTexture)
        return True;
    SDL_Renderer *parentRenderer = getWindowRenderer(parent);
    if (childWindowStruct->sdlRenderer)
        SDL_RenderPresent(childWindowStruct->sdlRenderer);

    SDL_Rect destRect;
    GET_WINDOW_POS(child, destRect.x, destRect.y);
    GET_WINDOW_DIMS(child, destRect.w, destRect.h);
    if (SDL_RenderCopy(parentRenderer, childWindowStruct->sdlTexture, NULL,
                       &destRect) != 0) {
        return False;
    }

    presentDrawableRectIfVisible(parent, &destRect);
    SDL_DestroyTexture(childWindowStruct->sdlTexture);
    childWindowStruct->sdlTexture = NULL;
    childWindowStruct->contentsMergedToParent = True;
    if (childWindowStruct->sdlRenderer) {
        destroyWindowRenderer(childWindowStruct->sdlRenderer);
        childWindowStruct->sdlRenderer = NULL;
    }
    return True;
}

void mapRequestedChildren(Display *display, Window window)
{
    Window *children = GET_CHILDREN(window);
    size_t i;
    for (i = 0; i < GET_WINDOW_STRUCT(window)->children.length; i++) {
        if (children[i] == None)
            continue;

        if (GET_WINDOW_STRUCT(children[i])->mapState == Mapped) {
            WindowStruct *childStruct = GET_WINDOW_STRUCT(children[i]);
            if (childStruct->sdlTexture) {
                if (!mergeWindowDrawables(window, children[i])) {
                    LOG("Failed to merge mapped child drawables in %s\n",
                        __func__);
                    continue;
                }
            }
            childStruct->contentsMergedToParent = False;
            if (!childStruct->inputOnly) {
                XClearArea(display, children[i], 0, 0, 0, 0, False);
                SDL_Rect exposeRect = {0, 0, childStruct->w, childStruct->h};
                postExposeEvent(display, children[i], &exposeRect, 1);
            }
            mapRequestedChildren(display, children[i]);
        } else if (GET_WINDOW_STRUCT(children[i])->mapState == MapRequested) {
            if (!mergeWindowDrawables(window, children[i])) {
                LOG("Failed to merge the window drawables in %s\n", __func__);
                return;
            }
            WindowStruct *childStruct = GET_WINDOW_STRUCT(children[i]);
            childStruct->mapState = Mapped;
            if (!childStruct->inputOnly && !childStruct->contentsMergedToParent)
                XClearArea(display, children[i], 0, 0, 0, 0, False);
            childStruct->contentsMergedToParent = False;
            postEvent(display, children[i], MapNotify);
            postEvent(display, children[i], VisibilityNotify);
            if (!childStruct->inputOnly) {
                SDL_Rect exposeRect = {0, 0, childStruct->w, childStruct->h};
                postExposeEvent(display, children[i], &exposeRect, 1);
            }
            mapRequestedChildren(display, children[i]);
        }
    }
}

Bool configureWindow(Display *display,
                     Window window,
                     unsigned long value_mask,
                     XWindowChanges *values)
{
    /* Touches SDL (SDL_SetWindowPosition/Size) and pumps. Reached only via
     * configureWindowRouted and internal main-thread event handling, so assert
     * the main-thread invariant at the SDL source.
     */
    requireMainEventThread("configureWindow");

    /* Validate here, on the main thread, rather than in the XConfigureWindow /
     * XMoveWindow / XResizeWindow entry points: those run on the calling
     * worker, where reading the window's WindowStruct would race a concurrent
     * main-thread destroy. Running it on main serializes it with tree mutation.
     * Side effect (deliberate): on a bad window the client X error handler that
     * TYPE_CHECK's handleError dispatches now runs on the main thread rather
     * than the calling worker. Client handlers are thread-agnostic in practice.
     */
    TYPE_CHECK(window, WINDOW, display, False);
    if (window == SCREEN_WINDOW)
        return True;

    /* A move, resize, or restack relocates this window's cells in the shared
     * top-level backing, so any text stamps recorded for them are now stale.
     */
    flushTextStampsForWindow(window);
    Bool hasChanged = False;
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);

    /* There is no external window-manager client in the SDL-backed
     * compatibility model. Honor the configure directly even if a toolkit
     * selected SubstructureRedirectMask internally.
     */
    Bool isMappedTopLevelWindow = IS_MAPPED_TOP_LEVEL_WINDOW(window);
    int oldX, oldY, oldWidth, oldHeight;
    GET_WINDOW_POS(window, oldX, oldY);
    GET_WINDOW_DIMS(window, oldWidth, oldHeight);
    MovedWindowSnapshot moveSnapshot = {0};
    Bool resizedWindow = False;
    Bool popupNeedsReposition = False;
    int resizedWidth = oldWidth, resizedHeight = oldHeight;
    if (!restackWindow(display, window, value_mask, values))
        return False;
    if (HAS_VALUE(value_mask, CWStackMode))
        hasChanged = True;
    if (HAS_VALUE(value_mask, CWX) || HAS_VALUE(value_mask, CWY)) {
        int x = oldX, y = oldY;
        if (HAS_VALUE(value_mask, CWX))
            x = values->x;
        if (HAS_VALUE(value_mask, CWY))
            y = values->y;
        if (isMappedTopLevelWindow && windowStruct->popupParent != None) {
            /* A parent-anchored popup is positioned relative to its parent
             * surface, not in absolute host coordinates. Record the new anchor
             * and defer the actual reposition to the single call after the size
             * block below, so a combined move+resize re-runs the positioner
             * once at the final size instead of first at the stale size.
             */
            windowStruct->x = x;
            windowStruct->y = y;
            windowStruct->popupAnchorX = x;
            windowStruct->popupAnchorY = y;
            if (x != oldX || y != oldY)
                popupNeedsReposition = True;
        } else if (isMappedTopLevelWindow) {
            int hostX = x, hostY = y;
            topLevelWindowHostPosition(window, x, y, &hostX, &hostY);

            /* SDL_SetWindowPosition makes SDL echo a MOVED for this window.
             * This move is client-initiated and already gets its
             * ConfigureNotify from the postEvent below, so record the exact
             * host origin as an echo token (mirroring the resize path's
             * snapEcho) and the filter drops the matching MOVED, or a bare move
             * would deliver two ConfigureNotifies since the filter now lets a
             * mapped top-level MOVED through to clear the drag trail. Only arm
             * on a real position change: an unmoved window emits no echo, and a
             * stale token could otherwise match an unrelated later MOVED. No
             * pump is needed: the token matches whenever the echo arrives,
             * synchronous or not, so it avoids the re-entrant mid-configure
             * event processing the old pump forced. A reparenting WM (FVWM on
             * Xvnc) reports a frame-adjusted origin that will not match, so
             * such a client XMoveWindow still sees one benign extra
             * ConfigureNotify.
             */
            if (x != oldX || y != oldY) {
                windowStruct->moveEchoHostX = hostX;
                windowStruct->moveEchoHostY = hostY;

                /* Publish the coordinates before the valid flag: SDL_AtomicSet
                 * carries a full barrier, so the filter that reads the flag
                 * with SDL_AtomicGet sees the matching origin, never a torn
                 * one.
                 */
                SDL_AtomicSet(&windowStruct->moveEchoValid, 1);
            }
            SDL_SetWindowPosition(windowStruct->sdlWindow, hostX, hostY);
            if (windowStruct->overrideRedirect) {
                windowStruct->x = x;
                windowStruct->y = y;
            } else {
                SDL_GetWindowPosition(windowStruct->sdlWindow, &windowStruct->x,
                                      &windowStruct->y);
            }
        } else {
            if (windowStruct->mapState != UnMapped &&
                (oldX != x || oldY != y)) {
                moveSnapshot = snapshotMovedWindow(window, oldX, oldY, oldWidth,
                                                   oldHeight);
            }
            windowStruct->x = x;
            windowStruct->y = y;
        }
        if (oldX != windowStruct->x || oldY != windowStruct->y)
            hasChanged = True;
    }
    if (HAS_VALUE(value_mask, CWWidth) || HAS_VALUE(value_mask, CWHeight)) {
        int width = oldWidth, height = oldHeight;

        /* A zero or negative width/height is a protocol BadValue. Report it so
         * a client that installed its own error handler still learns of the bad
         * request. The default handler is non-fatal, and toolkits routinely
         * configure a not-yet-sized widget this way during setup, so
         * terminating here would be worse than ignoring the request. A resize
         * must be atomic though: reject the whole request before touching
         * either dimension, so a bad width can never leave a valid height half
         * applied.
         */
        if ((HAS_VALUE(value_mask, CWWidth) && values->width <= 0) ||
            (HAS_VALUE(value_mask, CWHeight) && values->height <= 0)) {
            handleError(0, display, window, 0, BadValue, 0);

            /* The move half of a combined request is already committed to
             * ws->x/y above (the non-popup branch also moved its SDL window
             * eagerly). Apply the deferred popup reposition here too before
             * bailing on the bad resize, so the popup surface does not diverge
             * from ws->x/y and leave pointer translation offset.
             */
            if (popupNeedsReposition)
                repositionAnchoredPopup(window);
            if (moveSnapshot.surface)
                SDL_FreeSurface(moveSnapshot.surface);
            return False;
        }
        if (HAS_VALUE(value_mask, CWWidth))
            width = values->width;
        if (HAS_VALUE(value_mask, CWHeight))
            height = values->height;
#ifdef DEBUG_WINDOWS
        printWindowsHierarchy();
#endif
        LOG("configureWindow: window=%lu old=(%ux%u) new=(%ux%u) "
            "mappedTopLevel=%d\n",
            window, (unsigned) oldWidth, (unsigned) oldHeight, (unsigned) width,
            (unsigned) height, isMappedTopLevelWindow);
        if ((unsigned int) width != windowStruct->w ||
            (unsigned int) height != windowStruct->h) {
            if (isMappedTopLevelWindow) {
                /* SDL_SetWindowSize makes SDL post RESIZED/SIZE_CHANGED for
                 * this window. This resize is client-initiated and already gets
                 * its ConfigureNotify from the postEvent below, so arm the echo
                 * suppressor and pump synchronously to let the filter drop the
                 * echoed SDL events before disarming. Without this a client
                 * XResizeWindow would deliver two ConfigureNotifies.
                 * width/height are physical X11 pixels (top-levels are promoted
                 * to the backing size). SDL_SetWindowSize/GetWindowSize speak
                 * logical points, so convert through the top-level's effective
                 * HiDPI scale the same way snapTopLevelToResizeIncrements does;
                 * otherwise a 2x-Retina XResizeWindow would request a window
                 * twice the intended size and then record the logical size as
                 * physical. Self-scaling toolkits such as Motif/Tk keep logical
                 * geometry, so effectiveHiDpiScale returns 1.0 and the
                 * conversion is a no-op, as at scale 1.0 (Linux / CI dummy).
                 */
                double scaleX = effectiveHiDpiScaleX(windowStruct);
                double scaleY = effectiveHiDpiScaleY(windowStruct);
                int logicalWidth = (int) lround((double) width / scaleX);
                int logicalHeight = (int) lround((double) height / scaleY);
                SDL_AtomicSet(&windowStruct->suppressSdlResizeEcho, 1);
                SDL_SetWindowSize(windowStruct->sdlWindow, logicalWidth,
                                  logicalHeight);
                SDL_PumpEvents();
                SDL_AtomicSet(&windowStruct->suppressSdlResizeEcho, 0);
                SDL_GetWindowSize(windowStruct->sdlWindow, &logicalWidth,
                                  &logicalHeight);
                width = (int) lround((double) logicalWidth * scaleX);
                height = (int) lround((double) logicalHeight * scaleY);
                LOG("configureWindow: SDL reports actual=(%ux%u)\n",
                    (unsigned) width, (unsigned) height);
            }
            windowStruct->w = (unsigned int) width;
            windowStruct->h = (unsigned int) height;
            if (windowStruct->sdlTexture)
                resizeWindowTexture(window);
            if (isMappedTopLevelWindow && windowStruct->popupParent != None)
                popupNeedsReposition = True;
            hasChanged = True;
            resizedWindow = True;
            resizedWidth = width;
            resizedHeight = height;
        }
    }

    /* Re-run an anchored popup's positioner exactly once, after both the move
     * and resize blocks have settled windowStruct->x/y and the SDL window size.
     * An xdg_popup's geometry is fixed by its positioner at creation, and
     * SDL_SetWindowSize only grows the surface buffer, so without this the
     * compositor keeps clipping a menu resized after mapping (Motif switching
     * the armed pulldown from a short menu to a taller one) to its creation
     * rectangle. Doing it here, rather than inside each block, repositions a
     * combined move+resize a single time at the final geometry instead of first
     * at the stale size. Only anchored popups need it (popupParent is set only
     * on the SDL3 popup path); absolute top-levels move and resize through the
     * SDL_SetWindowPosition/Size calls above alone.
     */
    if (popupNeedsReposition)
        repositionAnchoredPopup(window);

    if (!hasChanged)
        return True;

    /* Any move/resize/restack on this window invalidates the sibling-occlusion
     * clip for everything under the same top-level: lower siblings may newly
     * see, and previously occluded windows may now expose pixels under the
     * moved one. ensureVisibleRegion recomputes lazily on demand; this call
     * site only marks the cache stale.
     */
    invalidateVisibleRegionForTopLevel(window);
    if (!postEvent(display, window, ConfigureNotify)) {
        if (moveSnapshot.surface)
            SDL_FreeSurface(moveSnapshot.surface);
        return False;
    }
    if (resizedWindow)
        postResizeConfigureForMappedChildren(
            display, window, oldWidth, oldHeight, resizedWidth, resizedHeight);

    if (isMappedTopLevelWindow && windowStruct->sdlWindow) {
        replayTargetOfferWindow(SDL_GetWindowID(windowStruct->sdlWindow),
                                windowStruct->x, windowStruct->y,
                                (int) windowStruct->w, (int) windowStruct->h);
    }

    timelineTapConfigure(window, windowStruct->x, windowStruct->y,
                         windowStruct->w, windowStruct->h);
    if (windowStruct->mapState != UnMapped &&
        (oldX != windowStruct->x || oldY != windowStruct->y ||
         (unsigned int) oldWidth != windowStruct->w ||
         (unsigned int) oldHeight != windowStruct->h)) {
        if (oldX != windowStruct->x || oldY != windowStruct->y) {
            postParentExposureForOldArea(display, window, oldX, oldY, oldWidth,
                                         oldHeight, window);
            Bool sameSize = (unsigned int) oldWidth == windowStruct->w &&
                            (unsigned int) oldHeight == windowStruct->h;
            Bool restored =
                sameSize && restoreMovedWindowSnapshot(window, &moveSnapshot);
            if (!sameSize && moveSnapshot.surface)
                SDL_FreeSurface(moveSnapshot.surface);
            postMovedWindowExposure(display, window, !restored);
        } else {
            Bool postedFullWindowExpose =
                postResizeExpose(display, window, oldWidth, oldHeight);

            /* Skip this full-window Expose when postResizeExpose already
             * emitted one (the mapped top-level whole-window-clear path), so
             * the client does not receive two identical full-window exposes.
             */
            if (isMappedTopLevelWindow && !postedFullWindowExpose) {
                SDL_Rect fullWindow = {
                    0,
                    0,
                    (int) windowStruct->w,
                    (int) windowStruct->h,
                };
                postExposeEvent(display, window, &fullWindow, 1);
            }
            if ((unsigned int) oldWidth > windowStruct->w ||
                (unsigned int) oldHeight > windowStruct->h) {
                /* Shrink in place: the window keeps its top-left content, so it
                 * must stay clipped; only the vacated L-shape is cleared.
                 */
                postParentExposureForOldArea(display, window, oldX, oldY,
                                             oldWidth, oldHeight, None);
            }
        }
    }
    return True;
}
