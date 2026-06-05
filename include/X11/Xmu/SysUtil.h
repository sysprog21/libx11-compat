#ifndef LIBX11_COMPAT_XMU_SYSUTIL_H
#define LIBX11_COMPAT_XMU_SYSUTIL_H

#include <X11/Xfuncproto.h>

_XFUNCPROTOBEGIN
int XmuGetHostname(char *buf_return, int maxlen);
int XmuSnprintf(char *str, int size, const char *fmt, ...)
    _X_ATTRIBUTE_PRINTF(3, 4);
_XFUNCPROTOEND

#endif
