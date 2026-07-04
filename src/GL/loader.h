#ifndef _GL4ES_LOADER_H_
#define _GL4ES_LOADER_H_

#include <stdbool.h>
#include "gl4es.h"
#include "gles.h"
#include "logs.h"


#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardext.h"
extern void *(APIENTRY_GL4ES *gles_getProcAddress)(const char *name);
extern void(APIENTRY_GL4ES *gl4es_getMainFBSize)(GLint *width, GLint *height);
NonAliasExportDecl(void *, proc_address, (void *lib, const char *name));
// will become references to dlopen'd gles and egl
extern void *gles, *bcm_host, *vcos, *gbm, *drm;
EXPORT extern void *egl;
#if defined __APPLE__ || defined __EMSCRIPTEN__
#define NO_LOADER
#endif

#define WARN_NULL(name)                                                        \
    if (name == NULL)                                                          \
        LOGD("warning, %s line %d function %s: " #name " is NULL\n", __FILE__, \
             __LINE__, __func__);

#define MSVC_SPC(MACRO, ARGS) MACRO ARGS

#define PUSH_IF_COMPILING_EXT(nam, ...)                   \
    if (glstate->list.active) {                           \
        if (!glstate->list.pending) {                     \
            NewStage(glstate->list.active, STAGE_GLCALL); \
            MSVC_SPC(push_##nam, (__VA_ARGS__));          \
            noerrorShim();                                \
            return (nam##_RETURN) 0;                      \
        } else                                            \
            gl4es_flush();                                \
    }

#define PUSH_IF_COMPILING(name) PUSH_IF_COMPILING_EXT(name, name##_ARG_NAMES)

#if (__STDC_VERSION__ > 201710L)  // > C23
#define THREAD_LOCAL thread_local
#elif (__STDC_VERSION__ >= 201112L)  // >= C11
#define THREAD_LOCAL _Thread_local
#elif defined(__GCC__) || defined(__clang__)
#define THREAD_LOCAL __thread
#else
#define THREAD_LOCAL
#endif

#define DEFINE_RAW(lib, name) static name##_PTR lib##_##name = NULL
#define LOAD_RAW(lib, name, ...)                         \
    {                                                    \
        static THREAD_LOCAL bool first = true;           \
        if (first) {                                     \
            first = false;                               \
            if (lib != NULL) {                           \
                lib##_##name = (name##_PTR) __VA_ARGS__; \
            }                                            \
            WARN_NULL(lib##_##name);                     \
        }                                                \
    }

#define LOAD_RAW_3(lib, name, fnc1, fnc2, ...)        \
    {                                                 \
        static THREAD_LOCAL bool first = true;        \
        if (first) {                                  \
            first = false;                            \
            if (lib != NULL) {                        \
                lib##_##name = (name##_PTR) fnc1;     \
                if (!lib##_##name)                    \
                    lib##_##name = (name##_PTR) fnc2; \
                if (!lib##_##name) {                  \
                    __VA_ARGS__                       \
                }                                     \
            }                                         \
        }                                             \
    }

#define LOAD_RAW_SILENT(lib, name, ...)                  \
    {                                                    \
        static THREAD_LOCAL bool first = true;           \
        if (first) {                                     \
            first = false;                               \
            if (lib != NULL) {                           \
                lib##_##name = (name##_PTR) __VA_ARGS__; \
            }                                            \
        }                                                \
    }

#define LOAD_RAW_ALT(lib, alt, name, ...)                \
    {                                                    \
        static THREAD_LOCAL bool first = true;           \
        if (first) {                                     \
            first = false;                               \
            if (lib != NULL) {                           \
                lib##_##name = (name##_PTR) __VA_ARGS__; \
            }                                            \
            if (lib##_##name == NULL)                    \
                lib##_##name = alt##_##name;             \
        }                                                \
    }

#define LOAD_LIB(lib, name) \
    DEFINE_RAW(lib, name);  \
    LOAD_RAW(lib, name, proc_address(lib, #name))
#define LOAD_LIB_SILENT(lib, name) \
    DEFINE_RAW(lib, name);         \
    LOAD_RAW_SILENT(lib, name, proc_address(lib, #name))
#define LOAD_LIB_ALT(lib, alt, name) \
    DEFINE_RAW(lib, name);           \
    LOAD_RAW_ALT(lib, alt, name, proc_address(lib, #name))

#define LOAD_GLES(name) LOAD_LIB(gles, name)
#define LOAD_GLES2(name) LOAD_LIB_SILENT(gles, name)
#define LOAD_GLES_OR_FPE(name) LOAD_LIB_ALT(gles, fpe, name)

#define LOAD_GLES_FPE(name)                              \
    DEFINE_RAW(gles, name);                              \
    if (hardext.esversion == 1) {                        \
        LOAD_RAW(gles, name, proc_address(gles, #name)); \
    } else {                                             \
        gles_##name = fpe_##name;                        \
    }

#define LOAD_EGL(name) LOAD_LIB(egl, name)

#define LOAD_GBM(name) LOAD_LIB(gbm, name)

#define LOAD_GLES_OES(name)                                    \
    DEFINE_RAW(gles, name);                                    \
    {                                                          \
        LOAD_RAW(gles, name, proc_address(gles, #name "OES")); \
    }

#define LOAD_GLES_EXT(name)                                    \
    DEFINE_RAW(gles, name);                                    \
    {                                                          \
        LOAD_RAW(gles, name, proc_address(gles, #name "EXT")); \
    }

#define LOAD_GLES2_OR_OES(name)                                 \
    DEFINE_RAW(gles, name);                                     \
    {                                                           \
        LOAD_RAW_SILENT(gles, name, proc_address(gles, #name)); \
    }


#endif  // _GL4ES_LOADER_H_
