#ifndef WINDOWINTERNAL_H
#define WINDOWINTERNAL_H

#include <pixman.h>
#include "window.h"

typedef struct _WindowSdlIdMapper {
    Window window;
    Uint32 sdlWindowId;
    struct _WindowSdlIdMapper *next;
} WindowSdlIdMapper;

void initWindowStruct(WindowStruct *windowStruct,
                      int x,
                      int y,
                      unsigned int width,
                      unsigned int height,
                      Visual *visual,
                      Colormap colormap,
                      Bool inputOnly,
                      unsigned long backgroundColor,
                      Pixmap backgroundPixmap);
Bool initScreenWindow(Display *display);
Window getWindowFromId(Uint32 sdlWindowId);

/* Resolve a window's effective background colour, walking up through
 * ParentRelative backgrounds, with an opaque alpha forced on. Used by the
 * present path to fill the margin newly exposed during a live host resize so it
 * shows a stable background colour instead of flickering garbage.
 */
unsigned long resolvedWindowBackgroundColor(Window window);
size_t collectMappedOverrideRedirectWindows(Window *out, size_t max);
Window findPopupParentToplevel(Window popup, int *offsetX, int *offsetY);
size_t collectPopupChildren(Window parent, Window *out, size_t max);
void drainAnchoredPopups(Display *display, Window window);
void repositionAnchoredPopup(Window window);
void unrealizeTopLevelWindow(Display *display, Window window);
void destroyScreenWindow(Display *display);
void destroyWindow(Display *display, Window window, Bool freeParentData);
Window getContainingWindow(Window window, int x, int y);
Window getDirectChildContainingPoint(Window window, int x, int y);
Bool windowPointInsideBoundingShape(WindowStruct *windowStruct, int x, int y);
void windowAbsoluteOrigin(Window window, int *xReturn, int *yReturn);

/* Saturate an int64 to the int range. Shared by the popup-anchor geometry in
 * window.c and window-internal.c so a hostile coordinate clamps instead of
 * wrapping.
 */
int clampToIntRange(int64_t v);

#ifndef LIBX11_COMPAT_SDL3
/* Force a present on every mapped top-level other than exclude that overlaps
 * rect. Used on SDL2 to repaint siblings a torn-down popup or a moved window
 * uncovered, since Xvnc does not reliably deliver their Expose. Scope is
 * intentionally the three paths that shrink a top-level's on-screen footprint
 * for real on SDL2: unmap, destroy, and a window-manager move. A client
 * XLowerWindow/XRestackWindows only reorders our internal child list (FVWM owns
 * the SDL/X stacking, so nothing is uncovered on screen), and a client shrink
 * via XResizeWindow is not covered here yet.
 */
void repaintTopLevelsOverlappingRect(Window exclude, SDL_Rect rect);
#endif
void translateWindowPoint(Window sourceWindow,
                          Window destinationWindow,
                          int sourceX,
                          int sourceY,
                          int *destinationXReturn,
                          int *destinationYReturn);
Bool addChildToWindow(Window parent, Window child);
Bool insertChildIntoWindow(Window parent, Window child, size_t index);
Bool moveChildToIndex(Window window, size_t targetIndex);
Bool moveChildToIndexAndExpose(Display *display,
                               Window window,
                               size_t targetIndex);
void removeChildFromParent(Window child);
Bool windowsOverlap(Window a, Window b);
void resizeWindowTexture(Window window);
void postResizeConfigureForMappedChildren(Display *display,
                                          Window window,
                                          int oldWidth,
                                          int oldHeight,
                                          int newWidth,
                                          int newHeight);
void unmapWindowInternal(Display *display, Window window, Bool fromConfigure);
void deleteWindowMapping(Window window);
void registerWindowMapping(Window window, Uint32 sdlWindowId);
void topLevelWindowHostPosition(Window window,
                                int logicalX,
                                int logicalY,
                                int *hostX,
                                int *hostY);
void topLevelWindowLogicalPosition(Window window,
                                   int hostX,
                                   int hostY,
                                   int *logicalX,
                                   int *logicalY);
Bool isParent(Window window1, Window window2);
void releaseActiveGrabsForUnviewableWindow(Display *display, Window window);
Bool isWindowEffectivelyViewable(Window window);
WindowProperty *findProperty(Array *properties, Atom property, size_t *index);
void freeWindowProperty(WindowProperty *property);
Bool mergeWindowDrawables(Window parent, Window child);
void mapRequestedChildren(Display *display, Window window);
Bool configureWindow(Display *display,
                     Window window,
                     unsigned long value_mask,
                     XWindowChanges *values);

/* Abort unless running on the main event thread. Enforces the "SDL work runs on
 * the main thread" invariant at the SDL sources in the window layer;
 * unconditional (not assert) so NDEBUG cannot turn a controlled abort into a
 * silent off-main SDL crash on macOS. who names the caller for the diagnostic.
 */
void requireMainEventThread(const char *who);

/* Route a void(Window) operation to the main event thread when called off it,
 * so its SDL calls run where macOS requires. On the main thread (the hot
 * event-handling path) this costs one atomic load then a direct call. An op
 * self-routes by passing itself: the routed invocation re-enters on the main
 * thread, where the same guard now passes and the body runs. Used by the
 * WM-state appliers, which XChangeProperty / XSetWMNormalHints can reach from a
 * client worker thread.
 */
void runWindowOpOnMain(void (*fn)(Window), Window window);

Window createInternalWindow(Display *display,
                            Window parent,
                            int x,
                            int y,
                            unsigned int width,
                            unsigned int height,
                            unsigned int border_width,
                            int depth,
                            unsigned int clazz,
                            Visual *visual,
                            unsigned long valueMask,
                            XSetWindowAttributes *attributes);

/* Initialize region to the given rect, or to an empty region when the rect is
 * empty or its right or bottom edge (x + w, y + h) would overflow pixman's
 * int32 coordinate space. Motif transiently sizes widget windows (unsigned)-1
 * before geometry management runs; feeding that to pixman_region32_init_rect
 * overflows the edge, which logs "Invalid rectangle passed" and clips the
 * window to nothing (the empty-gray dialog bug). Any rect that fits is passed
 * through unchanged, so legitimately large windows keep their exact region.
 */
void regionInitRectSafe(pixman_region32_t *region,
                        int x,
                        int y,
                        unsigned int w,
                        unsigned int h);

/* Sibling-occlusion clipping support. computeVisibleRegion writes the
 * caller-owned region (which must be uninitialized) and is the slow path;
 * drawing code should go through ensureVisibleRegion, which recomputes only
 * when the cached region was invalidated. invalidateVisibleRegionForTopLevel
 * walks the nearest top-level subtree and marks every descendant's cached
 * region stale.
 */
void computeVisibleRegion(Window window, pixman_region32_t *out);
const pixman_region32_t *ensureVisibleRegion(Window window);
void invalidateVisibleRegionForTopLevel(Window window);

/* Tear down a window's renderer. Every per-renderer texture cache (put-image
 * staging, glyph text, stipple stamps) must be dropped before the renderer is
 * destroyed, because SDL frees a renderer's textures with it and a surviving
 * cache entry would then dangle. Routing all destroys through here keeps the
 * invalidation set in one place instead of copied at each call site. No-op on a
 * NULL renderer.
 */
void destroyWindowRenderer(SDL_Renderer *renderer);

/* Motif WM hints bit layout. Used by both the XChangeProperty _MOTIF_WM_HINTS
 * write routing and the synthesized read in XGetWindowProperty; centralizing
 * the constants here keeps the two paths in sync.
 */
enum {
    MWM_HINTS_FUNCTIONS = (1L << 0),
    MWM_HINTS_DECORATIONS = (1L << 1),
    MWM_HINTS_INPUT_MODE = (1L << 2),
    MWM_HINTS_STATUS = (1L << 3),

    MWM_FUNC_ALL = (1L << 0),
    MWM_FUNC_RESIZE = (1L << 1),
    MWM_FUNC_MOVE = (1L << 2),
    MWM_FUNC_MINIMIZE = (1L << 3),
    MWM_FUNC_MAXIMIZE = (1L << 4),
    MWM_FUNC_CLOSE = (1L << 5),

    MWM_DECOR_ALL = (1L << 0),
    MWM_DECOR_BORDER = (1L << 1),
    MWM_DECOR_RESIZEH = (1L << 2),
    MWM_DECOR_TITLE = (1L << 3),
    MWM_DECOR_MENU = (1L << 4),
    MWM_DECOR_MINIMIZE = (1L << 5),
    MWM_DECOR_MAXIMIZE = (1L << 6),

    MWM_INPUT_MODELESS = 0,
    MWM_INPUT_PRIMARY_APPLICATION_MODAL = 1,
    MWM_INPUT_SYSTEM_MODAL = 2,
    MWM_INPUT_FULL_APPLICATION_MODAL = 3,
};

/* Decode a stored _MOTIF_WM_HINTS property and apply functions/decorations to
 * the SDL window. Called from XChangeProperty after a write lands and from
 * top-level map/reparent paths once the SDL window exists and is mapped. Safe
 * to invoke when no _MOTIF_WM_HINTS property is stored: no-op.
 */
void applyMotifWmHintsFromProperty(Window window);

/* Returns True when the window has a stored _MOTIF_WM_HINTS property that
 * declares MWM_HINTS_FUNCTIONS. When set, the client owns the resizable
 * decision through the Motif path, so the WM_NORMAL_HINTS default below must
 * defer to it.
 */
Bool topLevelHasMotifFunctionHints(Window window);

/* Decode the stored WM_NORMAL_HINTS (XSizeHints) and set the SDL window's
 * resizable flag the way a real window manager would: resizable unless the
 * client pinned a fixed size (both PMinSize and PMaxSize present with min ==
 * max). No-op until the top-level is realized and mapped, when no
 * WM_NORMAL_HINTS is stored, or when _MOTIF_WM_HINTS already governs functions.
 */
void applyNormalHintsResizableFromProperty(Window window);

/* Pure decoder behind applyNormalHintsResizableFromProperty: returns whether
 * the stored WM_NORMAL_HINTS mark the top-level as resizable (resizable unless
 * a fixed size is pinned), deferring to _MOTIF_WM_HINTS when that governs
 * functions. Does not touch SDL, so it is observable independent of the video
 * driver.
 *
 * Returns False when no usable WM_NORMAL_HINTS is stored.
 */
Bool topLevelResizableFromNormalHints(Window window);

/* Refresh the top-level's cached resize increments (widthInc/heightInc, base
 * and min sizes used by the live-resize drag-end snap) from the stored
 * WM_NORMAL_HINTS property. XSetWMSizeHints caches these directly from the
 * XSizeHints struct, but a client that writes WM_NORMAL_HINTS via a raw
 * XChangeProperty never goes through that path, so the property write hook
 * calls this to decode the property bytes and keep the cache in sync. Clears
 * hasResizeInc when the property has no usable PResizeInc. No-op on non-windows
 * or when no decodable WM_NORMAL_HINTS is stored.
 */
void cacheResizeIncrementsFromNormalHintsProperty(Window window);

/* Returns True when the window's stored hints declare it modal, either via
 * _NET_WM_STATE_MODAL or MWM_INPUT_FULL_APPLICATION_MODAL.
 */
Bool windowIsModal(Window window);

/* Resolve transient_for + modal hints and call SDL_SetWindowModalFor when both
 * sides are realized and the child is marked modal. Defers via
 * deferredTransientParent until both windows exist.
 */
void applyTransientForRelationship(Window window);

/* Drop the deferred transient_for record and any live SDL_SetWindowModalFor
 * binding. Called from the WM_TRANSIENT_FOR property-delete path.
 */
void clearTransientForRelationship(Window window);

/* Apply a single _NET_WM_STATE atom transition (add/remove) to the SDL window
 * and mirror it into the stored _NET_WM_STATE property. action: 0 remove, 1
 * add, 2 toggle.
 */
void applyNetWmStateAction(Display *display,
                           Window window,
                           long action,
                           Atom stateAtom);

/* Decode the stored _NET_WM_STATE atom list and apply recognized state bits to
 * the SDL window. Safe before realization: no-op until an SDL window exists.
 */
void applyNetWmStateFromProperty(Window window);

/* The window state the in-process WM derives from a stored _NET_WM_STATE atom
 * list, and the table pairing each honored substate atom with the field it
 * sets. The root _NET_SUPPORTED advertisement is built from the same table, so
 * an atom can never be announced without being applied, or applied without
 * being announced. Two atoms may share a field (both MAXIMIZED axes drive one
 * SDL maximize).
 */
typedef struct {
    Bool modal;
    Bool fullscreen;
    Bool maximized;
    Bool hidden;
    Bool above;
} NetWmStateSet;

typedef struct {
    Atom atom;
    size_t offset; /* offsetof(NetWmStateSet, field) */
} NetWmStateHonoredAtom;

extern const NetWmStateHonoredAtom netWmStateHonored[];
extern const size_t netWmStateHonoredCount;

/* _NET_WM_WINDOW_TYPE values the property handler acts on by dropping the SDL
 * border. Same single-source-of-truth contract as netWmStateHonored above.
 */
extern const Atom netWmWindowTypeHonored[];
extern const size_t netWmWindowTypeHonoredCount;

/* Pure decoder behind the borderless hook: True when any of the count types
 * names an honored borderless type. Split out of the XChangeProperty handler
 * because SDL_SetWindowBordered is a no-op under the dummy video driver, so the
 * decision is only observable headlessly through this predicate. Tolerates a
 * NULL list.
 */
Bool netWmWindowTypeWantsBorderless(const Atom *types, unsigned int count);

/* Create the EWMH supporting-WM-check window and publish _NET_SUPPORTED,
 * _NET_SUPPORTING_WM_CHECK and the check window's _NET_WM_NAME. Idempotent: a
 * second display opened against the same root reuses the existing check window.
 */
void publishEwmhWmSupport(Display *display);

/* True for the bit gravities whose retained contents stay pinned at the
 * top-left (0,0) corner on resize: NorthWestGravity and StaticGravity. The
 * resize backing-copy and expose paths model only this anchor and discard/full-
 * clear every other gravity, so they share this one predicate to stay in sync.
 */
Bool bitGravityIsTopLeftAnchored(int bitGravity);

#endif /* WINDOWINTERNAL_H */
