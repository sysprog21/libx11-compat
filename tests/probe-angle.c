/*
 * Manual validation: drive the GLX layer against a REAL EGL provider (not the
 * fake). Provider-neutral: it resolves gl* through glXGetProcAddress, so it
 * works against ANGLE on macOS and system Mesa on Linux. Not part of
 * check-unit.
 *
 * macOS (ANGLE-Metal, from make build-angle):
 *   SDL_VIDEODRIVER=dummy LIBX11_COMPAT_EGL=build/angle/libEGL.dylib \
 *     DYLD_LIBRARY_PATH=build:/opt/homebrew/lib build/tests/probe-angle
 * Linux (system Mesa; the EGL X11 platform needs a display, so run under Xvfb):
 *   xvfb-run -a env LIBX11_COMPAT_EGL=/usr/lib/x86_64-linux-gnu/libEGL.so \
 *     LD_LIBRARY_PATH=build build/tests/probe-angle
 */
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

typedef unsigned int GLenum;
typedef unsigned char GLubyte;
#define GL_VERSION 0x1F02
#define GL_RENDERER 0x1F01
#define GL_VENDOR 0x1F00

#define CHECK(c, m)                                       \
    do {                                                  \
        if (!(c)) {                                       \
            fprintf(stderr, "probe-angle FAIL: %s\n", m); \
            return 1;                                     \
        }                                                 \
    } while (0)

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    CHECK(dpy, "XOpenDisplay");
    int screen = DefaultScreen(dpy);

    CHECK(glXQueryExtension(dpy, NULL, NULL),
          "glXQueryExtension True (real provider loaded)");
    int maj = 0, min = 0;
    CHECK(glXQueryVersion(dpy, &maj, &min), "glXQueryVersion");
    printf("probe-angle: GLX %d.%d\n", maj, min);

    int fbAttribs[] = {GLX_RED_SIZE,
                       8,
                       GLX_GREEN_SIZE,
                       8,
                       GLX_BLUE_SIZE,
                       8,
                       GLX_ALPHA_SIZE,
                       8,
                       GLX_DEPTH_SIZE,
                       24,
                       GLX_DOUBLEBUFFER,
                       True,
                       None};
    int nfb = 0;
    GLXFBConfig *fbs = glXChooseFBConfig(dpy, screen, fbAttribs, &nfb);
    CHECK(fbs && nfb >= 1, "glXChooseFBConfig against real provider");

    GLXContext ctx =
        glXCreateNewContext(dpy, fbs[0], GLX_RGBA_TYPE, NULL, True);
    CHECK(ctx, "glXCreateNewContext against real provider");

    int pbAttribs[] = {GLX_PBUFFER_WIDTH, 64, GLX_PBUFFER_HEIGHT, 64, None};
    GLXPbuffer pbuf = glXCreatePbuffer(dpy, fbs[0], pbAttribs);
    CHECK(pbuf != None, "glXCreatePbuffer against real provider");

    CHECK(glXMakeCurrent(dpy, pbuf, ctx),
          "glXMakeCurrent binds a real provider context");

    /* Resolve glGetString through our glXGetProcAddress (which routes gl* to
     * the provider) and read back the real GL strings, proving a live GL
     * context.
     */
    const GLubyte *(*getString)(GLenum) =
        (const GLubyte *(*) (GLenum)) glXGetProcAddress(
            (const unsigned char *) "glGetString");
    CHECK(getString,
          "glXGetProcAddress resolves glGetString from the provider");
    const GLubyte *ver = getString(GL_VERSION);
    const GLubyte *rend = getString(GL_RENDERER);
    const GLubyte *vend = getString(GL_VENDOR);
    printf("probe-angle: GL_VERSION  = %s\n",
           ver ? (const char *) ver : "(null)");
    printf("probe-angle: GL_RENDERER = %s\n",
           rend ? (const char *) rend : "(null)");
    printf("probe-angle: GL_VENDOR   = %s\n",
           vend ? (const char *) vend : "(null)");
    CHECK(ver && ver[0], "glGetString(GL_VERSION) returns a real version");

    /* Print the verdict while the context (and the provider-owned vendor
     * string) is still current; the pointer dangles once the context is
     * destroyed below.
     */
    printf("probe-angle: ok (validated against real provider: %s)\n",
           vend ? (const char *) vend : "unknown provider");

    glXMakeCurrent(dpy, None, NULL);
    glXDestroyPbuffer(dpy, pbuf);
    glXDestroyContext(dpy, ctx);
    XFree(fbs);
    XCloseDisplay(dpy);
    return 0;
}
