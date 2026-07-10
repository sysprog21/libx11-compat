#ifndef XMMS_COMPAT_PREINCLUDE_H
#define XMMS_COMPAT_PREINCLUDE_H

#include <stdint.h>

#ifndef GUINT16_TO_LE
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define GUINT16_TO_LE(v) ((guint16) (v))
#define GINT16_TO_LE(v) ((gint16) (v))
#define GUINT32_TO_LE(v) ((guint32) (v))
#define GINT32_TO_LE(v) ((gint32) (v))
#define GUINT16_TO_BE(v) ((guint16) __builtin_bswap16((uint16_t) (v)))
#define GINT16_TO_BE(v) ((gint16) __builtin_bswap16((uint16_t) (v)))
#define GUINT32_TO_BE(v) ((guint32) __builtin_bswap32((uint32_t) (v)))
#define GINT32_TO_BE(v) ((gint32) __builtin_bswap32((uint32_t) (v)))
#else
#define GUINT16_TO_LE(v) ((guint16) __builtin_bswap16((uint16_t) (v)))
#define GINT16_TO_LE(v) ((gint16) __builtin_bswap16((uint16_t) (v)))
#define GUINT32_TO_LE(v) ((guint32) __builtin_bswap32((uint32_t) (v)))
#define GINT32_TO_LE(v) ((gint32) __builtin_bswap32((uint32_t) (v)))
#define GUINT16_TO_BE(v) ((guint16) (v))
#define GINT16_TO_BE(v) ((gint16) (v))
#define GUINT32_TO_BE(v) ((guint32) (v))
#define GINT32_TO_BE(v) ((gint32) (v))
#endif
#endif

#endif
