/*
 * paint - a minimalist Xlib paint demo.
 *
 * Drag mouse button 1 to draw freehand strokes. Click a color in the bottom
 * palette to change the brush color. Click the "clear" swatch (or press C)
 * to wipe the canvas. Press Esc or close the window to quit.
 *
 * Exercises: pointer motion + button drag, XAllocColor, XSetForeground,
 * XFillRectangle, XDrawLine, XSetWMProtocols / WM_DELETE_WINDOW.
 */

#include <stdio.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#define WIN_W 640
#define WIN_H 480
#define PALETTE_H 32
#define CANVAS_H (WIN_H - PALETTE_H)

typedef struct {
    const char *name;
    unsigned short r, g, b;
} ColorSpec;

static const ColorSpec PALETTE[] = {
    {"black", 0x0000, 0x0000, 0x0000},
    {"red", 0xFFFF, 0x2020, 0x2020},
    {"orange", 0xFFFF, 0x8000, 0x0000},
    {"yellow", 0xFFFF, 0xE000, 0x0000},
    {"green", 0x2080, 0xC080, 0x2080},
    {"blue", 0x2080, 0x6080, 0xFFFF},
    {"purple", 0x9080, 0x3080, 0xC080},
};
#define PALETTE_N ((int) (sizeof(PALETTE) / sizeof(PALETTE[0])))

/* The last slot in the palette strip is the "clear" button. */
#define SWATCH_N (PALETTE_N + 1)
#define SWATCH_W (WIN_W / SWATCH_N)

static unsigned long colors[PALETTE_N];
static unsigned long white_pixel;

static int current_color = 0;
static int dragging = 0;
static int last_x = 0, last_y = 0;

static void alloc_colors(Display *dpy, Colormap cmap)
{
    for (int i = 0; i < PALETTE_N; i++) {
        XColor c;
        c.red = PALETTE[i].r;
        c.green = PALETTE[i].g;
        c.blue = PALETTE[i].b;
        c.flags = DoRed | DoGreen | DoBlue;
        if (!XAllocColor(dpy, cmap, &c)) {
            fprintf(stderr, "XAllocColor failed for %s\n", PALETTE[i].name);
            colors[i] = BlackPixel(dpy, DefaultScreen(dpy));
        } else {
            colors[i] = c.pixel;
        }
    }
}

static void draw_palette(Display *dpy, Window win, GC gc)
{
    for (int i = 0; i < PALETTE_N; i++) {
        XSetForeground(dpy, gc, colors[i]);
        XFillRectangle(dpy, win, gc, i * SWATCH_W, CANVAS_H, SWATCH_W,
                       PALETTE_H);
        if (i == current_color) {
            XSetForeground(dpy, gc, white_pixel);
            XDrawRectangle(dpy, win, gc, i * SWATCH_W + 2, CANVAS_H + 2,
                           SWATCH_W - 5, PALETTE_H - 5);
        }
    }
    /* "Clear" swatch: white background with a label */
    XSetForeground(dpy, gc, white_pixel);
    XFillRectangle(dpy, win, gc, PALETTE_N * SWATCH_W, CANVAS_H, SWATCH_W,
                   PALETTE_H);
    XSetForeground(dpy, gc, colors[0]);
    XDrawString(dpy, win, gc, PALETTE_N * SWATCH_W + 12, CANVAS_H + 20, "clear",
                5);
}

static void clear_canvas(Display *dpy, Window win, GC gc)
{
    XSetForeground(dpy, gc, white_pixel);
    XFillRectangle(dpy, win, gc, 0, 0, WIN_W, CANVAS_H);
    draw_palette(dpy, win, gc);
    XFlush(dpy);
}

static void redraw(Display *dpy, Window win, GC gc)
{
    /* Canvas content is held by the renderer; we only need to repaint
     * chrome on Expose. The stroke history is not stored. */
    draw_palette(dpy, win, gc);
}

static void handle_button(Display *dpy,
                          Window win,
                          GC gc,
                          XButtonEvent *e)
{
    if (e->y >= CANVAS_H) {
        int idx = e->x / SWATCH_W;
        if (idx >= 0 && idx < PALETTE_N) {
            current_color = idx;
            XSetForeground(dpy, gc, colors[current_color]);
            draw_palette(dpy, win, gc);
        } else if (idx == PALETTE_N) {
            clear_canvas(dpy, win, gc);
        }
        return;
    }
    /* Canvas click: start a stroke with a single point. */
    XSetForeground(dpy, gc, colors[current_color]);
    XDrawLine(dpy, win, gc, e->x, e->y, e->x, e->y);
    dragging = 1;
    last_x = e->x;
    last_y = e->y;
}

static void handle_motion(Display *dpy,
                          Window win,
                          GC gc,
                          XMotionEvent *e)
{
    if (!dragging || e->y >= CANVAS_H)
        return;
    XSetForeground(dpy, gc, colors[current_color]);
    XDrawLine(dpy, win, gc, last_x, last_y, e->x, e->y);
    last_x = e->x;
    last_y = e->y;
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "XOpenDisplay failed\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    Colormap cmap = DefaultColormap(dpy, screen);

    white_pixel = WhitePixel(dpy, screen);
    Window win = XCreateSimpleWindow(dpy, root, 0, 0, WIN_W, WIN_H, 1,
                                     BlackPixel(dpy, screen), white_pixel);
    XStoreName(dpy, win, "paint");

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    alloc_colors(dpy, cmap);

    XSelectInput(dpy, win,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                     ButtonReleaseMask | ButtonMotionMask);
    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, NULL);
    XSetLineAttributes(dpy, gc, 2, LineSolid, CapRound, JoinRound);

    XEvent e;
    for (;;) {
        XNextEvent(dpy, &e);
        switch (e.type) {
        case Expose:
            if (e.xexpose.count == 0)
                redraw(dpy, win, gc);
            break;
        case ButtonPress:
            handle_button(dpy, win, gc, &e.xbutton);
            break;
        case ButtonRelease:
            dragging = 0;
            break;
        case MotionNotify:
            handle_motion(dpy, win, gc, &e.xmotion);
            break;
        case KeyPress: {
            KeySym ks = XLookupKeysym(&e.xkey, 0);
            if (ks == XK_Escape || ks == XK_q || ks == XK_Q)
                goto done;
            if (ks == XK_c || ks == XK_C)
                clear_canvas(dpy, win, gc);
        } break;
        case ClientMessage:
            if ((Atom) e.xclient.data.l[0] == wm_delete)
                goto done;
            break;
        }
    }
done:
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    return 0;
}
