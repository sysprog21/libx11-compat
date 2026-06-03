/* Synthesized config.h for the libXt build inside libx11-compat
 *
 * Copyright 2026 libx11-compat contributors
 * SPDX-License-Identifier: MIT
 */

/* libXt expects an autoconf-generated config.h to define a handful of
 * platform knobs. We are not running autoconf, so the knobs are pinned
 * to values that match the modern macOS / Linux environments libx11-compat
 * targets: poll(2) is always available, threads are enabled, and the
 * session-management surface is disabled so libICE/libSM stay out of the
 * dependency closure (see mk/libxt.mk).
 *
 * This header lives under include/libxt-build/ rather than the X11/
 * namespace because the basename ``config.h'' is too generic to share an
 * include path with anything else. mk/libxt.mk adds the parent directory
 * to the include path *only* for the libXt translation units.
 */

#ifndef LIBX11_COMPAT_LIBXT_CONFIG_H
#define LIBX11_COMPAT_LIBXT_CONFIG_H

#define PACKAGE_NAME "libXt"
#define PACKAGE_VERSION "1.3.1"
#define PACKAGE_STRING "libXt 1.3.1"
#define PACKAGE_BUGREPORT "https://github.com/sysprog21/libx11-compat/issues"
#define VERSION PACKAGE_VERSION

/* poll() is on every libx11-compat-supported platform. The select() fallback
 * path in NextEvent.c is bit-set-sized and predates large fd numbers; we
 * never want it. */
#define USE_POLL 1

/* Multi-threaded toolkit. Wires Threads.c's mutex hooks into the XtAppLock
 * machinery so XtAppContexts can be shared across threads. */
#define XTHREADS 1

/* Disable session-management hooks in Shell.c so libICE / libSM stay out
 * of the link line. SDL has no notion of XSMP and the in-process display
 * has no peer to negotiate with. */
#define XT_NO_SM 1

/* HAVE_REALLOCARRAY left undefined deliberately. reallocarray() exists in
 * Apple's libSystem and glibc >= 2.26, but the macOS SDK does not declare
 * it in <stdlib.h> even with _DARWIN_C_SOURCE, so the implicit-declaration
 * error kills the build before the linker would have found it. libXt's
 * Alloc.c falls back to Xrealloc + an explicit overflow check when this
 * macro is absent, which costs nothing in performance and works on every
 * supported host. */

/* asprintf is on every glibc and Apple libc. Initialize.c uses it for
 * defaults-file path assembly. */
#define HAVE_ASPRINTF 1

#endif /* LIBX11_COMPAT_LIBXT_CONFIG_H */
