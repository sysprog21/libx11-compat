#include "loader.h"

void(APIENTRY_GL4ES *gl4es_getMainFBSize)(GLint *width, GLint *height);

#if defined NO_LOADER

void *gles = (void *) (~(uintptr_t) 0);
void *egl = (void *) (~(uintptr_t) 0);
void *open_lib(const char **names, const char *override)
{
    return (void *) (~(uintptr_t) 0);
}
void load_libs() {}

#else
// PATH_MAX
#ifdef __linux__
#include <linux/limits.h>
#else
#include <limits.h>
#endif
#include "logs.h"
#include "init.h"
#include "envvars.h"

#ifndef DEFAULT_GLES
#define DEFAULT_GLES NULL
#endif
#ifndef DEFAULT_EGL
#define DEFAULT_EGL NULL
#endif

void *gles = NULL, *egl = NULL, *bcm_host = NULL, *vcos = NULL, *gbm = NULL,
     *drm = NULL;

static const char *path_prefix[] = {
    "", "/opt/vc/lib/", "/usr/local/lib/", "/usr/lib/", NULL,
};

static const char *lib_ext[] = {
    "so", "so.1", "so.2", "dylib", "dll", NULL,
};

static const char *gles2_lib[] = {"libGLESv2_CM", "libGLESv2", NULL};

static const char *gles_lib[] = {"libGLESv1_CM", "libGLES_CM", NULL};

static const char *egl_lib[] = {"libEGL", NULL};

void *open_lib(const char **names, const char *override)
{
    void *lib = NULL;

    char path_name[PATH_MAX + 1];
    int flags = RTLD_LOCAL | RTLD_NOW;
#if defined(RTLD_DEEPBIND)
    static int totest = 1;
    static int sanitizer = 0;
    if (totest) {
        totest = 0;
        char *p = getenv("LD_PRELOAD");
        if (p && strstr(p, "libasan.so"))
            sanitizer = 1;
    }
    // note: breaks address sanitizer
    if (!sanitizer && globals4es.deepbind)
        flags |= RTLD_DEEPBIND;
#endif
    if (override) {
        if ((lib = dlopen(override, flags))) {
            strncpy(path_name, override, PATH_MAX);
            if (!globals4es.nobanner)
                LOGD("LIBGL:loaded: %s\n", path_name);
            return lib;
        } else {
            LOGE("LIBGL_GLES override failed: %s\n", dlerror());
        }
    }
    for (int p = 0; path_prefix[p]; p++) {
        for (int i = 0; names[i]; i++) {
            for (int e = 0; lib_ext[e]; e++) {
                snprintf(path_name, PATH_MAX, "%s%s.%s", path_prefix[p],
                         names[i], lib_ext[e]);
                if ((lib = dlopen(path_name, flags))) {
                    if (!globals4es.nobanner)
                        LOGD("loaded: %s\n", path_name);
                    return lib;
                }
            }
        }
    }
    return lib;
}

void load_libs()
{
    static int first = 1;
    if (!first)
        return;
    first = 0;
    const char *gles_override = GetEnvVar("LIBGL_GLES");
    if (!gles_override) {
        gles_override = DEFAULT_GLES;
    }
    gles = open_lib((globals4es.es == 1) ? gles_lib : gles2_lib, gles_override);
    WARN_NULL(gles);

    egl = gles;
    WARN_NULL(egl);
}
#endif

// user-defined getProcAddress
void *(APIENTRY_GL4ES *gles_getProcAddress)(const char *name);

void *APIENTRY_GL4ES proc_address(void *lib, const char *name)
{
    if (gles_getProcAddress)
        return gles_getProcAddress(name);
#if defined __APPLE__
    // apple code seems to use RTLD_NEXT which is usually ((void*)-1) remove if
    // it not needed
    return dlsym((void *) (~(uintptr_t) 0), name);
#elif !defined NO_LOADER
    return dlsym(lib, name);
#else
    return NULL;
#endif
}
