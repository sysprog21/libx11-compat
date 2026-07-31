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

    /* Per-window accelerated present path. When a top-level window is realized
     * an accelerated SDL_Renderer is created directly on its SDL_Window; if
     * that succeeds the window presents its SCREEN-backing pixels through
     * presentRenderer (streaming texture -> SDL_RenderCopy ->
     * SDL_RenderPresent) instead of the SDL_GetWindowSurface software path,
     * giving a real Metal nextDrawable every frame and removing the
     * CAMetalLayer gravity race during live resize. This is a per-window
     * LIFETIME decision, taken once before any SDL_GetWindowSurface call (the
     * two are mutually exclusive on one window): presentUsesSoftware is set
     * True when the accelerated renderer cannot be created (dummy/headless
     * driver) or the kill switch forces it, and the window then keeps the
     * software path unchanged. presentTexture is a STREAMING RGBA8888 texture
     * on presentRenderer sized presentTexW x presentTexH, kept up to date by
     * uploading only dirty rects; presentReadback is the per-window CPU scratch
     * (grown on demand, sized by presentReadbackCap) that SDL_RenderReadPixels
     * fills before the upload. All torn down in destroyWindow.
     */
    SDL_Renderer *presentRenderer;
    SDL_Texture *presentTexture;
    int presentTexW;
    int presentTexH;
    unsigned char *presentReadback;
    size_t presentReadbackCap;
    Bool presentUsesSoftware;
    /* The position of this window relative to its parent. */
    int x, y;
    /* The dimensions of this window. */
    unsigned int w, h;

    /* Physical-pixel / SDL-logical-point ratio for this top-level window.
     * hiDpiPromoted says whether this window's X11 geometry (w/h above) uses
     * those physical pixels. Self-scaling toolkits such as Motif/Tk keep X11
     * geometry in logical pixels, so resize and pointer paths must ignore the
     * cached ratio even if the host display is HiDPI.
     */
    double hiDpiScaleX;
    double hiDpiScaleY;
    Bool hiDpiPromoted;

    /* Cached WM_NORMAL_HINTS resize increments (ICCCM PResizeInc), in the same
     * physical-pixel space as w/h. A real window manager snaps user resizes to
     * these; there is none here, so the shim replays that behaviour at drag end
     * (see snapTopLevelToResizeIncrements). hasResizeInc gates the snap so the
     * vast majority of clients, which never publish PResizeInc, are unaffected.
     * baseWidth/baseHeight and minWidth/minHeight anchor the snap the ICCCM
     * way: width = base + i * width_inc. Populated from XSetWMSizeHints.
     */
    Bool hasResizeInc;
    int widthInc;
    int heightInc;
    int baseWidth;
    int baseHeight;
    int minWidth;
    int minHeight;

    /* Live-resize coalescing state, per top-level so two
     * simultaneously-resizing windows do not overwrite each other's tracking (a
     * shared global would make neither ever settle). liveResizeLastSeen* is
     * this window's pixel size on the previous observer tick;
     * liveResizeSettleTicks counts consecutive ticks at that same size. Reset
     * when a fresh drag arms the observer.
     */
    int liveResizeLastSeenW;
    int liveResizeLastSeenH;
    int liveResizeSettleTicks;

    /* Nonzero while configureWindow is applying a client-initiated resize via
     * SDL_SetWindowSize. That call makes SDL post its own RESIZED/SIZE_CHANGED,
     * which the event filter must not turn into a second ConfigureNotify
     * (configureWindow already posts one). configureWindow pumps synchronously
     * while this is set so both echoes are dropped before it clears the flag; a
     * genuine host-driven resize arrives with the flag clear. Atomic because
     * the filter can run on SDL's event thread.
     */
    SDL_atomic_t suppressSdlResizeEcho;

    /* Coordinate token for the move path, mirroring snapEchoW/snapEchoH on the
     * resize path. configureWindow records the exact host origin it wrote via
     * SDL_SetWindowPosition for a client XMoveWindow (and posts the one
     * ConfigureNotify itself); the event filter drops the SDL MOVED echo whose
     * data1/data2 match that origin, clearing the token, so a bare move does
     * not escape as a second ConfigureNotify (the filter otherwise lets a
     * mapped top-level MOVED through to repaint the drag trail). Matching on
     * the exact origin rather than a timing window means the echo is caught
     * whenever it arrives without a mid-configure pump, and a genuine WM drag
     * to a different position is always kept. A reparenting WM that reports a
     * frame-adjusted origin still slips one benign extra ConfigureNotify
     * through, same as before. moveEchoValid gates use since the origin can be
     * zero or negative. The filter reads on SDL's event thread while
     * configureWindow writes on the main thread, so moveEchoValid is atomic:
     * configureWindow writes the coordinates and then SDL_AtomicSet(valid), and
     * the filter SDL_AtomicGet(valid) before reading the coordinates. SDL's
     * atomics carry a full barrier, so a valid token always exposes a
     * consistent origin (no torn coordinate read) rather than relying on store
     * ordering.
     */
    SDL_atomic_t moveEchoValid;
    int moveEchoHostX;
    int moveEchoHostY;

    /* One-shot: armed by showTopLevelWindow before SDL_ShowWindow. A mapped
     * top-level already posts an explicit MapNotify next to the show, so the
     * MapNotify that the SDL SHOWN echo would synthesize in convertEvent is
     * redundant; delivering both makes Xt/Tk map handlers run twice. Cleared
     * when that SHOWN is converted. Atomic because the flag is set on the
     * caller thread and read on the event thread.
     */
    SDL_atomic_t suppressSdlShowMap;

    /* Logical (point) size the drag-end increment snap last wrote via
     * SDL_SetWindowSize. That snap runs inside the live-resize present frame
     * and cannot pump to flush the echo, so it cannot use the flag above;
     * instead the filter drops resize echoes whose payload matches this size
     * (both the RESIZED and SIZE_CHANGED posts) and clears the key on the
     * matching RESIZED so it cannot suppress a later genuine resize. Zero when
     * no snap is pending. Only touched on the main/event thread.
     */
    int snapEchoW;
    int snapEchoH;
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

    /* For an override-redirect popup realized as an SDL3 xdg_popup: the mapped
     * parent surface it is anchored to. None when this window is not a
     * parent-anchored popup (every SDL2 window, and any SDL3 override-redirect
     * window for which no containing parent was found, which falls back to an
     * absolutely positioned top-level). Read by configureWindow to reposition
     * the popup relative to its parent instead of in absolute host coordinates.
     */
    Window popupParent;

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

/* Effective device-pixel scale for coordinate conversions, or 1.0 when the
 * window is not HiDPI-promoted (a self-scaling toolkit keeps X11 geometry in
 * logical points, so its ratio must be ignored). Single-sources the "promoted
 * and valid ratio" test that pointer, resize, and geometry paths would
 * otherwise each spell out.
 */
static inline double effectiveHiDpiScaleX(const WindowStruct *ws)
{
    return ws->hiDpiPromoted && ws->hiDpiScaleX > 0.0 ? ws->hiDpiScaleX : 1.0;
}

static inline double effectiveHiDpiScaleY(const WindowStruct *ws)
{
    return ws->hiDpiPromoted && ws->hiDpiScaleY > 0.0 ? ws->hiDpiScaleY : 1.0;
}

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
