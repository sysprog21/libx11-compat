/* Scripted event replay
 *
 * Read a small command file pointed to by $LIBX11_COMPAT_REPLAY and dispatch
 * its instructions as synthetic input via the XTest fake-event API. Each line
 * is a single command; whitespace separates fields; blank lines and lines
 * starting with # are ignored.
 *
 * Supported commands:
 *   delay  <ms>
 *   motion <x> <y>
 *   target-motion <x> <y> # x/y relative to the current replay target window
 *   button <n> press|release
 *   click  <x> <y>           # motion + button 1 press + release
 *   key    <scancode> press|release
 *
 * Why this exists: on macOS, external event-injection tools (cliclick, CGEvent,
 * AppleScript click) cannot reliably push mouse button events into SDL windows
 * belonging to non-bundled, non-keyWindow processes. The replay engine runs
 * inside the target process, hands its synthetic SDL_Event to SDL_PushEvent,
 * and the event flows through the normal onSdlEvent + convertEvent path,
 * identical to a real click. The macOS NSEvent dispatch chain is never
 * involved, so the limitation does not apply.
 *
 * Threading: parse runs in a detached pthread. SDL_PushEvent is thread-safe;
 * the parser otherwise touches no libx11-compat state.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include "sdl-compat.h"
#include "replay.h"
#include "replay-target.h"
#include "snapshot.h"
#include "state-snapshot.h"
#include "timeline.h"
#include "util.h"

static uint64_t replayLastInputAnchorMs = 0;

static uint64_t replayNowMs(void)
{
    return timelineNowMs();
}

static uint64_t replayWaitConvergeAnchorMs(uint64_t timeoutMs)
{
    uint64_t nowMs = replayNowMs();
    uint64_t anchorMs = replayLastInputAnchorMs;
    if (timeoutMs == 0)
        timeoutMs = 5000;
    if (anchorMs == 0 || (nowMs >= anchorMs && nowMs - anchorMs >= timeoutMs))
        return nowMs;
    return anchorMs;
}

static pthread_once_t replayOnce = PTHREAD_ONCE_INIT;
static Display *replayDisplay = NULL;
static char *replayPath = NULL;

/* Set to non-zero by replayStop() (from the last XCloseDisplay) to signal the
 * worker pthread that the host Display is going away. The replay thread polls
 * this between commands and inside its delay chunks, then exits cleanly.
 * Replay/XTest injection also stops accepting events once replay-target clears
 * the cached target, so this flag plus the join in replayStop() guarantees no
 * pushed event survives SDL_Quit.
 */
static SDL_atomic_t replayShouldStop;

/* Thread handle so replayStop can join the worker before the caller tears down
 * SDL. Without the join the worker -- previously detached -- could fire
 * SDL_PushEvent into a freed SDL state after SDL_Quit returned (gemini-flagged
 * race). spawned tracks whether the handle is valid, avoiding pthread_join on
 * PTHREAD_CANCELED-style stubs.
 */
static pthread_t replayThreadHandle;
static Bool replayThreadSpawned = False;

static void trim(char *s)
{
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char) end[-1]))
        --end;
    *end = '\0';
}

static Bool isPressWord(const char *dir)
{
    return strcasecmp(dir, "press") == 0 || strcasecmp(dir, "down") == 0;
}

/* Parse a single non-negative decimal coordinate, advance the cursor past
 * trailing whitespace, and reject anything that would wrap-around or land
 * outside int range.
 *
 * Returns True on success. Used by the focus-at verb so the x and y fields
 * share one rejection path.
 */
static Bool parseFocusAtCoord(const char **cursor, int *out)
{
    while (**cursor == ' ' || **cursor == '\t')
        (*cursor)++;
    char *endp = NULL;
    errno = 0;
    long value = strtol(*cursor, &endp, 10);
    if (*cursor == endp || errno == ERANGE || value < 0 || value > INT_MAX)
        return False;
    *cursor = endp;
    while (**cursor == ' ' || **cursor == '\t')
        (*cursor)++;
    *out = (int) value;
    return True;
}

static void writeWaitConvergeFailure(const char *path,
                                     int lineno,
                                     const char *reason)
{
    FILE *fp = fopen(path, "w");
    if (!fp) {
        fprintf(stderr,
                "replay: line %d: wait-converge failure marker %s failed\n",
                lineno, path);
        return;
    }
    fprintf(fp, "%s\n", reason);
    fclose(fp);
}

/* Replay diagnostics go to stderr unconditionally rather than through LOG().
 * $LIBX11_COMPAT_REPLAY is an end-user / test-driver control path; silently
 * dropping "file not found" or "unknown command" errors in release builds would
 * make script issues nearly impossible to diagnose.
 */
static void runScript(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "replay: cannot open '%s': %s\n", path,
                strerror(errno));
        return;
    }

    /* Lines longer than the buffer are split across fgets() calls. Detect
     * truncation (no trailing newline before EOF) and drain to the next \n so
     * the tail of an overlong line is never reparsed as a fresh command.
     */
    char line[1024];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (SDL_AtomicGet(&replayShouldStop))
            break;
        lineno++;
        size_t len = strlen(line);
        Bool truncated = (len == sizeof(line) - 1 && line[len - 1] != '\n');
        if (truncated) {
            int c;
            while ((c = fgetc(fp)) != '\n' && c != EOF)
                ;
            fprintf(stderr, "replay: line %d too long; truncated and skipped\n",
                    lineno);
            continue;
        }
        trim(line);
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        char cmd[32];
        int consumed = 0;
        if (sscanf(p, "%31s%n", cmd, &consumed) != 1)
            continue;
        char *args = p + consumed;
        while (*args == ' ' || *args == '\t')
            args++;

        if (!strcmp(cmd, "delay")) {
            /* Sleep in small chunks so replayStop() (joined from XCloseDisplay
             * before SDL_Quit) interrupts a long delay within at most one chunk
             * instead of blocking the X-client main thread for the full
             * requested duration. Use nanosleep's remaining-time output to
             * handle EINTR resumption -- a signal mid-sleep otherwise
             * short-changes the delay because the loop would decrement by the
             * requested slice rather than the actual time slept, making smoke
             * timing nondeterministic (codex-flagged).
             */
            unsigned long ms = strtoul(args, NULL, 10);
            const unsigned long chunk_ms = 50;
            while (ms > 0 && !SDL_AtomicGet(&replayShouldStop)) {
                unsigned long slice = ms > chunk_ms ? chunk_ms : ms;
                struct timespec req = {0, (long) (slice * 1000000L)};
                struct timespec rem;
                while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
                    if (SDL_AtomicGet(&replayShouldStop))
                        break;
                    req = rem;
                }
                ms -= slice;
            }
        } else if (!strcmp(cmd, "motion")) {
            int x = 0, y = 0;
            if (sscanf(args, "%d %d", &x, &y) == 2) {
                XTestFakeMotionEvent(replayDisplay, 0, x, y, 0);
                replayLastInputAnchorMs = replayNowMs();
            }
        } else if (!strcmp(cmd, "target-motion")) {
            int x = 0, y = 0;
            if (sscanf(args, "%d %d", &x, &y) == 2) {
                int rootX = 0, rootY = 0;
                if (replayTargetTranslateLocal(x, y, &rootX, &rootY))
                    XTestFakeMotionEvent(replayDisplay, 0, rootX, rootY, 0);
                replayLastInputAnchorMs = replayNowMs();
            }
        } else if (!strcmp(cmd, "button")) {
            unsigned int btn = 0;
            char dir[16] = {0};
            if (sscanf(args, "%u %15s", &btn, dir) == 2) {
                XTestFakeButtonEvent(replayDisplay, btn, isPressWord(dir), 0);
                replayLastInputAnchorMs = replayNowMs();
            }
        } else if (!strcmp(cmd, "click")) {
            int x = 0, y = 0;
            if (sscanf(args, "%d %d", &x, &y) == 2) {
                XTestFakeMotionEvent(replayDisplay, 0, x, y, 0);
                XTestFakeButtonEvent(replayDisplay, 1, True, 10);
                XTestFakeButtonEvent(replayDisplay, 1, False, 10);
                replayLastInputAnchorMs = replayNowMs();
            }
        } else if (!strcmp(cmd, "key")) {
            unsigned int code = 0;
            char dir[16] = {0};
            if (sscanf(args, "%u %15s", &code, dir) == 2) {
                XTestFakeKeyEvent(replayDisplay, code, isPressWord(dir), 0);
                replayLastInputAnchorMs = replayNowMs();
            }
        } else if (!strcmp(cmd, "focus-at")) {
            /* Translate target-local (x, y) to root coords, resolve the X
             * subwindow under that point, and call XSetInputFocus on it on the
             * main thread (via snapshotRequestFocusAtAndWait). Motif's
             * translation table normally invokes XmProcessTraversal from its
             * Arm() / BSelect() actions after a click, which then calls
             * XSetInputFocus and lets the widget paint its focus highlight.
             * Synthetic ButtonPress + ButtonRelease via the XTest fake-event
             * path does not reliably trigger XmProcessTraversal under every
             * Motif focus-policy configuration, so this verb is the opt-in knob
             * that forces the focus shift in tests that need it.
             *
             * Parsing: strtol with full-consumption + range checks. Reject
             * negatives and values outside int range so getContainingWindow
             * cannot be handed wrap-around coordinates that resolve to
             * SCREEN_WINDOW or walk past a valid window's bounds.
             */
            const char *cursor = args;
            int x = 0, y = 0;
            if (!parseFocusAtCoord(&cursor, &x)) {
                fprintf(stderr, "replay: line %d: focus-at bad x '%s'\n",
                        lineno, args);
                break;
            }
            if (!parseFocusAtCoord(&cursor, &y)) {
                fprintf(stderr, "replay: line %d: focus-at bad y '%s'\n",
                        lineno, args);
                break;
            }
            if (*cursor != '\0') {
                fprintf(stderr,
                        "replay: line %d: focus-at trailing garbage '%s'\n",
                        lineno, cursor);
                break;
            }
            int rc = snapshotRequestFocusAtAndWait(x, y);
            if (rc != 0)
                fprintf(stderr,
                        "replay: line %d: focus-at (%d, %d) failed "
                        "(rc=%d)\n",
                        lineno, x, y, rc);
        } else if (!strcmp(cmd, "wait-converge")) {
            /* Block the worker until the post-input event stream goes quiet,
             * with an explicit divergence trip-wire. Arguments are optional and
             * have sensible defaults; full form is
             *   wait-converge [bucket_ms quiet_buckets diverged_buckets
             *                  threshold_per_bucket timeout_ms
             *                  failure-marker=path]
             *
             * Validation: parse with strtoull and reject any "-" prefix or
             * trailing garbage so a typo like `wait-converge -1` does not get
             * silently accepted as ULONG_MAX. Each value is clamped against a
             * per-field ceiling that keeps the underlying nanosleep, int casts,
             * and counter math in spec.
             */
            unsigned long long fields[5] = {50ULL, 2ULL, 16ULL, 32ULL, 5000ULL};
            const unsigned long long ceilings[5] = {
                60000ULL,   /* bucket_ms: max one minute */
                1000ULL,    /* quiet_buckets: bounded by int cast */
                1000ULL,    /* diverged_buckets: bounded by int cast */
                1000000ULL, /* threshold_per_bucket: well above realistic */
                600000ULL,  /* timeout_ms: ten minutes */
            };
            const char *cursor = args;
            for (int idx = 0; idx < 5; idx++) {
                while (*cursor == ' ' || *cursor == '\t')
                    cursor++;
                if (*cursor == '\0')
                    break;
                /* Allow `failure-marker=` to appear at any position, including
                 * before any numeric field. Without this, forms like
                 *   wait-converge failure-marker=foo
                 *   wait-converge 200 failure-marker=foo
                 * are rejected by strtoull as "bad arg N" because the marker is
                 * not a digit. Break out here so remaining fields keep their
                 * defaults and the post-loop check handles the marker.
                 */
                if (!strncmp(cursor, "failure-marker=", 15))
                    break;
                if (*cursor == '-') {
                    fprintf(stderr,
                            "replay: line %d: wait-converge rejects "
                            "negative arg %d\n",
                            lineno, idx + 1);
                    cursor = NULL;
                    break;
                }
                char *endp = NULL;
                errno = 0;
                unsigned long long parsed = strtoull(cursor, &endp, 10);
                if (cursor == endp || errno == ERANGE) {
                    fprintf(stderr,
                            "replay: line %d: wait-converge bad arg %d\n",
                            lineno, idx + 1);
                    cursor = NULL;
                    break;
                }
                if (parsed > ceilings[idx])
                    parsed = ceilings[idx];
                fields[idx] = parsed;
                cursor = endp;
            }
            /* The promise in the leading comment is "reject trailing garbage";
             * without this post-loop check `wait-converge 200 2 50 200
             * 15000xyz` silently parses as 15000 and ignores the suffix. Skip
             * whitespace and reject anything else so a typo surfaces as a
             * deterministic failure (codex-flagged).
             */
            if (cursor) {
                while (*cursor == ' ' || *cursor == '\t')
                    cursor++;
                if (*cursor != '\0' &&
                    strncmp(cursor, "failure-marker=", 15) != 0) {
                    fprintf(stderr,
                            "replay: line %d: wait-converge trailing garbage "
                            "after 5 args: '%s'\n",
                            lineno, cursor);
                    cursor = NULL;
                }
            }
            if (!cursor) {
                /* Malformed input. Break out of the script so the runner cannot
                 * proceed past the failed synchronization point with the rest
                 * of the .replay still queued. A continue here would silently
                 * drop the verb and let the next state-snapshot / assert-state
                 * run against unsettled state, defeating the synchronization
                 * contract.
                 */
                break;
            }
            unsigned long long bucketMs = fields[0];
            unsigned long long quietBuckets = fields[1];
            unsigned long long divergedBuckets = fields[2];
            unsigned long long thresholdPerBucket = fields[3];
            unsigned long long timeoutMs = fields[4];
            const char *failurePath =
                cursor && *cursor != '\0' ? cursor + 15 : NULL;
            uint64_t anchorMs =
                replayWaitConvergeAnchorMs((uint64_t) timeoutMs);
            int rc = timelineWaitConverge(
                anchorMs, (uint64_t) bucketMs, (int) quietBuckets,
                (int) divergedBuckets, (uint64_t) thresholdPerBucket,
                (uint64_t) timeoutMs);
            if (rc < 0) {
                fprintf(stderr,
                        "replay: line %d: wait-converge diverged after "
                        "%llu ms windows\n",
                        lineno, bucketMs * divergedBuckets);
                if (failurePath)
                    writeWaitConvergeFailure(failurePath, lineno, "diverged");
            } else if (rc == 0) {
                fprintf(stderr,
                        "replay: line %d: wait-converge timed out after "
                        "%llu ms\n",
                        lineno, timeoutMs);
                if (failurePath)
                    writeWaitConvergeFailure(failurePath, lineno, "timeout");
            }
            /* Do not break the script on timeout/divergence (rc <= 0). The
             * runner's expanded replay puts a state-snapshot line immediately
             * after every wait-converge and blocks waiting for the JSON marker.
             * Breaking here leaves the marker unwritten, and the runner spends
             * the full timeout plus slack polling before raising. Letting the
             * next line run writes the JSON (with the current, possibly
             * unsettled state) so the runner can observe the failure marker
             * above instead of hanging on missing synchronization JSON.
             */
        } else if (!strcmp(cmd, "state-snapshot")) {
            /* Marshal the in-process focus / grab / window / property state to
             * a JSON file. Synchronous so a follow-up assert-state in the
             * runner sees a fully-written file.
             */
            if (*args == '\0') {
                fprintf(stderr,
                        "replay: line %d: state-snapshot needs a path\n",
                        lineno);
            } else {
                int rc = stateSnapshotRequestAndWait(args);
                if (rc != 0)
                    fprintf(stderr,
                            "replay: line %d: state-snapshot %s failed "
                            "(rc=%d)\n",
                            lineno, args, rc);
            }
        } else if (!strcmp(cmd, "snapshot")) {
            /* Capture the cached replay target window's backing surface to the
             * path that follows. Blocking: the main thread does the actual save
             * so the BMP exists by the time this command returns. The runner
             * inspects these files after runScript completes.
             */
            if (*args == '\0') {
                fprintf(stderr, "replay: line %d: snapshot needs a path\n",
                        lineno);
            } else {
                int rc = snapshotRequestAndWait(args);
                if (rc != 0)
                    fprintf(stderr,
                            "replay: line %d: snapshot %s failed (rc=%d)\n",
                            lineno, args, rc);
            }
        } else if (!strcmp(cmd, "resize")) {
            /* Resize the cached replay target window. Drives SDL_SetWindowSize
             * on the main thread, which the SDL Cocoa backend translates into a
             * window-resize event that surfaces as ConfigureNotify for the X
             * client.
             */
            int w = 0, h = 0;
            if (sscanf(args, "%d %d", &w, &h) == 2) {
                int rc = snapshotRequestResizeAndWait(w, h);
                if (rc != 0)
                    fprintf(stderr,
                            "replay: line %d: resize %dx%d failed (rc=%d)\n",
                            lineno, w, h, rc);
            } else {
                fprintf(stderr, "replay: line %d: resize expects W H\n",
                        lineno);
            }
        } else {
            fprintf(stderr, "replay: line %d: unknown command '%s'\n", lineno,
                    cmd);
        }
    }
    fclose(fp);
}

static void *replayThread(void *unused)
{
    (void) unused;
    LOG("replay: thread started, path=%s\n", replayPath);
    /* Brief settle delay so XMapWindow's host realization and the first
     * SDL_PumpEvents have finished before the script starts injecting.
     */
    struct timespec ts = {0, 300000000L};
    nanosleep(&ts, NULL);
    LOG("replay: starting script\n");
    runScript(replayPath);
    LOG("replay: script done\n");
    return NULL;
}

static void initOnce(void)
{
    const char *path = getenv("LIBX11_COMPAT_REPLAY");
    if (!path || !*path)
        return;
    replayPath = strdup(path);
    if (!replayPath)
        return;
    int rc = pthread_create(&replayThreadHandle, NULL, replayThread, NULL);
    if (rc != 0)
        LOG("replay: pthread_create failed: %d\n", rc);
    else
        replayThreadSpawned = True;
}

/* Capture the Display once on the first call; later calls (e.g. subsequent
 * top-level mappings) must not overwrite a pointer the background thread may
 * already be dereferencing. pthread_once still gates the actual thread spawn so
 * any racing first-callers serialize through it.
 */
void replayStartIfRequested(Display *display)
{
    if (!display)
        return;
    if (!replayDisplay)
        replayDisplay = display;
    pthread_once(&replayOnce, initOnce);
}

void replayStop(void)
{
    SDL_AtomicSet(&replayShouldStop, 1);

    /* Join synchronously so the caller (XCloseDisplay on the last display) can
     * safely tear down SDL afterward. The worker polls replayShouldStop between
     * commands and inside the chunked delay loop, so the join completes within
     * ~50ms of the flag being set -- bounded enough to keep XCloseDisplay's
     * latency reasonable.
     */
    if (replayThreadSpawned) {
        pthread_join(replayThreadHandle, NULL);
        replayThreadSpawned = False;
    }
}
