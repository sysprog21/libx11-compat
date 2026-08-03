/*
 * MIT License
 * Copyright (c) 2026 Edison Ditto
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define SCREEN_WIDTH 500
#define SCREEN_HEIGHT 500

typedef struct {
    float x;
    float y;
} MyPoint;

static void rotate(MyPoint *p, int width, int height, float t)
{
    float x = p->x - width / 2.0f;
    float y = p->y - height / 2.0f;

    p->x = x * cosf(t) - y * sinf(t) + width / 2.0f;
    p->y = x * sinf(t) + y * cosf(t) + height / 2.0f;
}

static void draw_lines(Display *dpy, Drawable drawable, GC gc,
                       int width, int height, float t)
{
    enum { spaces = 50, lines = spaces - 1 };
    XSegment segments[lines];
    int hs = width / spaces;

    for (int c = 1; c <= lines; c++) {
        MyPoint start = {.x = hs * c, .y = 0};
        MyPoint end = {.x = hs * c, .y = height};

        rotate(&start, width, height, t);
        rotate(&end, width, height, t);
        segments[c - 1].x1 = start.x;
        segments[c - 1].y1 = start.y;
        segments[c - 1].x2 = end.x;
        segments[c - 1].y2 = end.y;
    }

    XDrawSegments(dpy, drawable, gc, segments, lines);
}

/* Allocate an RGB pixel from the default colormap, falling back to black so a
 * failed allocation stays visible rather than reusing whatever pixel was set.
 */
static unsigned long alloc_rgb(Display *dpy, int screen,
                               unsigned short r, unsigned short g,
                               unsigned short b)
{
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor color = {.red = r, .green = g, .blue = b};
    if (XAllocColor(dpy, cmap, &color))
        return color.pixel;
    return XBlackPixel(dpy, screen);
}

/* Fold one delivered event into the loop state: track the first Expose and
 * honor the WM close request.
 */
static void handle_event(const XEvent *event, Atom delete_window, bool *exposed,
                         bool *running)
{
    if (event->type == Expose)
        *exposed = true;
    else if (event->type == ClientMessage &&
             (Atom)event->xclient.data.l[0] == delete_window)
        *running = false;
}

static void draw_frame(Display *dpy, Pixmap pixmap, GC gc, int screen, float t)
{
    XSetForeground(dpy, gc, XWhitePixel(dpy, screen));
    XFillRectangle(dpy, pixmap, gc, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);

    XSetForeground(dpy, gc, alloc_rgb(dpy, screen, 0xffff, 0, 0));
    draw_lines(dpy, pixmap, gc, SCREEN_WIDTH, SCREEN_HEIGHT, 0);

    XSetForeground(dpy, gc, alloc_rgb(dpy, screen, 0, 0xffff, 0));
    draw_lines(dpy, pixmap, gc, SCREEN_WIDTH, SCREEN_HEIGHT, sinf(t) * M_PI / 6);
}

/* Loop state for the browser animation-frame callback (the native path keeps
 * its own locals, including the headless --once mode).
 */
struct moire_ctx {
    Display *dpy;
    Window window;
    Pixmap pixmap;
    GC gc;
    int screen;
    Atom delete_window;
    float t;
    bool running;
    bool exposed;
};

#ifdef __EMSCRIPTEN__

static void moire_teardown(struct moire_ctx *c)
{
    XFreeGC(c->dpy, c->gc);
    XFreePixmap(c->dpy, c->pixmap);
    XDestroyWindow(c->dpy, c->window);
    XCloseDisplay(c->dpy);
}

/* One animation frame: drain events, draw the rotated grids, present, advance
 * the phase. The browser drives this, so no blocking and no ASYNCIFY. Free on
 * the close request so a page that reuses the module does not leak.
 */
static void moire_em_frame(void *arg)
{
    struct moire_ctx *c = arg;
    while (XPending(c->dpy)) {
        XEvent event;
        XNextEvent(c->dpy, &event);
        handle_event(&event, c->delete_window, &c->exposed, &c->running);
    }
    if (!c->running) {
        emscripten_cancel_main_loop();
        moire_teardown(c);
        return;
    }
    draw_frame(c->dpy, c->pixmap, c->gc, c->screen, c->t);
    XCopyArea(c->dpy, c->pixmap, c->window, c->gc, 0, 0, SCREEN_WIDTH,
              SCREEN_HEIGHT, 0, 0);
    XFlush(c->dpy);
    c->t = remainderf(c->t + M_PI / 600, 2 * M_PI);
}
#endif

int main(int argc, char **argv)
{
    bool once = argc > 1 && strcmp(argv[1], "--once") == 0;
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "moire: can't connect to X server\n");
        return EXIT_FAILURE;
    }

    int screen = DefaultScreen(dpy);
    Window window = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 50, 50,
                                        SCREEN_WIDTH, SCREEN_HEIGHT, 2,
                                        XBlackPixel(dpy, screen),
                                        XWhitePixel(dpy, screen));
    XStoreName(dpy, window, "moire");
    XSelectInput(dpy, window, ExposureMask);

    /* The back-buffer pixmap is fixed at the artwork size, so pin the window to
     * it: a resize would otherwise leave the grown area blank or stale.
     */
    XSizeHints *hints = XAllocSizeHints();
    if (hints) {
        hints->flags = PMinSize | PMaxSize;
        hints->min_width = hints->max_width = SCREEN_WIDTH;
        hints->min_height = hints->max_height = SCREEN_HEIGHT;
        XSetWMNormalHints(dpy, window, hints);
        XFree(hints);
    }

    Atom delete_window = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, window, &delete_window, 1);
    XMapWindow(dpy, window);

    Pixmap pixmap = XCreatePixmap(dpy, window, SCREEN_WIDTH, SCREEN_HEIGHT,
                                  DefaultDepth(dpy, screen));
    GC gc = XCreateGC(dpy, pixmap, 0, NULL);
    XSetLineAttributes(dpy, gc, 4, LineSolid, CapButt, JoinMiter);

    bool running = true, exposed = false;
    float t = 0.0f;

#ifdef __EMSCRIPTEN__
    (void) once; /* no command-line args in the browser */
    /* Drive off requestAnimationFrame; simulate_infinite_loop keeps the runtime
     * alive, so ctx must be static. No blocking, so no ASYNCIFY.
     */
    static struct moire_ctx ctx;
    ctx = (struct moire_ctx){.dpy = dpy, .window = window, .pixmap = pixmap,
                             .gc = gc, .screen = screen,
                             .delete_window = delete_window, .t = t,
                             .running = running, .exposed = exposed};
    emscripten_set_main_loop_arg(moire_em_frame, &ctx, 0, 1);
    return EXIT_SUCCESS; /* not reached */
#else
    while (running) {
        while (XPending(dpy)) {
            XEvent event;
            XNextEvent(dpy, &event);
            handle_event(&event, delete_window, &exposed, &running);
        }
        if (!running)
            break;

        /* One-shot: block for the initial Expose so the single frame presents
         * to a mapped window instead of being dropped before it is shown.
         */
        if (once && !exposed) {
            XEvent event;
            XNextEvent(dpy, &event);
            handle_event(&event, delete_window, &exposed, &running);
            continue;
        }

        draw_frame(dpy, pixmap, gc, screen, t);
        XCopyArea(dpy, pixmap, window, gc, 0, 0,
                  SCREEN_WIDTH, SCREEN_HEIGHT, 0, 0);
        XFlush(dpy);

        if (once)
            break;

        t = remainderf(t + M_PI / 600, 2 * M_PI);
        struct timespec req = {.tv_sec = 0, .tv_nsec = 16666000};
        nanosleep(&req, NULL);
    }

    XFreeGC(dpy, gc);
    XFreePixmap(dpy, pixmap);
    XDestroyWindow(dpy, window);
    XCloseDisplay(dpy);
    return EXIT_SUCCESS;
#endif
}
