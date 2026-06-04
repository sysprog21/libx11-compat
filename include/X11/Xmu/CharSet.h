#ifndef LIBX11_COMPAT_XMU_CHARSET_H
#define LIBX11_COMPAT_XMU_CHARSET_H

#include <X11/Xfuncproto.h>

_XFUNCPROTOBEGIN
void XmuCopyISOLatin1Lowered(char *dst_return, const char *src);
void XmuCopyISOLatin1Uppered(char *dst_return, const char *src);
int XmuCompareISOLatin1(const char *first, const char *second);
void XmuNCopyISOLatin1Lowered(char *dst_return, const char *src, int size);
void XmuNCopyISOLatin1Uppered(char *dst_return, const char *src, int size);
_XFUNCPROTOEND

#endif
