/*
 * life - Conway's Game of Life on a finite torus.
 *
 * Controls:
 *   Space   pause / resume
 *   s       single step (while paused)
 *   r       randomize the grid
 *   c       clear the grid
 *   q/Esc   quit
 *
 * Exercises: XCreatePixmap + XCopyArea double buffering, bulk XFillRectangle,
 * select() on ConnectionNumber for timed redraws, WM_DELETE_WINDOW.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define CELL_PX 8
#define GRID_W 80
#define GRID_H 60
#define WIN_W (GRID_W * CELL_PX)
#define WIN_H (GRID_H * CELL_PX)
#define STEP_MS 80

static unsigned char grid[GRID_H][GRID_W];
static unsigned char next_grid[GRID_H][GRID_W];

static void randomize(void)
{
    for (int y = 0; y < GRID_H; y++)
        for (int x = 0; x < GRID_W; x++)
            grid[y][x] = (rand() & 3) == 0 ? 1 : 0;
}

static void clear_grid(void)
{
    memset(grid, 0, sizeof(grid));
}

static void step(void)
{
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            int n = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0)
                        continue;
                    int nx = (x + dx + GRID_W) % GRID_W;
                    int ny = (y + dy + GRID_H) % GRID_H;
                    n += grid[ny][nx];
                }
            }
            next_grid[y][x] = grid[y][x] ? (n == 2 || n == 3) : (n == 3);
        }
    }
    memcpy(grid, next_grid, sizeof(grid));
}

static void render(Display *dpy,
                   Pixmap buf,
                   GC bg_gc,
                   GC fg_gc,
                   int paused)
{
    XFillRectangle(dpy, buf, bg_gc, 0, 0, WIN_W, WIN_H);
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            if (grid[y][x]) {
                XFillRectangle(dpy, buf, fg_gc, x * CELL_PX, y * CELL_PX,
                               CELL_PX - 1, CELL_PX - 1);
            }
        }
    }
    if (paused) {
        XDrawString(dpy, buf, fg_gc, 8, 16, "PAUSED  (space=run, s=step)", 27);
    }
}

static void present(Display *dpy, Window win, Pixmap buf, GC blit_gc)
{
    XCopyArea(dpy, buf, win, blit_gc, 0, 0, WIN_W, WIN_H, 0, 0);
    XFlush(dpy);
}

static long ms_since(const struct timeval *start)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - start->tv_sec) * 1000L +
           (now.tv_usec - start->tv_usec) / 1000L;
}

/* Loop state, so one frame runs from either the native loop or the browser
 * animation-frame callback. The grid itself is file-scope global state.
 */
struct life_ctx {
    Display *dpy;
    Window win;
    Pixmap buf;
    GC bg_gc, fg_gc, blit_gc;
    Atom wm_delete;
    int paused;
    struct timeval last_step;
    int quit;
};

/* One frame: drain events (keys toggle pause, step, randomize, clear, quit),
 * then advance the generation when running and due.
 */
static void life_frame(struct life_ctx *c)
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
            Bool needsRender = False;
            if (ks == XK_Escape || ks == XK_q || ks == XK_Q) {
                c->quit = 1;
                return;
            }
            if (ks == XK_space) {
                c->paused = !c->paused;
            } else if ((ks == XK_s || ks == XK_S) && c->paused) {
                step();
                needsRender = True;
            } else if (ks == XK_r || ks == XK_R) {
                randomize();
                needsRender = True;
            } else if (ks == XK_c || ks == XK_C) {
                clear_grid();
                needsRender = True;
            }
            if (needsRender) {
                render(c->dpy, c->buf, c->bg_gc, c->fg_gc, c->paused);
                present(c->dpy, c->win, c->buf, c->blit_gc);
            }
        } break;
        case ClientMessage:
            if ((Atom) e.xclient.data.l[0] == c->wm_delete) {
                c->quit = 1;
                return;
            }
            break;
        }
    }
    if (c->quit)
        return;

    if (!c->paused && ms_since(&c->last_step) >= STEP_MS) {
        step();
        render(c->dpy, c->buf, c->bg_gc, c->fg_gc, c->paused);
        present(c->dpy, c->win, c->buf, c->blit_gc);
        gettimeofday(&c->last_step, NULL);
    }
}

static void life_teardown(struct life_ctx *c)
{
    XFreeGC(c->dpy, c->blit_gc);
    XFreeGC(c->dpy, c->fg_gc);
    XFreeGC(c->dpy, c->bg_gc);
    XFreePixmap(c->dpy, c->buf);
    XDestroyWindow(c->dpy, c->win);
    XCloseDisplay(c->dpy);
}

#ifdef __EMSCRIPTEN__

/* The browser drives the loop one frame per animation frame; the STEP_MS gate
 * inside life_frame keeps the generation cadence, so no blocking and no
 * ASYNCIFY. Free on quit so a page that reuses the module does not leak.
 */
static void life_em_frame(void *arg)
{
    struct life_ctx *c = arg;
    life_frame(c);
    if (c->quit) {
        emscripten_cancel_main_loop();
        life_teardown(c);
    }
}
#endif

int main(void)
{
    srand((unsigned) time(NULL));

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
        XCreateSimpleWindow(dpy, root, 0, 0, WIN_W, WIN_H, 1, fg, bg);
    XStoreName(dpy, win, "life");

    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | StructureNotifyMask);
    XMapWindow(dpy, win);

    Pixmap buf = XCreatePixmap(dpy, win, WIN_W, WIN_H, depth);

    XGCValues gcv;
    gcv.foreground = bg;
    GC bg_gc = XCreateGC(dpy, buf, GCForeground, &gcv);
    gcv.foreground = fg;
    GC fg_gc = XCreateGC(dpy, buf, GCForeground, &gcv);
    GC blit_gc = XCreateGC(dpy, win, 0, NULL);

    randomize();

    struct life_ctx ctx = {.dpy = dpy, .win = win, .buf = buf, .bg_gc = bg_gc,
                           .fg_gc = fg_gc, .blit_gc = blit_gc,
                           .wm_delete = wm_delete, .paused = 0, .quit = 0};
    gettimeofday(&ctx.last_step, NULL);

#ifdef __EMSCRIPTEN__

    /* Drive off requestAnimationFrame; simulate_infinite_loop keeps the runtime
     * alive, so ctx must be static. No blocking, so no ASYNCIFY.
     */
    static struct life_ctx em_ctx;
    em_ctx = ctx;
    emscripten_set_main_loop_arg(life_em_frame, &em_ctx, 0, 1);
    return 0; /* not reached */
#else
    int x_fd = ConnectionNumber(dpy);
    while (!ctx.quit) {
        life_frame(&ctx);
        if (ctx.quit)
            break;
        /* Sleep until either an X event arrives or the next step is due. */
        long wait_ms = ctx.paused ? 200 : STEP_MS - ms_since(&ctx.last_step);
        if (wait_ms < 0)
            wait_ms = 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(x_fd, &rfds);
        struct timeval tv = {wait_ms / 1000, (wait_ms % 1000) * 1000};
        select(x_fd + 1, &rfds, NULL, NULL, &tv);
    }
    life_teardown(&ctx);
    return 0;
#endif
}
