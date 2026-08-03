/*
 * mandel - interactive Mandelbrot set viewer.
 *
 * Click button 1 to zoom in 2x at the cursor, button 3 to zoom out 2x. Press r
 * to reset the view, Esc or q to quit.
 *
 * Exercises: XCreateImage + XPutImage with a client-allocated raster (depth 32,
 * ZPixmap), ButtonPress dispatch by .button, dynamic recomputation on input,
 * WM_DELETE_WINDOW.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

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

/* Smooth purple-to-yellow ramp keyed off escape iteration count. Points in the
 * set (those that never escape) come out black.
 */
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

/* Loop state, so the native (blocking) loop and the browser animation-frame
 * callback share the event handling and repaint.
 */
struct mandel_ctx {
    Display *dpy;
    Window win;
    GC gc;
    XImage *image;
    Atom wm_delete;
    int dirty;
    int quit;
};

/* Fold one delivered event into the view: recompute on a click or reset, mark
 * dirty on expose, and request quit on q/Esc or the close button.
 */
static void mandel_handle(struct mandel_ctx *c, XEvent *e)
{
    switch (e->type) {
    case Expose:
        if (e->xexpose.count == 0)
            c->dirty = 1;
        break;
    case ButtonPress:
        if (e->xbutton.button == Button1) {
            recenter(e->xbutton.x, e->xbutton.y);
            scale *= 2.0;
        } else if (e->xbutton.button == Button3) {
            recenter(e->xbutton.x, e->xbutton.y);
            scale *= 0.5;
        } else {
            break;
        }
        compute((unsigned int *) c->image->data);
        c->dirty = 1;
        break;
    case KeyPress: {
        KeySym ks = XLookupKeysym(&e->xkey, 0);
        if (ks == XK_Escape || ks == XK_q || ks == XK_Q) {
            c->quit = 1;
            return;
        }
        if (ks == XK_r || ks == XK_R) {
            cx = -0.5;
            cy = 0.0;
            scale = 150.0;
            compute((unsigned int *) c->image->data);
            c->dirty = 1;
        }
    } break;
    case ClientMessage:
        if ((Atom) e->xclient.data.l[0] == c->wm_delete)
            c->quit = 1;
        break;
    }
}

static void mandel_repaint(struct mandel_ctx *c)
{
    if (c->dirty) {
        XPutImage(c->dpy, c->win, c->gc, c->image, 0, 0, 0, 0, WIN_W, WIN_H);
        XFlush(c->dpy);
        c->dirty = 0;
    }
}

static void mandel_teardown(struct mandel_ctx *c)
{
    XDestroyImage(c->image);
    XFreeGC(c->dpy, c->gc);
    XDestroyWindow(c->dpy, c->win);
    XCloseDisplay(c->dpy);
}

#ifdef __EMSCRIPTEN__

/* The browser drives the loop; drain events per animation frame, then repaint,
 * so the main thread never blocks and ASYNCIFY is not needed. Free on quit so a
 * page that reuses the module does not leak.
 */
static void mandel_em_frame(void *arg)
{
    struct mandel_ctx *c = arg;
    while (XPending(c->dpy)) {
        XEvent e;
        XNextEvent(c->dpy, &e);
        mandel_handle(c, &e);
        if (c->quit) {
            emscripten_cancel_main_loop();
            mandel_teardown(c);
            return;
        }
    }
    mandel_repaint(c);
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

    struct mandel_ctx ctx = {.dpy = dpy, .win = win, .gc = gc, .image = image,
                             .wm_delete = wm_delete, .dirty = 1, .quit = 0};

#ifdef __EMSCRIPTEN__

    /* Drive off requestAnimationFrame; simulate_infinite_loop keeps the runtime
     * alive, so ctx must be static. No blocking, so no ASYNCIFY.
     */
    static struct mandel_ctx em_ctx;
    em_ctx = ctx;
    emscripten_set_main_loop_arg(mandel_em_frame, &em_ctx, 0, 1);
    return 0; /* not reached */
#else
    XEvent e;
    while (!ctx.quit) {
        mandel_repaint(&ctx);
        XNextEvent(dpy, &e);
        mandel_handle(&ctx, &e);
    }
    mandel_teardown(&ctx);
    return 0;
#endif
}
