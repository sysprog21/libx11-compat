
#ifndef _DISPLAY_H
#define _DISPLAY_H

#include "resource-types.h"

#ifdef DEBUG_LIBX11_COMPAT
#include <stdio.h>
#include <stdlib.h>
#define GET_DISPLAY(display)                                             \
    (__extension__({                                                     \
        Display *_gd_d = (display);                                      \
        if (!_gd_d) {                                                    \
            fprintf(stderr, "%s:%d: GET_DISPLAY(NULL) in debug build\n", \
                    __FILE__, __LINE__);                                 \
            abort();                                                     \
        }                                                                \
        (_XPrivDisplay) _gd_d;                                           \
    }))
#else
#define GET_DISPLAY(display) ((_XPrivDisplay) (display))
#endif

void setLastRequestCode(Display *display, unsigned char requestCode);
unsigned char getLastRequestCode(Display *display);

#define SET_X_SERVER_REQUEST(display, requestId)                \
    do {                                                        \
        GET_DISPLAY(display)->request++;                        \
        setLastRequestCode(display, (unsigned char) requestId); \
    } while (0)

#endif  //_DISPLAY_H
