/*
 * Processing-style showcases as a standalone Xlib client.
 *
 * Click the tabs to switch showcases. Esc, q, or the Quit button exits.
 *
 * Exercises: XFillPolygon, XDrawLines, XFillArc, XDrawString, pointer input,
 * key input, and timed redraws.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#define WIN_W 800
#define WIN_H 600
#define SHOWCASE_COUNT 5
#define FRAME_MS 16
#define CONTROL_H 46
#define TAB_X 10
#define TAB_Y 8
#define TAB_W 108
#define TAB_H 30
#define TAB_STEP 118
#define QUIT_X 700
#define QUIT_Y 8
#define QUIT_W 80
#define QUIT_H 30
#define PI 3.14159265358979323846
#define TWO_PI (PI * 2.0)
#define HALF_PI (PI * 0.5)

typedef struct {
    Display *dpy;
    int screen;
    Window win;
    GC gc;
    Atom wm_delete;
    int showcase;
    int mouse_x;
    int mouse_y;
    int frame;
    int running;
} App;

typedef void (*ShowcaseDrawFunc)(App *app);

static const char *showcase_names[SHOWCASE_COUNT] = {
    "Primitives",
    "Clock",
    "Bezier",
    "Tree",
    "Waves",
};

static unsigned long rgb(int r, int g, int b)
{
    /* libX11-compat stores direct-color pixels as logical ARGB. */
    return 0xff000000UL | ((unsigned long) (r & 0xff) << 16) |
           ((unsigned long) (g & 0xff) << 8) | (unsigned long) (b & 0xff);
}

static double map_value(double value,
                        double in_start,
                        double in_stop,
                        double out_start,
                        double out_stop)
{
    return out_start + (out_stop - out_start) *
                           ((value - in_start) / (in_stop - in_start));
}

static void set_fg(App *app, unsigned long color)
{
    XSetForeground(app->dpy, app->gc, color);
}

static void fill_rect(App *app, int x, int y, unsigned int w, unsigned int h)
{
    XFillRectangle(app->dpy, app->win, app->gc, x, y, w, h);
}

static void stroke_width(App *app, unsigned int width)
{
    XSetLineAttributes(app->dpy, app->gc, (int) width, LineSolid, CapRound,
                       JoinRound);
}

static void fill_polygon(App *app, XPoint *points, int count)
{
    XFillPolygon(app->dpy, app->win, app->gc, points, count, Nonconvex,
                 CoordModeOrigin);
}

static void draw_line(App *app, int x0, int y0, int x1, int y1)
{
    XDrawLine(app->dpy, app->win, app->gc, x0, y0, x1, y1);
}

static void draw_text(App *app, int x, int y, const char *text)
{
    XDrawString(app->dpy, app->win, app->gc, x, y, text, (int) strlen(text));
}

static void fill_ellipse(App *app, int cx, int cy, int w, int h)
{
    XFillArc(app->dpy, app->win, app->gc, cx - w / 2, cy - h / 2,
             (unsigned int) w, (unsigned int) h, 0, 360 * 64);
}

static void draw_arc(App *app,
                     int cx,
                     int cy,
                     int w,
                     int h,
                     double start,
                     double end)
{
    XDrawArc(app->dpy, app->win, app->gc, cx - w / 2, cy - h / 2,
             (unsigned int) w, (unsigned int) h, (int) (start * 180.0 / PI * 64),
             (int) ((end - start) * 180.0 / PI * 64));
}

static int hit_showcase_button(int x, int y)
{
    if (y < TAB_Y || y > TAB_Y + TAB_H)
        return -1;
    for (int i = 0; i < SHOWCASE_COUNT; i++) {
        int left = TAB_X + i * TAB_STEP;
        if (x >= left && x <= left + TAB_W)
            return i;
    }
    return -1;
}

static int hit_quit_button(int x, int y)
{
    return x >= QUIT_X && x <= QUIT_X + QUIT_W && y >= QUIT_Y &&
           y <= QUIT_Y + QUIT_H;
}

static void draw_button(App *app,
                        int x,
                        int y,
                        int w,
                        int h,
                        const char *label,
                        int active)
{
    int hot = app->mouse_x >= x && app->mouse_x <= x + w && app->mouse_y >= y &&
              app->mouse_y <= y + h;
    set_fg(app, active ? rgb(30, 105, 185)
                       : hot ? rgb(220, 230, 240) : rgb(245, 245, 245));
    fill_rect(app, x, y, (unsigned int) w, (unsigned int) h);
    set_fg(app, active ? rgb(255, 255, 255) : rgb(80, 100, 120));
    XDrawRectangle(app->dpy, app->win, app->gc, x, y, (unsigned int) w,
                   (unsigned int) h);
    set_fg(app, active ? rgb(255, 255, 255) : rgb(20, 24, 28));
    draw_text(app, x + 12, y + 20, label);
}

static void draw_controls(App *app)
{
    set_fg(app, rgb(255, 255, 255));
    fill_rect(app, 0, 0, WIN_W, CONTROL_H);
    for (int i = 0; i < SHOWCASE_COUNT; i++) {
        draw_button(app, TAB_X + i * TAB_STEP, TAB_Y, TAB_W, TAB_H,
                    showcase_names[i], i == app->showcase);
    }
    draw_button(app, QUIT_X, QUIT_Y, QUIT_W, QUIT_H, "Quit", 0);
    set_fg(app, rgb(0, 0, 0));
    draw_text(app, 10, 58, "Click a tab to switch scenes");
}

static void draw_primitives(App *app)
{
    XPoint triangle1[] = {{30, 60}, {30, 560}, {120, 560}};
    XPoint quad[] = {{290, 60}, {350, 80}, {330, 560}, {250, 560}};
    XPoint triangle2[] = {{535, 60}, {700, 560}, {555, 560}};

    set_fg(app, rgb(24, 28, 32));
    fill_rect(app, 0, 0, WIN_W, WIN_H);
    set_fg(app, rgb(204, 204, 204));
    fill_polygon(app, triangle1, 3);
    set_fg(app, rgb(90, 150, 210));
    fill_rect(app, 145, 115, 85, 85);
    set_fg(app, rgb(230, 180, 90));
    fill_polygon(app, quad, 4);
    set_fg(app, rgb(245, 245, 245));
    fill_ellipse(app, 420, 190, 110, 110);
    set_fg(app, rgb(120, 210, 150));
    fill_polygon(app, triangle2, 3);
    set_fg(app, rgb(255, 255, 255));
    stroke_width(app, 6);
    draw_arc(app, 655, 390, 240, 240, PI, TWO_PI);
    stroke_width(app, 1);
}

static void draw_clock(App *app)
{
    time_t now_t = time(NULL);
    struct tm now;
    localtime_r(&now_t, &now);

    int cx = WIN_W / 2;
    int cy = WIN_H / 2 + 20;
    int radius = WIN_H / 2 - 45;
    double seconds_radius = radius * 0.72;
    double minutes_radius = radius * 0.60;
    double hours_radius = radius * 0.48;
    double diameter = radius * 1.8;

    set_fg(app, rgb(70, 74, 78));
    fill_rect(app, 0, 0, WIN_W, WIN_H);
    set_fg(app, rgb(248, 248, 248));
    fill_ellipse(app, cx, cy, (int) diameter, (int) diameter);
    set_fg(app, rgb(20, 20, 20));
    XDrawArc(app->dpy, app->win, app->gc, cx - (int) diameter / 2,
             cy - (int) diameter / 2, (unsigned int) diameter,
             (unsigned int) diameter, 0, 360 * 64);

    double s = map_value(now.tm_sec, 0, 60, 0, TWO_PI) - HALF_PI;
    double m = map_value(now.tm_min + now.tm_sec / 60.0, 0, 60, 0, TWO_PI) -
               HALF_PI;
    double h = map_value((now.tm_hour % 12) + now.tm_min / 60.0, 0, 12, 0,
                         TWO_PI) -
               HALF_PI;

    stroke_width(app, 1);
    draw_line(app, cx, cy, cx + (int) (cos(s) * seconds_radius),
              cy + (int) (sin(s) * seconds_radius));
    stroke_width(app, 3);
    draw_line(app, cx, cy, cx + (int) (cos(m) * minutes_radius),
              cy + (int) (sin(m) * minutes_radius));
    stroke_width(app, 5);
    draw_line(app, cx, cy, cx + (int) (cos(h) * hours_radius),
              cy + (int) (sin(h) * hours_radius));
    stroke_width(app, 1);

    for (int a = 0; a < 360; a += 6) {
        double angle = a * PI / 180.0;
        int x = cx + (int) (cos(angle) * seconds_radius);
        int y = cy + (int) (sin(angle) * seconds_radius);
        fill_rect(app, x - 1, y - 1, 2, 2);
    }
}

static void draw_bezier_segment(App *app,
                                double x0,
                                double y0,
                                double x1,
                                double y1,
                                double x2,
                                double y2,
                                double x3,
                                double y3)
{
    int px = (int) x0;
    int py = (int) y0;
    for (int i = 1; i <= 40; i++) {
        double t = i / 40.0;
        double u = 1.0 - t;
        double x = u * u * u * x0 + 3.0 * u * u * t * x1 +
                   3.0 * u * t * t * x2 + t * t * t * x3;
        double y = u * u * u * y0 + 3.0 * u * u * t * y1 +
                   3.0 * u * t * t * y2 + t * t * t * y3;
        draw_line(app, px, py, (int) x, (int) y);
        px = (int) x;
        py = (int) y;
    }
}

static void draw_bezier(App *app)
{
    set_fg(app, rgb(34, 32, 42));
    fill_rect(app, 0, 0, WIN_W, WIN_H);
    stroke_width(app, 3);
    for (int i = 0; i < 10; i++) {
        double t = app->frame * 0.025 + i * 0.35;
        set_fg(app, rgb(80 + i * 14, 190 - i * 10, 220));
        draw_bezier_segment(app, 90, 500 - i * 35, 220 + sin(t) * 120,
                            80 + i * 18, 520 + cos(t * 0.7) * 130,
                            540 - i * 24, 720, 90 + i * 38);
    }
    stroke_width(app, 1);
}

static void draw_branch(App *app, double x, double y, double angle, double len)
{
    double x2 = x + cos(angle) * len;
    double y2 = y + sin(angle) * len;
    draw_line(app, (int) x, (int) y, (int) x2, (int) y2);
    len *= 0.68;
    if (len <= 4.0)
        return;
    double sway = sin(app->frame * 0.025) * 0.10;
    draw_branch(app, x2, y2, angle - 0.55 + sway, len);
    draw_branch(app, x2, y2, angle + 0.50 + sway, len);
}

static void draw_tree(App *app)
{
    set_fg(app, rgb(18, 38, 42));
    fill_rect(app, 0, 0, WIN_W, WIN_H);
    set_fg(app, rgb(230, 230, 220));
    stroke_width(app, 2);
    draw_branch(app, WIN_W / 2.0, WIN_H - 20.0, -HALF_PI, 150.0);
    stroke_width(app, 1);
}

static void draw_waves(App *app)
{
    set_fg(app, rgb(12, 20, 35));
    fill_rect(app, 0, 0, WIN_W, WIN_H);
    stroke_width(app, 2);
    for (int y = 70; y < WIN_H; y += 22) {
        set_fg(app, rgb(60 + (y * 2) % 160, 150 + y % 90, 210));
        XPoint points[WIN_W / 10 + 1];
        int count = 0;
        for (int x = 0; x <= WIN_W; x += 10) {
            points[count].x = (short) x;
            points[count].y =
                (short) (y + sin((x + app->frame * 3) * 0.035) * 12.0);
            count++;
        }
        XDrawLines(app->dpy, app->win, app->gc, points, count, CoordModeOrigin);
    }
    stroke_width(app, 1);
}

static ShowcaseDrawFunc showcase_drawers[SHOWCASE_COUNT] = {
    draw_primitives,
    draw_clock,
    draw_bezier,
    draw_tree,
    draw_waves,
};

static void render(App *app)
{
    showcase_drawers[app->showcase](app);
    draw_controls(app);
    XFlush(app->dpy);
}

static void request_quit(App *app)
{
    app->running = 0;
}

static void destroy_app(App *app)
{
    if (!app->dpy)
        return;
    if (app->gc)
        XFreeGC(app->dpy, app->gc);
    if (app->win)
        XDestroyWindow(app->dpy, app->win);
    XCloseDisplay(app->dpy);
    app->dpy = NULL;
}

static void handle_key(App *app, XKeyEvent *event)
{
    KeySym key = XLookupKeysym(event, 0);
    if (key == XK_Escape || key == XK_q || key == XK_Q) {
        request_quit(app);
        return;
    }
    if (key >= XK_1 && key <= XK_5)
        app->showcase = (int) (key - XK_1);
}

static void handle_button(App *app, XButtonEvent *event)
{
    app->mouse_x = event->x;
    app->mouse_y = event->y;
    int button = hit_showcase_button(event->x, event->y);
    if (button >= 0) {
        app->showcase = button;
    } else if (hit_quit_button(event->x, event->y)) {
        request_quit(app);
    } else {
        app->showcase = (app->showcase + 1) % SHOWCASE_COUNT;
    }
}

int main(void)
{
    App app;
    memset(&app, 0, sizeof(app));
    app.running = 1;
    app.dpy = XOpenDisplay(NULL);
    if (!app.dpy) {
        fprintf(stderr, "XOpenDisplay failed\n");
        return 1;
    }

    app.screen = DefaultScreen(app.dpy);
    Window root = RootWindow(app.dpy, app.screen);
    unsigned long white = WhitePixel(app.dpy, app.screen);
    unsigned long black = BlackPixel(app.dpy, app.screen);
    app.win =
        XCreateSimpleWindow(app.dpy, root, 0, 0, WIN_W, WIN_H, 1, black, white);
    XStoreName(app.dpy, app.win, "processing showcases");
    app.wm_delete = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app.dpy, app.win, &app.wm_delete, 1);
    XSelectInput(app.dpy, app.win,
                 ExposureMask | KeyPressMask | ButtonPressMask |
                     PointerMotionMask);
    XMapWindow(app.dpy, app.win);

    app.gc = XCreateGC(app.dpy, app.win, 0, NULL);

    while (app.running) {
        while (XPending(app.dpy)) {
            XEvent event;
            XNextEvent(app.dpy, &event);
            switch (event.type) {
            case Expose:
                if (event.xexpose.count == 0)
                    render(&app);
                break;
            case KeyPress:
                handle_key(&app, &event.xkey);
                break;
            case ButtonPress:
                handle_button(&app, &event.xbutton);
                break;
            case MotionNotify:
                app.mouse_x = event.xmotion.x;
                app.mouse_y = event.xmotion.y;
                break;
            case ClientMessage:
                if ((Atom) event.xclient.data.l[0] == app.wm_delete) {
                    request_quit(&app);
                }
                break;
            }
        }

        if (!app.running)
            break;

        render(&app);
        app.frame++;
        usleep(FRAME_MS * 1000);
    }
    destroy_app(&app);
    return 0;
}
