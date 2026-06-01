/*
 * mandel - interactive Mandelbrot set viewer.
 *
 * Click button 1 to zoom in 2x at the cursor, button 3 to zoom out 2x.
 * Press r to reset the view, Esc or q to quit.
 *
 * Exercises: XCreateImage + XPutImage with a client-allocated raster
 * (depth 32, ZPixmap), ButtonPress dispatch by .button, dynamic
 * recomputation on input, WM_DELETE_WINDOW.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#define WIN_W 480
#define WIN_H 360
#define MAX_ITER 96

static double cx = -0.5, cy = 0.0;
static double scale = 150.0; /* pixels per unit in the complex plane */

/* Pack RGB into the shim's ARGB pixel format. Alpha is always opaque. */
static unsigned long pack(unsigned char r, unsigned char g, unsigned char b)
{
    return (0xFFu << 24) | ((unsigned long) r << 16) |
           ((unsigned long) g << 8) | b;
}

/* Smooth purple-to-yellow ramp keyed off escape iteration count. Points in
 * the set (those that never escape) come out black. */
static unsigned long color_for(int iter)
{
    if (iter >= MAX_ITER)
        return pack(0, 0, 0);
    double t = (double) iter / (double) MAX_ITER;
    unsigned char r = (unsigned char) (9.0 * (1 - t) * t * t * t * 255.0);
    unsigned char g = (unsigned char) (15.0 * (1 - t) * (1 - t) * t * t * 255.0);
    unsigned char b =
        (unsigned char) (8.5 * (1 - t) * (1 - t) * (1 - t) * t * 255.0);
    return pack(r, g, b);
}

static int escape(double zx0, double zy0)
{
    double x = 0, y = 0;
    int i = 0;
    while (i < MAX_ITER && x * x + y * y < 4.0) {
        double xt = x * x - y * y + zx0;
        y = 2 * x * y + zy0;
        x = xt;
        i++;
    }
    return i;
}

static void compute(unsigned int *pixels)
{
    for (int py = 0; py < WIN_H; py++) {
        double zy = cy + (py - WIN_H / 2) / scale;
        for (int px = 0; px < WIN_W; px++) {
            double zx = cx + (px - WIN_W / 2) / scale;
            pixels[py * WIN_W + px] = (unsigned int) color_for(escape(zx, zy));
        }
    }
}

static void recenter(int px, int py)
{
    cx += (px - WIN_W / 2) / scale;
    cy += (py - WIN_H / 2) / scale;
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "XOpenDisplay failed\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);
    Visual *visual = DefaultVisual(dpy, screen);
    int depth = DefaultDepth(dpy, screen);
    Window root = RootWindow(dpy, screen);

    Window win = XCreateSimpleWindow(dpy, root, 0, 0, WIN_W, WIN_H, 1,
                                     BlackPixel(dpy, screen),
                                     WhitePixel(dpy, screen));
    XStoreName(dpy, win, "mandel");

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask);
    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);

    /* XCreateImage takes ownership of `data`; XDestroyImage frees both. */
    char *data = malloc((size_t) WIN_W * WIN_H * 4);
    if (data == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    XImage *image = XCreateImage(dpy, visual, depth, ZPixmap, 0, data, WIN_W,
                                 WIN_H, 32, 0);
    if (image == NULL) {
        fprintf(stderr, "XCreateImage failed\n");
        free(data);
        return 1;
    }

    compute((unsigned int *) image->data);

    int dirty = 1;
    XEvent e;
    for (;;) {
        if (dirty) {
            XPutImage(dpy, win, gc, image, 0, 0, 0, 0, WIN_W, WIN_H);
            XFlush(dpy);
            dirty = 0;
        }
        XNextEvent(dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.count == 0)
                dirty = 1;
            break;
        case ButtonPress:
            if (e.xbutton.button == Button1) {
                recenter(e.xbutton.x, e.xbutton.y);
                scale *= 2.0;
            } else if (e.xbutton.button == Button3) {
                recenter(e.xbutton.x, e.xbutton.y);
                scale *= 0.5;
            } else {
                break;
            }
            compute((unsigned int *) image->data);
            dirty = 1;
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Escape || ks == XK_q || ks == XK_Q)
                goto done;
            if (ks == XK_r || ks == XK_R) {
                cx = -0.5;
                cy = 0.0;
                scale = 150.0;
                compute((unsigned int *) image->data);
                dirty = 1;
            }
        } break;
        case ClientMessage:
            if ((Atom) e.xclient.data.l[0] == wm_delete)
                goto done;
            break;
        }
    }
done:
    XDestroyImage(image);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
