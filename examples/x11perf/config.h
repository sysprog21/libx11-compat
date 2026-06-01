/* Local config for building upstream x11perf inside libX11-compat. */
#ifndef LIBX11_COMPAT_EXAMPLES_X11PERF_CONFIG_H
#define LIBX11_COMPAT_EXAMPLES_X11PERF_CONFIG_H

#include <sys/time.h>

#define PACKAGE_STRING "x11perf upstream master"
#define HAVE_GETTIMEOFDAY 1
#ifndef X_GETTIMEOFDAY
#define X_GETTIMEOFDAY(t) gettimeofday((t), NULL)
#endif

#endif
