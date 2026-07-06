/*
 * the KitCat wall clock: a cat whose eyes flick and whose tail swings like a
 * pendulum, with analog hour and minute hands on its belly.
 *
 * Press Esc or q (or close the window) to quit.
 *
 * Derived from the "cat" mode of xclock, MIT Project Athena's X clock. Original
 * xclock: Copyright 1985 Massachusetts Institute of Technology. Author Tony
 * Della Fera (DEC), 1984. Cat mode and Motif port: Philip J. Schneider (DEC),
 * 1990. The pendulum-tail and eye-tracking math and the cat bitmaps (catback,
 * catwhite, cattie, eyes) are taken from
 * https://github.com/barkythedog/catclock .
 *
 * This file is a self-contained raw-Xlib rework for libx11-compat: the Motif
 * and Xt toolkit shell and the alarm/chime audio have been removed, and the cat
 * face is composited directly with core Xlib. The cat face is built once into a
 * pixmap; each tick redraws the hands and blits the next tail and eye frame
 * with XCopyPlane, so the tail sweeps through N_TAILS pendulum positions and
 * back.
 *
 * License (MIT/X11, from the upstream xclock and its bitmaps):
 *
 *   Permission to use, copy, modify, distribute, and sell this software and
 *   its documentation for any purpose is hereby granted without fee, provided
 *   that the above copyright notice appear in all copies and that both that
 *   copyright notice and this permission notice appear in supporting
 *   documentation.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

/* 1bpp cat artwork, LSB-first XY bitmaps (see xbm files). catback/catwhite/
 * cattie are the three color layers of the body; eyes is the moving eye patch.
 * The tail carries no bitmap: upstream's tail.xbm is entirely blank and the
 * tail shape is drawn onto a cleared bitmap each frame (see build_tail).
 */
#include "catback.xbm"
#include "catwhite.xbm"
#include "cattie.xbm"
#include "eyes.xbm"

#define WIN_W 150      /* DEF_CAT_WIDTH */
#define WIN_H 300      /* DEF_CAT_HEIGHT */
#define CAT_BOTTOM 210 /* tail overlay starts just below this */
#define TAIL_H 89
#define N_TAILS 16               /* pendulum positions; also sets swing speed */
#define TICK_MS (1000 / N_TAILS) /* one tail step per tick, ~1s per sweep */

/* Clock-hand geometry, centered on the cat's belly. Lengths and width are the
 * upstream cat-mode fractions of the face radius.
 */
#define CENTER_X (WIN_W / 2)
#define CENTER_Y (WIN_H / 2)
#define RADIUS 39 /* round((min(W,H) - 2*padding) / 3.45), padding 8 */
#define MINUTE_LEN ((70 * RADIUS) / 100)
#define HOUR_LEN ((40 * RADIUS) / 100)
#define HAND_HALF_W (((7 * RADIUS) / 100) * 2)

static unsigned long cat_color;    /* black body */
static unsigned long detail_color; /* white face and belly */
static unsigned long tie_color;    /* red bow tie */
static unsigned long background;

/* Monotonic-enough millisecond clock for pacing the animation. */
static int64_t now_millis(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t) tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Test bit (x, y) of an XBM bit array. Rows are padded to whole bytes. */
static int xbm_bit(const char *bits, int width, int x, int y)
{
    int stride = (width + 7) / 8;
    return (bits[y * stride + (x >> 3)] >> (x & 7)) & 1;
}

/* Composite the three body layers into one color pixmap. Later layers win, so
 * the priority is tie over white over black over background, matching the
 * upstream opaque-stipple-then-stipple draw order.
 */
static Pixmap build_cat_face(Display *dpy, Window win, int depth, Visual *vis)
{
    Pixmap face = XCreatePixmap(dpy, win, WIN_W, WIN_H, depth);
    /* XCreateImage takes ownership of the buffer; XDestroyImage frees both. */
    char *data = malloc((size_t) WIN_W * WIN_H * 4);
    if (!data) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
    XImage *img =
        XCreateImage(dpy, vis, depth, ZPixmap, 0, data, WIN_W, WIN_H, 32, 0);
    if (!img) {
        fprintf(stderr, "XCreateImage failed\n");
        free(data);
        exit(1);
    }
    for (int y = 0; y < WIN_H; y++) {
        for (int x = 0; x < WIN_W; x++) {
            unsigned long c = background;
            if (xbm_bit(catback_bits, catback_width, x, y))
                c = cat_color;
            if (xbm_bit(catwhite_bits, catwhite_width, x, y))
                c = detail_color;
            if (xbm_bit(cattie_bits, cattie_width, x, y))
                c = tie_color;
            XPutPixel(img, x, y, c);
        }
    }
    GC gc = XCreateGC(dpy, face, 0, NULL);
    XPutImage(dpy, face, gc, img, 0, 0, 0, 0, WIN_W, WIN_H);
    XFreeGC(dpy, gc);
    XDestroyImage(img);
    return face;
}

/* Rotate a point clockwise by the angle whose sine and cosine are s and c. */
static XPoint rotate_point(XPoint p, double s, double c)
{
    XPoint r = {(short) (int) (p.x * c + p.y * s),
                (short) (int) (-p.x * s + p.y * c)};
    return r;
}

/* One-bit tail frame at pendulum phase t: the static lower body with the tail
 * swung to angle A*sin(t + phi). Returned bitmap is blitted with XCopyPlane.
 */
static Pixmap build_tail(Display *dpy, Window win, double t)
{
    static const XPoint tail[] = {{0, 0},   {0, 76},  {3, 82}, {10, 84},
                                  {18, 82}, {21, 76}, {21, 70}};
    const int n = sizeof(tail) / sizeof(tail[0]);
    const XPoint origin = {74, -15};
    XPoint off[n], swung[n];

    /* The tail hooks to one side, so hang it slightly left before swinging. */
    double a = -0.08, s = sin(a), c = cos(a);
    for (int i = 0; i < n; i++)
        off[i] = rotate_point(tail[i], s, c);

    a = 0.4 * sin(t + 3 * M_PI_2); /* pendulum */
    s = sin(a);
    c = cos(a);
    for (int i = 0; i < n; i++) {
        swung[i] = rotate_point(off[i], s, c);
        swung[i].x += origin.x;
        swung[i].y += origin.y;
    }

    Pixmap bm = XCreatePixmap(dpy, win, WIN_W, TAIL_H, 1);
    XGCValues v = {.foreground = 0,
                   .line_width = 15,
                   .cap_style = CapRound,
                   .join_style = JoinRound};
    GC gc = XCreateGC(
        dpy, bm, GCForeground | GCLineWidth | GCCapStyle | GCJoinStyle, &v);
    XFillRectangle(dpy, bm, gc, 0, 0, WIN_W, TAIL_H); /* clear the 1bpp canvas */
    XSetForeground(dpy, gc, 1);
    XDrawLines(dpy, bm, gc, swung, n, CoordModeOrigin);
    XFreeGC(dpy, gc);
    return bm;
}

/* One-bit eye frame at phase t: two pupils rotated on a sphere so the eyes
 * track back and forth in sync with the tail.
 */
static Pixmap build_eyes(Display *dpy, Window win, double t)
{
    XPoint pts[100];
    double angle = 0.7 * sin(t + 3 * M_PI_2) + M_PI / 2.0;
    int n = 0;

    /* Trace one pupil outline as a projected half-sphere, then again shifted
     * right for the second eye.
     */
    for (double u = -M_PI / 2.0; u < M_PI / 2.0; u += 0.25, n++) {
        double z = 2.0 + cos(u) * sin(angle + M_PI / 7.0);
        pts[n].x = (int) ((cos(u) * cos(angle + M_PI / 7.0) / z) * 23.0 + 12.0);
        pts[n].y = (int) ((sin(u) / z) * 23.0 + 11.0);
    }
    for (double u = M_PI / 2.0; u > -M_PI / 2.0; u -= 0.25, n++) {
        double z = 2.0 + cos(u) * sin(angle - M_PI / 7.0);
        pts[n].x = (int) ((cos(u) * cos(angle - M_PI / 7.0) / z) * 23.0 + 12.0);
        pts[n].y = (int) ((sin(u) / z) * 23.0 + 11.0);
    }

    Pixmap bm =
        XCreateBitmapFromData(dpy, win, eyes_bits, eyes_width, eyes_height);
    GC gc = XCreateGC(dpy, bm, 0, NULL);
    XSetForeground(dpy, gc, 1);
    XFillPolygon(dpy, bm, gc, pts, n, Nonconvex, CoordModeOrigin);
    for (int i = 0; i < n; i++)
        pts[i].x += 31;
    XFillPolygon(dpy, bm, gc, pts, n, Nonconvex, CoordModeOrigin);
    XFreeGC(dpy, gc);
    return bm;
}

/* Draw one filled clock hand (a thin triangle) at fraction f of a full turn,
 * outlined so it shows on any belly color.
 */
static void draw_hand(Display *dpy,
                      Drawable d,
                      GC fill,
                      GC edge,
                      int length,
                      double f)
{
    double a = 2.0 * M_PI * f, s = sin(a), c = cos(a);
    int hw = HAND_HALF_W;
    double ws = hw * s, wc = hw * c;
    XPoint tri[4] = {
        {CENTER_X + (int) (length * s), CENTER_Y - (int) (length * c)},
        {CENTER_X - (int) (ws + wc), CENTER_Y + (int) (wc - ws)},
        {CENTER_X - (int) (ws - wc), CENTER_Y + (int) (wc + ws)},
        {CENTER_X + (int) (length * s), CENTER_Y - (int) (length * c)},
    };
    XFillPolygon(dpy, d, fill, tri, 3, Convex, CoordModeOrigin);
    XDrawLines(dpy, d, edge, tri, 4, CoordModeOrigin);
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
    Visual *vis = DefaultVisual(dpy, screen);
    Window root = RootWindow(dpy, screen);

    background = WhitePixel(dpy, screen);
    cat_color = BlackPixel(dpy, screen);
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor white = {.red = 0xffff,
                    .green = 0xffff,
                    .blue = 0xffff,
                    .flags = DoRed | DoGreen | DoBlue};
    XColor red = {.red = 0xd000,
                  .green = 0x1000,
                  .blue = 0x1000,
                  .flags = DoRed | DoGreen | DoBlue};
    /* Fall back to the black/white monochrome pair if the colormap is full. */
    detail_color =
        XAllocColor(dpy, cmap, &white) ? white.pixel : WhitePixel(dpy, screen);
    tie_color =
        XAllocColor(dpy, cmap, &red) ? red.pixel : BlackPixel(dpy, screen);

    Window win = XCreateSimpleWindow(dpy, root, 0, 0, WIN_W, WIN_H, 0,
                                     cat_color, background);
    XStoreName(dpy, win, "catclock");
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    /* Fixed size: the artwork is 150x300 and does not scale. */
    XSizeHints hints = {.flags = PMinSize | PMaxSize,
                        .min_width = WIN_W,
                        .min_height = WIN_H,
                        .max_width = WIN_W,
                        .max_height = WIN_H};
    XSetWMNormalHints(dpy, win, &hints);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
    XMapWindow(dpy, win);

    Pixmap face = build_cat_face(dpy, win, depth, vis);
    Pixmap face_with_hands = XCreatePixmap(dpy, win, WIN_W, WIN_H, depth);
    Pixmap backbuffer = XCreatePixmap(dpy, win, WIN_W, WIN_H, depth);

    /* Precompute the tail and eye frames for each pendulum position. */
    Pixmap tail_pm[N_TAILS + 1], eye_pm[N_TAILS + 1];
    for (int i = 0; i <= N_TAILS; i++) {
        double t = i * M_PI / N_TAILS;
        tail_pm[i] = build_tail(dpy, win, t);
        eye_pm[i] = build_eyes(dpy, win, t);
    }

    XGCValues gcv;
    gcv.foreground = cat_color;
    GC hand_fill = XCreateGC(dpy, win, GCForeground, &gcv);
    gcv.foreground = detail_color;
    GC hand_edge = XCreateGC(dpy, win, GCForeground, &gcv);
    /* tail: black tail on white; eyes: black pupils on white eye patch. */
    gcv.foreground = cat_color;
    gcv.background = background;
    GC tail_gc = XCreateGC(dpy, win, GCForeground | GCBackground, &gcv);
    gcv.background = detail_color;
    GC eye_gc = XCreateGC(dpy, win, GCForeground | GCBackground, &gcv);
    GC blit_gc = XCreateGC(dpy, win, 0, NULL);

    int x_fd = ConnectionNumber(dpy);
    int cur_tail = 0, dir = 1, last_min = -1;
    int64_t next_frame = 0; /* wall-clock ms of the next tail step */
    for (;;) {
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            switch (e.type) {
            case Expose:
                last_min = -1;  /* force a full redraw */
                next_frame = 0; /* and do it now, not on the next tick */
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

        /* Advance one tail step per TICK_MS of real time. select() wakes early
         * on X activity, so the frame must be paced by the clock, not by the
         * loop rate, or the tail whips around as fast as events arrive.
         */
        int64_t now_ms = now_millis();
        if (now_ms >= next_frame) {
            time_t now_t = time(NULL);
            struct tm now;
            localtime_r(&now_t, &now);

            /* Reconstruct face_with_hands only when the minute changes. */
            if (now.tm_min != last_min) {
                XCopyArea(dpy, face, face_with_hands, blit_gc, 0, 0, WIN_W,
                          WIN_H, 0, 0);
                double m = now.tm_min / 60.0;
                double h = ((now.tm_hour % 12) + now.tm_min / 60.0) / 12.0;
                draw_hand(dpy, face_with_hands, hand_fill, hand_edge,
                          MINUTE_LEN, m);
                draw_hand(dpy, face_with_hands, hand_fill, hand_edge, HOUR_LEN,
                          h);
                last_min = now.tm_min;
            }

            /* Copy base face with hands to backbuffer. */
            XCopyArea(dpy, face_with_hands, backbuffer, blit_gc, 0, 0, WIN_W,
                      WIN_H, 0, 0);

            /* Composite tail and eyes onto backbuffer. */
            XCopyPlane(dpy, tail_pm[cur_tail], backbuffer, tail_gc, 0, 0, WIN_W,
                       TAIL_H, 0, CAT_BOTTOM + 1, 1);
            XCopyPlane(dpy, eye_pm[cur_tail], backbuffer, eye_gc, 0, 0,
                       eyes_width, eyes_height, 49, 30, 1);

            /* Blit the final composite frame to the window in a single op. */
            XCopyArea(dpy, backbuffer, win, blit_gc, 0, 0, WIN_W, WIN_H, 0, 0);
            XFlush(dpy);

            /* Bounce the tail index between 0 and N_TAILS. */
            if (cur_tail == 0)
                dir = 1;
            else if (cur_tail == N_TAILS)
                dir = -1;
            cur_tail += dir;
            /* Step from the previous deadline so render and event-handling time
             * does not stretch the cadence; if we fell a whole tick behind,
             * resync to now instead of bursting to catch up.
             */
            next_frame += TICK_MS;
            if (next_frame <= now_ms)
                next_frame = now_ms + TICK_MS;
        }

        int64_t wait = next_frame - now_ms;
        if (wait < 0)
            wait = 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x_fd, &rfds);
        struct timeval tv = {(time_t) (wait / 1000),
                             (suseconds_t) (wait % 1000) * 1000};
        select(x_fd + 1, &rfds, NULL, NULL, &tv);
    }
done:
    for (int i = 0; i <= N_TAILS; i++) {
        XFreePixmap(dpy, tail_pm[i]);
        XFreePixmap(dpy, eye_pm[i]);
    }
    XFreePixmap(dpy, backbuffer);
    XFreePixmap(dpy, face_with_hands);
    XFreePixmap(dpy, face);
    XFreeGC(dpy, blit_gc);
    XFreeGC(dpy, eye_gc);
    XFreeGC(dpy, tail_gc);
    XFreeGC(dpy, hand_edge);
    XFreeGC(dpy, hand_fill);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
