/* Timeline JSONL writer + atomic per-kind counters.
 *
 * Why this exists: pixel-diff replay tests see "the screen changed", but they
 * do not see who selected for what, which input verb caused which present, or
 * whether the post-input expose storm ever settled. The timeline records each X
 * event after convertEvent translates it, each configure / destroy at the point
 * the in-process state mutates, each present's window + pixel count, and each
 * focus / grab transition. Phase 3's convergence detector reads the per-kind
 * atomic counters directly (not the JSONL) so it does not race buffered file
 * writes from the present and event taps.
 *
 * The writer is gated on LIBX11_COMPAT_TIMELINE=1. Counters always increment so
 * timelineWaitConverge works whether or not the writer is on.
 */

#include "timeline.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL2/SDL.h>
#include "util.h"

static SDL_atomic_t timelineCounters[TIMELINE_KIND_COUNT];

/* Writer state. Both reads and writes are guarded by writerMutex; the present
 * tap and event tap race each other otherwise.
 */
static pthread_mutex_t writerMutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *writerFile = NULL;
static int writerInitState = 0; /* 0=unchecked, 1=on, -1=off */
static uint64_t baseMs = 0;

/* Stable strings for the JSONL "kind" field and counter lookup. Order matches
 * the TimelineKind enum exactly.
 */
static const char *const KIND_NAMES[TIMELINE_KIND_COUNT] = {
    [TIMELINE_KIND_EXPOSE] = "Expose",
    [TIMELINE_KIND_MAP_NOTIFY] = "MapNotify",
    [TIMELINE_KIND_UNMAP_NOTIFY] = "UnmapNotify",
    [TIMELINE_KIND_CONFIGURE_NOTIFY] = "ConfigureNotify",
    [TIMELINE_KIND_DESTROY_NOTIFY] = "DestroyNotify",
    [TIMELINE_KIND_BUTTON_PRESS] = "ButtonPress",
    [TIMELINE_KIND_BUTTON_RELEASE] = "ButtonRelease",
    [TIMELINE_KIND_KEY_PRESS] = "KeyPress",
    [TIMELINE_KIND_KEY_RELEASE] = "KeyRelease",
    [TIMELINE_KIND_MOTION_NOTIFY] = "MotionNotify",
    [TIMELINE_KIND_ENTER_NOTIFY] = "EnterNotify",
    [TIMELINE_KIND_LEAVE_NOTIFY] = "LeaveNotify",
    [TIMELINE_KIND_FOCUS_IN] = "FocusIn",
    [TIMELINE_KIND_FOCUS_OUT] = "FocusOut",
    [TIMELINE_KIND_PROPERTY_NOTIFY] = "PropertyNotify",
    [TIMELINE_KIND_CLIENT_MESSAGE] = "ClientMessage",
    [TIMELINE_KIND_SELECTION_NOTIFY] = "SelectionNotify",
    [TIMELINE_KIND_VISIBILITY_NOTIFY] = "VisibilityNotify",
    [TIMELINE_KIND_REPARENT_NOTIFY] = "ReparentNotify",
    [TIMELINE_KIND_PRESENT] = "Present",
    [TIMELINE_KIND_CONFIGURE] = "Configure",
    [TIMELINE_KIND_DESTROY] = "Destroy",
    [TIMELINE_KIND_SET_INPUT_FOCUS] = "SetInputFocus",
    [TIMELINE_KIND_GRAB_POINTER] = "GrabPointer",
    [TIMELINE_KIND_UNGRAB_POINTER] = "UngrabPointer",
    [TIMELINE_KIND_GRAB_KEYBOARD] = "GrabKeyboard",
    [TIMELINE_KIND_UNGRAB_KEYBOARD] = "UngrabKeyboard",
    [TIMELINE_KIND_DIVERGED] = "diverged",
};

static uint64_t monoMs(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t) now.tv_sec * 1000ULL +
           (uint64_t) (now.tv_nsec / 1000000L);
}

uint64_t timelineNowMs(void)
{
    return monoMs();
}

/* Open the writer lazily on the first emit so a process that never produces a
 * tap (e.g. unit tests) does not create an empty file.
 */
static FILE *getWriterLocked(void)
{
    if (writerInitState == 0) {
        const char *flag = getenv("LIBX11_COMPAT_TIMELINE");
        const char *path = getenv("LIBX11_COMPAT_TIMELINE_PATH");
        if (flag && *flag && *flag != '0') {
            FILE *fp = NULL;
            if (path && *path)
                fp = fopen(path, "w");
            else
                fp = stderr;
            if (fp) {
                writerFile = fp;
                if (fp != stderr)
                    setvbuf(fp, NULL, _IOLBF, 0);
                baseMs = monoMs();
                writerInitState = 1;
            } else {
                writerInitState = -1;
            }
        } else {
            writerInitState = -1;
        }
    }
    return writerInitState == 1 ? writerFile : NULL;
}

Bool timelineEnabled(void)
{
    Bool on;
    pthread_mutex_lock(&writerMutex);
    on = getWriterLocked() != NULL;
    pthread_mutex_unlock(&writerMutex);
    return on ? True : False;
}

uint64_t timelineCounter(TimelineKind kind)
{
    if ((unsigned) kind >= TIMELINE_KIND_COUNT)
        return 0;
    return (uint64_t) (unsigned) SDL_AtomicGet(&timelineCounters[kind]);
}

void timelineEmit(TimelineKind kind, Window window, const char *payloadJson)
{
    if ((unsigned) kind < TIMELINE_KIND_COUNT)
        SDL_AtomicAdd(&timelineCounters[kind], 1);

    pthread_mutex_lock(&writerMutex);
    FILE *fp = getWriterLocked();
    if (fp) {
        uint64_t t = monoMs() - baseMs;
        const char *name = (unsigned) kind < TIMELINE_KIND_COUNT
                               ? KIND_NAMES[kind]
                               : "Unknown";
        fprintf(fp, "{\"t_ms\":%llu,\"kind\":\"%s\",\"window\":%lu%s}\n",
                (unsigned long long) t, name, (unsigned long) window,
                payloadJson ? payloadJson : "");
        fflush(fp);
    }
    pthread_mutex_unlock(&writerMutex);
}

void timelineTapXEvent(const XEvent *xEvent)
{
    if (!xEvent)
        return;
    char payload[160];
    payload[0] = '\0';
    TimelineKind kind;
    Window window = 0;
    switch (xEvent->type) {
    case Expose:
        kind = TIMELINE_KIND_EXPOSE;
        window = xEvent->xexpose.window;
        snprintf(payload, sizeof(payload),
                 ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"count\":%d",
                 xEvent->xexpose.x, xEvent->xexpose.y, xEvent->xexpose.width,
                 xEvent->xexpose.height, xEvent->xexpose.count);
        break;
    case MapNotify:
        kind = TIMELINE_KIND_MAP_NOTIFY;
        window = xEvent->xmap.window;
        snprintf(payload, sizeof(payload), ",\"event\":%lu,\"override\":%d",
                 (unsigned long) xEvent->xmap.event,
                 xEvent->xmap.override_redirect ? 1 : 0);
        break;
    case UnmapNotify:
        kind = TIMELINE_KIND_UNMAP_NOTIFY;
        window = xEvent->xunmap.window;
        snprintf(payload, sizeof(payload), ",\"event\":%lu",
                 (unsigned long) xEvent->xunmap.event);
        break;
    case ConfigureNotify:
        kind = TIMELINE_KIND_CONFIGURE_NOTIFY;
        window = xEvent->xconfigure.window;
        snprintf(payload, sizeof(payload),
                 ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d", xEvent->xconfigure.x,
                 xEvent->xconfigure.y, xEvent->xconfigure.width,
                 xEvent->xconfigure.height);
        break;
    case DestroyNotify:
        kind = TIMELINE_KIND_DESTROY_NOTIFY;
        window = xEvent->xdestroywindow.window;
        break;
    case ButtonPress:
        kind = TIMELINE_KIND_BUTTON_PRESS;
        window = xEvent->xbutton.window;
        snprintf(payload, sizeof(payload),
                 ",\"button\":%u,\"x\":%d,\"y\":%d,\"state\":%u",
                 xEvent->xbutton.button, xEvent->xbutton.x, xEvent->xbutton.y,
                 xEvent->xbutton.state);
        break;
    case ButtonRelease:
        kind = TIMELINE_KIND_BUTTON_RELEASE;
        window = xEvent->xbutton.window;
        snprintf(payload, sizeof(payload),
                 ",\"button\":%u,\"x\":%d,\"y\":%d,\"state\":%u",
                 xEvent->xbutton.button, xEvent->xbutton.x, xEvent->xbutton.y,
                 xEvent->xbutton.state);
        break;
    case KeyPress:
        kind = TIMELINE_KIND_KEY_PRESS;
        window = xEvent->xkey.window;
        snprintf(payload, sizeof(payload), ",\"keycode\":%u,\"state\":%u",
                 xEvent->xkey.keycode, xEvent->xkey.state);
        break;
    case KeyRelease:
        kind = TIMELINE_KIND_KEY_RELEASE;
        window = xEvent->xkey.window;
        snprintf(payload, sizeof(payload), ",\"keycode\":%u,\"state\":%u",
                 xEvent->xkey.keycode, xEvent->xkey.state);
        break;
    case MotionNotify:
        kind = TIMELINE_KIND_MOTION_NOTIFY;
        window = xEvent->xmotion.window;
        snprintf(payload, sizeof(payload), ",\"x\":%d,\"y\":%d",
                 xEvent->xmotion.x, xEvent->xmotion.y);
        break;
    case EnterNotify:
        kind = TIMELINE_KIND_ENTER_NOTIFY;
        window = xEvent->xcrossing.window;
        snprintf(payload, sizeof(payload), ",\"mode\":%d,\"detail\":%d",
                 xEvent->xcrossing.mode, xEvent->xcrossing.detail);
        break;
    case LeaveNotify:
        kind = TIMELINE_KIND_LEAVE_NOTIFY;
        window = xEvent->xcrossing.window;
        snprintf(payload, sizeof(payload), ",\"mode\":%d,\"detail\":%d",
                 xEvent->xcrossing.mode, xEvent->xcrossing.detail);
        break;
    case FocusIn:
        kind = TIMELINE_KIND_FOCUS_IN;
        window = xEvent->xfocus.window;
        snprintf(payload, sizeof(payload), ",\"mode\":%d,\"detail\":%d",
                 xEvent->xfocus.mode, xEvent->xfocus.detail);
        break;
    case FocusOut:
        kind = TIMELINE_KIND_FOCUS_OUT;
        window = xEvent->xfocus.window;
        snprintf(payload, sizeof(payload), ",\"mode\":%d,\"detail\":%d",
                 xEvent->xfocus.mode, xEvent->xfocus.detail);
        break;
    case PropertyNotify:
        kind = TIMELINE_KIND_PROPERTY_NOTIFY;
        window = xEvent->xproperty.window;
        snprintf(payload, sizeof(payload), ",\"atom\":%lu,\"state\":%d",
                 (unsigned long) xEvent->xproperty.atom,
                 xEvent->xproperty.state);
        break;
    case ClientMessage:
        kind = TIMELINE_KIND_CLIENT_MESSAGE;
        window = xEvent->xclient.window;
        snprintf(payload, sizeof(payload),
                 ",\"message_type\":%lu,\"format\":%d",
                 (unsigned long) xEvent->xclient.message_type,
                 xEvent->xclient.format);
        break;
    case SelectionNotify:
        kind = TIMELINE_KIND_SELECTION_NOTIFY;
        window = xEvent->xselection.requestor;
        snprintf(payload, sizeof(payload),
                 ",\"selection\":%lu,\"target\":%lu,\"property\":%lu",
                 (unsigned long) xEvent->xselection.selection,
                 (unsigned long) xEvent->xselection.target,
                 (unsigned long) xEvent->xselection.property);
        break;
    case VisibilityNotify:
        kind = TIMELINE_KIND_VISIBILITY_NOTIFY;
        window = xEvent->xvisibility.window;
        snprintf(payload, sizeof(payload), ",\"state\":%d",
                 xEvent->xvisibility.state);
        break;
    case ReparentNotify:
        kind = TIMELINE_KIND_REPARENT_NOTIFY;
        window = xEvent->xreparent.window;
        snprintf(payload, sizeof(payload), ",\"parent\":%lu,\"x\":%d,\"y\":%d",
                 (unsigned long) xEvent->xreparent.parent, xEvent->xreparent.x,
                 xEvent->xreparent.y);
        break;
    default:
        return;
    }
    timelineEmit(kind, window, payload);
}

void timelineTapPresent(Window window, int rectCount, uint64_t pixels)
{
    char payload[80];
    snprintf(payload, sizeof(payload), ",\"rects\":%d,\"pixels\":%llu",
             rectCount, (unsigned long long) pixels);
    timelineEmit(TIMELINE_KIND_PRESENT, window, payload);
}

void timelineTapConfigure(Window window,
                          int x,
                          int y,
                          unsigned int w,
                          unsigned int h)
{
    char payload[96];
    snprintf(payload, sizeof(payload), ",\"x\":%d,\"y\":%d,\"w\":%u,\"h\":%u",
             x, y, w, h);
    timelineEmit(TIMELINE_KIND_CONFIGURE, window, payload);
}

void timelineTapDestroy(Window window)
{
    timelineEmit(TIMELINE_KIND_DESTROY, window, NULL);
}

void timelineTapSetInputFocus(Window window, int revertTo)
{
    char payload[48];
    snprintf(payload, sizeof(payload), ",\"revert_to\":%d", revertTo);
    timelineEmit(TIMELINE_KIND_SET_INPUT_FOCUS, window, payload);
}

void timelineTapGrabPointer(Window window, Bool active)
{
    timelineEmit(
        active ? TIMELINE_KIND_GRAB_POINTER : TIMELINE_KIND_UNGRAB_POINTER,
        window, NULL);
}

void timelineTapGrabKeyboard(Window window, Bool active)
{
    timelineEmit(
        active ? TIMELINE_KIND_GRAB_KEYBOARD : TIMELINE_KIND_UNGRAB_KEYBOARD,
        window, NULL);
}

int timelineWaitConverge(uint64_t anchorMs,
                         uint64_t bucketMs,
                         int quietBuckets,
                         int divergedBuckets,
                         uint64_t thresholdPerBucket,
                         uint64_t timeoutMs)
{
    if (bucketMs == 0)
        bucketMs = 50;
    if (quietBuckets <= 0)
        quietBuckets = 2;
    if (divergedBuckets <= 0)
        divergedBuckets = 16;
    if (timeoutMs == 0)
        timeoutMs = 5000;

    /* Watch the union of Expose, MapNotify, UnmapNotify, ConfigureNotify,
     * DestroyNotify, and Present. The first four are the post-input churn the
     * detector is meant to bound; Present anchors the actual on-screen settle.
     */
    static const TimelineKind WATCH[] = {
        TIMELINE_KIND_EXPOSE,         TIMELINE_KIND_MAP_NOTIFY,
        TIMELINE_KIND_UNMAP_NOTIFY,   TIMELINE_KIND_CONFIGURE_NOTIFY,
        TIMELINE_KIND_DESTROY_NOTIFY, TIMELINE_KIND_PRESENT,
    };
    const size_t WATCH_N = sizeof(WATCH) / sizeof(WATCH[0]);

    /* Snapshot counters as uint32_t so the bucket-to-bucket delta uses
     * modulo-2^32 unsigned subtraction. timelineCounter() widens to 64 bits but
     * the underlying SDL_atomic_t is 32 bits; widening the absolute value
     * before subtraction synthesizes a huge bogus delta at the 2^32 rollover.
     */
    uint32_t lastSeen[8] = {0};
    for (size_t i = 0; i < WATCH_N; i++)
        lastSeen[i] = (uint32_t) timelineCounter(WATCH[i]);

    uint64_t deadline = anchorMs + timeoutMs;
    int quiet = 0;
    int active = 0;
    /* Split bucketMs into tv_sec + tv_nsec so a bucket of >= 1000 ms does not
     * pass an out-of-range tv_nsec to nanosleep (POSIX requires < 1e9), which
     * would otherwise return EINVAL and convert the bucket loop into an
     * unthrottled spin.
     *
     * Convergence is declared after quietBuckets consecutive empty buckets,
     * regardless of whether any events were ever observed. That handles the
     * idle case (wait-converge in a steady state) without a separate code path.
     * Worker threads that dispatched input immediately before calling
     * wait-converge rely on the main thread draining the input within bucketMs;
     * on a healthy main loop that holds.
     */
    while (monoMs() < deadline) {
        struct timespec req = {
            (time_t) (bucketMs / 1000ULL),
            (long) ((bucketMs % 1000ULL) * 1000000ULL),
        };
        nanosleep(&req, NULL);

        uint64_t bucketEvents = 0;
        for (size_t i = 0; i < WATCH_N; i++) {
            uint32_t cur = (uint32_t) timelineCounter(WATCH[i]);
            uint32_t delta = cur - lastSeen[i];
            lastSeen[i] = cur;
            bucketEvents += (uint64_t) delta;
        }
        if (bucketEvents > thresholdPerBucket) {
            active++;
            quiet = 0;
            if (active >= divergedBuckets) {
                char payload[96];
                snprintf(payload, sizeof(payload),
                         ",\"buckets\":%d,\"bucket_ms\":%llu", active,
                         (unsigned long long) bucketMs);
                timelineEmit(TIMELINE_KIND_DIVERGED, 0, payload);
                return -1;
            }
        } else if (bucketEvents == 0) {
            active = 0;
            quiet++;
            if (quiet >= quietBuckets)
                return 1;
        } else {
            active = 0;
            quiet = 0;
        }
    }
    return 0;
}
