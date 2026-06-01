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
 * marked as "used", but never executed. */
#define LOG(msg, args...) ((void) (0 && fprintf(stderr, msg, ##args)))
#endif /* DEBUG_LIBX11_COMPAT */

#define WARN_UNIMPLEMENTED LOG("Hit unimplemented function %s.\n", __func__)

/* Hide private data symbols (lock function pointers, global mutex state)
 * from the dynamic symbol table so a system libX11.so.6 loaded into the
 * same process via SDL2's transitive dependency chain cannot interpose
 * on them. Without this, libX11.so.6 and libX11-compat.so share storage
 * for _XInitDisplayLock_fn et al., libX11.so.6's XInitThreads (or any
 * implicit init) writes a libX11.so.6 function address into the shared
 * slot, our XOpenDisplay invokes it, and Display->lock_fns ends up with
 * libX11.so.6's vtable -- crashing the next LockDisplay against a
 * mismatched struct layout. */
#if defined(__GNUC__) || defined(__clang__)
#define LIBX11_COMPAT_HIDDEN __attribute__((visibility("hidden")))
#else
#define LIBX11_COMPAT_HIDDEN
#endif

#include "X11/Xlib.h"

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

#endif /* UTIL_H */
