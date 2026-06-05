
#ifndef _DISPLAY_H
#define _DISPLAY_H

#include "resource-types.h"

#define GET_DISPLAY(display) ((_XPrivDisplay) (display))

void setLastRequestCode(Display *display, unsigned char requestCode);
unsigned char getLastRequestCode(Display *display);

#define SET_X_SERVER_REQUEST(display, requestId)                \
    do {                                                        \
        GET_DISPLAY(display)->request++;                        \
        setLastRequestCode(display, (unsigned char) requestId); \
    } while (0)

#endif  //_DISPLAY_H
