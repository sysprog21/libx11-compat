/*
 * Link + behavior regression for the GLX-over-EGL path in src/glx.c.
 *
 * Verifies the glX* surface links against libX11-compat and behaves coherently
 * in both configurations:
 *   - No EGL provider (e.g. macOS with no vendored libEGL): every entry point
 *     degrades to a safe NULL/False and never crashes.
 *   - EGL provider present (e.g. Linux with Mesa): the classic path resolves a
 *     visual and a context without faulting.
 *
 * One invariant holds regardless: the generic XQueryExtension("GLX") must
 * report GLX absent. glXQueryExtension is the provider-aware probe; the generic
 * name stays False by design (src/extension.c) so real toolkits are not steered
 * into a GLX wire-protocol path. Runs under SDL_VIDEODRIVER=dummy so it works
 * headless.
 */
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "test-glx-link FAIL: %s (%s:%d)\n", msg, __FILE__, \
                    __LINE__);                                                 \
            exit(1);                                                           \
        }                                                                      \
    } while (0)

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    CHECK(dpy != NULL, "XOpenDisplay");
    int screen = DefaultScreen(dpy);

    /* No context is current before any glXMakeCurrent. */
    CHECK(glXGetCurrentContext() == NULL, "no current context at startup");
    CHECK(glXGetCurrentDrawable() == None, "no current drawable at startup");

    /* NULL-tolerant accessors must never crash. */
    CHECK(glXGetProcAddress(NULL) == NULL, "glXGetProcAddress(NULL) is NULL");

    int errorBase = -1, eventBase = -1;
    Bool haveGlx = glXQueryExtension(dpy, &errorBase, &eventBase);

    /* Invariant: the core X extension probe must not claim GLX yet, whether or
     * not an EGL provider is loaded. This is the safe-probe default that keeps
     * a client that only queries the extension from tripping over an unbacked
     * path.
     */
    int op = -1, ev = -1, err = -1;
    CHECK(!XQueryExtension(dpy, "GLX", &op, &ev, &err),
          "XQueryExtension still reports GLX absent");

    int attribs[] = {GLX_RGBA, GLX_RED_SIZE,     8,   GLX_GREEN_SIZE,
                     8,        GLX_BLUE_SIZE,    8,   GLX_DEPTH_SIZE,
                     24,       GLX_DOUBLEBUFFER, None};

    /* Mode-agnostic robustness: these must be safe whether or not a provider is
     * loaded. A list that stops after a zero-valued token gets an extra guard
     * terminator; GLX also allows zero as a real value before later tokens. A
     * release-current call and a proc lookup must never fault.
     */
    int zeroOnly[] = {GLX_RED_SIZE, None, None};
    (void) glXChooseVisual(dpy, screen, zeroOnly);
    (void) glXMakeCurrent(dpy, None, NULL);
    glXUseXFont(None, 0, 0, 0);
    (void) glXGetProcAddress((const unsigned char *) "glClear");
    CHECK(glXGetProcAddressARB(NULL) == NULL, "glXGetProcAddressARB(NULL)");

    /* The GLX string queries must never return NULL, for any name. A real
     * client (glxinfo) strlen's the result without a NULL check, so NULL is a
     * crash. The unknown-name case (0x1234) is the one that used to return
     * NULL. This holds regardless of whether a provider is loaded
     * (client-library metadata).
     */
    CHECK(glXQueryExtensionsString(dpy, screen),
          "glXQueryExtensionsString !NULL");
    CHECK(glXGetClientString(dpy, GLX_VENDOR),
          "glXGetClientString VENDOR !NULL");
    CHECK(glXGetClientString(dpy, GLX_VERSION),
          "glXGetClientString VERSION !NULL");
    CHECK(glXGetClientString(dpy, GLX_EXTENSIONS),
          "glXGetClientString EXT !NULL");
    CHECK(glXGetClientString(dpy, 0x1234),
          "glXGetClientString unknown-name !NULL");
    CHECK(glXQueryServerString(dpy, screen, 0x1234),
          "glXQueryServerString unknown-name !NULL");

    /* glX* client entry points resolve through glXGetProcAddress regardless of
     * a provider (matching real GLX, which resolves its own client functions).
     * The canonical case is glXCreateContextAttribsARB, which apps always
     * resolve dynamically. An unknown glX name still returns NULL.
     */
    CHECK(glXGetProcAddress(
              (const unsigned char *) "glXCreateContextAttribsARB") != NULL,
          "glXGetProcAddress resolves a glX* entry point without a provider");
    CHECK(glXGetProcAddress((const unsigned char *) "glXUseXFont") != NULL,
          "glXGetProcAddress resolves glXUseXFont");
    CHECK(glXGetProcAddress((const unsigned char *) "glXNoSuchEntry") == NULL,
          "glXGetProcAddress returns NULL for an unknown glX name");

    if (!haveGlx) {
        /* Degrade path: no provider. Everything returns a safe empty result. */
        CHECK(glXChooseVisual(dpy, screen, attribs) == NULL,
              "glXChooseVisual returns NULL without a provider");
        CHECK(glXCreateContext(dpy, NULL, NULL, True) == NULL,
              "glXCreateContext returns NULL without a provider");
        int nitems = -1;
        CHECK(glXChooseFBConfig(dpy, screen, attribs, &nitems) == NULL,
              "glXChooseFBConfig returns NULL without a provider");
        CHECK(nitems == 0,
              "glXChooseFBConfig zeroes nitems on the degrade path");
        CHECK(glXQueryVersion(dpy, NULL, NULL) == False,
              "glXQueryVersion False without a provider");
        printf("test-glx-link: ok (no EGL provider; degrade path)\n");
    } else {
        /* Provider present: the classic path should be self-consistent. */
        int major = 0, minor = 0;
        CHECK(glXQueryVersion(dpy, &major, &minor),
              "glXQueryVersion True with a provider");
        CHECK(major >= 1, "GLX major version >= 1");

        XVisualInfo *vis = glXChooseVisual(dpy, screen, attribs);
        if (vis) {
            GLXContext ctx = glXCreateContext(dpy, vis, NULL, True);
            /* Context creation may still fail on a headless provider without a
             * usable config; only assert directness/cleanup when it succeeds.
             */
            if (ctx) {
                CHECK(glXIsDirect(dpy, ctx), "glXIsDirect reports direct");
                glXDestroyContext(dpy, ctx);
            }
            XFree(vis);
        }
        printf("test-glx-link: ok (EGL provider present)\n");
    }

    XCloseDisplay(dpy);
    return 0;
}
