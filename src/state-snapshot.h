#ifndef LIBX11_COMPAT_STATE_SNAPSHOT_H
#define LIBX11_COMPAT_STATE_SNAPSHOT_H

#include <stdint.h>
#include <X11/Xlib.h>
#include <SDL2/SDL.h>

/* Replay-time introspection. Runs on the main / X-client event thread (same
 * constraint src/snapshot.c enforces) so callbacks can safely read
 * WindowStruct and the focus / grab globals without racing window
 * destruction. The replay worker thread blocks via a SDL_USEREVENT
 * round-trip identical to snapshotRequestAndWait, then reads the populated
 * snapshot from disk.
 *
 * Sized for the demos the project actually runs (Motif, ViolaWWW, Mosaic,
 * Osiris); a window count of 64 covers each demo plus its popups with
 * margin. Overflows are reported by truncation_flagged so a regression
 * surfacing as "snapshot truncated" surfaces visibly instead of as a silent
 * undercount.
 */

#define UI_SNAPSHOT_MAX_WINDOWS 64
#define UI_SNAPSHOT_NAME_MAX 96
#define UI_SNAPSHOT_PROP_BYTES_MAX 128

typedef enum {
    UI_PROP_SOURCE_NONE = 0,
    UI_PROP_SOURCE_STORED,
    UI_PROP_SOURCE_SYNTHESIZED,
} UiPropSource;

/* Per-named-property record. Carries up to UI_SNAPSHOT_PROP_BYTES_MAX bytes
 * of the raw value so the property-drift verifier can do exact-byte diffs
 * without re-issuing XGetWindowProperty (which would re-trigger
 * synthesis).
 */
typedef struct {
    Atom atom;
    int format;
    unsigned long length;
    UiPropSource source;
    int truncated;
    unsigned char bytes[UI_SNAPSHOT_PROP_BYTES_MAX];
} UiSnapshotProperty;

typedef struct {
    Window window;
    Window parent;
    int x, y;
    unsigned int w, h;
    Uint32 sdl_window_id;
    Uint32 sdl_flags;
    int override_redirect;
    int map_state;
    long event_mask;
    char wm_class[UI_SNAPSHOT_NAME_MAX];
    char wm_name[UI_SNAPSHOT_NAME_MAX];
    /* WM_HINTS, WM_STATE, WM_PROTOCOLS, WM_CLASS, WM_NAME,
     * _MOTIF_WM_HINTS, _NET_WM_STATE, _NET_WM_WINDOW_TYPE. See
     * state-snapshot.c for the bound and the layout.
     */
    UiSnapshotProperty properties[8];
    int property_count;
} UiSnapshotWindow;

typedef struct {
    uint64_t captured_t_ms;
    Window focused_window;
    int revert_to;
    Window pointer_grab;
    Window keyboard_grab;
    int mapped_window_count;
    int popup_window_count;
    int event_queue_depth;
    int truncation_flagged;
    int window_count;
    UiSnapshotWindow windows[UI_SNAPSHOT_MAX_WINDOWS];
} UiSnapshot;

/* Synchronous request: marshal the snapshot on the main thread, write JSON
 * to path, signal the waiter. Returns 0 on success.
 */
int stateSnapshotRequestAndWait(const char *path);

/* Main-thread handler for the SDL_USEREVENT round-trip. */
int stateSnapshotHandleEvent(Display *display, const SDL_Event *event);

/* Does the supplied SDL_USEREVENT type belong to the state-snapshot module?
 * Used by the convertEvent dispatch to route the event to the right
 * handler (alongside snapshotOwnsEventType / presentWakeOwnsEventType).
 */
Bool stateSnapshotOwnsEventType(Uint32 eventType);

#endif
