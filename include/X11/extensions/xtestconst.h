/* XTest protocol constants kept here so libx11-compat consumers can
 * include <X11/extensions/XTest.h> without depending on a host
 * xorgproto / libXtst dev install. The pinned upstream tarball also
 * provides this file (synced into build/upstream/include/) but the
 * public copy under include/ is what shipped headers reference.
 *
 * Copyright 1992, 1998 The Open Group. SPDX-License-Identifier: MIT
 */
#ifndef _XTEST_CONST_H_
#define _XTEST_CONST_H_

#define XTestNumberEvents 0
#define XTestNumberErrors 0
#define XTestCurrentCursor ((Cursor) 1)
#define XTestMajorVersion 2
#define XTestMinorVersion 2
#define XTestExtensionName "XTEST"

#endif
