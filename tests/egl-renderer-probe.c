/*
 * Print the surfaceless GLES renderer and version.
 *
 * A diagnostic for the headless GLX render path: it reports which driver the
 * host's EGL provider actually binds under EGL_PLATFORM=surfaceless (llvmpipe
 * software vs a real or virtual GPU), which decides whether the paperplane
 * offscreen-pbuffer readback/composite has a framebuffer it can read back.
 *
 * Usage: EGL_PLATFORM=surfaceless egl-renderer-probe
 * Prints "RENDERER=... | VERSION=..." on success, a diagnostic and nonzero exit
 * on any EGL setup failure.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <stdio.h>

int main(void)
{
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)) {
        printf("eglInitialize failed\n");
        return 1;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfgAttribs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                           EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, cfgAttribs, &cfg, 1, &n) || n < 1) {
        printf("eglChooseConfig failed\n");
        return 1;
    }
    EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pbAttribs);
    EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (s == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(d, s, s, ctx)) {
        printf("eglMakeCurrent failed\n");
        return 1;
    }
    printf("RENDERER=%s | VERSION=%s\n", glGetString(GL_RENDERER),
           glGetString(GL_VERSION));
    return 0;
}
