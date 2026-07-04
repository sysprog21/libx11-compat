/*
 * EGL loader: dlopen libEGL and resolve the entry points the GLX layer needs.
 *
 * Search order (first hit wins):
 *   1. $LIBX11_COMPAT_EGL       -- explicit override, pins ANGLE vs Mesa.
 *   2. plugins/<platform>/libEGL -- the vendored ANGLE build shipped next to
 *      libx11-compat (macOS today), located relative to this module via
 *      dladdr so it works regardless of the install prefix.
 *   3. system SONAME            -- where Linux picks up Mesa.
 *
 * When nothing loads, the whole GLX path degrades to "extension absent": every
 * glX* entry point returns a safe NULL/False and XQueryExtension keeps
 * reporting GLX unsupported. That is the correct fallback, not a failure.
 */

#include "egl-wrapper.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static EglApi api;
static EGLDisplay defaultDisplay = EGL_NO_DISPLAY;
static EglClientApi clientApi = EGL_CLIENT_API_NONE;
static bool available = false;
static bool ready = false;
static pthread_once_t loadOnce = PTHREAD_ONCE_INIT;
static pthread_once_t readyOnce = PTHREAD_ONCE_INIT;

#if defined(__APPLE__)
#define EGL_PLATFORM_SUBDIR "macOS"
#define EGL_SONAME "libEGL.dylib"
static const char *const kSystemSonames[] = {"libEGL.dylib", NULL};
#else
#define EGL_PLATFORM_SUBDIR "linux"
#define EGL_SONAME "libEGL.so"
static const char *const kSystemSonames[] = {"libEGL.so.1", "libEGL.so", NULL};
#endif

/* Build "<dir of this module>/plugins/<platform>/libEGL" if we can locate our
 * own path; returns a malloc'd string or NULL. Caller frees.
 */
static char *pluginCandidatePath(void)
{
    Dl_info info;
    if (!dladdr((void *) pluginCandidatePath, &info) || !info.dli_fname)
        return NULL;

    const char *path = info.dli_fname;
    const char *slash = strrchr(path, '/');
    size_t dirLen = slash ? (size_t) (slash - path) : 0;

    const char *suffix = "/plugins/" EGL_PLATFORM_SUBDIR "/" EGL_SONAME;
    size_t suffixLen = strlen(suffix);
    char *out = malloc(dirLen + suffixLen + 1);
    if (!out)
        return NULL;
    memcpy(out, path, dirLen);
    memcpy(out + dirLen, suffix, suffixLen + 1); /* include the NUL */
    return out;
}

static void *dlopenEgl(void)
{
    const char *override = getenv("LIBX11_COMPAT_EGL");
    if (override && override[0]) {
        void *handle = dlopen(override, RTLD_NOW | RTLD_LOCAL);
        if (handle)
            return handle;
        LOG("EGL: LIBX11_COMPAT_EGL=%s did not load: %s\n", override,
            dlerror());
    }

    char *plugin = pluginCandidatePath();
    if (plugin) {
        void *handle = dlopen(plugin, RTLD_NOW | RTLD_LOCAL);
        free(plugin);
        if (handle)
            return handle;
    }

    for (const char *const *soname = kSystemSonames; *soname; soname++) {
        void *handle = dlopen(*soname, RTLD_NOW | RTLD_LOCAL);
        if (handle)
            return handle;
    }
    return NULL;
}

/* Resolve one entry point; sets *ok to false on the first miss so the caller
 * can reject a partial/incompatible libEGL rather than crash later.
 */
static void *resolve(void *handle, const char *name, bool *ok)
{
    void *sym = dlsym(handle, name);
    if (!sym) {
        LOG("EGL: missing symbol %s\n", name);
        *ok = false;
    }
    return sym;
}

/* Decide whether the provider offers desktop GL or is GLES-only. eglBindAPI is
 * the cheap signal; ANGLE rejects EGL_OPENGL_API, Mesa accepts it. A stricter
 * throwaway-context confirmation is deferred until a provider exists to test it
 * against; GLES is the safe default either way.
 */
static EglClientApi probeClientApi(void)
{
    if (api.bindAPI && api.bindAPI(EGL_OPENGL_API)) {
        /* Leave the provider bound to GLES by default; the GLX layer rebinds
         * per context as needed.
         */
        api.bindAPI(EGL_OPENGL_ES_API);
        return EGL_CLIENT_API_DESKTOP;
    }
    return EGL_CLIENT_API_GLES;
}

static void loadImpl(void)
{
    void *handle = dlopenEgl();
    if (!handle) {
        LOG("EGL: no libEGL provider found; GLX reported absent\n");
        return;
    }

    bool ok = true;
    api.getDisplay = resolve(handle, "eglGetDisplay", &ok);
    api.initialize = resolve(handle, "eglInitialize", &ok);
    api.terminate = resolve(handle, "eglTerminate", &ok);
    api.bindAPI = resolve(handle, "eglBindAPI", &ok);
    api.queryString = resolve(handle, "eglQueryString", &ok);
    api.chooseConfig = resolve(handle, "eglChooseConfig", &ok);
    api.getConfigAttrib = resolve(handle, "eglGetConfigAttrib", &ok);
    api.createContext = resolve(handle, "eglCreateContext", &ok);
    api.destroyContext = resolve(handle, "eglDestroyContext", &ok);
    api.createWindowSurface = resolve(handle, "eglCreateWindowSurface", &ok);
    api.createPbufferSurface = resolve(handle, "eglCreatePbufferSurface", &ok);
    api.destroySurface = resolve(handle, "eglDestroySurface", &ok);
    api.querySurface = resolve(handle, "eglQuerySurface", &ok);
    api.makeCurrent = resolve(handle, "eglMakeCurrent", &ok);
    api.swapBuffers = resolve(handle, "eglSwapBuffers", &ok);
    /* Optional: swap-interval control is not essential to rendering, so a
     * provider lacking it must not disable all of GLX. Soft-resolve (no ok
     * flag) and NULL-guard the one caller (glXSwapIntervalEXT).
     */
    api.swapInterval = dlsym(handle, "eglSwapInterval");
    api.getError = resolve(handle, "eglGetError", &ok);
    api.getProcAddress = resolve(handle, "eglGetProcAddress", &ok);
    if (!ok) {
        LOG("EGL: provider missing required entry points; GLX disabled\n");
        dlclose(handle);
        memset(&api, 0, sizeof(api));
        return;
    }

    /* dlopen + resolve only. eglInitialize (the expensive part: on Mesa it
     * spins up llvmpipe and its LLVM JIT) is deferred to readyImpl, run on
     * first actual GL use, so a client can query GLX and choose a visual
     * without paying it up front. See eglEnsureReady.
     */
    available = true;
    LOG("EGL: provider loaded (init deferred)\n");
}

/* The expensive half: initialize the default display and probe desktop-vs-GLES.
 * Deferred so it lands on the first glXMakeCurrent, after the client's window
 * is already mapped, rather than at glXChooseVisual before it exists.
 */
static void readyImpl(void)
{
    if (!available)
        return;
    EGLDisplay display = api.getDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !api.initialize(display, NULL, NULL)) {
        LOG("EGL: eglInitialize failed on the default display; GLX disabled\n");
        available = false;
        return;
    }
    defaultDisplay = display;
    clientApi = probeClientApi();
    ready = true;
    LOG("EGL: provider ready (%s)\n",
        clientApi == EGL_CLIENT_API_DESKTOP ? "desktop GL + GLES" : "GLES");
}

bool eglLoad(void)
{
    pthread_once(&loadOnce, loadImpl);
    return available;
}

bool eglEnsureReady(void)
{
    if (!eglLoad())
        return false;
    pthread_once(&readyOnce, readyImpl);
    return ready;
}

bool eglIsAvailable(void)
{
    return available;
}

const EglApi *eglApi(void)
{
    return &api;
}

/* The default display and the client-API probe are only valid once readyImpl
 * has run; ensure it here so any render-path caller (config choice, context
 * creation, make-current) transparently triggers the deferred initialize on
 * first use.
 */
EGLDisplay eglDefaultDisplay(void)
{
    eglEnsureReady();
    return defaultDisplay;
}

EglClientApi eglProviderClientApi(void)
{
    eglEnsureReady();
    return clientApi;
}
