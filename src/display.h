
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

/* Display-global HiDPI factor (physical pixels per logical point), probed once
 * at XOpenDisplay from a hidden throwaway SDL window. macOS reports the Retina
 * backing scale only via a live window, and clients such as xwpe load their
 * fixed-pixel fonts before any window exists, so the factor has to be known up
 * front. It scales the font pixel size and core metrics (src/font.c) to match
 * the physical-pixel window geometry promoted in realizeTopLevelWindow, keeping
 * the terminal grid identical while glyphs render at native resolution. 1.0 on
 * non-HiDPI hosts and under the CI dummy driver, so the scaling is a no-op.
 */
double compatGlobalHiDpiScale(void);
void compatSetGlobalHiDpiScale(double scale);

#define SET_X_SERVER_REQUEST(display, requestId)                \
    do {                                                        \
        GET_DISPLAY(display)->request++;                        \
        setLastRequestCode(display, (unsigned char) requestId); \
    } while (0)

#endif  //_DISPLAY_H
