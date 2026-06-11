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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <SDL2/SDL_atomic.h>
#include "replay.h"
#include "replay-target.h"
#include "snapshot.h"
#include "util.h"

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
            }
        } else if (!strcmp(cmd, "target-motion")) {
            int x = 0, y = 0;
            if (sscanf(args, "%d %d", &x, &y) == 2) {
                int rootX = 0, rootY = 0;
                if (replayTargetTranslateLocal(x, y, &rootX, &rootY))
                    XTestFakeMotionEvent(replayDisplay, 0, rootX, rootY, 0);
            }
        } else if (!strcmp(cmd, "button")) {
            unsigned int btn = 0;
            char dir[16] = {0};
            if (sscanf(args, "%u %15s", &btn, dir) == 2) {
                XTestFakeButtonEvent(replayDisplay, btn, isPressWord(dir), 0);
            }
        } else if (!strcmp(cmd, "click")) {
            int x = 0, y = 0;
            if (sscanf(args, "%d %d", &x, &y) == 2) {
                XTestFakeMotionEvent(replayDisplay, 0, x, y, 0);
                XTestFakeButtonEvent(replayDisplay, 1, True, 10);
                XTestFakeButtonEvent(replayDisplay, 1, False, 10);
            }
        } else if (!strcmp(cmd, "key")) {
            unsigned int code = 0;
            char dir[16] = {0};
            if (sscanf(args, "%u %15s", &code, dir) == 2)
                XTestFakeKeyEvent(replayDisplay, code, isPressWord(dir), 0);
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
