#include "window-internal.h"
#include <stdint.h>
#include "drawing.h"
#include "events.h"
#include "display.h"
#include "font.h"
#include "image.h"
#include "input.h"
#include "colors.h"
#include "replay-target.h"

Window SCREEN_WINDOW = None;

static void ensureMappingListLock(void);

static unsigned long resolvedWindowBackgroundColor(Window window)
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
    windowStruct->inputOnly = inputOnly;
    windowStruct->colormap = colormap;
    windowStruct->visual = visual;
    windowStruct->sdlTexture = NULL;
    windowStruct->sdlWindow = NULL;
    windowStruct->needsPresent = False;
    windowStruct->hasPresentRect = False;
    windowStruct->presentRect = (SDL_Rect) {0, 0, 0, 0};
    windowStruct->hasPresented = False;
    windowStruct->contentsMergedToParent = False;
    windowStruct->sdlRenderer = NULL;
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
    windowStruct->shapeBoundingMask = NULL;
    windowStruct->shapeBoundingOffsetX = 0;
    windowStruct->shapeBoundingOffsetY = 0;
    windowStruct->shapeClipMask = NULL;
    windowStruct->shapeClipOffsetX = 0;
    windowStruct->shapeClipOffsetY = 0;
#ifdef DEBUG_WINDOWS
    windowStruct->debugId = ((unsigned long) rand() << 16) | rand();
#endif /* DEBUG_WINDOWS */
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

void destroyScreenWindow(Display *display)
{
    if (SCREEN_WINDOW != None) {
        size_t i;
        Window *children = GET_CHILDREN(SCREEN_WINDOW);
        WindowStruct *windowStruct = GET_WINDOW_STRUCT(SCREEN_WINDOW);
        for (i = 0; i < windowStruct->children.length; i++) {
            destroyWindow(display, children[i], False);
        }
        invalidatePutImageStagingTexture(windowStruct->sdlRenderer);
        invalidateTextCacheForRenderer(windowStruct->sdlRenderer);
        SDL_DestroyRenderer(windowStruct->sdlRenderer);
        windowStruct->sdlRenderer = NULL;
        SDL_DestroyWindow(windowStruct->sdlWindow);
        freeArray(&windowStruct->children);
        free(windowStruct);
        FREE_XID(SCREEN_WINDOW);
        SCREEN_WINDOW = None;
    }
}

WindowSdlIdMapper *mappingListStart = NULL;

/* The mapping list is touched both by client threads (XCreateWindow,
 * XDestroyWindow) and by the SDL event-pump thread which translates
 * SDL_WINDOW events back to X Windows. Without a lock the linked-list
 * insert/unlink races, and a concurrent malloc/free corrupts the chain. */
static SDL_mutex *mappingListLock = NULL;

/* initScreenWindow primes the mutex on the single-threaded XOpenDisplay
 * path before SDL_Window / event-pump threads can reach the register /
 * lookup helpers; subsequent callers are harmless no-ops. If
 * SDL_CreateMutex fails we leave the lock NULL and the lock/unlock
 * wrappers below skip the SDL call. This is better than crashing on an
 * unrecoverable allocator fail. */
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
        if (mapper->sdlWindowId == sdlWindowId) {
            return mapper;
        }
    }
    return NULL;
}

void deleteWindowMapping(Window window)
{
    ensureMappingListLock();
    lockMappingList();
    /* Indirect pointer walk: link points at the slot that references the
     * current node, so unlinking the head needs no special case. */
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

Window getWindowFromId(Uint32 sdlWindowId)
{
    /* Read the Window value while holding the lock; returning the node
     * pointer would expose a use-after-free window if another thread
     * unlinked and freed the node between unlock and dereference. */
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

Window getContainingWindow(Window window, int x, int y)
{
    int i, child_x, child_y, child_w, child_h;
    Window *children = GET_CHILDREN(window);
    for (i = GET_WINDOW_STRUCT(window)->children.length - 1; i >= 0; i--) {
        if (!isWindowEffectivelyViewable(children[i]))
            continue;
        GET_WINDOW_POS(children[i], child_x, child_y);
        GET_WINDOW_DIMS(children[i], child_w, child_h);
        if (x >= child_x && x < child_x + child_w && y >= child_y &&
            y < child_y + child_h) {
            return getContainingWindow(children[i], x - child_x, y - child_y);
        }
    }
    return window;
}

void removeChildFromParent(Window child)
{
    if (child == SCREEN_WINDOW) {
        return;
    }
    Window parent = GET_PARENT(child);
    if (parent != None) {
        ssize_t childIndex =
            findInArray(&GET_WINDOW_STRUCT(parent)->children, (void *) child);
        if (childIndex != -1) {
            removeArray(&GET_WINDOW_STRUCT(parent)->children,
                        (size_t) childIndex, True);
        }
    }
}

void destroyWindow(Display *display, Window window, Bool freeParentData)
{
    /* Drain pre-cascade stale events for this window FIRST, before any
     * recursion. A focused descendant whose revert lands on us would
     * otherwise queue FocusIn(window) during the recursion, and a
     * naive discard at the end of this function would erase it. The
     * order is: drain own stale events -> recurse (children's reverts
     * may queue events for us, all survive) -> own revert -> teardown
     * -> DestroyNotify. */
    discardQueuedEventsForWindow(display, window);

    size_t i;
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    Window *children = GET_CHILDREN(window);
    for (i = 0; i < windowStruct->children.length; i++) {
        destroyWindow(display, children[i], False);
    }

    /* Auto-revert per Xlib spec; post-order recursion means each
     * ancestor's check sees the updated focus as the cascade unwinds. */
    revertKeyboardFocusForDestroyedWindow(display, window);

    /* Drop any passive button grabs on this window so a later XID
     * reuse cannot route events through a stale grab entry. */
    releaseButtonGrabsForWindow(window);
    /* Clear cached pointer-target XIDs so a queued SDL motion event
     * does not drive postPointerCrossingEvents -> buildWindowPathToRoot
     * into this window's freed WindowStruct. ASan caught this on the
     * test-xtest path where XDestroyWindow ran before the SDL motion
     * queue drained. */
    clearPointerStateForWindow(window);
    freeArray(&windowStruct->children);
    XFreeColormap(display, GET_COLORMAP(window));
    for (i = 0; i < windowStruct->properties.length; i++) {
        freeWindowProperty(windowStruct->properties.array[i]);
    }
    freeArray(&windowStruct->properties);
    if (windowStruct->windowName) {
        free(windowStruct->windowName);
    }
    if (windowStruct->icon) {
        SDL_FreeSurface(windowStruct->icon);
    }
    if (windowStruct->shapeBoundingMask)
        SDL_FreeSurface(windowStruct->shapeBoundingMask);
    if (windowStruct->shapeClipMask)
        SDL_FreeSurface(windowStruct->shapeClipMask);
    free(windowStruct->colormapWindows);
    if (windowStruct->sdlRenderer) {
        invalidatePutImageStagingTexture(windowStruct->sdlRenderer);
        invalidateTextCacheForRenderer(windowStruct->sdlRenderer);
        SDL_DestroyRenderer(windowStruct->sdlRenderer);
    }
    if (windowStruct->sdlTexture) {
        SDL_DestroyTexture(windowStruct->sdlTexture);
    }
    if (windowStruct->sdlWindow) {
        replayTargetForgetWindow(SDL_GetWindowID(windowStruct->sdlWindow));
        SDL_DestroyWindow(windowStruct->sdlWindow);
    }
    deleteWindowMapping(window);
    postEvent(display, window, DestroyNotify);
    if (freeParentData) {
        removeChildFromParent(window);
    }
    free(windowStruct);
    SET_XID_VALUE(window, NULL);
    SET_XID_TYPE(window, CLOSED_WINDOW);
}

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
    if (findInArray(children, (void *) child) >= 0) {
        return False;
    }
    if (!insertArray(children, (void *) child)) {
        return False;
    }
    if (index >= children->length) {
        index = children->length - 1;
    }
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
        if (parent == window1) {
            return True;
        }
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
    return True;
}

/* Returns True when the geometric rectangles of a and b overlap.
 * Endpoints are computed in int64_t so x + w cannot overflow when the
 * window struct holds extreme coordinates or dimensions.
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

/* TopIf/BottomIf/Opposite are conditional restacks. "Occludes" means an
 * upper sibling overlaps window; "occluded by" means a lower sibling
 * overlaps window. An unconditional raise/lower would violate the
 * Xlib spec when no overlap is present.
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
        return moveChildToIndex(window, target);
    }

    size_t idx = (size_t) index;
    switch (mode) {
    case Above:
        return moveChildToIndex(window, children->length - 1);
    case Below:
        return moveChildToIndex(window, 0);
    case TopIf:
        if (hasOccludingSiblingAbove(children, idx))
            return moveChildToIndex(window, children->length - 1);
        return True;
    case BottomIf:
        if (hasOccludedSiblingBelow(children, idx))
            return moveChildToIndex(window, 0);
        return True;
    case Opposite:
        if (hasOccludingSiblingAbove(children, idx))
            return moveChildToIndex(window, children->length - 1);
        if (hasOccludedSiblingBelow(children, idx))
            return moveChildToIndex(window, 0);
        return True;
    default:
        handleError(0, display, window, 0, BadValue, 0);
        return False;
    }
}

static void postParentExposureForOldArea(Display *display,
                                         Window window,
                                         int oldX,
                                         int oldY,
                                         int oldWidth,
                                         int oldHeight)
{
    Window parent = GET_PARENT(window);
    if (parent == None || IS_INPUT_ONLY(parent) ||
        GET_WINDOW_STRUCT(parent)->mapState == UnMapped)
        return;
    XClearArea(display, parent, oldX, oldY, (unsigned int) oldWidth,
               (unsigned int) oldHeight, False);
    SDL_Rect exposed = {oldX, oldY, oldWidth, oldHeight};
    postExposeEvent(display, parent, &exposed, 1);
}

static void postMovedWindowExposure(Display *display, Window window)
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
    XClearArea(display, window, expose.x, expose.y, (unsigned int) expose.w,
               (unsigned int) expose.h, False);
    postExposeEvent(display, window, &expose, 1);
}

static void postResizeExpose(Display *display,
                             Window window,
                             int oldWidth,
                             int oldHeight)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (windowStruct->inputOnly)
        return;
    if (!IS_MAPPED_TOP_LEVEL_WINDOW(window)) {
        SDL_Rect fullWindow = {
            0,
            0,
            (int) windowStruct->w,
            (int) windowStruct->h,
        };
        XClearArea(display, window, 0, 0, (unsigned int) fullWindow.w,
                   (unsigned int) fullWindow.h, False);
        postExposeEvent(display, window, &fullWindow, 1);
        return;
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
    if (count > 0) {
        postExposeEvent(display, window, rects, count);
    }
}

void freeWindowProperty(WindowProperty *property)
{
    if (!property) {
        return;
    }
    free(property->data);
    free(property);
}

void resizeWindowTexture(Window window)
{
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    if (!windowStruct->sdlTexture)
        return;
    SDL_Renderer *windowRenderer =
        GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;
    if (!windowRenderer)
        return;
    /* Caller must record the new dimensions in windowStruct->w/h before
     * calling. The SDL window may not actually be the target size yet
     * (resize events may be faked in tests), so we cannot re-query via
     * SDL_GetWindowSize here. */
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
    windowStruct->sdlTexture = newTexture;
    windowStruct->needsPresent = True;
    windowStruct->hasPresentRect = False;
    markWindowNeedsPresent(window);
    SDL_SetRenderTarget(windowRenderer,
                        prevTarget == oldTexture ? newTexture : prevTarget);
    SDL_DestroyTexture(oldTexture);
    invalidateSdlDrawStateCache();
}

Bool mergeWindowDrawables(Window parent, Window child)
{
    WindowStruct *childWindowStruct = GET_WINDOW_STRUCT(child);
    if (!childWindowStruct->sdlTexture) {
        return True;
    }
    SDL_Renderer *parentRenderer = getWindowRenderer(parent);
    if (childWindowStruct->sdlRenderer) {
        SDL_RenderPresent(childWindowStruct->sdlRenderer);
    }
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
        invalidatePutImageStagingTexture(childWindowStruct->sdlRenderer);
        invalidateTextCacheForRenderer(childWindowStruct->sdlRenderer);
        SDL_DestroyRenderer(childWindowStruct->sdlRenderer);
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
    if (window == SCREEN_WINDOW)
        return True;
    Bool hasChanged = False;
    WindowStruct *windowStruct = GET_WINDOW_STRUCT(window);
    /* There is no external window-manager client in the SDL-backed
     * compatibility model. Honor the configure directly even if a toolkit
     * selected SubstructureRedirectMask internally. */
    Bool isMappedTopLevelWindow = IS_MAPPED_TOP_LEVEL_WINDOW(window);
    int oldX, oldY, oldWidth, oldHeight;
    GET_WINDOW_POS(window, oldX, oldY);
    GET_WINDOW_DIMS(window, oldWidth, oldHeight);
    if (!restackWindow(display, window, value_mask, values)) {
        return False;
    }
    if (HAS_VALUE(value_mask, CWStackMode)) {
        hasChanged = True;
    }
    if (HAS_VALUE(value_mask, CWX) || HAS_VALUE(value_mask, CWY)) {
        int x = oldX, y = oldY;
        if (HAS_VALUE(value_mask, CWX)) {
            x = values->x;
        }
        if (HAS_VALUE(value_mask, CWY)) {
            y = values->y;
        }
        if (isMappedTopLevelWindow) {
            SDL_SetWindowPosition(windowStruct->sdlWindow, x, y);
            SDL_GetWindowPosition(windowStruct->sdlWindow, &windowStruct->x,
                                  &windowStruct->y);
        } else {
            windowStruct->x = x;
            windowStruct->y = y;
        }
        if (oldX != windowStruct->x || oldY != windowStruct->y) {
            hasChanged = True;
        }
    }
    if (HAS_VALUE(value_mask, CWWidth) || HAS_VALUE(value_mask, CWHeight)) {
        int width = oldWidth, height = oldHeight;
        if (HAS_VALUE(value_mask, CWWidth)) {
            width = values->width;
            if (width <= 0) {
                handleError(0, display, None, 0, BadValue, 0);
                return False;
            }
        }
        if (HAS_VALUE(value_mask, CWHeight)) {
            height = values->height;
            if (height <= 0) {
                handleError(0, display, None, 0, BadValue, 0);
                return False;
            }
        }
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
                SDL_SetWindowSize(windowStruct->sdlWindow, width, height);
                SDL_GetWindowSize(windowStruct->sdlWindow, &width, &height);
                LOG("configureWindow: SDL reports actual=(%ux%u)\n",
                    (unsigned) width, (unsigned) height);
            }
            windowStruct->w = (unsigned int) width;
            windowStruct->h = (unsigned int) height;
            if (windowStruct->sdlTexture) {
                resizeWindowTexture(window);
            }
            hasChanged = True;
            Window *children = GET_CHILDREN(window);
            for (size_t i = 0; i < windowStruct->children.length; i++) {
                Window child = children[i];
                WindowStruct *childStruct = GET_WINDOW_STRUCT(child);
                if (childStruct->mapState == Mapped &&
                    childStruct->winGravity != ForgetGravity) {
                    int dx, dy;
                    applyChildResizeGravity(child, oldWidth, oldHeight, width,
                                            height, &dx, &dy);
                    if (dx == 0 && dy == 0)
                        continue;
                    postEvent(display, child, GravityNotify);
                }
            }
        }
    }
    if (!hasChanged)
        return True;
    if (!postEvent(display, window, ConfigureNotify)) {
        return False;
    }
    if (windowStruct->mapState != UnMapped &&
        (oldX != windowStruct->x || oldY != windowStruct->y ||
         (unsigned int) oldWidth != windowStruct->w ||
         (unsigned int) oldHeight != windowStruct->h)) {
        if (oldX != windowStruct->x || oldY != windowStruct->y) {
            postParentExposureForOldArea(display, window, oldX, oldY, oldWidth,
                                         oldHeight);
            postMovedWindowExposure(display, window);
        } else {
            postResizeExpose(display, window, oldWidth, oldHeight);
            if (isMappedTopLevelWindow) {
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
                postParentExposureForOldArea(display, window, oldX, oldY,
                                             oldWidth, oldHeight);
            }
        }
    }
    return True;
}
