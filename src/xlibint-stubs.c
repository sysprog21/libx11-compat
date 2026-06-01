/* Definitions for symbols upstream libX11 expects from XlibInt.c and
 * the lcWrap/lcConv i18n helpers, which libx11-compat does not compile.
 *
 * Copyright 2025 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

/* Match libX11's own internal build: defining _XLIBINT_ before Xlibint.h
 * suppresses the WIN32 macro rewrites that would alias _XCreateMutex_fn
 * (and friends) to *_p indirections. On non-WIN32 platforms the define is
 * a no-op but it keeps the storage we provide here in sync with whatever
 * upstream emits.
 */
#define _XLIBINT_
#include "X11/Xlibint.h"

#ifdef XTHREADS
#include <X11/Xthreads.h>

LockInfoPtr _Xglobal_lock = NULL;
void (*_XCreateMutex_fn)(LockInfoPtr) = NULL;
void (*_XFreeMutex_fn)(LockInfoPtr) = NULL;
void (*_XLockMutex_fn)(LockInfoPtr
#if defined(XTHREADS_WARN) || defined(XTHREADS_FILE_LINE)
                       ,
                       char *,
                       int
#endif
                       ) = NULL;
void (*_XUnlockMutex_fn)(LockInfoPtr
#if defined(XTHREADS_WARN) || defined(XTHREADS_FILE_LINE)
                         ,
                         char *,
                         int
#endif
                         ) = NULL;
xthread_t (*_Xthread_self_fn)(void) = NULL;

/* lcWrap.c owns _Xi18n_lock and lcConv.c owns _conv_lock in upstream;
 * neither file is compiled here, so locking.c initializes through these
 * stubs at XInitThreads() time.
 */
LockInfoPtr _Xi18n_lock = NULL;
LockInfoPtr _conv_lock = NULL;

#endif /* XTHREADS */

/* XCB-side hooks invoked by upstream locking.c around LockDisplay. The
 * compatibility layer does not speak the X11 protocol, so there is no
 * request queue to flush and the hooks collapse to no-ops.
 */
void _XIDHandler(Display *dpy)
{
    (void) dpy;
}

void _XSeqSyncFunction(Display *dpy)
{
    (void) dpy;
}
