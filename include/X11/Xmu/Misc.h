#ifndef LIBX11_COMPAT_XMU_MISC_H
#define LIBX11_COMPAT_XMU_MISC_H

/* INT_MAX written as a hex literal. Computing it via (1 << 31) - 1 would shift
 * into the sign bit of a signed int, which C99 6.5.7p4 leaves undefined.
 */
#define MAXDIMENSION 0x7FFFFFFF
#define Max(x, y) (((x) > (y)) ? (x) : (y))
#define Min(x, y) (((x) < (y)) ? (x) : (y))
#define AssignMax(x, y) \
    do {                \
        if ((y) > (x))  \
            (x) = (y);  \
    } while (0)
#define AssignMin(x, y) \
    do {                \
        if ((y) < (x))  \
            (x) = (y);  \
    } while (0)

#endif
