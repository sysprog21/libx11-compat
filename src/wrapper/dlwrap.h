#ifndef LIBX11_COMPAT_WRAPPER_DLWRAP_H
#define LIBX11_COMPAT_WRAPPER_DLWRAP_H

/* Shared dlopen/dlsym plumbing for the SDL and SDL_ttf wrapper shims. Both
 * shims forward their exported symbols to the real host library resolved at
 * runtime; only the candidate library names and the human-readable label
 * differ, so the loader, the lazy symbol resolver, and the per-symbol thunk
 * macros live here once.
 */

#include <dlfcn.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

/* ASan (and other interceptor-based sanitizers) aborts on dlopen with
 * RTLD_DEEPBIND because deep binding bypasses their symbol interception. Drop
 * the flag in sanitizer builds. The escape hatch LIBX11_COMPAT_NO_RTLD_DEEPBIND
 * lets the build system force the same downgrade when a sanitizer variant the
 * wrapper does not auto-detect is in play.
 *
 * Caveat: RTLD_DEEPBIND was protecting against the real SDL library looking up
 * SDL_* symbols via RTLD_DEFAULT and finding a wrapper's exports instead
 * (which would recurse). RTLD_LOCAL on the dlopen sites doesn't fully replace
 * that; it only hides the loaded library's symbols from later lookups, it
 * doesn't stop the library itself from peeking back into the global scope where
 * the wrapper lives. In practice SDL calls its own internal symbols directly
 * (resolved against its own .so at its link time) rather than via RTLD_DEFAULT,
 * so the recursion has not been observed. If a future SDL release reintroduces
 * RTLD_DEFAULT lookups for its own symbols, sanitizer runs will need to link
 * the real library directly and skip the wrapper.
 */
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if defined(LIBX11_COMPAT_NO_RTLD_DEEPBIND) ||                               \
    defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) ||         \
    defined(__SANITIZE_HWADDRESS__) || __has_feature(address_sanitizer) ||   \
    __has_feature(hwaddress_sanitizer) || __has_feature(memory_sanitizer) || \
    __has_feature(thread_sanitizer)
#undef RTLD_DEEPBIND
#define RTLD_DEEPBIND 0
#endif

#define DLWRAP_FLAGS (RTLD_LAZY | RTLD_LOCAL | RTLD_DEEPBIND)

/* Resolve and cache the real library handle into the caller-owned slot. The
 * slot is accessed with atomic ACQUIRE/RELEASE so concurrent first callers have
 * a well-defined outcome under the C memory model. The race is benign because
 * dlopen reference-counts the underlying library, but the CAS still matters:
 * racing first callers each take a dlopen refcount, so the losers dlclose to
 * balance their open and adopt the winner's handle rather than leaking it.
 */
static inline void *dlwrapOpen(void **handleSlot,
                               const char *overrideEnv,
                               const char *const *candidates,
                               const char *libDesc)
{
    void *cached = __atomic_load_n(handleSlot, __ATOMIC_ACQUIRE);
    if (cached)
        return cached;

    const char *override = getenv(overrideEnv);
    void *opened = NULL;
    if (override && override[0])
        opened = dlopen(override, DLWRAP_FLAGS);
    for (size_t i = 0; !opened && candidates[i]; i++)
        opened = dlopen(candidates[i], DLWRAP_FLAGS);
    if (!opened) {
        fprintf(stderr, "libX11-compat: failed to load %s: %s\n", libDesc,
                dlerror());
        abort();
    }

    void *expected = NULL;
    if (__atomic_compare_exchange_n(handleSlot, &expected, opened, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return opened;
    dlclose(opened);
    return expected;
}

static inline void *dlwrapSym(void *handle,
                              const char *name,
                              const char *libDesc)
{
    void *symbol = dlsym(handle, name);
    if (!symbol) {
        fprintf(stderr, "libX11-compat: failed to resolve %s symbol %s: %s\n",
                libDesc, name, dlerror());
        abort();
    }
    return symbol;
}

/* Define an exported thunk that lazily resolves its real implementation through
 * resolver(#name) and caches it in a function-local slot using the same atomic
 * ACQUIRE/RELEASE dance as dlwrapOpen. resolver is a symbol-lookup function
 * taking the symbol name and returning its address.
 */
#define DLWRAP_THUNK(resolver, ret, name, args, callargs)               \
    ret SDLCALL name args                                               \
    {                                                                   \
        typedef ret(SDLCALL * RealFunc) args;                           \
        static RealFunc realFunc;                                       \
        RealFunc cached = __atomic_load_n(&realFunc, __ATOMIC_ACQUIRE); \
        if (!cached) {                                                  \
            cached = (RealFunc) resolver(#name);                        \
            __atomic_store_n(&realFunc, cached, __ATOMIC_RELEASE);      \
        }                                                               \
        return cached callargs;                                         \
    }

#define DLWRAP_THUNK_VOID(resolver, name, args, callargs)               \
    void SDLCALL name args                                              \
    {                                                                   \
        typedef void(SDLCALL * RealFunc) args;                          \
        static RealFunc realFunc;                                       \
        RealFunc cached = __atomic_load_n(&realFunc, __ATOMIC_ACQUIRE); \
        if (!cached) {                                                  \
            cached = (RealFunc) resolver(#name);                        \
            __atomic_store_n(&realFunc, cached, __ATOMIC_RELEASE);      \
        }                                                               \
        cached callargs;                                                \
    }

#endif
