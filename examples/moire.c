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
}
