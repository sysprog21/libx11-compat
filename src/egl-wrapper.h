/*
 * Minimal EGL loader for the GLX-over-EGL path.
 *
 * libx11-compat dlopens libEGL at runtime by SONAME; the concrete provider is
 * resolved on the host (ANGLE on macOS, Mesa on Linux). We deliberately do NOT
 * depend on a system <EGL/egl.h>: only the handful of EGL types, tokens, and
 * entry points the GLX layer needs are declared here. This keeps the build
 * self-contained and lets a machine with no EGL still compile libx11-compat and
 * simply report GLX absent.
 *
 * All platform-specific library loading lives behind this wrapper (not in
 * src/glx.c) so a future Windows port is a new loader backend rather than a
 * rewrite of the GLX logic.
 */
#ifndef LIBX11_COMPAT_EGL_WRAPPER_H
#define LIBX11_COMPAT_EGL_WRAPPER_H

#include <stdbool.h>
#include <stdint.h>

#include "util.h" /* LIBX11_COMPAT_HIDDEN */

/* EGL scalar and handle types (ABI matches the Khronos definitions). */
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;
typedef int32_t EGLint;
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLNativeDisplayType;
typedef void *EGLNativeWindowType;
typedef void (*EGLFuncPointer)(void);

#define EGL_FALSE 0
#define EGL_TRUE 1
#define EGL_NONE 0x3038
#define EGL_NO_DISPLAY ((EGLDisplay) 0)
#define EGL_NO_CONTEXT ((EGLContext) 0)
#define EGL_NO_SURFACE ((EGLSurface) 0)
#define EGL_DEFAULT_DISPLAY ((EGLNativeDisplayType) 0)

/* eglBindAPI arguments. */
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_OPENGL_API 0x30A2

/* eglQueryString names. */
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_EXTENSIONS 0x3055
#define EGL_CLIENT_APIS 0x308D

/* Config attributes. */
#define EGL_ALPHA_SIZE 0x3021
#define EGL_BLUE_SIZE 0x3022
#define EGL_GREEN_SIZE 0x3023
#define EGL_RED_SIZE 0x3024
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SAMPLES 0x3031
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_SURFACE_TYPE 0x3033
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_RENDERABLE_TYPE_VALUE_NONE 0
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_CONFIG_ID 0x3028

/* EGL_SURFACE_TYPE bits. */
#define EGL_PBUFFER_BIT 0x0001
#define EGL_WINDOW_BIT 0x0004

/* EGL_RENDERABLE_TYPE bits. */
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x00000040
#define EGL_OPENGL_BIT 0x0008

/* Context / surface creation attributes. */
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056

/* Resolved EGL entry points. Names mirror the C API; a NULL member means the
 * symbol was not found (treated as provider-unavailable).
 */
typedef struct EglApi {
    EGLDisplay (*getDisplay)(EGLNativeDisplayType);
    EGLBoolean (*initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*terminate)(EGLDisplay);
    EGLBoolean (*bindAPI)(EGLenum);
    const char *(*queryString)(EGLDisplay, EGLint);
    EGLBoolean (*chooseConfig)(EGLDisplay,
                               const EGLint *,
                               EGLConfig *,
                               EGLint,
                               EGLint *);
    EGLBoolean (*getConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    EGLContext (*createContext)(EGLDisplay,
                                EGLConfig,
                                EGLContext,
                                const EGLint *);
    EGLBoolean (*destroyContext)(EGLDisplay, EGLContext);
    EGLSurface (*createWindowSurface)(EGLDisplay,
                                      EGLConfig,
                                      EGLNativeWindowType,
                                      const EGLint *);
    EGLSurface (*createPbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLBoolean (*destroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*querySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*swapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*swapInterval)(EGLDisplay, EGLint);
    EGLint (*getError)(void);
    EGLFuncPointer (*getProcAddress)(const char *);
} EglApi;

/* Provider client-API capability, decided once at load. */
typedef enum EglClientApi {
    EGL_CLIENT_API_NONE = 0, /* no provider loaded */
    EGL_CLIENT_API_GLES,     /* OpenGL ES only (e.g. ANGLE) */
    EGL_CLIENT_API_DESKTOP   /* desktop GL available too (e.g. Mesa) */
} EglClientApi;

/* Load libEGL on first call and cache the outcome.
 *
 * Returns true when a provider is loaded (dlopen + entry points resolved). This
 * is the cheap half: it does NOT eglInitialize the display, so a client can
 * probe GLX and choose a visual without paying the provider's initialize cost
 * (on Mesa, llvmpipe + LLVM). Call eglEnsureReady before any actual GL use.
 * Safe to call from multiple threads; the load runs once.
 */
LIBX11_COMPAT_HIDDEN bool eglLoad(void);

/* Ensure the provider's default display is initialized (the expensive half,
 * deferred by eglLoad). Runs once, lands on the first render-path use.
 *
 * Returns true when the display is ready.
 * eglDefaultDisplay/eglProviderClientApi call it for you.
 */
LIBX11_COMPAT_HIDDEN bool eglEnsureReady(void);

/* True once eglLoad has succeeded. Cheap; does not trigger a load. */
LIBX11_COMPAT_HIDDEN bool eglIsAvailable(void);

/* Resolved entry points; valid only when eglIsAvailable() is true. */
LIBX11_COMPAT_HIDDEN const EglApi *eglApi(void);

/* The initialized default EGLDisplay, or EGL_NO_DISPLAY when unavailable. */
LIBX11_COMPAT_HIDDEN EGLDisplay eglDefaultDisplay(void);

/* Provider capability decided at load. EGL_CLIENT_API_NONE when unavailable. */
LIBX11_COMPAT_HIDDEN EglClientApi eglProviderClientApi(void);

#endif /* LIBX11_COMPAT_EGL_WRAPPER_H */
