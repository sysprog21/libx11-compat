/*
 * Exercises the GLX->EGL translation in src/glx.c against a fake EGL provider
 * (tests/fake-egl.c), so the classic + FBConfig paths, context/surface
 * bookkeeping, and attribute mapping are validated without ANGLE or Mesa.
 *
 * The provider is selected by pointing LIBX11_COMPAT_EGL at the fake object
 * BEFORE the first GLX call (the loader resolves libEGL once, lazily). This is
 * the complement to tests/test-glx-link.c, which covers the no-provider degrade
 * path; the two run in separate processes because the EGL load is a one-shot.
 *
 * FAKE_EGL_PATH is provided by the build rule. Runs under
 * SDL_VIDEODRIVER=dummy.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/glx.h>

#ifndef FAKE_EGL_PATH
#define FAKE_EGL_PATH "build/tests/libEGL-fake.so"
#endif

#define CHECK(cond, msg)                                                 \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "test-glx-provider FAIL: %s (%s:%d)\n", msg, \
                    __FILE__, __LINE__);                                 \
            exit(1);                                                     \
        }                                                                \
    } while (0)

int main(void)
{
    /* Select the fake provider before any GLX call triggers the one-shot load.
     */
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("LIBX11_COMPAT_EGL", FAKE_EGL_PATH, 1);

    Display *dpy = XOpenDisplay(NULL);
    CHECK(dpy != NULL, "XOpenDisplay");
    int screen = DefaultScreen(dpy);

    /* Open the same fake object to read its introspection counters (it is
     * loaded RTLD_LOCAL by the layer, so we dlopen the identical path to share
     * its state). Missing dladdr/dlsym is a test-setup failure, not a pass.
     */
    void *fake = dlopen(FAKE_EGL_PATH, RTLD_NOW | RTLD_LOCAL);
    CHECK(fake != NULL, "dlopen fake EGL for introspection");
    int (*liveContexts)(void) = dlsym(fake, "fake_egl_live_contexts");
    int (*liveSurfaces)(void) = dlsym(fake, "fake_egl_live_surfaces");
    long (*swapCount)(void) = dlsym(fake, "fake_egl_swap_count");
    long (*surfacesCreated)(void) = dlsym(fake, "fake_egl_surfaces_created");
    long (*releaseCount)(void) = dlsym(fake, "fake_egl_release_count");
    int (*currentDrawEqualsRead)(void) =
        dlsym(fake, "fake_egl_current_draw_equals_read");
    int (*hasCurrentRead)(void) = dlsym(fake, "fake_egl_has_current_read");
    void (*lastChannels)(int *, int *, int *, int *, int *, int *) =
        dlsym(fake, "fake_egl_last_config_channels");
    int (*lastShareNonnull)(void) = dlsym(fake, "fake_egl_last_share_nonnull");
    int (*lastClientVersion)(void) =
        dlsym(fake, "fake_egl_last_client_version");
    CHECK(liveContexts && liveSurfaces && swapCount && surfacesCreated &&
              releaseCount && currentDrawEqualsRead && hasCurrentRead &&
              lastChannels && lastShareNonnull && lastClientVersion,
          "fake introspection symbols resolve");

    /* With a provider present the GLX-specific probe reports GLX available. The
     * generic XQueryExtension("GLX") stays False on purpose (see
     * src/extension.c) so real toolkits are not steered into a GLX-protocol
     * path.
     */
    CHECK(glXQueryExtension(dpy, NULL, NULL),
          "glXQueryExtension True with provider");
    int glxOp = 0, glxEv = 0, glxErr = 0;
    CHECK(!XQueryExtension(dpy, "GLX", &glxOp, &glxEv, &glxErr),
          "XQueryExtension keeps GLX absent even with provider");
    int major = 0, minor = 0;
    CHECK(glXQueryVersion(dpy, &major, &minor), "glXQueryVersion");
    CHECK(major == 1 && minor == 4, "GLX version is exactly 1.4");

    /* Classic path with DISTINCT per-channel sizes so a swapped channel token
     * in the GLX->EGL translation is caught (equal sizes would hide it).
     */
    int attribs[] = {GLX_RGBA, GLX_RED_SIZE,     8,   GLX_GREEN_SIZE,
                     6,        GLX_BLUE_SIZE,    5,   GLX_ALPHA_SIZE,
                     4,        GLX_DEPTH_SIZE,   24,  GLX_STENCIL_SIZE,
                     8,        GLX_DOUBLEBUFFER, None};
    XVisualInfo *vis = glXChooseVisual(dpy, screen, attribs);
    CHECK(vis != NULL, "glXChooseVisual returns a visual");

    int shallowAttribs[] = {GLX_RGBA, GLX_RED_SIZE, 1, None};
    XVisualInfo *vis2 = glXChooseVisual(dpy, screen, shallowAttribs);
    CHECK(vis2 != NULL && vis2->visualid != vis->visualid,
          "glXChooseVisual returns separately tracked visuals");
    int preservedRed = 0;
    CHECK(glXGetConfig(dpy, vis, GLX_RED_SIZE, &preservedRed) == 0 &&
              preservedRed == 8,
          "a later glXChooseVisual does not overwrite the earlier visual");
    XFree(vis2);

    struct {
        int attrib, want;
        const char *label;
    } expect[] = {
        {GLX_RED_SIZE, 8, "red"},      {GLX_GREEN_SIZE, 6, "green"},
        {GLX_BLUE_SIZE, 5, "blue"},    {GLX_ALPHA_SIZE, 4, "alpha"},
        {GLX_DEPTH_SIZE, 24, "depth"}, {GLX_STENCIL_SIZE, 8, "stencil"}};
    for (unsigned i = 0; i < sizeof(expect) / sizeof(expect[0]); i++) {
        int got = -1;
        CHECK(glXGetConfig(dpy, vis, expect[i].attrib, &got) == 0,
              "glXGetConfig succeeds");
        CHECK(got == expect[i].want,
              "glXGetConfig returns the requested channel size");
    }
    int dbl = -1;
    CHECK(glXGetConfig(dpy, vis, GLX_DOUBLEBUFFER, &dbl) == 0,
          "glXGetConfig doublebuffer");
    CHECK(dbl == 1, "glXGetConfig reports double-buffered");
    int bogus = 0;
    CHECK(glXGetConfig(dpy, vis, 0x12345, &bogus) == GLX_BAD_ATTRIBUTE,
          "glXGetConfig rejects an unknown attribute");

    GLXContext ctx = glXCreateContext(dpy, vis, NULL, True);
    CHECK(ctx != NULL, "glXCreateContext");
    CHECK(glXIsDirect(dpy, ctx), "glXIsDirect True for a real context");

    /* Bind to a drawable (backed by a fake pbuffer), swap, then release. */
    Window win =
        XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, 64, 48, 0,
                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    CHECK(win != None, "XCreateSimpleWindow");
    XMapWindow(dpy, win);
    XSync(dpy, False);

    CHECK(glXMakeCurrent(dpy, win, ctx), "glXMakeCurrent binds the drawable");
    CHECK(glXGetCurrentContext() == ctx, "current context is the one we bound");
    CHECK(glXGetCurrentDrawable() == win, "current drawable is the window");

    /* Independent encode check: the EGL side must have received each GLX
     * channel request in the CORRECT EGL channel slot (catches a swapped table
     * entry the round-trip above cannot, since it uses the same table both
     * ways). The config choice is deferred to this first make-current (lazy
     * init), so the provider only records the channels now, not at
     * glXChooseVisual.
     */
    int er = 0, eg = 0, eb = 0, ea = 0, ed = 0, es = 0;
    lastChannels(&er, &eg, &eb, &ea, &ed, &es);
    CHECK(er == 8 && eg == 6 && eb == 5 && ea == 4 && ed == 24 && es == 8,
          "make-current encoded each channel to the correct EGL token");
    /* Headless, the window has no on-screen surface, so it is backed by a
     * single-buffered pbuffer. glXSwapBuffers must NOT dispatch eglSwapBuffers
     * to such a surface: the swap presents nothing and, under ANGLE, recycles
     * the backing so a later readback returns a stale frame. The offscreen
     * current window is presented by reading it back and compositing instead,
     * so the provider swap count stays put.
     */
    long swapsBefore = swapCount();
    glXSwapBuffers(dpy, win);
    CHECK(swapCount() == swapsBefore,
          "glXSwapBuffers does not swap an offscreen pbuffer-backed window");
    CHECK(glXMakeCurrent(dpy, None, NULL), "glXMakeCurrent release");
    CHECK(glXGetCurrentContext() == NULL, "no current context after release");

    /* A repeat make-current on the same drawable must reuse its surface. */
    CHECK(glXMakeCurrent(dpy, win, ctx), "glXMakeCurrent rebind");
    CHECK(glXMakeCurrent(dpy, None, NULL), "glXMakeCurrent release again");

    /* Resize handling: after the window resizes, the next make-current rebuilds
     * the surface at the new size (old destroyed, exactly one new created), and
     * glXQueryDrawable reflects the new geometry.
     */
    long createdBeforeResize = surfacesCreated();
    int liveBeforeResize = liveSurfaces();
    XResizeWindow(dpy, win, 100, 80);
    XSync(dpy, False);
    CHECK(glXMakeCurrent(dpy, win, ctx), "rebind after resize");
    CHECK(surfacesCreated() == createdBeforeResize + 1,
          "resize created exactly one new surface");
    CHECK(liveSurfaces() == liveBeforeResize,
          "resize freed the old surface (net live count unchanged)");
    unsigned int rw = 0, rh = 0;
    glXQueryDrawable(dpy, win, GLX_WIDTH, &rw);
    glXQueryDrawable(dpy, win, GLX_HEIGHT, &rh);
    CHECK(rw == 100 && rh == 80,
          "glXQueryDrawable reflects the resized window");
    CHECK(glXMakeCurrent(dpy, None, NULL), "release after resize");

    /* Swap-time resize: a render loop binds once then loops render+swap. A
     * resize between binds must be picked up at glXSwapBuffers, not deferred to
     * the next make-current.
     */
    CHECK(glXMakeCurrent(dpy, win, ctx), "bind for swap loop");
    long createdBeforeSwapResize = surfacesCreated();
    XResizeWindow(dpy, win, 120, 90);
    XSync(dpy, False);
    glXSwapBuffers(dpy, win);
    CHECK(surfacesCreated() == createdBeforeSwapResize + 1,
          "glXSwapBuffers picked up the resize and rebuilt the surface");
    unsigned int sw = 0, sh = 0;
    glXQueryDrawable(dpy, win, GLX_WIDTH, &sw);
    glXQueryDrawable(dpy, win, GLX_HEIGHT, &sh);
    CHECK(sw == 120 && sh == 90, "query reflects the swap-time resize");
    CHECK(glXMakeCurrent(dpy, None, NULL), "release after swap resize");

    int zeroAttribs[] = {GLX_RGBA, GLX_ALPHA_SIZE, 0, GLX_DEPTH_SIZE, 24, None};
    XVisualInfo *zeroVis = glXChooseVisual(dpy, screen, zeroAttribs);
    CHECK(zeroVis != NULL, "glXChooseVisual accepts a zero-valued attribute");
    XVisualInfo *zeroVis2 = glXChooseVisual(dpy, screen, zeroAttribs);
    CHECK(zeroVis2 != NULL && zeroVis2->visualid == zeroVis->visualid,
          "repeated zero-valued visual attributes reuse the visual");
    XFree(zeroVis2);
    GLXContext zeroCtx = glXCreateContext(dpy, zeroVis, NULL, True);
    CHECK(zeroCtx != NULL, "context for zero-valued visual attributes");
    CHECK(glXMakeCurrent(dpy, win, zeroCtx),
          "bind context with zero-valued visual attributes");
    lastChannels(&er, &eg, &eb, &ea, &ed, &es);
    CHECK(ea == 0 && ed == 24,
          "zero-valued visual attribute does not terminate the list");
    CHECK(glXMakeCurrent(dpy, None, NULL),
          "release zero-valued visual context");
    glXDestroyContext(dpy, zeroCtx);
    XFree(zeroVis);

    /* proc-address resolves a gl* extension symbol via the provider, and a glX*
     * client entry point to our own implementation.
     */
    CHECK(glXGetProcAddress((const unsigned char *) "glClear") != NULL,
          "glXGetProcAddress resolves a gl* symbol via the provider");
    CHECK(glXGetProcAddress((const unsigned char *) "glNope") == NULL,
          "glXGetProcAddress returns NULL for an unknown symbol");
    CHECK(glXGetProcAddress(
              (const unsigned char *) "glXCreateContextAttribsARB") ==
              (void (*)(void)) glXCreateContextAttribsARB,
          "glXGetProcAddress returns our glXCreateContextAttribsARB");

    CHECK(glXMakeCurrent(dpy, win, ctx),
          "bind before destroying current context");
    long releasesBeforeDestroy = releaseCount();
    glXDestroyContext(dpy, ctx);
    CHECK(releaseCount() == releasesBeforeDestroy + 1,
          "glXDestroyContext releases a current EGL context before destroying "
          "it");
    CHECK(glXGetCurrentContext() == NULL,
          "glXDestroyContext clears the current GLX context");
    XFree(vis);

    /* FBConfig path: choose, read attributes, make a context, free the array.
     */
    int fbAttribs[] = {GLX_RED_SIZE,
                       8,
                       GLX_GREEN_SIZE,
                       8,
                       GLX_SAMPLE_BUFFERS,
                       0,
                       GLX_BLUE_SIZE,
                       8,
                       GLX_DEPTH_SIZE,
                       24,
                       None};
    int nfb = 0;
    GLXFBConfig *fbs = glXChooseFBConfig(dpy, screen, fbAttribs, &nfb);
    CHECK(fbs != NULL && nfb >= 1, "glXChooseFBConfig returns a config");
    int fbRed = -1;
    CHECK(glXGetFBConfigAttrib(dpy, fbs[0], GLX_RED_SIZE, &fbRed) == 0,
          "glXGetFBConfigAttrib red");
    CHECK(fbRed == 8, "FBConfig red size is 8");
    int fbDepth = -1;
    CHECK(glXGetFBConfigAttrib(dpy, fbs[0], GLX_DEPTH_SIZE, &fbDepth) == 0,
          "glXGetFBConfigAttrib depth");
    CHECK(fbDepth == 24,
          "zero-valued FBConfig attribute does not terminate the list");
    int renderType = 0;
    CHECK(glXGetFBConfigAttrib(dpy, fbs[0], GLX_RENDER_TYPE, &renderType) == 0,
          "glXGetFBConfigAttrib render type");
    CHECK(renderType == GLX_RGBA_BIT, "FBConfig render type is RGBA");

    XVisualInfo *fbVis = glXGetVisualFromFBConfig(dpy, fbs[0]);
    CHECK(fbVis != NULL, "glXGetVisualFromFBConfig");

    GLXContext fbCtx = glXCreateContext(dpy, fbVis, NULL, True);
    CHECK(fbCtx != NULL, "glXCreateContext from an FBConfig visual");
    glXDestroyContext(dpy, fbCtx);
    XFree(fbVis);

    GLXContext arbCtx =
        glXCreateContextAttribsARB(dpy, fbs[0], NULL, True, NULL);
    CHECK(arbCtx != NULL, "glXCreateContextAttribsARB");
    glXDestroyContext(dpy, arbCtx);

    /* FBConfig attributes real config-enumeration loops read must not fault: a
     * stable nonzero id and sensible defaults, not BadAttribute.
     */
    int fbId = 0, caveat = -1, vtype = -1, trans = -1;
    CHECK(glXGetFBConfigAttrib(dpy, fbs[0], GLX_FBCONFIG_ID, &fbId) == 0 &&
              fbId != 0,
          "glXGetFBConfigAttrib GLX_FBCONFIG_ID is nonzero");
    CHECK(glXGetFBConfigAttrib(dpy, fbs[0], GLX_CONFIG_CAVEAT, &caveat) == 0 &&
              caveat == GLX_NONE,
          "GLX_CONFIG_CAVEAT is GLX_NONE");
    CHECK(glXGetFBConfigAttrib(dpy, fbs[0], GLX_X_VISUAL_TYPE, &vtype) == 0 &&
              vtype == GLX_TRUE_COLOR,
          "GLX_X_VISUAL_TYPE is TrueColor");
    CHECK(
        glXGetFBConfigAttrib(dpy, fbs[0], GLX_TRANSPARENT_TYPE, &trans) == 0 &&
            trans == GLX_NONE,
        "GLX_TRANSPARENT_TYPE is GLX_NONE");

    /* Repeated glXGetVisualFromFBConfig on the same config must reuse one
     * synthetic visual rather than burn a table slot per call; 200 calls far
     * exceeds the fixed table, so without reuse the table exhausts and later
     * calls return NULL. The visual id is stable across calls.
     */
    XVisualInfo *reuseVis = glXGetVisualFromFBConfig(dpy, fbs[0]);
    CHECK(reuseVis != NULL, "glXGetVisualFromFBConfig first call");
    VisualID reuseId = reuseVis ? reuseVis->visualid : 0;
    if (reuseVis)
        XFree(reuseVis);
    Bool reuseStable = True;
    for (int i = 0; i < 200; i++) {
        XVisualInfo *r = glXGetVisualFromFBConfig(dpy, fbs[0]);
        if (!r || r->visualid != reuseId)
            reuseStable = False;
        if (r)
            XFree(r);
    }
    CHECK(reuseStable,
          "200x glXGetVisualFromFBConfig reuses one visual, no exhaustion");

    /* Context sharing must reach the provider (a shared context passes share to
     * eglCreateContext), not be dropped. Destroying the share parent while a
     * child still shares with it must defer the parent's teardown: GLX keeps
     * the shared objects alive until every sharing context is gone, so the
     * parent's EGL context must outlive its glXDestroyContext, not be freed out
     * from under the child.
     */
    GLXContext base =
        glXCreateNewContext(dpy, fbs[0], GLX_RGBA_TYPE, NULL, True);
    CHECK(base != NULL, "base context for sharing");
    GLXContext shared =
        glXCreateNewContext(dpy, fbs[0], GLX_RGBA_TYPE, base, True);
    CHECK(shared != NULL && lastShareNonnull(),
          "glXCreateNewContext forwarded the share list to eglCreateContext");
    int ctxWithShare = liveContexts();
    glXDestroyContext(dpy, base); /* parent first, child still references it */
    CHECK(liveContexts() == ctxWithShare,
          "destroying a shared-from context defers while a child still shares");
    CHECK(glXMakeCurrent(dpy, win, shared),
          "the child still binds after its share parent was destroyed");
    CHECK(glXMakeCurrent(dpy, None, NULL), "release the child");
    glXDestroyContext(dpy, shared); /* last reference: both reclaimed now */
    CHECK(liveContexts() == ctxWithShare - 2,
          "the deferred share parent is reclaimed once the child is gone");

    /* glXCreateContextAttribsARB: a desktop core profile cannot be honored on a
     * GLES-only provider (reject, do not silently downgrade); MAJOR>=3 selects
     * an EGL ES3 client version.
     */
    int coreAttribs[] = {GLX_CONTEXT_PROFILE_MASK_ARB,
                         GLX_CONTEXT_CORE_PROFILE_BIT_ARB, None};
    CHECK(glXCreateContextAttribsARB(dpy, fbs[0], NULL, True, coreAttribs) ==
              NULL,
          "glXCreateContextAttribsARB rejects a desktop core profile");
    int es3Attribs[] = {GLX_CONTEXT_MAJOR_VERSION_ARB, 3, None};
    GLXContext es3 =
        glXCreateContextAttribsARB(dpy, fbs[0], NULL, True, es3Attribs);
    CHECK(es3 != NULL && lastClientVersion() == 3,
          "MAJOR=3 maps to an EGL ES3 client version");
    glXDestroyContext(dpy, es3);

    /* GLX 1.3 pbuffer lifecycle: create an offscreen drawable, query its size,
     * bind a context to it, then destroy it and confirm the surface is freed.
     */
    int pbAttribs[] = {GLX_PBUFFER_WIDTH, 32, GLX_PBUFFER_HEIGHT, 16, None};
    GLXPbuffer pbuf = glXCreatePbuffer(dpy, fbs[0], pbAttribs);
    CHECK(pbuf != None, "glXCreatePbuffer");
    unsigned int pw = 0, ph = 0;
    glXQueryDrawable(dpy, pbuf, GLX_WIDTH, &pw);
    glXQueryDrawable(dpy, pbuf, GLX_HEIGHT, &ph);
    CHECK(pw == 32 && ph == 16, "glXQueryDrawable returns the pbuffer size");

    GLXContext pbCtx =
        glXCreateNewContext(dpy, fbs[0], GLX_RGBA_TYPE, NULL, True);
    CHECK(pbCtx != NULL, "context for the pbuffer");
    CHECK(glXMakeContextCurrent(dpy, win, pbuf, pbCtx),
          "bind distinct draw/read drawables");
    CHECK(!currentDrawEqualsRead(), "EGL draw/read surfaces are distinct");
    XResizeWindow(dpy, win, 130, 95);
    XSync(dpy, False);
    glXSwapBuffers(dpy, win);
    CHECK(hasCurrentRead() && !currentDrawEqualsRead(),
          "swap-time resize preserves the read surface");
    CHECK(glXMakeCurrent(dpy, None, NULL), "release distinct draw/read bind");

    CHECK(glXMakeCurrent(dpy, pbuf, pbCtx), "glXMakeCurrent on a pbuffer");
    CHECK(glXGetCurrentReadDrawable() == pbuf,
          "read drawable tracks the current pbuffer");
    glXSwapBuffers(dpy, pbuf); /* offscreen; must not crash */
    CHECK(glXMakeCurrent(dpy, None, NULL), "release the pbuffer");
    glXDestroyContext(dpy, pbCtx);

    int surfacesBefore = liveSurfaces();
    glXDestroyPbuffer(dpy, pbuf);
    CHECK(liveSurfaces() == surfacesBefore - 1,
          "glXDestroyPbuffer frees the pbuffer surface");

    /* Passing a non-pbuffer XID (the window) must be rejected, not tear down
     * the window's surface or free its core XID.
     */
    int surfacesGuard = liveSurfaces();
    glXDestroyPbuffer(dpy, win);
    CHECK(liveSurfaces() == surfacesGuard,
          "glXDestroyPbuffer ignores a non-pbuffer XID");

    /* GLX 1.3 on-screen drawable: glXCreateWindow aliases an X window; binding
     * it makes a surface; glXDestroyWindow releases that surface but leaves the
     * X window usable (it can be bound again).
     */
    Window xwin2 =
        XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 0, 0, 40, 30, 0,
                            BlackPixel(dpy, screen), WhitePixel(dpy, screen));
    CHECK(xwin2 != None, "XCreateSimpleWindow for GLXWindow");
    XMapWindow(dpy, xwin2);
    XSync(dpy, False);
    GLXWindow glxwin = glXCreateWindow(dpy, fbs[0], xwin2, NULL);
    CHECK(glxwin != None, "glXCreateWindow");
    GLXContext winCtx =
        glXCreateNewContext(dpy, fbs[0], GLX_RGBA_TYPE, NULL, True);
    CHECK(winCtx != NULL, "context for the GLXWindow");
    CHECK(glXMakeCurrent(dpy, glxwin, winCtx), "bind the GLXWindow");
    int surfacesWithGlxWin = liveSurfaces();
    /* Destroy WHILE current: the calling thread must be unbound, not left with
     * a context pointing at a destroyed surface.
     */
    glXDestroyWindow(dpy, glxwin);
    CHECK(liveSurfaces() == surfacesWithGlxWin - 1,
          "glXDestroyWindow releases the GL surface");
    CHECK(glXGetCurrentContext() == NULL,
          "glXDestroyWindow while current unbinds the calling thread");
    /* The X window still exists: it can be bound again (surface recreated). */
    CHECK(glXMakeCurrent(dpy, glxwin, winCtx),
          "the X window survives glXDestroyWindow and rebinds");
    CHECK(glXMakeCurrent(dpy, None, NULL), "release after rebind");
    glXDestroyContext(dpy, winCtx);
    XDestroyWindow(dpy, xwin2);
    XSync(dpy, False);

    XFree(fbs);

    /* Every context created above was destroyed: zero context balance. */
    CHECK(liveContexts() == 0, "all GLX contexts released at the provider");

    /* The window was made current, so it has a live EGL surface. Destroying the
     * window must tear that surface down via the glx.c teardown hook (no leak,
     * no stale mapping on XID reuse).
     */
    CHECK(liveSurfaces() >= 1, "the bound window has a live EGL surface");
    XDestroyWindow(dpy, win);
    XSync(dpy, False);
    CHECK(liveSurfaces() == 0, "XDestroyWindow tore down the drawable surface");

    dlclose(fake);
    XCloseDisplay(dpy);
    printf("test-glx-provider: ok (fake EGL provider)\n");
    return 0;
}
