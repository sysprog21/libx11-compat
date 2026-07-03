#ifndef LIBX11_COMPAT_ANGLE_COMPAT_COMPRESSION_UTILS_PORTABLE_H
#define LIBX11_COMPAT_ANGLE_COMPAT_COMPRESSION_UTILS_PORTABLE_H

/* Stands in for Chromium's compression_utils_portable.h, which ANGLE includes
 * for gzip helpers. Minimal zlib-backed implementation; no Chromium base.
 */
#include <stdint.h>
#include <string.h>
#include <zlib.h>

namespace zlib_internal
{

inline uLong GzipExpectedCompressedSize(uLong source_size)
{
    return compressBound(source_size) + 18;
}

inline int GzipCompressHelper(Bytef *dest,
                              uLong *dest_len,
                              const Bytef *source,
                              uLong source_len,
                              void *malloc_fn,
                              void *free_fn)
{
    (void) malloc_fn;
    (void) free_fn;

    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = const_cast<Bytef *>(source);
    stream.avail_in = static_cast<uInt>(source_len);
    stream.next_out = dest;
    stream.avail_out = static_cast<uInt>(*dest_len);

    int rc = deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                          MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
    if (rc != Z_OK)
    {
        return rc;
    }

    rc = deflate(&stream, Z_FINISH);
    if (rc == Z_STREAM_END)
    {
        *dest_len = stream.total_out;
        rc = Z_OK;
    }
    deflateEnd(&stream);
    return rc;
}

inline uint32_t GetGzipUncompressedSize(const uint8_t *source, size_t source_len)
{
    if (source_len < 4)
    {
        return 0;
    }

    const uint8_t *size = source + source_len - 4;
    return static_cast<uint32_t>(size[0]) | (static_cast<uint32_t>(size[1]) << 8) |
           (static_cast<uint32_t>(size[2]) << 16) |
           (static_cast<uint32_t>(size[3]) << 24);
}

inline int GzipUncompressHelper(Bytef *dest,
                                uLong *dest_len,
                                const Bytef *source,
                                uLong source_len)
{
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    stream.next_in = const_cast<Bytef *>(source);
    stream.avail_in = static_cast<uInt>(source_len);
    stream.next_out = dest;
    stream.avail_out = static_cast<uInt>(*dest_len);

    int rc = inflateInit2(&stream, MAX_WBITS + 32);
    if (rc != Z_OK)
    {
        return rc;
    }

    rc = inflate(&stream, Z_FINISH);
    if (rc == Z_STREAM_END)
    {
        *dest_len = stream.total_out;
        rc = Z_OK;
    }
    inflateEnd(&stream);
    return rc;
}

}  // namespace zlib_internal

#endif
