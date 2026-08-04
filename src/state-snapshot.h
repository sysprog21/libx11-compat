#ifndef LIBX11_COMPAT_STATE_SNAPSHOT_H
#define LIBX11_COMPAT_STATE_SNAPSHOT_H

#include <stdint.h>
#include <X11/Xlib.h>
#include "sdl-compat.h"

/* Replay-time introspection. Runs on the main / X-client event thread (same
 * constraint src/snapshot.c enforces) so callbacks can safely read WindowStruct
 * and the focus / grab globals without racing window destruction. The replay
 * worker thread blocks via a SDL_USEREVENT round-trip identical to
 * snapshotRequestAndWait, then reads the populated snapshot from disk.
 *
 * Sized for the demos the project actually runs (Motif, ViolaWWW, Mosaic,
 * Osiris); a window count of 64 covers each demo plus its popups with margin.
 * Overflows are reported by truncation_flagged so a regression surfacing as
 * "snapshot truncated" surfaces visibly instead of as a silent undercount.
 */

#define UI_SNAPSHOT_MAX_WINDOWS 64
#define UI_SNAPSHOT_NAME_MAX 96
#define UI_SNAPSHOT_PROP_BYTES_MAX 128

/* Number of named properties captured per window. Must equal TRACKED_ATOM_COUNT
 * in state-snapshot.c; a _Static_assert there catches any drift so this array
 * can never under-run the tracked set.
 */
#define UI_SNAPSHOT_TRACKED_PROP_MAX 8

typedef enum {
    UI_PROP_SOURCE_NONE = 0,
    UI_PROP_SOURCE_STORED,
    UI_PROP_SOURCE_SYNTHESIZED,
} UiPropSource;

/* Per-named-property record. Carries up to UI_SNAPSHOT_PROP_BYTES_MAX bytes of
 * the raw value so the property-drift verifier can do exact-byte diffs without
 * re-issuing XGetWindowProperty (which would re-trigger synthesis).
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
    int shape_bounding;
    int shape_bounding_origin;
    int shape_bounding_bottom_left;
    int shape_bounding_bottom_left_hit;
    int shape_clip;
    long event_mask;
    char wm_class[UI_SNAPSHOT_NAME_MAX];
    char wm_name[UI_SNAPSHOT_NAME_MAX];

    /* WM_HINTS, WM_CLASS, WM_NAME, _NET_WM_NAME, _NET_WM_STATE,
     * _NET_WM_WINDOW_TYPE, _MOTIF_WM_HINTS, WM_PROTOCOLS. See TRACKED_ATOMS in
     * state-snapshot.c for the authoritative order and bound.
     */
    UiSnapshotProperty properties[UI_SNAPSHOT_TRACKED_PROP_MAX];
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

/* Synchronous request: marshal the snapshot on the main thread, write JSON to
 * path, signal the waiter.
 *
 * Returns 0 on success.
 */
int stateSnapshotRequestAndWait(const char *path);

/* Main-thread handler for the SDL_USEREVENT round-trip. */
int stateSnapshotHandleEvent(Display *display, const SDL_Event *event);

/* Does the supplied SDL_USEREVENT type belong to the state-snapshot module?
 * Used by the convertEvent dispatch to route the event to the right handler
 * (alongside snapshotOwnsEventType / presentWakeOwnsEventType).
 */
Bool stateSnapshotOwnsEventType(Uint32 eventType);

#endif
