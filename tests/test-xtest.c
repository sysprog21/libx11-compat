/* In-tree regression for the XTest fake-event path in src/xtest.c.
 *
 * Opens a display, maps a top-level window with mouse + key event masks, then
 * drives synthetic events via XTestFakeMotionEvent / XTestFakeButtonEvent /
 * XTestFakeKeyEvent and asserts the matching X events come back out of the
 * queue. Without this test the XTest path is exercised only by ViolaWWW + the
 * replay engine, neither of which runs from make check.
 *
 * Runs under SDL_VIDEODRIVER=dummy so it works on headless CI.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include "sdl-compat.h"
#include "replay-target.h"

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "test-xtest FAIL: %s (%s:%d)\n", msg, __FILE__, \
                    __LINE__);                                              \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

/* Pull count events from the queue, waiting on each one via XNextEvent.
 * Returns the number of events of wanted_type that appeared.
 */
static int drain_until(Display *dpy, int wanted_type, int max_iterations)
{
    int hits = 0;
    for (int i = 0; i < max_iterations; i++) {
        XEvent ev;
        if (XPending(dpy) == 0)
            break;
        XNextEvent(dpy, &ev);
        if (ev.type == wanted_type)
            hits++;
    }
    return hits;
}

static XEvent next_event_of_type(Display *dpy,
                                 int wanted_type,
                                 int max_iterations,
                                 const char *msg)
{
    for (int i = 0; i < max_iterations && XPending(dpy) > 0; i++) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == wanted_type)
            return ev;
    }
    CHECK(0, msg);
    XEvent never;
    memset(&never, 0, sizeof(never));
    return never;
}

static void expect_connection_readable(Display *dpy, const char *msg)
{
    fd_set fds;
    FD_ZERO(&fds);
    int fd = ConnectionNumber(dpy);
    FD_SET(fd, &fds);
    struct timeval tv = {0, 200000};
    int rc = select(fd + 1, &fds, NULL, NULL, &tv);
    CHECK(rc > 0 && FD_ISSET(fd, &fds), msg);
}

typedef struct {
    Display *dpy;
    int screen;
    int motion_ok;
    int press_ok;
    int release_ok;
} AsyncFakeClickArgs;

static void *async_fake_click(void *opaque)
{
    AsyncFakeClickArgs *args = opaque;
    args->motion_ok =
        XTestFakeMotionEvent(args->dpy, args->screen, 130, 145, 0);
    args->press_ok = XTestFakeButtonEvent(args->dpy, Button1, True, 0);
    args->release_ok = XTestFakeButtonEvent(args->dpy, Button1, False, 0);
    return NULL;
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    CHECK(dpy != NULL, "XOpenDisplay");

    /* XTest must advertise itself once the display is open. */
    int opcode = 0, evt = 0, err = 0, major = 0, minor = 0;
    CHECK(XQueryExtension(dpy, XTestExtensionName, &opcode, &evt, &err),
          "XQueryExtension advertises XTEST");
    CHECK(opcode != 0, "XTEST generic opcode is non-zero");
    CHECK(XTestQueryExtension(dpy, &evt, &err, &major, &minor),
          "XTestQueryExtension");
    CHECK(major >= 2, "XTest major version >= 2");

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Window win =
        XCreateSimpleWindow(dpy, root, 70, 80, 320, 240, 0,
                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    CHECK(win != None, "XCreateSimpleWindow");
    XSelectInput(dpy, win,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     KeyPressMask | KeyReleaseMask | ExposureMask);
    XMapWindow(dpy, win);
    XSync(dpy, False);

    /* Drain the initial Expose / MapNotify burst so the assertions below count
     * only XTest-driven events.
     */
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
    }

    /* Button-only callers should click at the current pointer location, not at
     * the zero-initialized fake-motion cache.
     */
    Uint32 target_id = replayTargetWindowId();
    CHECK(target_id != 0, "XTest cached target window id");
    int translated_root_x = 0;
    int translated_root_y = 0;
    CHECK(replayTargetTranslateLocal(25, 35, &translated_root_x,
                                     &translated_root_y),
          "replay target translated local coordinates to root coordinates");
    CHECK(translated_root_x == 95, "replay target local-to-root translated x");
    CHECK(translated_root_y == 115, "replay target local-to-root translated y");
    SDL_Window *target_sdl_window = SDL_GetWindowFromID(target_id);
    CHECK(target_sdl_window != NULL, "XTest cached target SDL window");
    SDL_WarpMouseInWindow(target_sdl_window, 40, 50);
    SDL_PumpEvents();
    int mouse_x = 0, mouse_y = 0;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    CHECK(mouse_x == 40 && mouse_y == 50,
          "test setup moved SDL pointer to window-local 40,50");
    CHECK(XTestFakeButtonEvent(dpy, 1, True, 0) == 1,
          "button-only XTestFakeButtonEvent press");
    CHECK(XTestFakeButtonEvent(dpy, 1, False, 0) == 1,
          "button-only XTestFakeButtonEvent release");
    XSync(dpy, False);
    XEvent button_only_press =
        next_event_of_type(dpy, ButtonPress, 32, "button-only ButtonPress");
    XEvent button_only_release =
        next_event_of_type(dpy, ButtonRelease, 32, "button-only ButtonRelease");
    CHECK(button_only_press.xbutton.window == win,
          "button-only ButtonPress routed to test window");
    CHECK(button_only_release.xbutton.window == win,
          "button-only ButtonRelease routed to test window");
    CHECK(button_only_press.xbutton.x_root == 110,
          "button-only ButtonPress keeps current root x");
    CHECK(button_only_press.xbutton.y_root == 130,
          "button-only ButtonPress keeps current root y");
    CHECK(button_only_press.xbutton.x == 40,
          "button-only ButtonPress keeps current window-local x");
    CHECK(button_only_press.xbutton.y == 50,
          "button-only ButtonPress keeps current window-local y");

    AsyncFakeClickArgs async_args = {dpy, screen, 0, 0, 0};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, async_fake_click, &async_args) == 0,
          "pthread_create async XTest click");
    CHECK(pthread_join(thread, NULL) == 0, "pthread_join async XTest click");
    CHECK(async_args.motion_ok == 1 && async_args.press_ok == 1 &&
              async_args.release_ok == 1,
          "async XTest fake calls returned success");
    expect_connection_readable(
        dpy,
        "async XTest push woke the X connection fd before event conversion");
    XEvent async_motion =
        next_event_of_type(dpy, MotionNotify, 32, "async MotionNotify");
    XEvent async_press =
        next_event_of_type(dpy, ButtonPress, 32, "async ButtonPress");
    XEvent async_release =
        next_event_of_type(dpy, ButtonRelease, 32, "async ButtonRelease");
    CHECK(async_motion.xmotion.x_root == 130,
          "async MotionNotify keeps root x");
    CHECK(async_motion.xmotion.y_root == 145,
          "async MotionNotify keeps root y");
    CHECK(async_press.xbutton.x_root == 130, "async ButtonPress keeps root x");
    CHECK(async_press.xbutton.y_root == 145, "async ButtonPress keeps root y");
    CHECK(async_release.xbutton.x_root == 130,
          "async ButtonRelease keeps root x");
    CHECK(async_release.xbutton.y_root == 145,
          "async ButtonRelease keeps root y");

    CHECK(XTestFakeRelativeMotionEvent(dpy, 5, -3, 0) == 1,
          "relative-only XTestFakeRelativeMotionEvent");
    XSync(dpy, False);
    XEvent relative_motion =
        next_event_of_type(dpy, MotionNotify, 32, "relative-only MotionNotify");
    CHECK(relative_motion.xmotion.window == win,
          "relative-only MotionNotify routed to test window");
    CHECK(relative_motion.xmotion.x_root == 135,
          "relative-only MotionNotify keeps current-plus-delta root x");
    CHECK(relative_motion.xmotion.y_root == 142,
          "relative-only MotionNotify keeps current-plus-delta root y");
    CHECK(relative_motion.xmotion.x == 65,
          "relative-only MotionNotify keeps current-plus-delta local x");
    CHECK(relative_motion.xmotion.y == 62,
          "relative-only MotionNotify keeps current-plus-delta local y");

    /* MotionNotify path. */
    CHECK(XTestFakeMotionEvent(dpy, screen, 100, 120, 0) == 1,
          "XTestFakeMotionEvent return");
    XSync(dpy, False);
    XEvent motion = next_event_of_type(dpy, MotionNotify, 16,
                                       "MotionNotify reached the queue");
    CHECK(motion.xmotion.window == win, "MotionNotify routed to test window");
    CHECK(motion.xmotion.x_root == 100, "MotionNotify keeps root x");
    CHECK(motion.xmotion.y_root == 120, "MotionNotify keeps root y");
    CHECK(motion.xmotion.x == 30, "MotionNotify converts to window-local x");
    CHECK(motion.xmotion.y == 40, "MotionNotify converts to window-local y");

    /* ButtonPress / ButtonRelease pair, button 1. */
    CHECK(XTestFakeButtonEvent(dpy, 1, True, 0) == 1,
          "XTestFakeButtonEvent press");
    CHECK(XTestFakeButtonEvent(dpy, 1, False, 0) == 1,
          "XTestFakeButtonEvent release");
    XSync(dpy, False);

    XEvent press =
        next_event_of_type(dpy, ButtonPress, 32, "ButtonPress arrived");
    XEvent release =
        next_event_of_type(dpy, ButtonRelease, 32, "ButtonRelease arrived");
    CHECK(press.xbutton.window == win, "ButtonPress routed to test window");
    CHECK(release.xbutton.window == win, "ButtonRelease routed to test window");
    CHECK(press.xbutton.x_root == 100, "ButtonPress keeps root x");
    CHECK(press.xbutton.y_root == 120, "ButtonPress keeps root y");
    CHECK(press.xbutton.x == 30, "ButtonPress converts to window-local x");
    CHECK(press.xbutton.y == 40, "ButtonPress converts to window-local y");
    CHECK(release.xbutton.x_root == 100, "ButtonRelease keeps root x");
    CHECK(release.xbutton.y_root == 120, "ButtonRelease keeps root y");
    CHECK(release.xbutton.x == 30, "ButtonRelease converts to window-local x");
    CHECK(release.xbutton.y == 40, "ButtonRelease converts to window-local y");

    /* KeyPress path. The exact keysym mapping is host-locale-dependent in the
     * SDL_GetKeyFromScancode path; assert only that *some* KeyPress shows up
     * for a non-zero code so a future regression that drops the push entirely
     * would still fail this test.
     */
    CHECK(XTestFakeKeyEvent(dpy, 'a', True, 0) == 1, "XTestFakeKeyEvent press");
    CHECK(XTestFakeKeyEvent(dpy, 'a', False, 0) == 1,
          "XTestFakeKeyEvent release");
    XSync(dpy, False);
    int keys = drain_until(dpy, KeyPress, 32);
    CHECK(keys > 0, "KeyPress arrived");

    /* With an IC focused, the bare keydown is suppressed in favor of the IM
     * commit. XTest must still deliver the character: it emits a companion
     * text event that becomes a keycode==0 commit, which even the simple 8-bit
     * XLookupString resolves (the path Mosaic's URL field uses).
     */
    XIM im = XOpenIM(dpy, NULL, NULL, NULL);
    CHECK(im != NULL, "XOpenIM");
    XIC ic = XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                       XNClientWindow, win, XNFocusWindow, win, NULL);
    CHECK(ic != NULL, "XCreateIC");
    XSetICFocus(ic);
    while (XPending(dpy) > 0) {
        XEvent drain;
        XNextEvent(dpy, &drain);
    }
    CHECK(XTestFakeKeyEvent(dpy, 'a', True, 0) == 1, "XTest key press with IC");
    CHECK(XTestFakeKeyEvent(dpy, 'a', False, 0) == 1,
          "XTest key release with IC");
    XSync(dpy, False);
    Bool gotCommit = False;
    for (int i = 0; i < 32 && XPending(dpy) > 0; i++) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == KeyPress && ev.xkey.keycode == 0) {
            char buf[8] = {0};
            KeySym ks = NoSymbol;
            int n = XLookupString(&ev.xkey, buf, sizeof(buf), &ks, NULL);
            CHECK(n == 1 && buf[0] == 'a',
                  "XLookupString resolved the XTest IM commit");
            gotCommit = True;
            break;
        }
    }
    CHECK(gotCommit, "XTest typing under a focused IC produced an IM commit");
    XUnsetICFocus(ic);
    XDestroyIC(ic);
    XCloseIM(im);

    XUnmapWindow(dpy, win);
    XSync(dpy, False);
    CHECK(replayTargetWindowId() != target_id,
          "XUnmapWindow retired cached XTest target");
    XMapWindow(dpy, win);
    XSync(dpy, False);
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
    }
    target_id = replayTargetWindowId();
    CHECK(target_id != 0, "XTest re-cached target after remap");

    /* Target re-adoption after retire: XDestroyWindow the big window, then map
     * a small popup followed by a similarly-sized main window. The high water
     * mark floor in replayTargetOfferWindow must reject the popup so the
     * subsequent main wins the cache, instead of the popup grabbing the slot
     * just because it raced the new main into the queue. This is the gap
     * surfaced by check-smoke-motif: when fileview's language dialog dismisses,
     * several Motif drag popups (10x10 override-redirect) map alongside the new
     * XmMainWindow and would otherwise become the smoke target.
     */
    XDestroyWindow(dpy, win);
    XSync(dpy, False);
    CHECK(replayTargetWindowId() != target_id,
          "XDestroyWindow retired cached XTest target");
    /* Pump events so the destroyed-window cleanup propagates through the queue
     * before mapping replacement windows.
     */
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
    }
    /* Tiny popup-shaped window (would normally land first). */
    Window popup =
        XCreateSimpleWindow(dpy, root, 0, 0, 10, 10, 0, BlackPixel(dpy, screen),
                            WhitePixel(dpy, screen));
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    XChangeWindowAttributes(dpy, popup, CWOverrideRedirect, &attrs);
    XMapWindow(dpy, popup);
    XSync(dpy, False);
    /* Same-size successor "main" window. */
    Window successor =
        XCreateSimpleWindow(dpy, root, 0, 0, 320, 240, 0,
                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, successor,
                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    XMapWindow(dpy, successor);
    XSync(dpy, False);
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
    }
    CHECK(XTestFakeButtonEvent(dpy, 1, True, 0) == 1,
          "post-retire cached-position ButtonPress returned 1");
    CHECK(XTestFakeButtonEvent(dpy, 1, False, 0) == 1,
          "post-retire cached-position ButtonRelease returned 1");
    XSync(dpy, False);
    XEvent successor_press =
        next_event_of_type(dpy, ButtonPress, 32,
                           "post-retire cached-position ButtonPress arrived");
    CHECK(successor_press.xbutton.window == successor,
          "post-retire cached-position ButtonPress routed to successor");
    CHECK(successor_press.xbutton.x_root == 100,
          "post-retire cached-position ButtonPress preserved root x");
    CHECK(successor_press.xbutton.y_root == 120,
          "post-retire cached-position ButtonPress preserved root y");
    CHECK(successor_press.xbutton.x == 100,
          "post-retire cached-position ButtonPress rebased local x");
    CHECK(successor_press.xbutton.y == 120,
          "post-retire cached-position ButtonPress rebased local y");

    /* Fire a synthetic motion and confirm the test receives a MotionNotify on
     * the successor and not on the popup. The popup did not select
     * PointerMotionMask, so even if xtest accidentally targeted it the event
     * would be silently dropped instead of arriving here.
     */
    CHECK(XTestFakeMotionEvent(dpy, screen, 80, 90, 0) == 1,
          "post-retire fake motion returned 1");
    XSync(dpy, False);
    int successorMotions = 0;
    for (int i = 0; i < 16 && XPending(dpy) > 0; i++) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == MotionNotify && ev.xmotion.window == successor)
            successorMotions++;
    }
    CHECK(successorMotions >= 1,
          "post-retire motion routed to the successor (high-water re-adopt)");
    XDestroyWindow(dpy, popup);
    XDestroyWindow(dpy, successor);
    XSync(dpy, False);

    /* Nested sub-window routing. A synthetic event carries only a root
     * coordinate, so xtest leans on getContainingWindow descending the child
     * tree to the deepest window that contains the point. Every assertion above
     * used a childless window, so that descent went untested, yet it is exactly
     * the path a toolbox click drives under the differential replay: each xfig
     * tool icon is its own X sub-window. Build a parent holding two spaced
     * children with a bare gap between them and confirm a click lands on the
     * geometrically correct child, with a gap click reaching neither. A descent
     * that misroutes by an offset, the failure mode behind the xfig draw-line
     * divergence, trips here under SDL_VIDEODRIVER=dummy.
     */
    Window form =
        XCreateSimpleWindow(dpy, root, 0, 0, 200, 200, 0,
                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    CHECK(form != None, "nested-routing parent create");
    /* The parent selects no input on purpose: a click that misses every child
     * must drop, mirroring an xfig click on the inert "Drawing" label band that
     * selects no tool on system X11.
     */
    XMapWindow(dpy, form);
    Window toolTop =
        XCreateSimpleWindow(dpy, form, 10, 10, 30, 30, 0,
                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    Window toolBottom =
        XCreateSimpleWindow(dpy, form, 10, 110, 30, 30, 0,
                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    XSelectInput(dpy, toolTop, ButtonPressMask | PointerMotionMask);
    XSelectInput(dpy, toolBottom, ButtonPressMask | PointerMotionMask);
    XMapWindow(dpy, toolTop);
    XMapWindow(dpy, toolBottom);
    XSync(dpy, False);
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
    }
    CHECK(replayTargetWindowId() != 0, "nested-routing parent became target");

    /* Point inside the top tool routes to toolTop at its child-local origin. */
    CHECK(XTestFakeMotionEvent(dpy, screen, 25, 25, 0) == 1,
          "nested-routing top motion returned 1");
    XSync(dpy, False);
    XEvent topMotion =
        next_event_of_type(dpy, MotionNotify, 32, "nested-routing top motion");
    CHECK(topMotion.xmotion.window == toolTop,
          "nested-routing motion routed to the top tool");
    CHECK(topMotion.xmotion.x == 15 && topMotion.xmotion.y == 15,
          "nested-routing top motion converted to child-local coords");

    /* Point inside the bottom tool routes to toolBottom, proving the descent
     * tracks each child's distinct offset rather than a single fixed origin.
     */
    CHECK(XTestFakeMotionEvent(dpy, screen, 25, 125, 0) == 1,
          "nested-routing bottom motion returned 1");
    XSync(dpy, False);
    XEvent bottomMotion = next_event_of_type(dpy, MotionNotify, 32,
                                             "nested-routing bottom motion");
    CHECK(bottomMotion.xmotion.window == toolBottom,
          "nested-routing motion routed to the bottom tool");
    CHECK(bottomMotion.xmotion.x == 15 && bottomMotion.xmotion.y == 15,
          "nested-routing bottom motion converted to child-local coords");

    /* Point in the gap between the tools hits the inert parent and drops, never
     * leaking onto a tool. This is the assertion the differential needs: a
     * click off every tool must select no tool on both backends.
     */
    CHECK(XTestFakeMotionEvent(dpy, screen, 25, 70, 0) == 1,
          "nested-routing gap motion returned 1");
    XSync(dpy, False);
    int gapToolMotions = 0;
    for (int i = 0; i < 16 && XPending(dpy) > 0; i++) {
        XEvent ev;
        XNextEvent(dpy, &ev);
        if (ev.type == MotionNotify &&
            (ev.xmotion.window == toolTop || ev.xmotion.window == toolBottom))
            gapToolMotions++;
    }
    CHECK(gapToolMotions == 0,
          "nested-routing gap motion reached neither tool");

    XDestroyWindow(dpy, toolTop);
    XDestroyWindow(dpy, toolBottom);
    XDestroyWindow(dpy, form);
    XSync(dpy, False);
    while (XPending(dpy) > 0) {
        XEvent ev;
        XNextEvent(dpy, &ev);
    }

    /* Idempotence: a fake event after the target window is destroyed must not
     * fault, and XTestForgetTargetWindow inside libx11-compat
     * unrealizeTopLevelWindow should leave subsequent calls inert.
     */
    int rc = XTestFakeMotionEvent(dpy, screen, 0, 0, 0);
    CHECK(rc == 0 || rc == 1, "post-destroy fake call did not crash");

    XCloseDisplay(dpy);
    printf("test-xtest: ok\n");
    return 0;
}
