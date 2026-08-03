/*
 * clock - analog clock face driven by the system time.
 *
 * Press Esc or q (or close the window) to quit.
 *
 * Exercises: XDrawArc / XFillArc, XDrawLine, XCopyArea-based double buffering
 * off a pixmap, gettimeofday-paced redraw via select() on ConnectionNumber,
 * XSetForeground for swapping hand colors, WM_DELETE_WINDOW.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define WIN_SIZE 256
#define TICK_MS 100

static void draw_hand(Display *dpy,
                      Drawable d,
                      GC gc,
                      int cx,
                      int cy,
                      double angle,
                      double length)
{
    int x = cx + (int) (sin(angle) * length);
    int y = cy - (int) (cos(angle) * length);
    XDrawLine(dpy, d, gc, cx, cy, x, y);
}

static void render(Display *dpy,
                   Pixmap buf,
                   GC bg_gc,
                   GC face_gc,
                   GC hour_gc,
                   GC second_gc,
                   const struct tm *now)
{
    const int cx = WIN_SIZE / 2;
    const int cy = WIN_SIZE / 2;
    const int r = WIN_SIZE / 2 - 8;

    XFillRectangle(dpy, buf, bg_gc, 0, 0, WIN_SIZE, WIN_SIZE);
    /* Face outline. XDrawArc spans 360 * 64 sixteenths of a degree. */
    XDrawArc(dpy, buf, face_gc, cx - r, cy - r, r * 2, r * 2, 0, 360 * 64);
    /* Hour ticks: 12 short radial lines. */
    for (int i = 0; i < 12; i++) {
        double a = i * (2.0 * M_PI / 12.0);
        int x0 = cx + (int) (sin(a) * (r - 8));
        int y0 = cy - (int) (cos(a) * (r - 8));
        int x1 = cx + (int) (sin(a) * r);
        int y1 = cy - (int) (cos(a) * r);
        XDrawLine(dpy, buf, face_gc, x0, y0, x1, y1);
    }

    /* Smooth hand positions: minute hand creeps with seconds, hour hand with
     * minutes.
     */
    double s = now->tm_sec;
    double m = now->tm_min + s / 60.0;
    double h = (now->tm_hour % 12) + m / 60.0;
    draw_hand(dpy, buf, hour_gc, cx, cy, h * (2.0 * M_PI / 12.0), r * 0.50);
    draw_hand(dpy, buf, hour_gc, cx, cy, m * (2.0 * M_PI / 60.0), r * 0.75);
    draw_hand(dpy, buf, second_gc, cx, cy, s * (2.0 * M_PI / 60.0), r * 0.90);

    /* Cap dot at the pivot. */
    XFillArc(dpy, buf, face_gc, cx - 3, cy - 3, 6, 6, 0, 360 * 64);
}

static void present(Display *dpy, Window win, Pixmap buf, GC blit_gc)
{
    XCopyArea(dpy, buf, win, blit_gc, 0, 0, WIN_SIZE, WIN_SIZE, 0, 0);
    XFlush(dpy);
}

/* Everything the render loop needs, so one frame can run from either the native
 * loop or the browser's animation-frame callback.
 */
struct clock_ctx {
    Display *dpy;
    Window win;
    Pixmap buf;
    GC bg_gc, face_gc, hour_gc, second_gc, blit_gc;
    Atom wm_delete;
    int last_sec;
    int quit;
};

/* One frame: drain pending events, then redraw when the second changes. Sets
 * quit on a request to exit so the caller stops its loop.
 */
static void clock_frame(struct clock_ctx *c)
{
    while (XPending(c->dpy)) {
        XEvent e;
        XNextEvent(c->dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.count == 0)
                present(c->dpy, c->win, c->buf, c->blit_gc);
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Escape || ks == XK_q || ks == XK_Q)
                c->quit = 1;
        } break;
        case ClientMessage:
            if ((Atom) e.xclient.data.l[0] == c->wm_delete)
                c->quit = 1;
            break;
        }
    }
    if (c->quit)
        return;

    time_t now_t = time(NULL);
    struct tm now;
    localtime_r(&now_t, &now);
    if (now.tm_sec != c->last_sec) {
        render(c->dpy, c->buf, c->bg_gc, c->face_gc, c->hour_gc, c->second_gc,
               &now);
        present(c->dpy, c->win, c->buf, c->blit_gc);
        c->last_sec = now.tm_sec;
    }
}

static void clock_teardown(struct clock_ctx *c)
{
    XFreeGC(c->dpy, c->blit_gc);
    XFreeGC(c->dpy, c->second_gc);
    XFreeGC(c->dpy, c->hour_gc);
    XFreeGC(c->dpy, c->face_gc);
    XFreeGC(c->dpy, c->bg_gc);
    XFreePixmap(c->dpy, c->buf);
    XDestroyWindow(c->dpy, c->win);
    XCloseDisplay(c->dpy);
}

#ifdef __EMSCRIPTEN__

/* The browser drives the loop one frame per animation frame, so the main thread
 * never blocks and ASYNCIFY is not needed. Free on quit so a page that reuses
 * the module (an SPA embed) does not leak the Display and its resources.
 */
static void clock_em_frame(void *arg)
{
    struct clock_ctx *c = arg;
    clock_frame(c);
    if (c->quit) {
        emscripten_cancel_main_loop();
        clock_teardown(c);
    }
}
#endif

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "XOpenDisplay failed\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);
    int depth = DefaultDepth(dpy, screen);
    Window root = RootWindow(dpy, screen);
    unsigned long bg = WhitePixel(dpy, screen);
    unsigned long fg = BlackPixel(dpy, screen);

    Window win =
        XCreateSimpleWindow(dpy, root, 0, 0, WIN_SIZE, WIN_SIZE, 1, fg, bg);
    XStoreName(dpy, win, "clock");

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
    XMapWindow(dpy, win);

    Pixmap buf = XCreatePixmap(dpy, win, WIN_SIZE, WIN_SIZE, depth);

    /* Allocate a red pixel for the second hand. */
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor red = {.red = 0xE000, .green = 0x2000, .blue = 0x2000,
                  .flags = DoRed | DoGreen | DoBlue};
    XAllocColor(dpy, cmap, &red);

    XGCValues gcv;
    gcv.foreground = bg;
    GC bg_gc = XCreateGC(dpy, buf, GCForeground, &gcv);
    gcv.foreground = fg;
    GC face_gc = XCreateGC(dpy, buf, GCForeground, &gcv);
    gcv.foreground = fg;
    GC hour_gc = XCreateGC(dpy, buf, GCForeground, &gcv);
    XSetLineAttributes(dpy, hour_gc, 3, LineSolid, CapRound, JoinRound);
    gcv.foreground = red.pixel;
    GC second_gc = XCreateGC(dpy, buf, GCForeground, &gcv);
    GC blit_gc = XCreateGC(dpy, win, 0, NULL);

    struct clock_ctx ctx = {.dpy = dpy, .win = win, .buf = buf,
                            .bg_gc = bg_gc, .face_gc = face_gc,
                            .hour_gc = hour_gc, .second_gc = second_gc,
                            .blit_gc = blit_gc, .wm_delete = wm_delete,
                            .last_sec = -1, .quit = 0};

#ifdef __EMSCRIPTEN__

    /* Drive the loop off requestAnimationFrame (fps 0). simulate_infinite_loop
     * (1) makes this call not return, matching the native for(;;), so ctx must
     * outlive it: keep it static. No blocking, so no ASYNCIFY.
     */
    static struct clock_ctx em_ctx;
    em_ctx = ctx;
    emscripten_set_main_loop_arg(clock_em_frame, &em_ctx, 0, 1);
    return 0; /* not reached */
#else
    int x_fd = ConnectionNumber(dpy);
    while (!ctx.quit) {
        clock_frame(&ctx);
        if (ctx.quit)
            break;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x_fd, &rfds);
        struct timeval tv = {0, TICK_MS * 1000};
        select(x_fd + 1, &rfds, NULL, NULL, &tv);
    }
    clock_teardown(&ctx);
    return 0;
#endif
}
