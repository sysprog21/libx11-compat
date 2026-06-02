#include <X11/Xlib.h>
#include <stdio.h>
#include <sys/time.h>

static double now_seconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double) tv.tv_sec + (double) tv.tv_usec / 1000000.0;
}

static void bench_many_small_draw_arcs(Display *display, Pixmap pixmap, GC gc)
{
    double start = now_seconds();
    for (int i = 0; i < 2000; i++) {
        int x = i % 96;
        int y = (i / 96) % 96;
        XDrawArc(display, pixmap, gc, x, y, 8, 8, 0, 180 * 64);
    }
    double elapsed = now_seconds() - start;
    printf("many-small-XDrawArc %.6f sec %.3f usec/op\n", elapsed,
           elapsed * 1000000.0 / 2000.0);
}

static void bench_large_fill_arc(Display *display, Pixmap pixmap, GC gc)
{
    double start = now_seconds();
    for (int i = 0; i < 50; i++)
        XFillArc(display, pixmap, gc, 8, 8, 112, 112, 0, 360 * 64);
    double elapsed = now_seconds() - start;
    printf("large-XFillArc %.6f sec %.3f usec/op\n", elapsed,
           elapsed * 1000000.0 / 50.0);
}

static void bench_convex_polygon(Display *display, Pixmap pixmap, GC gc)
{
    XPoint points[] = {{8, 8}, {120, 12}, {80, 120}, {16, 96}};
    double start = now_seconds();
    for (int i = 0; i < 1000; i++)
        XFillPolygon(display, pixmap, gc, points, 4, Convex, CoordModeOrigin);
    double elapsed = now_seconds() - start;
    printf("convex-XFillPolygon %.6f sec %.3f usec/op\n", elapsed,
           elapsed * 1000000.0 / 1000.0);
}

static void bench_self_intersecting_polygon(Display *display,
                                            Pixmap pixmap,
                                            GC gc)
{
    XPoint points[] = {{16, 16}, {112, 112}, {112, 16}, {16, 112}};
    double start = now_seconds();
    for (int i = 0; i < 1000; i++)
        XFillPolygon(display, pixmap, gc, points, 4, Complex, CoordModeOrigin);
    double elapsed = now_seconds() - start;
    printf("self-intersecting-XFillPolygon %.6f sec %.3f usec/op\n", elapsed,
           elapsed * 1000000.0 / 1000.0);
}

static void bench_wide_line(Display *display, Pixmap pixmap, GC gc)
{
    XSetLineAttributes(display, gc, 5, LineSolid, CapButt, JoinMiter);
    double start = now_seconds();
    for (int i = 0; i < 1000; i++)
        XDrawLine(display, pixmap, gc, 4, 4, 124, 124);
    double elapsed = now_seconds() - start;
    printf("wide-XDrawLine %.6f sec %.3f usec/op\n", elapsed,
           elapsed * 1000000.0 / 1000.0);
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "XOpenDisplay failed\n");
        return 1;
    }
    Window root = RootWindow(display, DefaultScreen(display));
    Pixmap pixmap =
        XCreatePixmap(display, root, 128, 128, DefaultDepth(display, 0));
    GC gc = XCreateGC(display, pixmap, 0, NULL);
    XSetForeground(display, gc, 0xFFFF0000);
    bench_many_small_draw_arcs(display, pixmap, gc);
    bench_large_fill_arc(display, pixmap, gc);
    bench_convex_polygon(display, pixmap, gc);
    bench_self_intersecting_polygon(display, pixmap, gc);
    bench_wide_line(display, pixmap, gc);
    XFreeGC(display, gc);
    XFreePixmap(display, pixmap);
    XCloseDisplay(display);
    return 0;
}
