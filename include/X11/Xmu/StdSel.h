#ifndef LIBX11_COMPAT_XMU_STDSEL_H
#define LIBX11_COMPAT_XMU_STDSEL_H

#include <X11/Intrinsic.h>
#include <X11/Xfuncproto.h>

_XFUNCPROTOBEGIN
Boolean XmuConvertStandardSelection(Widget w,
                                    Time time,
                                    Atom *selection,
                                    Atom *target,
                                    Atom *type_return,
                                    XPointer *value_return,
                                    unsigned long *length_return,
                                    int *format_return);
_XFUNCPROTOEND

#endif
