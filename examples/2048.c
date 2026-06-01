/*
 * Xlib implementation of the 2048 sliding-tile game.
 *
 * Controls: arrow keys to slide, Esc to quit.
 */

#include <stdlib.h>
#include <time.h>

static void turn(short *mat)
{
    srand(time(NULL));
    unsigned short x, y;
    unsigned short result[4] = {2, 2, 2, 4};
    while (1) {
        x = rand() % 4;
        y = rand() % 4;
        if (*(mat + (x * 4) + y) == 0)
            break;
    }
    *(mat + (x * 4) + y) = result[x];
}

short *m_init()
{
    short *mat = calloc(16, sizeof(short));

    turn(mat);
    turn(mat);
    return mat;
}

void move(short *mat, char move)
{
    if (move == 'L') {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                if (j > 0 && *(mat + (i * 4) + j) != 0) {
                    for (int k = j; k > 0; k--) {
                        if (*(mat + (i * 4) + k - 1) == *(mat + (i * 4) + k)) {
                            *(mat + (i * 4) + k - 1) += *(mat + (i * 4) + k);
                            *(mat + (i * 4) + k) = 0;
                        } else if (*(mat + (i * 4) + k - 1) == 0) {
                            *(mat + (i * 4) + k - 1) = *(mat + (i * 4) + k);
                            *(mat + (i * 4) + k) = 0;
                        }
                    }
                }
            }
        }
    } else if (move == 'R') {
        for (int i = 0; i < 4; i++) {
            for (int j = 3; j >= 0; j--) {
                if (j < 4 && *(mat + (i * 4) + j) != 0) {
                    for (int k = j; k < 3; k++) {
                        if (*(mat + (i * 4) + k + 1) == *(mat + (i * 4) + k)) {
                            *(mat + (i * 4) + k + 1) += *(mat + (i * 4) + k);
                            *(mat + (i * 4) + k) = 0;
                        } else if (*(mat + (i * 4) + k + 1) == 0) {
                            *(mat + (i * 4) + k + 1) = *(mat + (i * 4) + k);
                            *(mat + (i * 4) + k) = 0;
                        }
                    }
                }
            }
        }
    } else if (move == 'U') {
        for (int i = 0; i < 4; i++) {
            for (int j = 0; j < 4; j++) {
                if (i > 0 && *(mat + (i * 4) + j) != 0) {
                    for (int k = i; k > 0; k--) {
                        if (*(mat + ((k - 1) * 4) + j) ==
                            *(mat + (k * 4) + j)) {
                            *(mat + ((k - 1) * 4) + j) += *(mat + (k * 4) + j);
                            *(mat + (k * 4) + j) = 0;
                        } else if (*(mat + ((k - 1) * 4) + j) == 0) {
                            *(mat + ((k - 1) * 4) + j) = *(mat + (k * 4) + j);
                            *(mat + (k * 4) + j) = 0;
                        }
                    }
                }
            }
        }
    } else if (move == 'D') {
        for (int i = 3; i >= 0; i--) {
            for (int j = 0; j < 4; j++) {
                if (i < 4 && *(mat + (i * 4) + j) != 0) {
                    for (int k = i; k < 3; k++) {
                        if (*(mat + ((k + 1) * 4) + j) ==
                            *(mat + (k * 4) + j)) {
                            *(mat + ((k + 1) * 4) + j) += *(mat + (k * 4) + j);
                            *(mat + (k * 4) + j) = 0;
                        } else if (*(mat + ((k + 1) * 4) + j) == 0) {
                            *(mat + ((k + 1) * 4) + j) = *(mat + (k * 4) + j);
                            *(mat + (k * 4) + j) = 0;
                        }
                    }
                }
            }
        }
    }
    turn(mat);
}

#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xutil.h>

#define d_side 285

void close_x(Display *dis, Window win, GC gc)
{
    XFreeGC(dis, gc);
    XDestroyWindow(dis, win);
    XCloseDisplay(dis);
    exit(0);
}

void redraw(Display *dis, Window win, GC gc, short *mat)
{
    XClearWindow(dis, win);
    /* short can render up to 6 digits ("-32768" or "32767") plus NUL. */
    char str[8];
    XDrawString(dis, win, gc, 10, 9, "Press Esc to quit", 17);
    for (int i = 10, x = 0; i < 265; i += 67, x++) {
        for (int j = 10, y = 0; j < 275; j += 67, y++) {
            XDrawRectangle(dis, win, gc, i, j, 64, 64);
            printf("%d ", *(mat + x * 4 + y));
            snprintf(str, sizeof str, "%d", *(mat + x * 4 + y));
            XDrawString(dis, win, gc, 32 + (j), 32 + (i), str, strlen(str));
        }
        printf("\n");
    }
}

void keyhandler(XKeyEvent *e, Display *dis, Window win, GC gc, short *mat)
{
    KeySym ksym;

    ksym = XLookupKeysym(e, 0);
    switch (tolower(ksym)) {
    case XK_Escape:
        printf("Exiting");
        close_x(dis, win, gc);
        break;

    case XK_Left:
        printf("move left\n");
        move(mat, 'L');
        redraw(dis, win, gc, mat);
        fflush(stdout);
        break;

    case XK_Right:
        printf("move right\n");
        move(mat, 'R');
        redraw(dis, win, gc, mat);
        fflush(stdout);
        break;

    case XK_Up:
        printf("move up\n");
        move(mat, 'U');
        redraw(dis, win, gc, mat);
        fflush(stdout);
        break;

    case XK_Down:
        printf("move down\n");
        move(mat, 'D');
        redraw(dis, win, gc, mat);
        fflush(stdout);
        break;

    default:
        break;
    }
}

int main()
{
    Display *dis = XOpenDisplay(None);
    int screen = DefaultScreen(dis);
    XSetWindowAttributes watt;
    watt.background_pixel = WhitePixel(dis, screen);
    watt.background_pixmap = ParentRelative;


    int x = 800;
    int y = 450;
    Window win =
        XCreateWindow(dis, RootWindow(dis, screen), x, y, d_side, d_side, 3,
                      DefaultDepth(dis, screen), InputOutput,
                      DefaultVisual(dis, screen), None, &watt);

    XSetWindowBackground(dis, win, WhitePixel(dis, screen));
    XClassHint *w_class = XAllocClassHint();
    w_class->res_class = "no-tile";
    w_class->res_name = "2048";
    XSetClassHint(dis, win, w_class);

    short *mat = m_init();

    XSelectInput(dis, win, ExposureMask | ButtonPressMask | KeyPressMask);
    XMapWindow(dis, win);
    GC gc = XCreateGC(dis, win, 0, 0);
    redraw(dis, win, gc, mat);
    XFlush(dis);


    XEvent e;
    while (1) {
        XNextEvent(dis, &e);

        switch (e.type) {
        case Expose:
            if (e.xexpose.count == 0)
                redraw(dis, win, gc, mat);
            break;
        case KeyPress:
            keyhandler(&e.xkey, dis, win, gc, mat);
            break;
        default:
            break;
        }
    }

    return 0;
}
