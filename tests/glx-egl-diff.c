/*
 * Approach-A GLX differential harness.
 *
 * Renders one fixed, deterministic GLES2 scene two ways and writes the readback
 * (WxH RGBA) to stdout:
 *   glx: through our GLX layer (glXChooseFBConfig -> glXCreateNewContext ->
 *        glXCreatePbuffer -> glXMakeCurrent), which translates to EGL and the
 *        host provider.
 *   egl: through direct EGL on the SAME provider (eglChooseConfig ->
 *        eglCreatePbufferSurface -> eglCreateContext -> eglMakeCurrent).
 *
 * Both bottom out in the same driver (ANGLE on macOS, Mesa on Linux), so the
 * only variable between the two dumps is our GLX->EGL translation. The caller
 * diffs the two dumps; a real translation bug shows up as a pixel difference.
 * The scene has no animation or time dependence, so the two runs are
 * comparable.
 *
 * Usage: glx-egl-diff glx|egl > out.rgba
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GL/glx.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 128
#define H 128

static const char *VS =
    "attribute vec2 pos;\n"
    "attribute vec3 col;\n"
    "varying vec3 vcol;\n"
    "void main() { vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *FS =
    "precision mediump float;\n"
    "varying vec3 vcol;\n"
    "void main() { gl_FragColor = vec4(vcol, 1.0); }\n";

static GLuint compileShader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    return sh;
}

static int shaderOk(GLuint sh)
{
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    return ok == GL_TRUE;
}

/* Draw a fixed gradient triangle over a solid clear, then read it back. No time
 * or randomness, so the glx and egl runs must produce the same pixels.
 *
 * Returns 0 on success. Fails on any shader/link/GL error and if the triangle
 * left no mark (a silent failure would otherwise make both sides match a blank
 * clear color and pass vacuously).
 */
static int renderScene(unsigned char *out)
{
    glViewport(0, 0, W, H);
    glClearColor(0.15f, 0.25f, 0.35f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    GLuint vs = compileShader(GL_VERTEX_SHADER, VS);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, FS);
    if (!shaderOk(vs) || !shaderOk(fs))
        return 10;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "pos");
    glBindAttribLocation(prog, 1, "col");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
        return 11;
    glUseProgram(prog);

    static const float verts[] = {
        -0.8f, -0.8f, 1.0f, 0.0f, 0.0f, 0.8f, -0.8f, 0.0f,
        1.0f,  0.0f,  0.0f, 0.8f, 0.0f, 0.0f, 1.0f,
    };
    GLuint vbo;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *) 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                          (const void *) (2 * sizeof(float)));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, out);
    if (glGetError() != GL_NO_ERROR)
        return 12;

    /* The gradient triangle drives one channel near 255 near each vertex; if no
     * pixel is that saturated the draw produced nothing and this is a blank
     * clear.
     */
    for (int i = 0; i < W * H; i++) {
        if (out[i * 4] > 200 || out[i * 4 + 1] > 200 || out[i * 4 + 2] > 200)
            return 0;
    }
    return 13;
}

static int runGlx(unsigned char *out)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    int screen = DefaultScreen(dpy);
    /* Aligned with the direct-EGL config below: RGBA8, pbuffer-capable, RGBA
     * render type. The scene uses no depth or double buffering, so requesting
     * neither keeps both paths asking the driver for the same pixel format.
     */
    int cfgAttribs[] = {GLX_RED_SIZE,
                        8,
                        GLX_GREEN_SIZE,
                        8,
                        GLX_BLUE_SIZE,
                        8,
                        GLX_ALPHA_SIZE,
                        8,
                        GLX_DRAWABLE_TYPE,
                        GLX_PBUFFER_BIT,
                        GLX_RENDER_TYPE,
                        GLX_RGBA_BIT,
                        None};
    int n = 0;
    GLXFBConfig *fb = glXChooseFBConfig(dpy, screen, cfgAttribs, &n);
    if (!fb || n < 1)
        return 2;
    /* Request an ES2 context explicitly so this matches the direct-EGL side
     * (EGL_OPENGL_ES2_BIT) on a provider that also offers desktop GL (Mesa); a
     * plain glXCreateNewContext would default to desktop GL there and the two
     * paths would no longer be comparing the same API.
     */
    int ctxAttribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
                        GLX_CONTEXT_PROFILE_MASK_ARB,
                        GLX_CONTEXT_ES2_PROFILE_BIT_EXT, None};
    GLXContext ctx =
        glXCreateContextAttribsARB(dpy, fb[0], NULL, True, ctxAttribs);
    int pbAttribs[] = {GLX_PBUFFER_WIDTH, W, GLX_PBUFFER_HEIGHT, H, None};
    GLXPbuffer pb = ctx ? glXCreatePbuffer(dpy, fb[0], pbAttribs) : None;
    int rc = 3;
    if (ctx && pb != None && glXMakeCurrent(dpy, pb, ctx))
        rc = renderScene(out) ? 6 : 0;
    if (pb != None)
        glXDestroyPbuffer(dpy, pb);
    if (ctx) {
        glXMakeCurrent(dpy, None, NULL);
        glXDestroyContext(dpy, ctx);
    }
    XFree(fb);
    XCloseDisplay(dpy);
    return rc;
}

static int runEgl(unsigned char *out)
{
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (d == EGL_NO_DISPLAY)
        return 1;
    if (!eglInitialize(d, NULL, NULL))
        return 2;
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfgAttribs[] = {EGL_RED_SIZE,
                           8,
                           EGL_GREEN_SIZE,
                           8,
                           EGL_BLUE_SIZE,
                           8,
                           EGL_ALPHA_SIZE,
                           8,
                           EGL_SURFACE_TYPE,
                           EGL_PBUFFER_BIT,
                           EGL_RENDERABLE_TYPE,
                           EGL_OPENGL_ES2_BIT,
                           EGL_NONE};
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, cfgAttribs, &cfg, 1, &n) || n < 1)
        return 3;
    EGLint pbAttribs[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pbAttribs);
    if (s == EGL_NO_SURFACE)
        return 4;
    EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT)
        return 5;
    if (!eglMakeCurrent(d, s, s, ctx))
        return 6;
    int rc = renderScene(out) ? 7 : 0;
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(d, s);
    eglDestroyContext(d, ctx);
    eglTerminate(d);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 2 ||
        (strcmp(argv[1], "glx") != 0 && strcmp(argv[1], "egl") != 0)) {
        fprintf(stderr, "usage: %s glx|egl > out.rgba\n", argv[0]);
        return 2;
    }
    unsigned char *buf = calloc((size_t) W * H, 4);
    if (!buf)
        return 2;
    int rc = strcmp(argv[1], "glx") == 0 ? runGlx(buf) : runEgl(buf);
    if (rc) {
        fprintf(stderr, "glx-egl-diff: %s backend failed (rc=%d)\n", argv[1],
                rc);
        free(buf);
        return 1;
    }
    fwrite(buf, 1, (size_t) W * H * 4, stdout);
    free(buf);
    return 0;
}
