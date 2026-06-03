/* Stub X11/ICE/ICElib.h for libx11-compat's libXt build
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

/* libXt's Shell.c includes this header at the top of the file even when
 * XT_NO_SM keeps every ICE/SM call out of the compiled translation unit.
 * Provide an opaque IceConn type plus the small set of helpers and
 * constants the in-tree compatibility surface references so the header
 * resolves without dragging libICE into the build closure.
 */
#ifndef _LIBX11_COMPAT_ICELIB_H_
#define _LIBX11_COMPAT_ICELIB_H_

typedef struct _IceConn *IceConn;

/* Real libICE defines IceProcessMessagesStatus as an enum too, but the
 * values themselves are commonly probed via macros in some downstream
 * code. Mirror with #ifndef so any later #include of the real header
 * (e.g. through a Motif build pulling system X11 headers) does not
 * collide on the enum tag. */
#ifndef IceProcessMessagesSuccess
typedef enum {
    IceProcessMessagesSuccess = 0,
    IceProcessMessagesIOError = 1,
    IceProcessMessagesConnectionClosed = 2
} IceProcessMessagesStatus;
#endif

#endif /* _LIBX11_COMPAT_ICELIB_H_ */
