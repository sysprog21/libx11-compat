#ifndef UTIL_H
#define UTIL_H

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif /* MAX */
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif /* MIN */

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof(array[0]))

#if defined(__cplusplus) || defined(c_plusplus)
#define CLASS_ATTRIBUTE c_class
#else
#define CLASS_ATTRIBUTE class
#endif

#define TO_STRING_HELPER(x) #x
#define TO_STRING(x) TO_STRING_HELPER(x)

#include <stdio.h>

#ifdef DEBUG_LIBX11_COMPAT
#define LOG(msg, args...) fprintf(stderr, msg, ##args)
#else

/* Release builds short-circuit the call so args are still type-checked and
 * marked as "used", but never executed.
 */
#define LOG(msg, args...) ((void) (0 && fprintf(stderr, msg, ##args)))
#endif /* DEBUG_LIBX11_COMPAT */

/* Debug builds always log the hit; release builds log only when the env flag is
 * set (see libx11CompatWarnUnimplemented below), so the default release path
 * stays a no-op. The do/while wrapper lets a plain WARN_UNIMPLEMENTED;
 * statement expand safely in any context.
 */
#ifdef DEBUG_LIBX11_COMPAT
#define WARN_UNIMPLEMENTED_ENABLED 1
#else
#define WARN_UNIMPLEMENTED_ENABLED libx11CompatWarnUnimplemented()
#endif /* DEBUG_LIBX11_COMPAT */

#define WARN_UNIMPLEMENTED                                                 \
    do {                                                                   \
        if (WARN_UNIMPLEMENTED_ENABLED)                                    \
            fprintf(stderr, "Hit unimplemented function %s.\n", __func__); \
    } while (0)

/* Unconditional diagnostic trace, independent of DEBUG_LIBX11_COMPAT and of
 * stderr (curses apps such as xwpe take over the terminal, so stderr LOG output
 * is unreliable). When the LIBX11_COMPAT_TRACE environment variable names a
 * file, each call appends one formatted line to it; otherwise it is a no-op.
 */
void compatTrace(const char *fmt, ...);

/* Hide private data symbols (lock function pointers, global mutex state) from
 * the dynamic symbol table so a system libX11.so.6 loaded into the same process
 * via SDL2's transitive dependency chain cannot interpose on them. Without
 * this, libX11.so.6 and libX11-compat.so share storage for _XInitDisplayLock_fn
 * et al., libX11.so.6's XInitThreads (or any implicit init) writes a
 * libX11.so.6 function address into the shared slot, libX11-compat's
 * XOpenDisplay invokes it, and Display->lock_fns ends up with libX11.so.6's
 * vtable, crashing the next LockDisplay against a mismatched struct layout.
 */
#if defined(__GNUC__) || defined(__clang__)
#define LIBX11_COMPAT_HIDDEN __attribute__((visibility("hidden")))
#else
#define LIBX11_COMPAT_HIDDEN
#endif

/* Thread-local storage qualifier that degrades to plain static in the
 * single-threaded (non-pthread) Emscripten build. That build does not
 * zero-initialize a thread's TLS block, so a __thread variable reads
 * engine-dependent garbage on first use: under headless WebKitGTK the
 * XOpenDisplay recursion guard tripped on its own uninitialized flag and every
 * client came up "Can't open display". With no pthreads there is exactly one
 * thread, so a plain static is behaviorally identical and lives in the data
 * segment, which is initialized correctly. Every other target keeps real
 * thread-local storage.
 */
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
#define COMPAT_THREAD_LOCAL
#else
#define COMPAT_THREAD_LOCAL __thread
#endif

/* Returns nonzero when LIBX11_COMPAT_WARN_UNIMPLEMENTED is set to a non-empty,
 * non-"0" value. Lets a release build surface unimplemented-call coverage on
 * demand (e.g. a missing-Xlib gate driving a real app) without a debug rebuild.
 * Internal helper in the sense that clients do not call it directly, but
 * deliberately exported (not marked hidden): sibling compat libraries
 * (libXmu-compat, libXext-compat, ...) are built from sources that expand
 * WARN_UNIMPLEMENTED and link against libX11-compat for this helper, so it must
 * stay resolvable in the dynamic symbol table.
 */
int libx11CompatWarnUnimplemented(void);

#include "X11/Xlib.h"

Bool compatSelfScalingToolkitLoaded(void);

/* HiDPI is handled by presenting the client-sized X backing through SDL's
 * high-DPI output, not by changing X11 geometry behind the client's back. This
 * predicate gates the older physical-pixel promotion path, off by default and
 * opt-in via LIBX11_COMPAT_HIDPI_PROMOTE. Centralizing it keeps geometry
 * promotion and core-font scaling on one switch so they cannot drift.
 */
Bool compatHiDpiPromoteToolkits(void);

typedef struct {
    void **array;
    size_t length;
    size_t capacity;
} Array;

Bool initArray(Array *a, size_t initialSize);
Bool insertArray(Array *a, void *element);
void *removeArray(Array *a, size_t index, Bool preserveOrder);
ssize_t findInArray(Array *a, void *element);
ssize_t findInArrayN(Array *a, void *element, size_t startIndex);
ssize_t findInArrayCmp(Array *a,
                       void *element,
                       Bool (*cmpFunc)(void *, void *));
ssize_t findInArrayNCmp(Array *a,
                        void *element,
                        size_t startIndex,
                        Bool (*cmpFunc)(void *, void *));
void swapArray(Array *a, size_t index1, size_t index2);
void freeArray(Array *a);
Bool matchWildcard(const char *wildcard, const char *string);

/* Read a numeric env knob, clamped. A value below "lo" is a typo rather than an
 * intent, so it falls back to "def"; one above "hi" asks for more than the code
 * can honor, so it saturates. Trailing junk or an out-of-range parse leaves the
 * default. One parser so the knobs cannot disagree on what they accept.
 */
long long compatEnvClamped(const char *name,
                           long long def,
                           long long lo,
                           long long hi);


#endif /* UTIL_H */
