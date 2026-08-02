#pragma once

static inline char *tgetstr(const char *id, char **area)
{
    (void) id;
    (void) area;
    return 0;
}

/* Real tgetnum returns -1 when a numeric capability is absent; callers such as
 * xwpe's we_term.c rely on that sentinel (e.g. "if (tgetnum("Co") < 0)
 * default") to tell "missing" from a genuine zero.
 */
static inline int tgetnum(const char *id)
{
    (void) id;
    return -1;
}
static inline int tgetflag(const char *id)
{
    (void) id;
    return 0;
}
static inline char *tigetstr(const char *id)
{
    (void) id;
    return 0;
}
static inline int tigetnum(const char *id)
{
    (void) id;
    return 0;
}
static inline int tgetent(char *bp, const char *name)
{
    (void) bp;
    (void) name;
    return 0;
}
static inline char *tgoto(const char *cap, int col, int row)
{
    (void) col;
    (void) row;
    return (char *) cap;
}
static inline int tputs(const char *str, int affcnt, int (*putc_fn)(int))
{
    (void) affcnt;
    while (str && *str)
        putc_fn(*str++);
    return 0;
}
