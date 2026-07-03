/*
 * Regression for the deferred EGL initialize path: if a provider loads but
 * eglInitialize fails, GLX must stop advertising itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

#ifndef FAKE_EGL_PATH
#define FAKE_EGL_PATH "build/tests/libEGL-fake.so"
#endif

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fprintf(stderr, "test-glx-init-fail FAIL: %s (%s:%d)\n", msg, \
                    __FILE__, __LINE__);                                  \
            exit(1);                                                      \
        }                                                                 \
    } while (0)

int main(void)
{
    setenv("LIBX11_COMPAT_EGL", FAKE_EGL_PATH, 1);
    setenv("FAKE_EGL_FAIL_INITIALIZE", "1", 1);

    Display *dpy = XOpenDisplay(NULL);
    CHECK(dpy != NULL, "XOpenDisplay");
    int screen = DefaultScreen(dpy);

    int attribs[] = {GLX_RGBA, GLX_RED_SIZE, 8, None};
    XVisualInfo *vis = glXChooseVisual(dpy, screen, attribs);
    CHECK(vis != NULL, "glXChooseVisual works before deferred init");
    GLXContext ctx = glXCreateContext(dpy, vis, NULL, True);
    CHECK(ctx != NULL, "lazy glXCreateContext works before deferred init");

    Window win =
        XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, 16, 16, 0,
                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    CHECK(win != None, "XCreateSimpleWindow");
    XMapWindow(dpy, win);
    XSync(dpy, False);

    CHECK(!glXMakeCurrent(dpy, win, ctx),
          "glXMakeCurrent fails on init failure");
    CHECK(!glXQueryExtension(dpy, NULL, NULL),
          "glXQueryExtension turns false after init failure");
    CHECK(!glXQueryVersion(dpy, NULL, NULL),
          "glXQueryVersion turns false after init failure");
    CHECK(glXChooseVisual(dpy, screen, attribs) == NULL,
          "glXChooseVisual turns NULL after init failure");

    glXDestroyContext(dpy, ctx);
    XFree(vis);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    puts("test-glx-init-fail: ok");
    return 0;
}
