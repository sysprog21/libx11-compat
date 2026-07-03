/*
 * glxgears showcase for the GLX-over-EGL/ANGLE pipeline.
 *
 * A faithful port of the classic mesa glxgears (Brian Paul's three-gear scene)
 * to GLES2, driven through this project's GLX layer:
 *
 *   GLX (libX11-compat) -> EGL -> ANGLE -> Metal
 *
 * The original is fixed-function desktop GL (glBegin/glEnd, matrix stack,
 * glLight), which ANGLE (OpenGL ES only) cannot run, so the same 3D lit gears,
 * layout, rotation coupling, perspective view, and FPS report are reproduced
 * with shaders and vertex buffers. It behaves like mesa glxgears: a window with
 * three meshing gears spinning, printing frames-per-second every five seconds.
 *
 * Two run modes (identical rendering):
 *   glxgears           - on-screen window (CAMetalLayer surface), until closed.
 *   glxgears-headless  - offscreen pbuffer, bounded by GLXGEARS_FRAMES, with a
 *                        silent readback sanity check for CI.
 *
 * The gl* calls link directly against ANGLE's libGLESv2; dyld resolves one
 * libGLESv2 image (the make targets pin the app rpath and the libEGL loader's
 * dlopen at the same build/angle/libGLESv2.dylib), so those calls reach the
 * same context glXMakeCurrent set. Against a different ANGLE tree, resolve gl*
 * through glXGetProcAddress instead.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>
#include <GLES2/gl2.h>

#define WIN_W 300
#define WIN_H 300

/* gear geometry: pos + normal triangles, ported from mesa gear() */

typedef struct {
    float px, py, pz, nx, ny, nz;
} Vertex;

typedef struct {
    Vertex *v;
    int n, cap;
} Mesh;

static void meshPush(Mesh *m, float px, float py, float pz, float nx, float ny,
                     float nz)
{
    if (m->n >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 1024;
        Vertex *nv = realloc(m->v, (size_t) m->cap * sizeof(Vertex));
        if (!nv) {
            free(m->v);
            fprintf(stderr, "glxgears: out of memory growing mesh\n");
            exit(1);
        }
        m->v = nv;
    }
    m->v[m->n++] = (Vertex){px, py, pz, nx, ny, nz};
}

/* One quad (v0..v3) as two triangles, all carrying the face normal n. */
static void quad(Mesh *m, const float *v0, const float *v1, const float *v2,
                 const float *v3, float nx, float ny, float nz)
{
    meshPush(m, v0[0], v0[1], v0[2], nx, ny, nz);
    meshPush(m, v1[0], v1[1], v1[2], nx, ny, nz);
    meshPush(m, v2[0], v2[1], v2[2], nx, ny, nz);
    meshPush(m, v0[0], v0[1], v0[2], nx, ny, nz);
    meshPush(m, v2[0], v2[1], v2[2], nx, ny, nz);
    meshPush(m, v3[0], v3[1], v3[2], nx, ny, nz);
}

/* Build a 3D gear centered at the origin in the XY plane, thickness along Z. */
static void buildGear(Mesh *m, float innerR, float outerR, float width,
                      int teeth, float toothDepth)
{
    float r0 = innerR;
    float r1 = outerR - toothDepth / 2.0f;
    float r2 = outerR + toothDepth / 2.0f;
    float da = 2.0f * (float) M_PI / teeth / 4.0f;
    float w = width * 0.5f;

    for (int i = 0; i < teeth; i++) {
        float a = i * 2.0f * (float) M_PI / teeth;
        float c = cosf(a), s = sinf(a);
        float c1 = cosf(a + da), s1 = sinf(a + da);
        float c2 = cosf(a + 2 * da), s2 = sinf(a + 2 * da);
        float c3 = cosf(a + 3 * da), s3 = sinf(a + 3 * da);
        float an = 2.0f * (float) M_PI / teeth;
        float cn = cosf(a + an), sn = sinf(a + an);

        /* Front face (normal +Z): annulus segment + tooth. */
        float f_r0a[3] = {r0 * c, r0 * s, w}, f_r1a[3] = {r1 * c, r1 * s, w};
        float f_r1c[3] = {r1 * c3, r1 * s3, w};
        float f_r0n[3] = {r0 * cn, r0 * sn, w}, f_r1n[3] = {r1 * cn, r1 * sn, w};
        quad(m, f_r1a, f_r1c, f_r0n, f_r0a, 0, 0, 1);
        quad(m, f_r0a, f_r0n, f_r1n, f_r1c, 0, 0, 1);
        /* Front tooth face (+Z). */
        float t0[3] = {r1 * c, r1 * s, w}, t1[3] = {r2 * c1, r2 * s1, w};
        float t2[3] = {r2 * c2, r2 * s2, w}, t3[3] = {r1 * c3, r1 * s3, w};
        quad(m, t0, t1, t2, t3, 0, 0, 1);

        /* Back face (normal -Z). */
        float b_r1a[3] = {r1 * c, r1 * s, -w}, b_r0a[3] = {r0 * c, r0 * s, -w};
        float b_r1c[3] = {r1 * c3, r1 * s3, -w};
        float b_r0n[3] = {r0 * cn, r0 * sn, -w}, b_r1n[3] = {r1 * cn, r1 * sn, -w};
        quad(m, b_r0a, b_r0n, b_r1n, b_r1c, 0, 0, -1);
        quad(m, b_r1a, b_r0a, b_r1c, b_r1n, 0, 0, -1);
        float u0[3] = {r1 * c3, r1 * s3, -w}, u1[3] = {r2 * c2, r2 * s2, -w};
        float u2[3] = {r2 * c1, r2 * s1, -w}, u3[3] = {r1 * c, r1 * s, -w};
        quad(m, u0, u1, u2, u3, 0, 0, -1);

        /* Outward tooth faces (side normals). */
        float uu = r2 * c1 - r1 * c, vv = r2 * s1 - r1 * s;
        float len = sqrtf(uu * uu + vv * vv);
        uu /= len;
        vv /= len;
        float o0[3] = {r1 * c, r1 * s, w}, o0b[3] = {r1 * c, r1 * s, -w};
        float o1[3] = {r2 * c1, r2 * s1, w}, o1b[3] = {r2 * c1, r2 * s1, -w};
        quad(m, o0, o0b, o1b, o1, vv, -uu, 0);
        float o2[3] = {r2 * c2, r2 * s2, w}, o2b[3] = {r2 * c2, r2 * s2, -w};
        quad(m, o1, o1b, o2b, o2, c, s, 0);
        uu = r1 * c3 - r2 * c2;
        vv = r1 * s3 - r2 * s2;
        len = sqrtf(uu * uu + vv * vv);
        uu /= len;
        vv /= len;
        float o3[3] = {r1 * c3, r1 * s3, w}, o3b[3] = {r1 * c3, r1 * s3, -w};
        quad(m, o2, o2b, o3b, o3, vv, -uu, 0);
        float o4[3] = {r1 * cn, r1 * sn, w}, o4b[3] = {r1 * cn, r1 * sn, -w};
        quad(m, o3, o3b, o4b, o4, c, s, 0);

        /* Inside cylinder (inward normal). */
        float in0[3] = {r0 * c, r0 * s, -w}, in1[3] = {r0 * c, r0 * s, w};
        float in2[3] = {r0 * cn, r0 * sn, w}, in3[3] = {r0 * cn, r0 * sn, -w};
        quad(m, in0, in1, in2, in3, -c, -s, 0);
    }
}

/* column-major 4x4 matrix helpers (GL convention) */

static void mIdentity(float *m)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mMul(float *r, const float *a, const float *b)
{
    float t[16];
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            float s = 0.0f;
            for (int k = 0; k < 4; k++)
                s += a[k * 4 + row] * b[col * 4 + k];
            t[col * 4 + row] = s;
        }
    memcpy(r, t, sizeof(t));
}

static void mFrustum(float *m, float l, float r, float b, float t, float n,
                     float f)
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = 2 * n / (r - l);
    m[5] = 2 * n / (t - b);
    m[8] = (r + l) / (r - l);
    m[9] = (t + b) / (t - b);
    m[10] = -(f + n) / (f - n);
    m[11] = -1.0f;
    m[14] = -2 * f * n / (f - n);
}

/* Match the viewport and projection to the real GL drawable size, queried via
 * glXQueryDrawable. On a HiDPI display the on-screen drawable is larger than
 * the window's logical size, so a fixed WIN_W viewport would draw into a
 * corner; this keeps the gears filling the window and correct across a resize.
 * Recomputes only when the size changes.
 *
 * Returns without touching GL if the size is unavailable.
 */
static void updateViewport(Display *dpy, GLXDrawable win, float *proj,
                           unsigned int *lastW, unsigned int *lastH)
{
    unsigned int w = 0, h = 0;
    glXQueryDrawable(dpy, win, GLX_WIDTH, &w);
    glXQueryDrawable(dpy, win, GLX_HEIGHT, &h);
    if (w == 0 || h == 0) {
        /* Size unavailable (e.g. drawable not ready). Keep the current viewport
         * once one is set; only bootstrap to the window's creation size on the
         * very first call so proj is initialized before the render loop.
         */
        if (*lastW != 0)
            return;
        w = WIN_W;
        h = WIN_H;
    }
    if (w == *lastW && h == *lastH)
        return;
    *lastW = w;
    *lastH = h;
    glViewport(0, 0, (int) w, (int) h);
    float aspect = (float) h / (float) w;
    mFrustum(proj, -1.0f, 1.0f, -aspect, aspect, 5.0f, 60.0f);
}

static void mTranslate(float *m, float x, float y, float z)
{
    mIdentity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static void mRotate(float *m, float deg, float x, float y, float z)
{
    float rad = deg * (float) M_PI / 180.0f;
    float c = cosf(rad), s = sinf(rad);
    float len = sqrtf(x * x + y * y + z * z);
    if (len > 0.0f) {
        x /= len;
        y /= len;
        z /= len;
    }
    mIdentity(m);
    m[0] = x * x * (1 - c) + c;
    m[1] = y * x * (1 - c) + z * s;
    m[2] = x * z * (1 - c) - y * s;
    m[4] = x * y * (1 - c) - z * s;
    m[5] = y * y * (1 - c) + c;
    m[6] = y * z * (1 - c) + x * s;
    m[8] = x * z * (1 - c) + y * s;
    m[9] = y * z * (1 - c) - x * s;
    m[10] = z * z * (1 - c) + c;
}

/* Upper-left 3x3 (rotation part) as the normal matrix; no scale is used. */
static void normalMat3(float *n3, const float *m)
{
    n3[0] = m[0];
    n3[1] = m[1];
    n3[2] = m[2];
    n3[3] = m[4];
    n3[4] = m[5];
    n3[5] = m[6];
    n3[6] = m[8];
    n3[7] = m[9];
    n3[8] = m[10];
}

/* shaders: single directional light, like mesa glxgears */

static const char *kVertexShader =
    "attribute vec3 aPos;\n"
    "attribute vec3 aNormal;\n"
    "uniform mat4 uMvp;\n"
    "uniform mat4 uModel;\n"
    "uniform mat3 uNormalMat;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vWorld;\n"
    "void main() {\n"
    "  gl_Position = uMvp * vec4(aPos, 1.0);\n"
    "  vNormal = uNormalMat * aNormal;\n"
    "  vWorld = (uModel * vec4(aPos, 1.0)).xyz;\n"
    "}\n";
static const char *kFragmentShader =
    "precision mediump float;\n"
    "uniform vec4 uColor;\n"
    "varying vec3 vNormal;\n"
    "varying vec3 vWorld;\n"
    "void main() {\n"
    "  vec3 N = normalize(vNormal);\n"
    "  vec3 L = normalize(vec3(5.0, 5.0, 10.0) - vWorld);\n"
    "  float d = max(dot(N, L), 0.0);\n"
    "  gl_FragColor = vec4(uColor.rgb * (0.2 + 0.8 * d), 1.0);\n"
    "}\n";

static GLuint compileShader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "glxgears: shader compile failed: %s\n", log);
        return 0;
    }
    return sh;
}

typedef struct {
    GLuint vbo;
    int count;
    float tx, ty, phase, dir;
    float color[4];
} Gear;

static double nowSeconds(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1.0e6;
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "glxgears: cannot open display\n");
        return 1;
    }
    int screen = DefaultScreen(dpy);

    if (!glXQueryExtension(dpy, NULL, NULL)) {
        fprintf(stderr,
                "glxgears: no GLX provider (set LIBX11_COMPAT_EGL to a libEGL)\n");
        return 77;
    }

    int fbAttribs[] = {GLX_RED_SIZE,     8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
                       GLX_ALPHA_SIZE,   8, GLX_DEPTH_SIZE, 16,
                       GLX_DOUBLEBUFFER, True, None};
    int nfb = 0;
    GLXFBConfig *fbs = glXChooseFBConfig(dpy, screen, fbAttribs, &nfb);
    if (!fbs || nfb < 1) {
        fprintf(stderr, "glxgears: glXChooseFBConfig failed\n");
        return 1;
    }

    int ctxAttribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 2,
                        GLX_CONTEXT_MINOR_VERSION_ARB, 0,
                        GLX_CONTEXT_PROFILE_MASK_ARB,
                        GLX_CONTEXT_ES2_PROFILE_BIT_EXT, None};
    GLXContext ctx =
        glXCreateContextAttribsARB(dpy, fbs[0], NULL, True, ctxAttribs);
    if (!ctx)
        ctx = glXCreateNewContext(dpy, fbs[0], GLX_RGBA_TYPE, NULL, True);
    if (!ctx) {
        fprintf(stderr, "glxgears: context creation failed\n");
        return 1;
    }

    unsigned long black = BlackPixel(dpy, screen);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, WIN_W,
                                     WIN_H, 0, black, black);
    XStoreName(dpy, win, "glxgears");
    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);
    XSelectInput(dpy, win, StructureNotifyMask | KeyPressMask);
    XMapWindow(dpy, win);
    XSync(dpy, False);
    if (!glXMakeCurrent(dpy, win, ctx)) {
        fprintf(stderr, "glxgears: glXMakeCurrent failed\n");
        return 1;
    }

    /* Report the GL implementation, like mesa glxgears, so the active renderer
     * is visible every run (for example the ANGLE Metal backend on the GPU).
     */
    const GLubyte *rend = glGetString(GL_RENDERER);
    const GLubyte *vers = glGetString(GL_VERSION);
    const GLubyte *vend = glGetString(GL_VENDOR);
    printf("GL_RENDERER   = %s\n", rend ? (const char *) rend : "(null)");
    printf("GL_VERSION    = %s\n", vers ? (const char *) vers : "(null)");
    printf("GL_VENDOR     = %s\n", vend ? (const char *) vend : "(null)");

    /* Benchmark mode: turn vsync off so the render loop is not display-locked;
     * together with skipping the frame sleep below this reveals real GPU
     * throughput instead of the ~60 FPS display cap.
     */
    int benchmark = getenv("GLXGEARS_BENCHMARK") != NULL;
    if (benchmark) {
        void (*swapInterval)(Display *, GLXDrawable, int) =
            (void (*)(Display *, GLXDrawable, int)) glXGetProcAddress(
                (const unsigned char *) "glXSwapIntervalEXT");
        if (swapInterval)
            swapInterval(dpy, win, 0);
        printf("glxgears: benchmark mode (vsync off, frame sleep disabled)\n");
    }

    GLuint vs = compileShader(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs)
        return 1;
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "aPos");
    glBindAttribLocation(prog, 1, "aNormal");
    glLinkProgram(prog);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "glxgears: program link failed: %s\n", log);
        return 1;
    }
    glUseProgram(prog);
    GLint uMvp = glGetUniformLocation(prog, "uMvp");
    GLint uModel = glGetUniformLocation(prog, "uModel");
    GLint uNormalMat = glGetUniformLocation(prog, "uNormalMat");
    GLint uColor = glGetUniformLocation(prog, "uColor");

    /* The three classic gears: geometry, position, rotation phase, direction.
     */
    Gear gears[3] = {
        {0, 0, -3.0f, -2.0f, 0.0f, 1.0f, {0.8f, 0.1f, 0.0f, 1.0f}},
        {0, 0, 3.1f, -2.0f, -9.0f, -2.0f, {0.0f, 0.8f, 0.2f, 1.0f}},
        {0, 0, -3.1f, 4.2f, -25.0f, -2.0f, {0.2f, 0.2f, 1.0f, 1.0f}},
    };
    struct {
        float inner, outer, width, depth;
        int teeth;
    } spec[3] = {
        {1.0f, 4.0f, 1.0f, 0.7f, 20},
        {0.5f, 2.0f, 2.0f, 0.7f, 10},
        {1.3f, 2.0f, 0.5f, 0.7f, 10},
    };
    for (int g = 0; g < 3; g++) {
        Mesh m = {0, 0, 0};
        buildGear(&m, spec[g].inner, spec[g].outer, spec[g].width, spec[g].teeth,
                  spec[g].depth);
        gears[g].count = m.n;
        glGenBuffers(1, &gears[g].vbo);
        glBindBuffer(GL_ARRAY_BUFFER, gears[g].vbo);
        glBufferData(GL_ARRAY_BUFFER, (long) m.n * sizeof(Vertex), m.v,
                     GL_STATIC_DRAW);
        free(m.v);
    }

    glEnable(GL_DEPTH_TEST);

    float proj[16], view[16], tmp[16];
    /* Viewport and projection follow the real GL drawable size (physical pixels
     * on HiDPI), refreshed each frame so a resize is handled like mesa
     * glxgears.
     */
    unsigned int vpW = 0, vpH = 0;
    updateViewport(dpy, win, proj, &vpW, &vpH);
    /* View: back off 40 units, tilt 20 deg X and 30 deg Y like mesa glxgears.
     */
    float vT[16], vRx[16], vRy[16];
    mTranslate(vT, 0.0f, 0.0f, -40.0f);
    mRotate(vRx, 20.0f, 1.0f, 0.0f, 0.0f);
    mRotate(vRy, 30.0f, 0.0f, 1.0f, 0.0f);
    mMul(tmp, vT, vRx);
    mMul(view, tmp, vRy);

    /* GLXGEARS_FRAMES caps the run (headless CI); unset means run until closed.
     */
    int maxFrames = 0;
    const char *fenv = getenv("GLXGEARS_FRAMES");
    if (fenv && atoi(fenv) > 0)
        maxFrames = atoi(fenv);
    int capFrame = maxFrames > 5 ? 5 : (maxFrames ? maxFrames - 1 : 0);

    size_t frameBytes = (size_t) WIN_W * WIN_H * 4;
    unsigned char *frameA = malloc(frameBytes), *frameB = malloc(frameBytes);
    if (!frameA || !frameB)
        return 1;

    double t0 = nowSeconds(), fpsT0 = t0;
    int fpsFrames = 0, running = 1;
    float angle = 0.0f;
    for (int frame = 0; running && (!maxFrames || frame < maxFrames); frame++) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == ClientMessage &&
                (Atom) ev.xclient.data.l[0] == wmDelete)
                running = 0;
            else if (ev.type == KeyPress)
                running = 0;
            else if (ev.type == ConfigureNotify)
                /* Re-match the viewport only on an actual resize, not every
                 * frame: glXQueryDrawable is not free at high frame rates.
                 */
                updateViewport(dpy, win, proj, &vpW, &vpH);
        }

        double t = nowSeconds();
        angle = 70.0f * (float) (t - t0); /* 70 deg/sec, like mesa glxgears */

        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int g = 0; g < 3; g++) {
            float mT[16], mR[16], model[16], mvp[16], n3[9];
            mTranslate(mT, gears[g].tx, gears[g].ty, 0.0f);
            mRotate(mR, angle * gears[g].dir + gears[g].phase, 0.0f, 0.0f, 1.0f);
            mMul(tmp, mT, mR);
            mMul(model, view, tmp);
            mMul(mvp, proj, model);
            normalMat3(n3, model);
            glUniformMatrix4fv(uMvp, 1, GL_FALSE, mvp);
            glUniformMatrix4fv(uModel, 1, GL_FALSE, model);
            glUniformMatrix3fv(uNormalMat, 1, GL_FALSE, n3);
            glUniform4fv(uColor, 1, gears[g].color);
            glBindBuffer(GL_ARRAY_BUFFER, gears[g].vbo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                  (const void *) 0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                                  (const void *) (3 * sizeof(float)));
            glDrawArrays(GL_TRIANGLES, 0, gears[g].count);
        }

        if (maxFrames && (frame == 0 || frame == capFrame))
            glReadPixels(0, 0, WIN_W, WIN_H, GL_RGBA, GL_UNSIGNED_BYTE,
                         frame == 0 ? frameA : frameB);

        glXSwapBuffers(dpy, win);
        if (!maxFrames && !benchmark)
            usleep(16000);

        /* FPS report every 5 seconds, like mesa glxgears. */
        fpsFrames++;
        if (t - fpsT0 >= 5.0) {
            double secs = t - fpsT0;
            printf("%d frames in %.1f seconds = %.3f FPS\n", fpsFrames, secs,
                   fpsFrames / secs);
            fflush(stdout);
            fpsT0 = t;
            fpsFrames = 0;
        }
    }

    int ok = 1;
    if (maxFrames) {
        /* Headless CI: silently confirm the pipeline drew and animated. */
        long nonBg = 0, changed = 0, total = (long) WIN_W * WIN_H;
        for (long i = 0; i < total * 4; i += 4) {
            if (frameA[i] || frameA[i + 1] || frameA[i + 2])
                nonBg++;
            if (frameA[i] != frameB[i] || frameA[i + 1] != frameB[i + 1] ||
                frameA[i + 2] != frameB[i + 2])
                changed++;
        }
        ok = nonBg > total / 20 && changed > total / 200;
        if (!ok)
            fprintf(stderr, "glxgears: FAIL (blank or static: %ld lit, %ld moved)\n",
                    nonBg, changed);
    }

    free(frameA);
    free(frameB);
    for (int g = 0; g < 3; g++)
        glDeleteBuffers(1, &gears[g].vbo);
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyContext(dpy, ctx);
    XDestroyWindow(dpy, win);
    XFree(fbs);
    XCloseDisplay(dpy);
    return ok ? 0 : 1;
}
