#ifndef LIBX11_COMPAT_TIMELINE_H
#define LIBX11_COMPAT_TIMELINE_H

#include <stdint.h>
#include <X11/Xlib.h>
#include "sdl-compat.h"

/* Stable JSONL trace of X events, window lifecycle, present activity, focus and
 * grab transitions. Gated on LIBX11_COMPAT_TIMELINE=1; off in release builds
 * the helpers compile to a single env-var-cached load + branch so the tap call
 * sites stay cheap.
 *
 * Per-kind atomic counters (read via timelineCounter) back the in-process
 * convergence detector even when the file writer is disabled; the counters are
 * always live so an embedded wait-converge replay verb works against an
 * untapped build.
 */

typedef enum {
    TIMELINE_KIND_EXPOSE = 0,
    TIMELINE_KIND_MAP_NOTIFY,
    TIMELINE_KIND_UNMAP_NOTIFY,
    TIMELINE_KIND_CONFIGURE_NOTIFY,
    TIMELINE_KIND_DESTROY_NOTIFY,
    TIMELINE_KIND_BUTTON_PRESS,
    TIMELINE_KIND_BUTTON_RELEASE,
    TIMELINE_KIND_KEY_PRESS,
    TIMELINE_KIND_KEY_RELEASE,
    TIMELINE_KIND_MOTION_NOTIFY,
    TIMELINE_KIND_ENTER_NOTIFY,
    TIMELINE_KIND_LEAVE_NOTIFY,
    TIMELINE_KIND_FOCUS_IN,
    TIMELINE_KIND_FOCUS_OUT,
    TIMELINE_KIND_PROPERTY_NOTIFY,
    TIMELINE_KIND_CLIENT_MESSAGE,
    TIMELINE_KIND_SELECTION_NOTIFY,
    TIMELINE_KIND_VISIBILITY_NOTIFY,
    TIMELINE_KIND_REPARENT_NOTIFY,
    TIMELINE_KIND_PRESENT,
    TIMELINE_KIND_CONFIGURE,
    TIMELINE_KIND_DESTROY,
    TIMELINE_KIND_SET_INPUT_FOCUS,
    TIMELINE_KIND_GRAB_POINTER,
    TIMELINE_KIND_UNGRAB_POINTER,
    TIMELINE_KIND_GRAB_KEYBOARD,
    TIMELINE_KIND_UNGRAB_KEYBOARD,
    TIMELINE_KIND_DIVERGED,
    TIMELINE_KIND_COUNT
} TimelineKind;

Bool timelineEnabled(void);
uint64_t timelineNowMs(void);

/* Emit one JSON line. payloadJson, if non-NULL and non-empty, is spliced as a
 * trailing fragment of the JSON object (must begin with ',' and contain raw
 * escaped JSON members, e.g. ",\"x\":12"). Safe to call with payload == NULL.
 */
void timelineEmit(TimelineKind kind, Window window, const char *payloadJson);

/* Tap helper for convertEvent. Looks at the converted XEvent and routes the
 * right kind + window. No-op when the timeline writer is disabled but the
 * counters still increment.
 */
void timelineTapXEvent(const XEvent *xEvent);

/* Tap helpers for non-event call sites. */
void timelineTapPresent(Window window, int rectCount, uint64_t pixels);
void timelineTapConfigure(Window window,
                          int x,
                          int y,
                          unsigned int w,
                          unsigned int h);
void timelineTapDestroy(Window window);
void timelineTapSetInputFocus(Window window, int revertTo);
void timelineTapGrabPointer(Window window, Bool active);
void timelineTapGrabKeyboard(Window window, Bool active);

/* Per-kind atomic counter (monotonically increasing). Used by the convergence
 * detector + summary assertions.
 */
uint64_t timelineCounter(TimelineKind kind);

/* Block the calling thread until input quiesces or the budget elapses.
 *   anchorMs   -- snapshot of timelineNowMs() taken before the input verb
 *                 that this call is following
 *   bucketMs   -- size of an idle-quiet bucket (e.g. 50)
 *   quietBuckets -- number of consecutive empty buckets that means converged
 *   divergedBuckets -- number of consecutive over-threshold buckets that
 *                  means diverged
 *   thresholdPerBucket -- per-bucket event count that is "active" (e.g. 1)
 *   timeoutMs -- hard ceiling (e.g. 5000)
 * Returns:
 *   1 -- converged
 *   0 -- timed out without ever reaching quiet
 * -1 -- divergent (logged as a "diverged" record)
 */
int timelineWaitConverge(uint64_t anchorMs,
                         uint64_t bucketMs,
                         int quietBuckets,
                         int divergedBuckets,
                         uint64_t thresholdPerBucket,
                         uint64_t timeoutMs);

#endif
