#ifndef LIBX11_COMPAT_INTRINSIC_WRAPPER_H
#define LIBX11_COMPAT_INTRINSIC_WRAPPER_H

/* Some Motif headers default to compact resource-name tables whose offsets are
 * tied to the libXm they were built with. Compat clients link against the
 * system libXm, so force Motif's literal resource-name macros before
 * <Xm/XmStrDefs.h> sees <X11/Intrinsic.h>.
 */
#ifndef XMSTRINGDEFINES
#define XMSTRINGDEFINES 1
#endif

#include_next <X11/Intrinsic.h>

#endif
