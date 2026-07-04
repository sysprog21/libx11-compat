/*
 * Approach-A GLX differential harness.
 *
 * Renders a set of fixed, deterministic GLES2 cases two ways and writes the
 * concatenated readbacks (NUM_CASES * WxH RGBA) to stdout. The "glx" backend
 * runs through our GLX layer (glXChooseFBConfig, glXCreateContextAttribsARB,
 * glXCreatePbuffer, glXMakeCurrent), which translates to EGL and the host
 * provider. The "egl" backend runs through direct EGL on the SAME provider
 * (eglChooseConfig, eglCreatePbufferSurface, eglCreateContext, eglMakeCurrent).
 *
 * Both bottom out in the same driver (ANGLE on macOS, Mesa on Linux), so the
 * only variable between the two dumps is our GLX->EGL translation. The caller
 * diffs the two dumps byte for byte; a real translation bug shows up as a pixel
 * difference. The scenes have no animation or time dependence, so the two runs
 * are comparable.
 *
 * Cases (concatenated in this order):
 *   0 "single"  one context, one pbuffer, one gradient triangle. Exercises the
 *               plain choose/create/makecurrent/readback path.
 *   1 "switch"  two non-shared contexts each with its own pbuffer, made
 *               current in an interleaved order (B, then A) before A is read
 *               back. Exercises the per-context/per-surface registry and the
 *               makeCurrent rebind the offscreen-composite path leans on. A
 *               layer that confused the two drawables or contexts would read
 *               B's frame back from A and diverge from direct EGL, which does
 *               not.
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
#define NUM_CASES 2
#define CASE_BYTES ((size_t) W * H * 4)

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

/* Read back the current framebuffer and reject a blank clear. */
static int readScene(unsigned char *out)
{
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

/* Draw a fixed gradient triangle over a solid clear, then optionally read it
 * back. The seed shifts the clear color and rotates the vertex colors so two
 * contexts draw visibly different frames; with no time or randomness the glx
 * and egl runs still produce identical pixels for the same seed.
 *
 * Returns 0 on success. Fails on any shader/link/GL error and if a requested
 * readback shows the triangle left no mark (a silent failure would otherwise
 * make both sides match a blank clear color and pass vacuously).
 */
static int renderScene(unsigned char *out, int seed)
{
    glViewport(0, 0, W, H);
    if (seed == 0)
        glClearColor(0.15f, 0.25f, 0.35f, 1.0f);
    else
        glClearColor(0.35f, 0.15f, 0.25f, 1.0f);
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

    /* Per-vertex colors, rotated by seed so context B's frame differs from A's.
     * Positions are fixed; only the color channels rotate.
     */
    float c[3][3] = {
        {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    int r = seed % 3;
    const float pos[3][2] = {{-0.8f, -0.8f}, {0.8f, -0.8f}, {0.0f, 0.8f}};
    float verts[3 * 5];
    for (int v = 0; v < 3; v++) {
        verts[v * 5 + 0] = pos[v][0];
        verts[v * 5 + 1] = pos[v][1];
        verts[v * 5 + 2] = c[(v + r) % 3][0];
        verts[v * 5 + 3] = c[(v + r) % 3][1];
        verts[v * 5 + 4] = c[(v + r) % 3][2];
    }
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
    if (glGetError() != GL_NO_ERROR)
        return 12;

    return out ? readScene(out) : 0;
}

/* ---- GLX backend -------------------------------------------------------- */

/* Create an ES2 context and a WxH pbuffer for the chosen FBConfig. Returns 0
 * and fills ctxOut and pbOut on success. Kept separate from make-current so the
 * "switch" case can hold two live pairs at once.
 */
static int glxMakePair(Display *dpy,
                       GLXFBConfig fb,
                       GLXContext *ctxOut,
                       GLXPbuffer *pbOut)
{
    /* ES2 context explicitly so this matches the direct-EGL side
     * (EGL_OPENGL_ES2_BIT) on a provider that also offers desktop GL (Mesa); a
     * plain glXCreateNewContext would default to desktop GL there and the two
     * paths would no longer compare the same API.
     */
    int ctxAttribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
                        GLX_CONTEXT_PROFILE_MASK_ARB,
                        GLX_CONTEXT_ES2_PROFILE_BIT_EXT, None};
    GLXContext ctx =
        glXCreateContextAttribsARB(dpy, fb, NULL, True, ctxAttribs);
    if (!ctx)
        return 4;
    int pbAttribs[] = {GLX_PBUFFER_WIDTH, W, GLX_PBUFFER_HEIGHT, H, None};
    GLXPbuffer pb = glXCreatePbuffer(dpy, fb, pbAttribs);
    if (pb == None) {
        glXDestroyContext(dpy, ctx);
        return 5;
    }
    *ctxOut = ctx;
    *pbOut = pb;
    return 0;
}

static int runGlx(unsigned char *out)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy)
        return 1;
    int screen = DefaultScreen(dpy);
    /* Aligned with the direct-EGL config below: RGBA8, pbuffer-capable, RGBA
     * render type. The scenes use no depth or double buffering, so requesting
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
    if (!fb || n < 1) {
        XCloseDisplay(dpy);
        return 2;
    }

    int rc = 3;
    GLXContext a = NULL, b = NULL;
    GLXPbuffer pa = None, pb = None;

    /* Case 0 "single". */
    if (glxMakePair(dpy, fb[0], &a, &pa) != 0)
        goto out;
    if (!glXMakeCurrent(dpy, pa, a)) {
        rc = 6;
        goto out;
    }
    if (renderScene(out, 0)) {
        rc = 7;
        goto out;
    }

    /* Case 1 "switch": bring up a second pair, draw B first, rebind A, read A.
     * A stays the same context/pbuffer as case 0, but now with a live sibling
     * and an intervening make-current, so a registry or rebind bug shows up as
     * A reading back B's frame.
     */
    if (glxMakePair(dpy, fb[0], &b, &pb) != 0) {
        rc = 8;
        goto out;
    }
    if (!glXMakeCurrent(dpy, pb, b) || renderScene(NULL, 1)) {
        rc = 9;
        goto out;
    }
    if (!glXMakeCurrent(dpy, pa, a) || readScene(out + CASE_BYTES)) {
        rc = 14;
        goto out;
    }
    rc = 0;

out:
    glXMakeCurrent(dpy, None, NULL);
    if (pb != None)
        glXDestroyPbuffer(dpy, pb);
    if (b)
        glXDestroyContext(dpy, b);
    if (pa != None)
        glXDestroyPbuffer(dpy, pa);
    if (a)
        glXDestroyContext(dpy, a);
    XFree(fb);
    XCloseDisplay(dpy);
    return rc;
}

/* ---- direct-EGL reference backend --------------------------------------- */

static int eglMakePair(EGLDisplay d,
                       EGLConfig cfg,
                       EGLContext *ctxOut,
                       EGLSurface *surfOut)
{
    EGLint pbAttribs[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pbAttribs);
    if (s == EGL_NO_SURFACE)
        return 4;
    EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctxAttribs);
    if (ctx == EGL_NO_CONTEXT) {
        eglDestroySurface(d, s);
        return 5;
    }
    *ctxOut = ctx;
    *surfOut = s;
    return 0;
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
    if (!eglChooseConfig(d, cfgAttribs, &cfg, 1, &n) || n < 1) {
        eglTerminate(d);
        return 3;
    }

    int rc = 3;
    EGLContext a = EGL_NO_CONTEXT, b = EGL_NO_CONTEXT;
    EGLSurface pa = EGL_NO_SURFACE, pb = EGL_NO_SURFACE;

    /* Case 0 "single". */
    if (eglMakePair(d, cfg, &a, &pa) != 0)
        goto out;
    if (!eglMakeCurrent(d, pa, pa, a)) {
        rc = 6;
        goto out;
    }
    if (renderScene(out, 0)) {
        rc = 7;
        goto out;
    }

    /* Case 1 "switch": same interleaving as the GLX side. */
    if (eglMakePair(d, cfg, &b, &pb) != 0) {
        rc = 8;
        goto out;
    }
    if (!eglMakeCurrent(d, pb, pb, b) || renderScene(NULL, 1)) {
        rc = 9;
        goto out;
    }
    if (!eglMakeCurrent(d, pa, pa, a) || readScene(out + CASE_BYTES)) {
        rc = 14;
        goto out;
    }
    rc = 0;

out:
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (pb != EGL_NO_SURFACE)
        eglDestroySurface(d, pb);
    if (b != EGL_NO_CONTEXT)
        eglDestroyContext(d, b);
    if (pa != EGL_NO_SURFACE)
        eglDestroySurface(d, pa);
    if (a != EGL_NO_CONTEXT)
        eglDestroyContext(d, a);
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
    unsigned char *buf = calloc(NUM_CASES, CASE_BYTES);
    if (!buf)
        return 2;
    int rc = strcmp(argv[1], "glx") == 0 ? runGlx(buf) : runEgl(buf);
    if (rc) {
        fprintf(stderr, "glx-egl-diff: %s backend failed (rc=%d)\n", argv[1],
                rc);
        free(buf);
        return 1;
    }
    fwrite(buf, 1, NUM_CASES * CASE_BYTES, stdout);
    free(buf);
    return 0;
}
