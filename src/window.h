#ifndef _WINDOW_H_
#define _WINDOW_H_

#include "sdl-compat.h"
#include <pixman.h>
#include "window-debug.h"
#include "resource-types.h"
#include "util.h"

typedef struct {
    Atom property;
    int dataFormat;
    unsigned int dataLength;
    Atom type;
    unsigned char *data;
} WindowProperty;

typedef enum {
    UnMapped,
    Mapped,
    /* A child mapped while an ancestor is unmapped keeps MapRequested until
     * that ancestor becomes mapped. Unmapping an ancestor does not demote
     * already mapped descendants; XGetWindowAttributes reports them as
     * IsUnviewable while any ancestor is unmapped.
     */
    MapRequested
} MapState;

typedef struct {
    /* Parent window of this window, never NULL (except SCREEN_WINDOW). */
    Window parent;
    /* List of children */
    Array children;
    /* This is the drawing target of the window and its children while it is
     * unmapped. Might be NULL.
     */
    SDL_Texture *sdlTexture;
    /* This is the SDL Window handler to the real window of this window. Only
     * set if this window is a mapped top level window.
     */
    SDL_Window *sdlWindow;
    /* Cached SDL_MetalView (a CAMetalLayer-backed view) created once by the
     * optional GLX layer for on-screen ANGLE rendering, reused across surface
     * rebuilds so resizes do not stack layers or leak views. void * to avoid a
     * hard SDL_Metal type dependency here; NULL unless GLX bound this window.
     */
    void *glxMetalView;
    /* Composite scratch for an offscreen GLX child widget: a streaming staging
     * texture and a CPU row-flip buffer, both sized to glxCompositeW x
     * glxCompositeH and reused across frames. glxCompositeToWindow reallocates
     * them only when the widget's composite size changes, so a steadily
     * animating GL canvas does no per-frame allocation. NULL/0 until GLX
     * composites this window; torn down with the rest of the SDL resources.
     */
    SDL_Texture *glxCompositeTexture;
    unsigned char *glxCompositeFlip;
    int glxCompositeW;
    int glxCompositeH;
    /* Readback scratch for the same widget, filled by glReadPixels before the
     * flip. Cached here (grown on demand, sized by glxReadbackCap) so it is
     * freed with the window and never leaks per thread; see
     * glxAcquireCompositeReadback.
     */
    unsigned char *glxReadbackBuf;
    size_t glxReadbackCap;
    /* True when a top-level window's backing texture must be copied to its
     * SDL_Window surface on the next flush/sync.
     */
    Bool needsPresent;
    Bool hasPresentRect;
    Bool loggedPresentScale;
    SDL_Rect presentRect;
    /* Dirty region in window-local coords for the next present.
     * drawWindowDataToScreen walks this region and emits one
     * SDL_RenderReadPixels per rect, then submits the whole set via
     * SDL_UpdateWindowSurfaceRects. fullyDirty is a sentinel that collapses the
     * region to the entire window when the rect budget (LIBX11_COMPAT_
     * DIRTY_MAX_RECTS, default 16) is exceeded or a NULL-rect mark arrives; the
     * read side then falls back to a single full-window readback. Initialized
     * in initWindowStruct; finalized in destroyWindow / destroyScreenWindow.
     */
    pixman_region32_t dirty;
    Bool fullyDirty;
    /* True after a mapped top-level window has completed at least one
     * successful update to its SDL_Window surface.
     */
    Bool hasPresented;
    Bool contentsMergedToParent;
    /* The renderer of this window. Only set if sdlWindow or sdlTexture is set.
     */
    SDL_Renderer *sdlRenderer;
    /* The position of this window relative to its parent. */
    int x, y;
    /* The dimensions of this window. */
    unsigned int w, h;
    Bool inputOnly;
    Visual *visual;
    Colormap colormap;
    unsigned long backgroundColor;
    Pixmap background;
    int colormapWindowsCount;
    Window *colormapWindows;
    Array properties;
    /* The window name. Only used if this window has a corresponding sdlWindow.
     */
    char *windowName;
    /* The icon of this window. Only used if this window has a corresponding
     * sdlWindow.
     */
    SDL_Surface *icon;
    Cursor cursor;
    unsigned int borderWidth;
    unsigned long borderPixel;
    Pixmap borderPixmap;
    int depth;
    int bitGravity;
    int winGravity;
    /* Indicates if this window is Mapped, if mapping it is requested or if it
     * is Unmapped.
     */
    MapState mapState;
    long eventMask;
    Bool overrideRedirect;
    Bool internal;
    SDL_Surface *shapeBoundingMask;
    int shapeBoundingOffsetX;
    int shapeBoundingOffsetY;
    SDL_Surface *shapeClipMask;
    int shapeClipOffsetX;
    int shapeClipOffsetY;
    /* WM_TRANSIENT_FOR target stored before either window is realized;
     * applyTransientForRelationship reads this at realize time to wire
     * SDL_SetWindowModalFor once the modal flag is also set.
     */
    Window deferredTransientParent;
    Bool deferredTransientApplied;
    /* Cached sibling-occlusion clip in window-local coords. Equals (0,0,w,h)
     * minus the union of higher siblings on this window's parent, walked up the
     * ancestor chain. Recomputed lazily through ensureVisibleRegion;
     * invalidated by configureWindow and the restack helpers when stacking,
     * geometry, or mapping changes. Drawing primitives in src/drawing.c
     * intersect each clip rect with this region before issuing
     * SDL_RenderSetClipRect, so a higher sibling cannot be drawn through by a
     * lower one.
     */
    pixman_region32_t visibleRegion;
    Bool visibleRegionValid;
#ifdef DEBUG_WINDOWS
    /* Random id used for debugging. */
    unsigned long debugId;
#endif /* DEBUG_WINDOWS */
} WindowStruct;

#include "window-internal.h"

extern Window SCREEN_WINDOW;

#define GET_VISUAL(window) GET_WINDOW_STRUCT(window)->visual
#define GET_COLORMAP(window) GET_WINDOW_STRUCT(window)->colormap
#define GET_PARENT(window) GET_WINDOW_STRUCT(window)->parent
#define GET_CHILDREN(window) \
    ((Window *) GET_WINDOW_STRUCT(window)->children.array)
#define IS_TOP_LEVEL(window) \
    (window != SCREEN_WINDOW && GET_PARENT(window) == SCREEN_WINDOW)
#define IS_MAPPED_TOP_LEVEL_WINDOW(window) \
    (IS_TOP_LEVEL(window) && GET_WINDOW_STRUCT(window)->sdlWindow)
#define IS_INPUT_ONLY(window) GET_WINDOW_STRUCT(window)->inputOnly
#define GET_WINDOW_POS(window, out_x, out_y) \
    out_x = GET_WINDOW_STRUCT(window)->x;    \
    out_y = GET_WINDOW_STRUCT(window)->y

#define GET_WINDOW_DIMS(window, width, height) \
    width = GET_WINDOW_STRUCT(window)->w;      \
    height = GET_WINDOW_STRUCT(window)->h
#define HAS_VALUE(valueMask, value) (value & valueMask)

#endif /* _WINDOW_H_ */
