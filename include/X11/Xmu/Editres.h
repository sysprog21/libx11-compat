/* Stub Xmu/Editres.h for the libXt build
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

/* The real Xmu/Editres.h declares the editres-protocol hook that libXt's
 * Shell.c registers so an external editres client can introspect a live
 * widget tree. libx11-compat ships no Xmu, no editres client targets this
 * in-process display, and Shell.c's only use is the function-pointer
 * registration. A no-op _XEditResCheckMessages keeps Shell.c compilable
 * and link-safe without dragging libXmu into the build.
 */
#ifndef _XMU_EDITRES_H_
#define _XMU_EDITRES_H_

#include <X11/Intrinsic.h>

#define EDITRES_NAME "Editres"
#define EDITRES_COMMAND_ATOM "Comm"
#define EDITRES_PROTOCOL_ATOM "EditresProtocol"
#define EDITRES_VERSION 5

static inline void _XEditResCheckMessages(Widget widget,
                                          XtPointer data,
                                          XEvent *event,
                                          Boolean *cont)
{
    (void) widget;
    (void) data;
    (void) event;
    (void) cont;
}

#endif /* _XMU_EDITRES_H_ */
