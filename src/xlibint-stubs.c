/*
 * Definitions for symbols upstream libX11 expects from XlibInt.c and the
 * lcWrap/lcConv i18n helpers, which libx11-compat does not compile.
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

/* Match libX11's own internal build: defining _XLIBINT_ before Xlibint.h
 * suppresses the WIN32 macro rewrites that would alias _XCreateMutex_fn (and
 * friends) to *_p indirections. On non-WIN32 platforms the define is a no-op
 * but it keeps the storage provided here in sync with whatever upstream emits.
 */
#define _XLIBINT_
#include "X11/Xlibint.h"
#include "util.h"

#ifdef XTHREADS
#include <X11/Xthreads.h>

/* Every lock function pointer and global lock pointer below is
 * LIBX11_COMPAT_HIDDEN. A system libX11.so.6 loaded into the same process via
 * SDL2's transitive deps exports identical names; without hidden visibility the
 * dynamic linker picks one slot as canonical and both libraries write through
 * it, so libX11.so.6's XInitThreads (or implicit init) would clobber
 * libx11-compat state and the LockDisplay macro would dispatch into libX11.so.6
 * code against the libX11-compat Display layout. See util.h for the full
 * rationale.
 */
LIBX11_COMPAT_HIDDEN LockInfoPtr _Xglobal_lock = NULL;
LIBX11_COMPAT_HIDDEN void (*_XCreateMutex_fn)(LockInfoPtr) = NULL;
LIBX11_COMPAT_HIDDEN void (*_XFreeMutex_fn)(LockInfoPtr) = NULL;
LIBX11_COMPAT_HIDDEN void (*_XLockMutex_fn)(LockInfoPtr
#if defined(XTHREADS_WARN) || defined(XTHREADS_FILE_LINE)
                                            ,
                                            char *,
                                            int
#endif
                                            ) = NULL;
LIBX11_COMPAT_HIDDEN void (*_XUnlockMutex_fn)(LockInfoPtr
#if defined(XTHREADS_WARN) || defined(XTHREADS_FILE_LINE)
                                              ,
                                              char *,
                                              int
#endif
                                              ) = NULL;
LIBX11_COMPAT_HIDDEN xthread_t (*_Xthread_self_fn)(void) = NULL;

/* lcWrap.c owns _Xi18n_lock and lcConv.c owns _conv_lock in upstream; neither
 * file is compiled here, so locking.c initializes through these stubs at
 * XInitThreads() time.
 */
LIBX11_COMPAT_HIDDEN LockInfoPtr _Xi18n_lock = NULL;
LIBX11_COMPAT_HIDDEN LockInfoPtr _conv_lock = NULL;

#endif /* XTHREADS */

/* XCB-side hooks invoked by upstream locking.c around LockDisplay. The
 * compatibility layer does not speak the X11 protocol, so there is no request
 * queue to flush and the hooks collapse to no-ops.
 */
void _XIDHandler(Display *dpy)
{
    (void) dpy;
}

void _XSeqSyncFunction(Display *dpy)
{
    (void) dpy;
}
