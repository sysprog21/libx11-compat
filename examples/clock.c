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

    int x_fd = ConnectionNumber(dpy);
    int last_sec = -1;
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            switch (e.type) {
            case Expose:
                if (e.xexpose.count == 0)
                    present(dpy, win, buf, blit_gc);
                break;
            case KeyPress: {
                KeySym ks = XLookupKeysym(&e.xkey, 0);
                if (ks == XK_Escape || ks == XK_q || ks == XK_Q)
                    goto done;
            } break;
            case ClientMessage:
                if ((Atom) e.xclient.data.l[0] == wm_delete)
                    goto done;
                break;
            }
        }

        time_t now_t = time(NULL);
        struct tm now;
        localtime_r(&now_t, &now);
        if (now.tm_sec != last_sec) {
            render(dpy, buf, bg_gc, face_gc, hour_gc, second_gc, &now);
            present(dpy, win, buf, blit_gc);
            last_sec = now.tm_sec;
        }

#ifdef __EMSCRIPTEN__

        /* The browser main thread cannot block on select() or the canvas never
         * paints. emscripten_sleep yields to the browser (needs -sASYNCIFY) so
         * SDL delivers canvas repaints and input, then resumes the loop. The X
         * connection fd is not polled in this path.
         */
        (void) x_fd;
        emscripten_sleep(TICK_MS);
#else
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x_fd, &rfds);
        struct timeval tv = {0, TICK_MS * 1000};
        select(x_fd + 1, &rfds, NULL, NULL, &tv);
#endif
    }
done:
    XFreeGC(dpy, blit_gc);
    XFreeGC(dpy, second_gc);
    XFreeGC(dpy, hour_gc);
    XFreeGC(dpy, face_gc);
    XFreeGC(dpy, bg_gc);
    XFreePixmap(dpy, buf);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
