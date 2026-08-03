#pragma once

#include <stddef.h>
#include <wchar.h>

typedef int WINDOW;
typedef unsigned long chtype;
typedef unsigned long mmask_t;
typedef int cchar_t;
typedef struct {
    int id, x, y, z;
    mmask_t bstate;
} MEVENT;

#define OK 0
#define ERR (-1)
#define TRUE 1
#define FALSE 0
#define LINES 25
#define COLS 80
#define COLORS 16

#define ACS_ULCORNER '+'
#define ACS_URCORNER '+'
#define ACS_LLCORNER '+'
#define ACS_LRCORNER '+'
#define ACS_HLINE '-'
#define ACS_VLINE '|'
#define ACS_CKBOARD '#'
#define ACS_BLOCK '#'

#define A_NORMAL 0
#define A_STANDOUT 1
#define A_UNDERLINE 2
#define A_REVERSE 4
#define A_BLINK 8
#define A_DIM 16
#define A_BOLD 32
#define COLOR_PAIR(n) (n)

#define ALL_MOUSE_EVENTS 0
#define REPORT_MOUSE_POSITION 0
#define BUTTON1_PRESSED 1
#define BUTTON1_RELEASED 2
#define BUTTON1_CLICKED 4
#define BUTTON2_PRESSED 8
#define BUTTON2_RELEASED 16
#define BUTTON2_CLICKED 32
#define BUTTON3_PRESSED 64
#define BUTTON3_RELEASED 128
#define BUTTON3_CLICKED 256
#define BUTTON4_PRESSED 512
#define BUTTON4_RELEASED 1024
#define BUTTON4_CLICKED 2048
#define BUTTON1_DOUBLE_CLICKED 4096
#define BUTTON2_DOUBLE_CLICKED 8192
#define BUTTON3_DOUBLE_CLICKED 16384
#define BUTTON1_TRIPLE_CLICKED 32768
#define BUTTON2_TRIPLE_CLICKED 65536
#define BUTTON3_TRIPLE_CLICKED 131072

#define KEY_CODE_YES 0400
#define KEY_F(n) (KEY_CODE_YES + 200 + (n))
#define KEY_UP (KEY_CODE_YES + 101)
#define KEY_DOWN (KEY_CODE_YES + 102)
#define KEY_LEFT (KEY_CODE_YES + 103)
#define KEY_RIGHT (KEY_CODE_YES + 104)
#define KEY_IC (KEY_CODE_YES + 105)
#define KEY_DC (KEY_CODE_YES + 106)
#define KEY_PPAGE (KEY_CODE_YES + 107)
#define KEY_NPAGE (KEY_CODE_YES + 108)
#define KEY_HOME (KEY_CODE_YES + 109)
#define KEY_END (KEY_CODE_YES + 110)
#define KEY_BTAB (KEY_CODE_YES + 111)
#define KEY_RESIZE (KEY_CODE_YES + 112)
#define KEY_MOUSE (KEY_CODE_YES + 113)
#define KEY_HELP (KEY_CODE_YES + 114)
#define KEY_LL (KEY_CODE_YES + 115)
#define KEY_PREVIOUS (KEY_CODE_YES + 116)
#define KEY_NEXT (KEY_CODE_YES + 117)
#define KEY_BACKSPACE 0407
#define KEY_ENTER 0527

/* A plain "static WINDOW *stdscr" in this header would give every translation
 * unit its own copy. Nothing ever assigns stdscr here (upstream deliberately
 * leaves it to initscr and tolerates stdscr being a non-lvalue macro, as a
 * reentrant ncursesw makes it), and every consumer below ignores the pointer,
 * so define it as a single shared null constant instead of a per-TU object.
 */
#define stdscr ((WINDOW *) 0)

static inline int mousemask(mmask_t a, mmask_t *b)
{
    (void) a;
    (void) b;
    return 0;
}
static inline int getmouse(MEVENT *e)
{
    (void) e;
    return ERR;
}
static inline int clearok(WINDOW *w, int b)
{
    (void) w;
    (void) b;
    return OK;
}
static inline WINDOW *initscr(void)
{
    return stdscr;
}
static inline int cbreak(void)
{
    return OK;
}
static inline int noecho(void)
{
    return OK;
}
static inline int nonl(void)
{
    return OK;
}
static inline int intrflush(WINDOW *w, int b)
{
    (void) w;
    (void) b;
    return OK;
}
static inline int keypad(WINDOW *w, int b)
{
    (void) w;
    (void) b;
    return OK;
}
static inline int set_escdelay(int ms)
{
    (void) ms;
    return OK;
}
static inline int mouseinterval(int ms)
{
    (void) ms;
    return OK;
}
static inline int has_colors(void)
{
    return 0;
}
static inline int start_color(void)
{
    return OK;
}
static inline int init_pair(short p, short f, short b)
{
    (void) p;
    (void) f;
    (void) b;
    return OK;
}
static inline int endwin(void)
{
    return OK;
}
static inline int addch(chtype c)
{
    (void) c;
    return OK;
}
static inline int attrset(int a)
{
    (void) a;
    return OK;
}
static inline int color_set(short p, void *opts)
{
    (void) p;
    (void) opts;
    return OK;
}
static inline int attr_set(int a, short p, void *opts)
{
    (void) a;
    (void) p;
    (void) opts;
    return OK;
}
static inline int setcchar(cchar_t *c,
                           const wchar_t *w,
                           int a,
                           short p,
                           const void *opts)
{
    (void) c;
    (void) w;
    (void) a;
    (void) p;
    (void) opts;
    return OK;
}
static inline int add_wch(const cchar_t *c)
{
    (void) c;
    return OK;
}
static inline int refresh(void)
{
    return OK;
}
static inline int move(int y, int x)
{
    (void) y;
    (void) x;
    return OK;
}
static inline int timeout(int ms)
{
    (void) ms;
    return OK;
}
#define getch() ERR
static inline int ungetch(int c)
{
    (void) c;
    return OK;
}
static inline int nodelay(WINDOW *w, int b)
{
    (void) w;
    (void) b;
    return OK;
}
static inline int flushinp(void)
{
    return OK;
}
