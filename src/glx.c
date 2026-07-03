/*
 * GLX API implemented as a thin translation onto EGL.
 *
 * The core GLX objects correspond to EGL ones, with attribute translation:
 *   GLXContext  <-> EGLContext
 *   GLXDrawable <-> EGLSurface
 *   GLXFBConfig <-> EGLConfig     (XVisualInfo is synthesized from an
 * EGLConfig)
 *
 * Provider (ANGLE on macOS, Mesa on Linux) is resolved at runtime by
 * src/egl-wrapper.c. When no provider is present every entry point degrades to
 * a safe NULL/False so an Xlib+GLX client falls back cleanly. glXQueryExtension
 * is the provider-aware probe; the generic XQueryExtension("GLX") deliberately
 * stays absent (src/extension.c) so real toolkits are never steered into a GLX
 * wire-protocol path this in-process layer does not serve.
 *
 * Phase 1 scope: the classic and FBConfig context/config paths, attribute
 * translation, and graceful degradation. The current drawable is backed by an
 * offscreen EGL pbuffer sized to the X drawable; that is the headless smoke
 * path (glReadPixels). On-screen presentation through an ANGLE CAMetalLayer
 * window surface is Phase 2 and is called out where it lands.
 */
#include <GL/glx.h>
#include <dlfcn.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
/* Minimal objc runtime decls (see pinMetalLayerToPointSize). The real
 * <objc/runtime.h> pulls in <objc/objc.h>, whose bool/BOOL definitions clash
 * with the X11 headers this file includes, so declare just the two entry points
 * used and cast objc_msgSend per call.
 */
extern void *sel_registerName(const char *name);
extern void objc_msgSend(void);
#endif

#include "drawing.h"
#include "egl-wrapper.h"
#include "resource-types.h"
#include "sdl-compat.h"
#include "util.h"

/* Which client API a context should bind. AUTO picks desktop GL when the
 * provider supports it (Mesa) and GLES otherwise (ANGLE), which is the right
 * default for a classic glXCreateContext: on a desktop-GLX system a plain GLX
 * context is desktop GL. GLES/DESKTOP force the choice for
 * glXCreateContextAttribsARB.
 */
typedef enum {
    CTX_API_AUTO,
    CTX_API_GLES,
    CTX_API_DESKTOP,
} CtxApiPref;

/* Room to copy a glXChooseVisual attribute list for deferred config choice. GLX
 * visual lists are short (RGBA, DOUBLEBUFFER, a few sized channels); this is
 * generous and bounded.
 */
#define GLX_LAZY_ATTRIBS 64

/* Upper bound on attribute-list length. GLX lists are None-terminated; the cap
 * keeps a missing terminator from turning into an unbounded scan.
 */
#define MAX_ATTRIB_TOKENS 256

static Bool visualAttribHasValue(int attrib)
{
    switch (attrib) {
    case GLX_USE_GL:
    case GLX_RGBA:
    case GLX_STEREO:
    case GLX_DOUBLEBUFFER:
        return False;
    default:
        return True;
    }
}

static Bool copyVisualAttribs(const int *attribList, int *out, int *outCount)
{
    int n = 0;
    /* A token is 1 int (boolean) or 2 (valued); the attribute name decides, so
     * a value of None/0 is copied as a value, never mistaken for the
     * terminator. This matches Mesa: glXChooseVisual({GLX_ALPHA_SIZE, 0,
     * GLX_DEPTH_SIZE, 24, None}) keeps the depth request rather than stopping
     * at the zero alpha. Deciding value-vs-terminator by inspecting the next
     * int instead (stop when p[1] == None) cannot work, since None == 0 makes a
     * legal zero value and the terminator identical, and it would drop every
     * attribute after a zero.
     *
     * The list must be None-terminated (the GLX contract). An unterminated list
     * whose last token is valued (e.g. {GLX_DEPTH_SIZE, None} with no trailing
     * terminator) is undefined here exactly as in Mesa, which also consumes the
     * None as the value and reads past it; callers pass a terminator, and the
     * tests use a guard None (see test-glx-link.c) to exercise this safely. The
     * output-buffer bound rejects before n reaches GLX_LAZY_ATTRIBS.
     */
    for (const int *p = attribList; p && *p != None;) {
        int width = visualAttribHasValue(*p) ? 2 : 1;
        if (n + width >= GLX_LAZY_ATTRIBS)
            return False;
        out[n++] = *p;
        if (width == 2)
            out[n++] = p[1];
        p += width;
    }
    out[n++] = None;
    *outCount = n;
    return True;
}

struct __GLXcontextRec {
    EGLContext egl;
    EGLConfig config;
    int screen;
    Bool isDirect;
    EGLenum apiType; /* EGL_OPENGL_API (desktop) or EGL_OPENGL_ES_API */
    /* Lazy creation. A classic glXCreateContext records the request here and
     * defers the real config choice + eglCreateContext (and, through them, the
     * provider's expensive eglInitialize) to the first glXMakeCurrent -- by
     * which point the client's window is already mapped. realized flips true
     * once the EGL context above is live. FBConfig/ARB contexts are created
     * eagerly and start realized.
     */
    Bool realized;
    int glxAttribs[GLX_LAZY_ATTRIBS]; /* copied glXChooseVisual list */
    int glxAttribCount;
    int clientVersion;
    CtxApiPref pref;
    GLXContext share;
    /* Per-context gl4es state (NewGLState), created on first make-current.
     * gl4es keeps its state in a global that its own glXMakeCurrent switches;
     * since we use our glX*, we switch it ourselves so each context renders
     * with its own state (multiple contexts on one window: multictx,
     * sharedtex). NULL when no gl4es is linked.
     */
    void *gl4esState;
    /* Deferred destruction. GLX lets a client destroy a context that is still
     * current or still shared; the objects live until it is neither. So
     * glXDestroyContext only marks destroyPending, and the real teardown runs
     * once no thread has it current (currentCount) and no child shares its
     * objects (shareRefs). All three are guarded by glxLock.
     */
    int currentCount;
    int shareRefs;
    Bool destroyPending;
};

struct __GLXFBConfigRec {
    EGLConfig egl;
    int screen;
};

/* Synthetic visual id -> EGLConfig/attributes, populated by glXChooseVisual and
 * glXGetVisualFromFBConfig so glXCreateContext/glXGetConfig can recover what a
 * returned XVisualInfo stands for.
 */
typedef struct {
    VisualID visual;
    Bool lazy;        /* True: config deferred, use glxAttribs */
    EGLConfig config; /* valid when !lazy */
    int glxAttribs[GLX_LAZY_ATTRIBS]; /* copied glXChooseVisual list when lazy
                                       */
    int glxAttribCount;
} VisualConfig;

/* Drawable -> EGLSurface, created lazily on first glXMakeCurrent (or eagerly by
 * glXCreatePbuffer). width/height are the surface size, answered by
 * glXQueryDrawable.
 */
typedef struct {
    GLXDrawable drawable;
    EGLSurface surface;
    unsigned int width, height;
    /* True when the surface presents itself (top-level window: CAMetalLayer on
     * macOS, X11 window on Linux). False for an offscreen pbuffer, including a
     * pbuffer standing in for an embedded GL child widget whose pixels must be
     * read back and composited into its parent (see glXSwapBuffers).
     */
    Bool onScreen;
} DrawableSurface;

#define MAX_VISUAL_CONFIGS 64
#define MAX_DRAWABLE_SURFACES 64
#define MAX_FBCONFIGS 64

static VisualConfig visualConfigs[MAX_VISUAL_CONFIGS];
static int visualConfigCount;
static VisualID nextSyntheticVisual = (VisualID) ~0UL;
static DrawableSurface drawableSurfaces[MAX_DRAWABLE_SURFACES];
static int drawableSurfaceCount;
/* Persistent FBConfig records. Real GLX hands out GLXFBConfig handles that live
 * for the display's lifetime; the caller frees only the array from
 * glXChooseFBConfig, not the handles. We keep the records here and return
 * pointers into this table so the handles stay valid and nothing leaks per
 * call.
 */
static struct __GLXFBConfigRec fbConfigPool[MAX_FBCONFIGS];
static int fbConfigPoolCount;

/* Guards the registries above. Kept out of the current-context state, which is
 * thread-local (real GLX tracks the current context per thread).
 */
static pthread_mutex_t glxLock = PTHREAD_MUTEX_INITIALIZER;

static __thread Display *currentDisplay;
static __thread GLXContext currentContext;
static __thread GLXDrawable currentDrawable;
static __thread GLXDrawable currentReadDrawable;
/* The EGL draw surface this thread's context is bound to. glXSwapBuffers
 * rebinds only when the drawable's surface differs from this (a resize rebuilt
 * it, or it was destroyed and recreated), so the common per-frame path skips a
 * redundant eglMakeCurrent while a changed surface still self-heals.
 */
static __thread EGLSurface currentDrawSurface;
static __thread EGLSurface currentReadSurface;

/* gl4es hooks, soft-resolved once by initGlTranslationLayerIfPresent. All NULL
 * when no gl4es is linked, so a plain GLES/ANGLE client is unaffected. The
 * state hooks (NewGLState/ActivateGLState/DeleteGLState) let us drive gl4es's
 * per-context state, which its own glXMakeCurrent would otherwise own -- but
 * the client uses ours.
 */
static void (*gl4esSetResolver)(void *(*) (const char *) ) = NULL;
static void (*gl4esInit)(void) = NULL;
static void *(*gl4esNewState)(void *shared, int es2only) = NULL;
static void (*gl4esActivateState)(void *state) = NULL;
static void (*gl4esDeleteState)(void *state) = NULL;

/* Insert-or-find the table slot for a visual (caller holds glxLock).
 *
 * Returns the slot, or NULL when the table is full. A fresh slot is zeroed.
 */
static VisualConfig *visualSlotLocked(VisualID visual)
{
    for (int i = 0; i < visualConfigCount; i++)
        if (visualConfigs[i].visual == visual)
            return &visualConfigs[i];
    if (visualConfigCount >= MAX_VISUAL_CONFIGS)
        return NULL;
    VisualConfig *slot = &visualConfigs[visualConfigCount++];
    memset(slot, 0, sizeof(*slot));
    slot->visual = visual;
    return slot;
}

/* Return an existing eager visual already mapped to this EGLConfig, or 0. Lets
 * repeated glXGetVisualFromFBConfig on the same config reuse one synthetic
 * visual instead of burning a table slot per call and eventually exhausting
 * MAX_VISUAL_CONFIGS. Caller holds glxLock.
 */
static VisualID findEagerVisualLocked(EGLConfig config)
{
    for (int i = 0; i < visualConfigCount; i++) {
        VisualConfig *v = &visualConfigs[i];
        if (!v->lazy && v->config == config)
            return v->visual;
    }
    return 0;
}

/* Eager mapping: a visual synthesized from a known EGLConfig (the FBConfig
 * path).
 */
static Bool rememberVisualConfig(VisualID visual, EGLConfig config)
{
    pthread_mutex_lock(&glxLock);
    VisualConfig *slot = visualSlotLocked(visual);
    if (slot) {
        slot->lazy = False;
        slot->config = config;
    }
    pthread_mutex_unlock(&glxLock);
    return slot ? True : False;
}

/* Lazy mapping: record a glXChooseVisual attribute list so glXCreateContext /
 * realizeLazyContext can choose the config later, after the window is up.
 * Copies the None-terminated list; rejects one longer than GLX_LAZY_ATTRIBS.
 */
static Bool rememberVisualLazy(VisualID visual, const int *attribList)
{
    int a[GLX_LAZY_ATTRIBS];
    int n = 0;
    if (!copyVisualAttribs(attribList, a, &n))
        return False; /* list longer than the cap */

    pthread_mutex_lock(&glxLock);
    VisualConfig *slot = visualSlotLocked(visual);
    if (slot) {
        slot->lazy = True;
        memcpy(slot->glxAttribs, a, (size_t) n * sizeof(int));
        slot->glxAttribCount = n;
    }
    pthread_mutex_unlock(&glxLock);
    return slot ? True : False;
}

/* Return an existing lazy visual whose recorded attribute list matches
 * attribList, or 0 if none. Lets a repeated identical glXChooseVisual reuse one
 * slot instead of burning a fresh synthetic visual per call: the table is small
 * (MAX_VISUAL_CONFIGS) and never reclaimed, so an app that re-chooses the same
 * visual per window would otherwise exhaust it and lose GLX. Caller holds
 * glxLock. Normalizes the list the same way rememberVisualLazy records it.
 */
static VisualID findLazyVisualLocked(const int *attribList)
{
    int a[GLX_LAZY_ATTRIBS];
    int n = 0;
    if (!copyVisualAttribs(attribList, a, &n))
        return 0; /* too long to have been recorded */
    for (int i = 0; i < visualConfigCount; i++) {
        VisualConfig *v = &visualConfigs[i];
        if (v->lazy && v->glxAttribCount == n &&
            !memcmp(v->glxAttribs, a, (size_t) n * sizeof(int)))
            return v->visual;
    }
    return 0;
}

static Bool lookupVisual(VisualID visual, VisualConfig *out)
{
    Bool found = False;
    pthread_mutex_lock(&glxLock);
    for (int i = 0; i < visualConfigCount; i++)
        if (visualConfigs[i].visual == visual) {
            *out = visualConfigs[i];
            found = True;
            break;
        }
    pthread_mutex_unlock(&glxLock);
    return found;
}

static VisualID allocSyntheticVisualID(void)
{
    pthread_mutex_lock(&glxLock);
    VisualID id = nextSyntheticVisual--;
    pthread_mutex_unlock(&glxLock);
    return id;
}

/* Intern an EGLConfig into the persistent FBConfig pool and return its stable
 * handle, or NULL if the pool is full.
 */
static GLXFBConfig internFBConfig(EGLConfig config, int screen)
{
    GLXFBConfig handle = NULL;
    pthread_mutex_lock(&glxLock);
    for (int i = 0; i < fbConfigPoolCount; i++) {
        if (fbConfigPool[i].egl == config && fbConfigPool[i].screen == screen) {
            handle = &fbConfigPool[i];
            break;
        }
    }
    if (!handle && fbConfigPoolCount < MAX_FBCONFIGS) {
        struct __GLXFBConfigRec *rec = &fbConfigPool[fbConfigPoolCount++];
        rec->egl = config;
        rec->screen = screen;
        handle = rec;
    }
    pthread_mutex_unlock(&glxLock);
    return handle;
}

/* Append the common "renderable type + surface type" tail and the terminator to
 * a translated EGL attribute list.
 *
 * Returns the new count, or -1 on overflow.
 */
static int finishEglAttribs(EGLint *egl, size_t n, size_t cap)
{
    if (n + 5 > cap)
        return -1;
    egl[n++] = EGL_RENDERABLE_TYPE;
    /* Require desktop GL too on a desktop-capable provider (Mesa) so the chosen
     * config can back a desktop context, not only a GLES one. ANGLE is
     * GLES-only, so asking for EGL_OPENGL_BIT there would return no configs.
     */
    egl[n++] =
        EGL_OPENGL_ES2_BIT |
        (eglProviderClientApi() == EGL_CLIENT_API_DESKTOP ? EGL_OPENGL_BIT : 0);
    /* One chosen config must back both an on-screen window surface and an
     * offscreen pbuffer (child GL widgets composite through a pbuffer), so
     * require both bits. The value written here must stay the second-to-last
     * attrib: chooseEglConfigs patches it (attribs[count-2]) to pbuffer-only
     * when a surfaceless provider offers no window configs.
     */
    egl[n++] = EGL_SURFACE_TYPE;
    egl[n++] = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
    egl[n++] = EGL_NONE;
    return (int) n;
}

/* GLX sized-attribute tokens and their EGL config-attribute equivalents. This
 * one to one map drives both directions: translating a GLX attribute list into
 * an EGL one (glXChooseVisual, glXChooseFBConfig) and answering a single
 * attribute query (glXGetConfig, glXGetFBConfigAttrib).
 */
static const struct {
    int glx;
    EGLint egl;
} sizeAttribMap[] = {
    {GLX_RED_SIZE, EGL_RED_SIZE},     {GLX_GREEN_SIZE, EGL_GREEN_SIZE},
    {GLX_BLUE_SIZE, EGL_BLUE_SIZE},   {GLX_ALPHA_SIZE, EGL_ALPHA_SIZE},
    {GLX_DEPTH_SIZE, EGL_DEPTH_SIZE}, {GLX_STENCIL_SIZE, EGL_STENCIL_SIZE},
    {GLX_SAMPLES, EGL_SAMPLES},       {GLX_SAMPLE_BUFFERS, EGL_SAMPLE_BUFFERS},
};

/* Map a GLX sized-attribute token to its EGL equivalent.
 *
 * Returns True and sets *egl on a hit, False for a token that has no EGL
 * sized-attribute analog.
 */
static Bool glxSizeAttribToEgl(int glx, EGLint *egl)
{
    for (size_t i = 0; i < sizeof(sizeAttribMap) / sizeof(sizeAttribMap[0]);
         i++) {
        if (sizeAttribMap[i].glx == glx) {
            *egl = sizeAttribMap[i].egl;
            return True;
        }
    }
    return False;
}

/* Append one EGL token+value pair to a translated attribute list at *n,
 * advancing *n.
 *
 * Returns False if the pair would overflow the cap.
 */
static Bool pushEglAttrib(EGLint *egl,
                          size_t cap,
                          size_t *n,
                          EGLint tok,
                          EGLint val)
{
    if (*n + 2 > cap)
        return False;
    egl[(*n)++] = tok;
    egl[(*n)++] = val;
    return True;
}

/* Translate a glXChooseVisual attribute list (boolean tokens carry no value;
 * everything else is token + value, terminated by None) into an EGL attribute
 * list.
 *
 * Returns the count written, or -1 on overflow. *doubleBuffered reports whether
 * GLX_DOUBLEBUFFER was requested (EGL picks this at surface time).
 */
static int translateVisualAttribs(const int *glx,
                                  EGLint *egl,
                                  size_t cap,
                                  Bool *doubleBuffered)
{
    size_t n = 0;
    *doubleBuffered = False;

    const int *p = glx;
    for (int i = 0; p && i < MAX_ATTRIB_TOKENS && *p != None; i++) {
        if (*p == GLX_DOUBLEBUFFER)
            *doubleBuffered = True;
        if (!visualAttribHasValue(*p)) {
            /* Boolean token, no value: RGBA is the EGL default, USE_GL and
             * STEREO are ignored, DOUBLEBUFFER handled above.
             */
            p++;
            continue;
        }
        /* Value-carrying token. A value of None/0 is legal; only an attribute
         * token of None terminates the list. A sized token maps to an EGL
         * attribute; other valued tokens have no EGL equivalent, so their value
         * is skipped.
         */
        EGLint eglTok;
        if (glxSizeAttribToEgl(*p, &eglTok) &&
            !pushEglAttrib(egl, cap, &n, eglTok, p[1]))
            return -1;
        p += 2; /* consume token + value */
    }

    return finishEglAttribs(egl, n, cap);
}

/* Fill an XVisualInfo standing in for the chosen EGLConfig using the screen
 * default Visual* but a unique synthetic visualid for GLX bookkeeping.
 */
static XVisualInfo *synthesizeVisual(Display *dpy,
                                     int screen,
                                     VisualID visualid)
{
    Visual *visual = DefaultVisual(dpy, screen);
    XVisualInfo *info = calloc(1, sizeof(*info));
    if (!info)
        return NULL;
    info->visual = visual;
    info->visualid = visualid;
    info->screen = screen;
    info->depth = DefaultDepth(dpy, screen);
    info->class = TrueColor;
    if (visual) {
        info->red_mask = visual->red_mask;
        info->green_mask = visual->green_mask;
        info->blue_mask = visual->blue_mask;
        info->bits_per_rgb = visual->bits_per_rgb;
        info->colormap_size = visual->map_entries;
    }
    return info;
}

/* Choose EGL configs for a translated attribute list, retrying pbuffer-only if
 * the provider has no window configs. A surfaceless or software provider (Mesa
 * on a host with no display) offers only pbuffer configs; the offscreen render
 * is then composited to the window. attribCount is the count returned by
 * finishEglAttribs, whose surface-type value sits at attribs[attribCount - 2]
 * (just before EGL_NONE) and is patched to EGL_PBUFFER_BIT on the retry.
 *
 * Returns the number of configs found.
 */
static EGLint chooseEglConfigs(EGLint *attribs,
                               int attribCount,
                               EGLConfig *configs,
                               EGLint maxConfigs)
{
    const EglApi *egl = eglApi();
    EGLDisplay display = eglDefaultDisplay();
    EGLint numConfigs = 0;
    egl->chooseConfig(display, attribs, configs, maxConfigs, &numConfigs);
    if (numConfigs < 1) {
        attribs[attribCount - 2] = EGL_PBUFFER_BIT;
        egl->chooseConfig(display, attribs, configs, maxConfigs, &numConfigs);
    }
    return numConfigs;
}

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList)
{
    /* Only confirm a provider is loaded (cheap, no eglInitialize); the config
     * choice and the provider's expensive initialize are deferred to the first
     * glXMakeCurrent, by which point the client has built and mapped its
     * window. Record the attribute list against the synthesized visual so the
     * deferred glXCreateContext can pick a matching config later
     * (realizeLazyContext).
     */
    if (!eglLoad())
        return NULL;
    /* Reuse an existing slot for an identical request so repeated
     * glXChooseVisual calls do not exhaust the fixed, never-reclaimed table.
     */
    pthread_mutex_lock(&glxLock);
    VisualID vid = findLazyVisualLocked(attribList);
    pthread_mutex_unlock(&glxLock);
    if (!vid) {
        vid = allocSyntheticVisualID();
        if (!rememberVisualLazy(vid, attribList)) {
            LOG("glXChooseVisual: visual table full or attribute list too "
                "long\n");
            return NULL;
        }
    }
    XVisualInfo *info = synthesizeVisual(dpy, screen, vid);
    if (!info)
        return NULL;
    return info;
}

/* Create the EGL context for a config + api preference, returning it and the
 * bound api type (or EGL_NO_CONTEXT). Shared by the eager
 * createContextFromConfig and the deferred realizeLazyContext. The provider
 * must already be ready.
 */
static EGLContext createEglContext(EGLConfig config,
                                   int clientVersion,
                                   GLXContext share,
                                   CtxApiPref pref,
                                   EGLenum *outApiType)
{
    const EglApi *egl = eglApi();
    Bool desktop;
    if (pref == CTX_API_DESKTOP)
        desktop = True;
    else if (pref == CTX_API_GLES)
        desktop = False;
    else /* CTX_API_AUTO: desktop GL if the provider offers it (Mesa), else GLES
          * (ANGLE). This is the correct default for a plain GLX context. */
        desktop = (eglProviderClientApi() == EGL_CLIENT_API_DESKTOP);

    EGLenum apiType = desktop ? EGL_OPENGL_API : EGL_OPENGL_ES_API;
    if (!egl->bindAPI(apiType))
        return EGL_NO_CONTEXT;

    /* A desktop GL context takes no EGL_CONTEXT_CLIENT_VERSION: that attribute
     * selects a GLES version and is rejected for desktop GL. GLES contexts
     * carry the requested version (2 or 3).
     */
    const EGLint esAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, clientVersion,
                                EGL_NONE};
    const EGLint deskAttribs[] = {EGL_NONE};
    EGLContext shareEgl = share ? share->egl : EGL_NO_CONTEXT;
    EGLContext eglCtx =
        egl->createContext(eglDefaultDisplay(), config, shareEgl,
                           desktop ? deskAttribs : esAttribs);
    if (eglCtx == EGL_NO_CONTEXT && desktop && pref == CTX_API_AUTO) {
        /* AUTO preferred desktop GL but the chosen config may not advertise it
         * (EGL only guarantees the renderable type asked for at choose time);
         * fall back to GLES rather than fail a context an ES config would
         * satisfy.
         */
        apiType = EGL_OPENGL_ES_API;
        if (egl->bindAPI(apiType))
            eglCtx = egl->createContext(eglDefaultDisplay(), config, shareEgl,
                                        esAttribs);
    }
    if (eglCtx == EGL_NO_CONTEXT)
        return EGL_NO_CONTEXT;
    *outApiType = apiType;
    return eglCtx;
}

static Bool realizeLazyContext(GLXContext ctx);

/* Deferred-destroy helper (defined below, near glXDestroyContext): drop a
 * reference taken on a share context and reclaim it if its destroy was deferred
 * and that was the last reference. No-op when share is NULL.
 */
static void releaseShareRef(GLXContext share);

/* Eager context (FBConfig / ARB path): the config is known, so create the EGL
 * context now and hand back a fully realized GLXContext.
 */
static GLXContext createContextFromConfig(EGLConfig config,
                                          int screen,
                                          Bool direct,
                                          int clientVersion,
                                          GLXContext share,
                                          CtxApiPref pref)
{
    /* Reference the share context up front, before realizing or handing it to
     * eglCreateContext, so a concurrent glXDestroyContext cannot free it out
     * from under this child's creation. Released on any failure below. A lazy
     * share (classic glXCreateContext not yet bound) has no EGL context yet;
     * realize it so EGL actually shares objects instead of silently creating an
     * unshared context.
     */
    if (share) {
        pthread_mutex_lock(&glxLock);
        share->shareRefs++;
        pthread_mutex_unlock(&glxLock);
    }
    if (share && !realizeLazyContext(share)) {
        releaseShareRef(share);
        return NULL;
    }
    EGLenum apiType;
    EGLContext eglCtx =
        createEglContext(config, clientVersion, share, pref, &apiType);
    if (eglCtx == EGL_NO_CONTEXT) {
        releaseShareRef(share);
        return NULL;
    }
    GLXContext ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        eglApi()->destroyContext(eglDefaultDisplay(), eglCtx);
        releaseShareRef(share);
        return NULL;
    }
    ctx->egl = eglCtx;
    ctx->config = config;
    ctx->screen = screen;
    ctx->isDirect = direct;
    ctx->apiType = apiType;
    ctx->realized = True;
    ctx->share = share; /* reference already held above */
    return ctx;
}

/* Realize a context deferred by glXCreateContext: initialize the provider (the
 * expensive step this whole lazy path exists to postpone), choose a config for
 * the recorded glXChooseVisual attributes, and create the EGL context. Called
 * from glXMakeContextCurrent, after the client's window is mapped.
 */
static Bool realizeLazyContext(GLXContext ctx)
{
    if (ctx->realized)
        return True;
    if (!eglEnsureReady())
        return False;
    if (ctx->share && !realizeLazyContext(ctx->share))
        return False;

    EGLint eglAttribs[64];
    Bool doubleBuffered;
    int written = translateVisualAttribs(ctx->glxAttribs, eglAttribs, 64,
                                         &doubleBuffered);
    if (written < 0)
        return False;
    (void) doubleBuffered;
    EGLConfig config;
    if (chooseEglConfigs(eglAttribs, written, &config, 1) < 1)
        return False;

    EGLenum apiType;
    EGLContext eglCtx = createEglContext(config, ctx->clientVersion, ctx->share,
                                         ctx->pref, &apiType);
    if (eglCtx == EGL_NO_CONTEXT)
        return False;
    ctx->egl = eglCtx;
    ctx->config = config;
    ctx->apiType = apiType;
    ctx->realized = True;
    return True;
}

GLXContext glXCreateContext(Display *dpy,
                            XVisualInfo *vis,
                            GLXContext shareList,
                            Bool direct)
{
    (void) dpy;
    if (!eglLoad() || !vis)
        return NULL;

    VisualConfig vc;
    if (!lookupVisual(vis->visualid, &vc)) {
        LOG("glXCreateContext: unknown visual 0x%lx\n",
            (unsigned long) vis->visualid);
        return NULL;
    }
    /* A visual from an FBConfig carries a known config; create eagerly. */
    if (!vc.lazy)
        return createContextFromConfig(vc.config, vis->screen, direct, 2,
                                       shareList, CTX_API_AUTO);

    /* glXChooseVisual visual: record the request and defer the EGL context (and
     * the provider initialize) to the first glXMakeCurrent.
     */
    GLXContext ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->egl = EGL_NO_CONTEXT;
    ctx->screen = vis->screen;
    ctx->isDirect = direct;
    ctx->clientVersion = 2;
    ctx->pref = CTX_API_AUTO;
    ctx->share = shareList;
    ctx->realized = False;
    memcpy(ctx->glxAttribs, vc.glxAttribs,
           (size_t) vc.glxAttribCount * sizeof(int));
    ctx->glxAttribCount = vc.glxAttribCount;

    /* Reference the share up front so it cannot be freed while we realize this
     * child. A context that shares objects with another must be realized now,
     * not deferred: the share's EGL context has to exist at creation time. Only
     * standalone contexts (the common case, e.g. paperplane) stay deferred and
     * get the fast window-up path.
     */
    if (shareList) {
        pthread_mutex_lock(&glxLock);
        shareList->shareRefs++;
        pthread_mutex_unlock(&glxLock);
    }
    if (shareList && !realizeLazyContext(ctx)) {
        releaseShareRef(shareList);
        free(ctx);
        return NULL;
    }
    return ctx;
}

static void tearDownContext(GLXContext ctx);

/* Caller holds glxLock. If ctx has a pending destroy and is now unreferenced
 * (no thread current, no sharing child), consume the pending flag and return
 * True so exactly one caller reclaims it; the consumed flag stops any other
 * path from freeing it too.
 *
 * Returns False otherwise.
 */
static Bool claimContextTeardownLocked(GLXContext ctx)
{
    if (ctx && ctx->destroyPending && ctx->currentCount == 0 &&
        ctx->shareRefs == 0) {
        ctx->destroyPending = False;
        return True;
    }
    return False;
}

/* Set this thread's current context, maintaining the process-wide current-use
 * count so glXDestroyContext can tell whether any thread still has a context
 * bound. Drops this thread's current reference (the bind path installs its own
 * reference directly, so this only ever clears). If that leaves the context
 * reclaimable, this claims it under the same lock (so no other thread can also
 * free it) and returns it; the caller must then run tearDownContextChain on it
 * after any EGL unbind.
 *
 * Returns NULL when nothing became reclaimable. The caller owns the other
 * current* TLS fields.
 */
static GLXContext clearThreadCurrentContext(void)
{
    GLXContext prev = currentContext;
    if (!prev)
        return NULL;
    GLXContext claimed = NULL;
    pthread_mutex_lock(&glxLock);
    prev->currentCount--;
    if (claimContextTeardownLocked(prev))
        claimed = prev;
    pthread_mutex_unlock(&glxLock);
    currentContext = NULL;
    return claimed;
}

/* Reclaim a context whose teardown this thread has already claimed, then walk
 * up the share chain: each freed child drops the reference it held on its share
 * parent, which is claimed and freed in turn if that was the parent's last
 * reference. No lock held on entry; the per-context claim guarantees exactly
 * one thread frees each context, so there is no double free.
 */
static void tearDownContextChain(GLXContext ctx)
{
    while (ctx) {
        GLXContext share = ctx->share;
        tearDownContext(ctx);
        GLXContext next = NULL;
        if (share) {
            pthread_mutex_lock(&glxLock);
            share->shareRefs--;
            if (claimContextTeardownLocked(share))
                next = share;
            pthread_mutex_unlock(&glxLock);
        }
        ctx = next;
    }
}

/* Drop a reference taken on a share context (context-creation failure paths)
 * and reclaim it if its destroy was deferred and that was the last reference.
 */
static void releaseShareRef(GLXContext share)
{
    if (!share)
        return;
    GLXContext dead = NULL;
    pthread_mutex_lock(&glxLock);
    share->shareRefs--;
    if (claimContextTeardownLocked(share))
        dead = share;
    pthread_mutex_unlock(&glxLock);
    if (dead)
        tearDownContextChain(dead);
}

/* Drop a current-use reference reserved by glXMakeContextCurrent (a failed
 * bind, or the duplicate reservation left by rebinding the same context) and
 * reclaim the context if its destroy was deferred and that was the last
 * reference.
 */
static void releaseCurrentRef(GLXContext ctx)
{
    if (!ctx)
        return;
    GLXContext dead = NULL;
    pthread_mutex_lock(&glxLock);
    ctx->currentCount--;
    if (claimContextTeardownLocked(ctx))
        dead = ctx;
    pthread_mutex_unlock(&glxLock);
    if (dead)
        tearDownContextChain(dead);
}

/* Actually reclaim ctx: run gl4es's GLES-object deletes with ctx briefly
 * current, destroy the EGL context, free the record. Reached only once ctx is
 * unreferenced, so currentContext is some other context or none, never ctx.
 */
static void tearDownContext(GLXContext ctx)
{
    const EglApi *egl = eglApi();
    Bool haveEgl = eglIsAvailable() && ctx->egl != EGL_NO_CONTEXT;
    EGLDisplay dpyEgl = haveEgl ? eglDefaultDisplay() : EGL_NO_DISPLAY;

    /* gl4es DeleteGLState issues real GLES deletes (textures, FBOs, programs),
     * so bind ctx surfaceless just for the teardown and restore the thread's
     * previous binding after. Bind ctx's own EGL API first, and the restored
     * context's API before rebinding it, since desktop-GL and GLES contexts can
     * mix. If ctx cannot be made current, the eglDestroyContext below still
     * reclaims the objects.
     */
    if (gl4esDeleteState && ctx->gl4esState) {
        if (!haveEgl) {
            gl4esDeleteState(ctx->gl4esState);
        } else {
            egl->bindAPI(ctx->apiType);
            if (egl->makeCurrent(dpyEgl, EGL_NO_SURFACE, EGL_NO_SURFACE,
                                 ctx->egl)) {
                gl4esDeleteState(ctx->gl4esState);
                if (currentContext)
                    egl->bindAPI(currentContext->apiType);
                egl->makeCurrent(
                    dpyEgl, currentDrawSurface, currentReadSurface,
                    currentContext ? currentContext->egl : EGL_NO_CONTEXT);
            } else {
                gl4esDeleteState(ctx->gl4esState);
            }
        }
    }
    if (haveEgl)
        egl->destroyContext(dpyEgl, ctx->egl);
    free(ctx);
}

void glXDestroyContext(Display *dpy, GLXContext ctx)
{
    (void) dpy;
    if (!ctx)
        return;

    /* Mark the destroy first, then drop this thread's own current reference. A
     * different thread that still has ctx current, or a child that still shares
     * its objects, keeps it alive until they release; the last release claims
     * and reclaims it (GLX deferred destroy). Destroying a context another
     * thread has current is otherwise undefined, matching the drawable-teardown
     * policy in glxDrawableDestroyed.
     */
    pthread_mutex_lock(&glxLock);
    ctx->destroyPending = True;
    pthread_mutex_unlock(&glxLock);

    GLXContext dead;
    if (currentContext == ctx) {
        if (eglIsAvailable())
            eglApi()->makeCurrent(eglDefaultDisplay(), EGL_NO_SURFACE,
                                  EGL_NO_SURFACE, EGL_NO_CONTEXT);
        currentDisplay = NULL;
        currentDrawable = None;
        currentReadDrawable = None;
        currentDrawSurface = EGL_NO_SURFACE;
        currentReadSurface = EGL_NO_SURFACE;
        dead = clearThreadCurrentContext();
    } else {
        pthread_mutex_lock(&glxLock);
        dead = claimContextTeardownLocked(ctx) ? ctx : NULL;
        pthread_mutex_unlock(&glxLock);
    }
    if (dead)
        tearDownContextChain(dead);
}

/* Find or lazily create the EGL surface backing an X drawable. Phase 1 uses an
 * offscreen pbuffer sized to the drawable (headless path); the on-screen
 * CAMetalLayer window surface is Phase 2.
 *
 * Known Phase 1 limitation: surfaces are not torn down when the X drawable is
 * destroyed (that needs XDestroyWindow integration), so an XID that is
 * destroyed and reused would map to a stale surface. Bounded by
 * MAX_DRAWABLE_SURFACES; the single-window first cut never hits this.
 * Return the index of drawable in the surface table, or -1 if absent. Caller
 * must hold glxLock; this is a pure read and makes no provider call.
 */
static int findDrawableLocked(GLXDrawable drawable)
{
    for (int i = 0; i < drawableSurfaceCount; i++) {
        if (drawableSurfaces[i].drawable == drawable)
            return i;
    }
    return -1;
}

/* Register (drawable -> surface, size) race-safely. If another thread already
 * registered a surface for the same drawable (a create race), destroy ours and
 * return theirs; if the table is full, destroy ours and return EGL_NO_SURFACE.
 * A drawable therefore maps to exactly one surface. Called after the surface is
 * created, outside any provider call held under the lock.
 */
static EGLSurface registerDrawableSurface(GLXDrawable drawable,
                                          EGLSurface surface,
                                          unsigned int width,
                                          unsigned int height,
                                          Bool onScreen)
{
    EGLSurface result = surface;
    pthread_mutex_lock(&glxLock);
    int idx = findDrawableLocked(drawable);
    if (idx >= 0)
        result =
            drawableSurfaces[idx].surface; /* another thread won the race */
    else if (drawableSurfaceCount < MAX_DRAWABLE_SURFACES)
        drawableSurfaces[drawableSurfaceCount++] =
            (DrawableSurface) {drawable, surface, width, height, onScreen};
    else
        result = EGL_NO_SURFACE; /* table full */
    pthread_mutex_unlock(&glxLock);

    /* Destroy our surface unless it is the one now installed in the table. The
     * provider call stays outside the lock.
     */
    if (result != surface)
        eglApi()->destroySurface(eglDefaultDisplay(), surface);
    return result;
}

/* Drop a window drawable's cached surface if the window has been resized since
 * the surface was created, so the next surfaceForDrawable rebuilds it at the
 * new size. Pbuffers have a fixed size and no X geometry, so they are left
 * alone. The provider call stays outside the lock.
 *
 * This destroys the stale surface before the replacement is created; if the
 * rebuild then fails (provider OOM), the drawable is simply left without a
 * cached surface and the following surfaceForDrawable/make-current retries.
 * That is an accepted degradation, not a leak.
 */
static void refreshDrawableSurfaceIfResized(Display *dpy, GLXDrawable drawable)
{
    if (drawable == None || GET_XID_TYPE(drawable) == GLX_PBUFFER_RESOURCE)
        return;

    Window root;
    int x, y;
    unsigned int width, height, borderWidth, depth;
    if (!XGetGeometry(dpy, drawable, &root, &x, &y, &width, &height,
                      &borderWidth, &depth))
        return;
    if (width == 0)
        width = 1;
    if (height == 0)
        height = 1;

    EGLSurface stale = EGL_NO_SURFACE;
    pthread_mutex_lock(&glxLock);
    int idx = findDrawableLocked(drawable);
    if (idx >= 0 && (drawableSurfaces[idx].width != width ||
                     drawableSurfaces[idx].height != height)) {
        stale = drawableSurfaces[idx].surface;
        drawableSurfaces[idx] = drawableSurfaces[--drawableSurfaceCount];
    }
    pthread_mutex_unlock(&glxLock);
    if (stale != EGL_NO_SURFACE)
        eglApi()->destroySurface(eglDefaultDisplay(), stale);
}

/* Try to create an ON-SCREEN EGL window surface for a mapped X window by
 * handing ANGLE the window's CAMetalLayer (macOS).
 *
 * Returns EGL_NO_SURFACE when the drawable is not a presentable window
 * (headless / dummy SDL driver / a pbuffer XID / no Metal layer), so the caller
 * falls back to an offscreen pbuffer.
 */
#if defined(__APPLE__) && SDL_VERSION_ATLEAST(2, 0, 14)
/* Pin the window's CAMetalLayer to a 1:1 point-to-pixel drawable.
 *
 * The SDL window is created with SDL_WINDOW_ALLOW_HIGHDPI (window.c) so the 2D
 * path can render sharp on Retina. That makes the Metal layer's drawable 2x on
 * a 2x display. A GLX client, though, learns its size through X (XGetGeometry
 * and ConfigureNotify report points, the size everything else in this layer
 * uses) and calls glViewport with that, so on Retina it would viewport a
 * 300x300 point window into the bottom-left quarter of a 600x600 pixel
 * framebuffer and the scene renders quarter-size in the corner. X11 has no
 * notion of display scaling, so the correct emulation is a framebuffer that
 * matches the point-sized window the client sees. Set contentsScale to 1.0 and
 * pin drawableSize to the window's point size; ANGLE then sizes its default
 * framebuffer to match glViewport and the scene fills the window (at logical
 * resolution, i.e. slightly soft on Retina, consistent with the 1x X11 backing
 * the 2D path also uses).
 */
static void pinMetalLayerToPointSize(void *layer, SDL_Window *sdlWindow)
{
    int w = 0, h = 0;
    SDL_GetWindowSize(sdlWindow, &w, &h);
    if (w <= 0 || h <= 0)
        return;
    ((void (*)(void *, void *, double)) objc_msgSend)(
        layer, sel_registerName("setContentsScale:"), 1.0);
    /* CGSize is two CGFloats (doubles on 64-bit); passed by value. */
    struct CGSize {
        double width;
        double height;
    } size = {(double) w, (double) h};
    ((void (*)(void *, void *, struct CGSize)) objc_msgSend)(
        layer, sel_registerName("setDrawableSize:"), size);
}
#endif

static EGLSurface createOnScreenWindowSurface(GLXDrawable drawable,
                                              EGLConfig config)
{
    /* SDL_Metal_CreateView is a 2.0.12 addition but SDL_Metal_GetLayer (used
     * below) landed in 2.0.14, so gate on 2.0.14 (node11 ships 2.0.10). Gate on
     * both Apple and the version so a macOS SDL2 build older than 2.0.14 falls
     * cleanly to the offscreen pbuffer path below instead of failing to compile
     * or link. SDL3 satisfies the version test.
     */
#if defined(__APPLE__) && SDL_VERSION_ATLEAST(2, 0, 14)
    if (GET_XID_TYPE(drawable) != WINDOW)
        return EGL_NO_SURFACE;
    WindowStruct *ws = GET_WINDOW_STRUCT(drawable);
    if (!ws || !ws->sdlWindow)
        return EGL_NO_SURFACE;
    /* Create the Metal view once per window and cache it (destroyed in
     * destroyWindow). Reusing it across surface rebuilds avoids stacking
     * CAMetalLayers or leaking views on resize. Note: view/layer creation is
     * macOS UI work; callers must drive GLX for a given window from the thread
     * that owns its SDL window (the demo binds on the main thread).
     */
    Bool createdView = False;
    if (!ws->glxMetalView) {
        SDL_MetalView view = SDL_Metal_CreateView(ws->sdlWindow);
        if (!view)
            return EGL_NO_SURFACE;
        ws->glxMetalView = view;
        createdView = True;
    }
    void *layer = SDL_Metal_GetLayer((SDL_MetalView) ws->glxMetalView);
    if (layer)
        pinMetalLayerToPointSize(layer, ws->sdlWindow);
    EGLSurface surface =
        layer ? eglApi()->createWindowSurface(eglDefaultDisplay(), config,
                                              (EGLNativeWindowType) layer, NULL)
              : EGL_NO_SURFACE;
    /* A provider that cannot back a window surface from the CAMetalLayer
     * (surfaceless Mesa renders to pbuffers, not Metal) leaves the window on
     * the offscreen pbuffer + composite path. Tear down the Metal view we just
     * created so it does not suppress the 2D software present: drawing.c skips
     * that present only when glxMetalView is set, on the invariant that a set
     * view means Metal truly owns the window. A pre-existing view (ANGLE reuse,
     * where the surface does succeed) is left intact.
     */
    if (surface == EGL_NO_SURFACE && createdView) {
        SDL_Metal_DestroyView((SDL_MetalView) ws->glxMetalView);
        ws->glxMetalView = NULL;
    }
    return surface;
#elif defined(__linux__)
    /* On Linux the provider (Mesa) is an X11 EGL platform, so hand it the SDL
     * window's native X11 window for an on-screen surface. sdlX11WindowHandle
     * returns 0 for a non-X11 (dummy driver) window, in which case the caller
     * falls back to an offscreen pbuffer.
     */
    if (GET_XID_TYPE(drawable) != WINDOW)
        return EGL_NO_SURFACE;
    WindowStruct *ws = GET_WINDOW_STRUCT(drawable);
    if (!ws || !ws->sdlWindow)
        return EGL_NO_SURFACE;
    unsigned long xwin = sdlX11WindowHandle(ws->sdlWindow);
    if (!xwin)
        return EGL_NO_SURFACE;
    return eglApi()->createWindowSurface(eglDefaultDisplay(), config,
                                         (EGLNativeWindowType) xwin, NULL);
#else
    (void) drawable;
    (void) config;
    return EGL_NO_SURFACE;
#endif
}

static EGLSurface surfaceForDrawable(Display *dpy,
                                     GLXDrawable drawable,
                                     EGLConfig config)
{
    pthread_mutex_lock(&glxLock);
    int idx = findDrawableLocked(drawable);
    EGLSurface existing =
        (idx >= 0) ? drawableSurfaces[idx].surface : EGL_NO_SURFACE;
    Bool full = drawableSurfaceCount >= MAX_DRAWABLE_SURFACES;
    pthread_mutex_unlock(&glxLock);
    if (existing != EGL_NO_SURFACE)
        return existing;
    if (full)
        return EGL_NO_SURFACE;

    Window root;
    int x, y;
    unsigned int width, height, borderWidth, depth;
    if (!XGetGeometry(dpy, drawable, &root, &x, &y, &width, &height,
                      &borderWidth, &depth)) {
        /* An invalid drawable must not silently become a 1x1 surface; fail so
         * glXMakeCurrent reports the error instead of binding to a bad XID.
         */
        return EGL_NO_SURFACE;
    }
    if (width == 0)
        width = 1;
    if (height == 0)
        height = 1;

    /* Prefer an on-screen window surface (a top-level window presents to the
     * display); fall back to an offscreen pbuffer headless, or for an embedded
     * child window that has no native surface of its own (its rendered pixels
     * are read back and composited into its parent at swap time).
     */
    Bool onScreen = True;
    EGLSurface surface = createOnScreenWindowSurface(drawable, config);
    if (surface == EGL_NO_SURFACE) {
        onScreen = False;
        const EGLint pbufferAttribs[] = {EGL_WIDTH, (EGLint) width, EGL_HEIGHT,
                                         (EGLint) height, EGL_NONE};
        surface = eglApi()->createPbufferSurface(eglDefaultDisplay(), config,
                                                 pbufferAttribs);
    }
    if (surface == EGL_NO_SURFACE)
        return EGL_NO_SURFACE;

    return registerDrawableSurface(drawable, surface, width, height, onScreen);
}

/* GLES resolver handed to a desktop-GL translation layer (gl4es): it must call
 * the real GLES entry points, which are the provider's (ANGLE), never the
 * public gl* the translation layer itself exports. Routing through the
 * provider's getProcAddress keeps those distinct even when the layer is
 * statically linked into the client (where a bare glClear would otherwise bind
 * to the layer's own exported glClear and recurse).
 */
static void *glTranslationGlesResolver(const char *name)
{
    if (!eglIsAvailable())
        return NULL;
    return (void *) eglApi()->getProcAddress(name);
}

/* If a desktop-GL to GLES translation layer (gl4es) is present in the process,
 * point it at the provider's GLES resolver and initialize it once a GLES
 * context is current. The layer sets up per-context state on first init and
 * needs a live context to query, so this runs after eglMakeCurrent, not at
 * library load. Both hooks are soft-resolved via dlsym so the layer stays
 * optional: a plain GLES/ANGLE client that never links it is unaffected.
 * set_getprocaddress just stores a pointer and initialize_gl4es guards against
 * re-entry, so calling both on every make-current is safe.
 */
static void resolveGl4esHooks(void)
{
    gl4esSetResolver = (void (*)(void *(*) (const char *) )) dlsym(
        RTLD_DEFAULT, "set_getprocaddress");
    gl4esInit = (void (*)(void)) dlsym(RTLD_DEFAULT, "initialize_gl4es");
    gl4esNewState =
        (void *(*) (void *, int) ) dlsym(RTLD_DEFAULT, "NewGLState");
    gl4esActivateState =
        (void (*)(void *)) dlsym(RTLD_DEFAULT, "ActivateGLState");
    gl4esDeleteState = (void (*)(void *)) dlsym(RTLD_DEFAULT, "DeleteGLState");
}

static void initGlTranslationLayerIfPresent(void)
{
    /* Resolve the hooks once. pthread_once, not a plain flag, so a concurrent
     * first glXMakeCurrent cannot observe a half-published pointer set.
     */
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, resolveGl4esHooks);
    if (gl4esSetResolver)
        gl4esSetResolver(glTranslationGlesResolver);
    if (gl4esInit)
        gl4esInit();
}

/* Give ctx its own gl4es state (created lazily, once gl4es is initialized and a
 * context is current) and make it the active one. Recurses to give a shared
 * context its state first so object sharing is wired correctly. No-op without
 * gl4es. Must run after initGlTranslationLayerIfPresent (which initializes
 * gl4es and needs a live GLES context to probe).
 */
static void activateGl4esContextState(GLXContext ctx)
{
    if (!gl4esNewState)
        return;
    if (!ctx->gl4esState) {
        void *shared = NULL;
        if (ctx->share) {
            activateGl4esContextState(ctx->share);
            shared = ctx->share->gl4esState;
        }
        ctx->gl4esState =
            gl4esNewState(shared, ctx->apiType == EGL_OPENGL_ES_API ? 1 : 0);
    }
    if (gl4esActivateState && ctx->gl4esState)
        gl4esActivateState(ctx->gl4esState);
}

Bool glXMakeContextCurrent(Display *dpy,
                           GLXDrawable draw,
                           GLXDrawable read,
                           GLXContext ctx)
{
    if (!eglLoad())
        return False;

    const EglApi *egl = eglApi();
    if (!ctx || draw == None) {
        /* Release. Only touch the provider if it was ever initialized: a client
         * that unbinds defensively before any render must not trigger the
         * deferred eglInitialize (the whole point of the lazy path) just to
         * clear an already-empty binding. eglDefaultDisplay() would force it,
         * so gate on eglIsAvailable + an actual current context.
         */
        if (currentContext && eglIsAvailable())
            egl->makeCurrent(eglDefaultDisplay(), EGL_NO_SURFACE,
                             EGL_NO_SURFACE, EGL_NO_CONTEXT);
        currentDisplay = NULL;
        currentDrawable = None;
        currentReadDrawable = None;
        currentDrawSurface = EGL_NO_SURFACE;
        currentReadSurface = EGL_NO_SURFACE;
        GLXContext prev = clearThreadCurrentContext();
        if (prev)
            tearDownContextChain(prev);
        return True;
    }

    /* Reserve a current-use reference before anything touches ctx, including
     * realizing a deferred context, so a concurrent glXDestroyContext cannot
     * free it while this thread installs it. Released on any failure below; on
     * success it becomes this thread's current reference.
     */
    pthread_mutex_lock(&glxLock);
    ctx->currentCount++;
    pthread_mutex_unlock(&glxLock);

    /* First bind of a deferred glXCreateContext: create the EGL context now
     * (this is where the provider's expensive initialize finally lands, after
     * the window is already on screen).
     */
    if (!realizeLazyContext(ctx)) {
        releaseCurrentRef(ctx);
        return False;
    }

    /* Rebuild the surface if the window was resized since it was last bound, so
     * rendering and readback match the current geometry.
     */
    refreshDrawableSurfaceIfResized(dpy, draw);
    if (read != draw && read != None)
        refreshDrawableSurfaceIfResized(dpy, read);

    EGLSurface drawSurface = surfaceForDrawable(dpy, draw, ctx->config);
    if (drawSurface == EGL_NO_SURFACE) {
        releaseCurrentRef(ctx);
        return False;
    }
    EGLSurface readSurface = (read == draw || read == None)
                                 ? drawSurface
                                 : surfaceForDrawable(dpy, read, ctx->config);

    egl->bindAPI(ctx->apiType);
    if (!egl->makeCurrent(eglDefaultDisplay(), drawSurface, readSurface,
                          ctx->egl)) {
        releaseCurrentRef(ctx);
        return False;
    }

    initGlTranslationLayerIfPresent();
    activateGl4esContextState(ctx);

    currentDisplay = dpy;
    currentDrawable = draw;
    currentReadDrawable = (read == None) ? draw : read;
    currentDrawSurface = drawSurface;
    currentReadSurface = readSurface;
    /* Install: the reserved reference above is now this thread's current one,
     * so just let go of the previously-current context. Rebinding the same
     * context leaves the reservation as a duplicate, so drop it.
     */
    GLXContext prev = currentContext;
    currentContext = ctx;
    if (prev == ctx)
        releaseCurrentRef(ctx);
    else
        releaseCurrentRef(prev);
    return True;
}

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx)
{
    return glXMakeContextCurrent(dpy, drawable, drawable, ctx);
}

/* Destroy the EGL surface backing an X drawable and drop its registry entry.
 * Called from the window teardown path (src/window-internal.c) via the hook
 * pointer below, so a destroyed or reused XID cannot map to a stale surface and
 * the surface is not leaked. Named glxDrawableDestroyed (lowercase) so it is an
 * internal hook, not part of the public glX* surface. No-op when nothing is
 * bound to drawable.
 *
 * If the calling thread has the drawable current it is unbound first (see
 * below). Destroying a drawable that ANOTHER thread still has current is an
 * application error (destroying a bound EGL surface is undefined per EGL); this
 * cannot reach across threads to unbind, it only drops the process-wide
 * registry mapping, which is thread-safe.
 */
static void glxDrawableDestroyed(GLXDrawable drawable)
{
    if (!eglIsAvailable())
        return;
    EGLSurface surface = EGL_NO_SURFACE;
    pthread_mutex_lock(&glxLock);
    for (int i = 0; i < drawableSurfaceCount; i++) {
        if (drawableSurfaces[i].drawable == drawable) {
            surface = drawableSurfaces[i].surface;
            drawableSurfaces[i] = drawableSurfaces[--drawableSurfaceCount];
            break;
        }
    }
    pthread_mutex_unlock(&glxLock);
    if (surface == EGL_NO_SURFACE)
        return;

    /* If THIS thread has the drawable current, unbind before destroying the
     * surface so its context is not left bound to a destroyed surface
     * (undefined in EGL). This only ever touches the calling thread's own
     * __thread state, which is always safe. If a DIFFERENT thread has it
     * current we cannot reach that thread's binding; destroying a drawable
     * another thread still has current is an application error, as noted above.
     */
    if (currentDrawable == drawable || currentReadDrawable == drawable) {
        eglApi()->makeCurrent(eglDefaultDisplay(), EGL_NO_SURFACE,
                              EGL_NO_SURFACE, EGL_NO_CONTEXT);
        currentDisplay = NULL;
        currentDrawable = None;
        currentReadDrawable = None;
        currentDrawSurface = EGL_NO_SURFACE;
        currentReadSurface = EGL_NO_SURFACE;
        GLXContext prev = clearThreadCurrentContext();
        if (prev)
            tearDownContextChain(prev);
    }
    eglApi()->destroySurface(eglDefaultDisplay(), surface);
}

/* Register the teardown hook with the always-present window layer at load, so a
 * destroyed X drawable frees its EGL surface (see src/window-internal.c). When
 * GLX=0 this whole file is absent and the hook pointer there stays NULL.
 */
extern void (*glxDrawableDestroyedHook)(GLXDrawable drawable);
__attribute__((constructor)) static void glxRegisterHooks(void)
{
    glxDrawableDestroyedHook = glxDrawableDestroyed;
}

static void compositeOffscreenWindow(GLXDrawable window,
                                     unsigned int w,
                                     unsigned int h);

void glXSwapBuffers(Display *dpy, GLXDrawable drawable)
{
    if (!eglLoad())
        return;
    const EglApi *egl = eglApi();

    EGLSurface surface = EGL_NO_SURFACE;
    if (drawable == currentDrawable && currentContext) {
        /* Swap is the per-frame checkpoint for a render loop that binds once
         * and then loops render+swap: pick up a window resize here and rebind
         * the rebuilt surface, otherwise the loop never sees the new size.
         * Rebind ONLY when the surface actually differs from what the context
         * is bound to (a resize rebuilt it, or it was destroyed and recreated
         * out of band) so the steady-state per-frame path skips a redundant
         * eglMakeCurrent, while a changed surface still self-heals. Comparing
         * surfaces, not just a resize flag, keeps the context from being left
         * current on a freed one.
         */
        refreshDrawableSurfaceIfResized(dpy, drawable);
        surface = surfaceForDrawable(dpy, drawable, currentContext->config);
        EGLSurface readSurface = surface;
        if (currentReadDrawable != drawable) {
            refreshDrawableSurfaceIfResized(dpy, currentReadDrawable);
            readSurface = surfaceForDrawable(dpy, currentReadDrawable,
                                             currentContext->config);
        }
        if (surface == EGL_NO_SURFACE || readSurface == EGL_NO_SURFACE) {
            egl->makeCurrent(eglDefaultDisplay(), EGL_NO_SURFACE,
                             EGL_NO_SURFACE, EGL_NO_CONTEXT);
            currentDisplay = NULL;
            currentDrawable = None;
            currentReadDrawable = None;
            currentDrawSurface = EGL_NO_SURFACE;
            currentReadSurface = EGL_NO_SURFACE;
            GLXContext prev = clearThreadCurrentContext();
            if (prev)
                tearDownContextChain(prev);
            return;
        }
        if (surface != currentDrawSurface ||
            readSurface != currentReadSurface) {
            egl->bindAPI(currentContext->apiType);
            if (egl->makeCurrent(eglDefaultDisplay(), surface, readSurface,
                                 currentContext->egl)) {
                currentDrawSurface = surface;
                currentReadSurface = readSurface;
            } else {
                /* Rebind failed: drop current state so a later make-current
                 * retries rather than leaving TLS claiming a surface it is not
                 * actually bound to. Unbind EGL first so a deferred-destroy
                 * context is not reclaimed while still current on this thread.
                 */
                egl->makeCurrent(eglDefaultDisplay(), EGL_NO_SURFACE,
                                 EGL_NO_SURFACE, EGL_NO_CONTEXT);
                currentDisplay = NULL;
                currentDrawable = None;
                currentReadDrawable = None;
                currentDrawSurface = EGL_NO_SURFACE;
                currentReadSurface = EGL_NO_SURFACE;
                GLXContext prev = clearThreadCurrentContext();
                if (prev)
                    tearDownContextChain(prev);
                return;
            }
        }
    } else {
        /* Swapping a drawable this thread does not have current. The surface is
         * read out under the lock, but the swap itself runs unlocked: swapping
         * a drawable while another thread destroys it is undefined per EGL, the
         * same application-error contract glxDrawableDestroyed documents. A
         * single-threaded client (every real workload) cannot hit that race.
         */
        pthread_mutex_lock(&glxLock);
        int idx = findDrawableLocked(drawable);
        if (idx >= 0)
            surface = drawableSurfaces[idx].surface;
        pthread_mutex_unlock(&glxLock);
    }
    if (surface != EGL_NO_SURFACE)
        egl->swapBuffers(eglDefaultDisplay(), surface);

    /* Embedded GL child widget: a WINDOW backed by an offscreen pbuffer (it is
     * not a top-level window, so it has no native surface). Read the rendered
     * frame back and composite it into the window so it appears in its toolkit
     * shell. Only reachable on the fast path, where the drawable is current and
     * can be read back.
     */
    if (drawable == currentDrawable && GET_XID_TYPE(drawable) == WINDOW) {
        unsigned int w = 0, h = 0;
        Bool offscreen = False;
        pthread_mutex_lock(&glxLock);
        int idx = findDrawableLocked(drawable);
        if (idx >= 0 && !drawableSurfaces[idx].onScreen) {
            offscreen = True;
            w = drawableSurfaces[idx].width;
            h = drawableSurfaces[idx].height;
        }
        pthread_mutex_unlock(&glxLock);
        if (offscreen && w > 0 && h > 0)
            compositeOffscreenWindow(drawable, w, h);
    }
}

/* Read back an offscreen (pbuffer-backed) window's current frame and hand it to
 * the compositor. glReadPixels is core GL, so it is provider- or
 * process-resolved once; its GL enum arguments are spelled out at the call to
 * avoid pulling in a GL header.
 */
typedef void (*GlReadPixelsFn)(int x,
                               int y,
                               int width,
                               int height,
                               unsigned int format,
                               unsigned int type,
                               void *pixels);
typedef void (*GlPixelStoreiFn)(unsigned int pname, int param);

/* Readback entry points, resolved once. A concurrent first swap on two windows
 * would race a plain "static Bool resolved" flag (a second thread can observe
 * the flag set before the pointers are stored, then read null and drop a
 * frame), so the resolve runs under pthread_once.
 */
static GlReadPixelsFn compositeReadPixels = NULL;
static GlPixelStoreiFn compositePixelStorei = NULL;
static void (*compositeFlush)(void) = NULL;
static pthread_once_t compositeResolveOnce = PTHREAD_ONCE_INIT;

static void resolveCompositeReadback(void)
{
    /* A statically-linked desktop-GL translation layer (gl4es), probed by its
     * init symbol. Its public gl* are process-global, so dlsym(RTLD_DEFAULT)
     * resolves them. The bare-dlsym fallback below is unsafe only on macOS: the
     * process may have Apple's system libGL loaded (via SDL), whose
     * glReadPixels/glFlush crash when called with no CGL context, so there we
     * trust a process-global GL symbol only when gl4es is actually present. On
     * other platforms the system GL is a real driver, so the fallback is always
     * safe and is needed when a provider's eglGetProcAddress returns null for
     * core GL (legal before EGL 1.5).
     */
    Bool haveGl4es = dlsym(RTLD_DEFAULT, "initialize_gl4es") != NULL;
    Bool systemGlSafe = haveGl4es;
#ifndef __APPLE__
    systemGlSafe = True;
#endif

    /* Read pixels through the provider (ANGLE reads its own bound framebuffer,
     * which is where gl4es rendered); fall back to a process-global
     * glReadPixels only when the provider does not export one and that is safe.
     * Left null when neither supplies it, so compositeOffscreenWindow skips the
     * readback rather than crashing.
     */
    compositeReadPixels =
        (GlReadPixelsFn) eglApi()->getProcAddress("glReadPixels");
    if (!compositeReadPixels && systemGlSafe)
        compositeReadPixels =
            (GlReadPixelsFn) dlsym(RTLD_DEFAULT, "glReadPixels");

    /* glReadPixels honors GL_PACK_ALIGNMENT; we force it to 4 before each read
     * (see compositeOffscreenWindow) so a client that set 8 cannot pad rows
     * past the tightly packed readback buffer. Resolved from the same source as
     * the read so it targets the same context state.
     */
    compositePixelStorei =
        (GlPixelStoreiFn) eglApi()->getProcAddress("glPixelStorei");
    if (!compositePixelStorei && systemGlSafe)
        compositePixelStorei =
            (GlPixelStoreiFn) dlsym(RTLD_DEFAULT, "glPixelStorei");

    /* gl4es batches immediate-mode and fixed-function draws and only emits them
     * to real GLES on a flush. It flushes inside its own glXSwapBuffers, but
     * the client calls ours, so a sparse frame (one glRectf) can still be in
     * the batch when we read back. When the layer is linked, drain it with its
     * public glFlush (gl4es_glFlush emits the batch; the provider's raw glFlush
     * would not). glFlush, not glFinish: the glReadPixels below already blocks
     * for the read, so the extra GPU sync buys nothing. With no layer, the
     * provider's own glFlush (or nothing) is correct and safe.
     */
    if (haveGl4es)
        compositeFlush = (void (*)(void)) dlsym(RTLD_DEFAULT, "glFlush");
    if (!compositeFlush)
        compositeFlush = (void (*)(void)) eglApi()->getProcAddress("glFlush");
}

static void compositeOffscreenWindow(GLXDrawable window,
                                     unsigned int w,
                                     unsigned int h)
{
    pthread_once(&compositeResolveOnce, resolveCompositeReadback);
    GlReadPixelsFn readPixels = compositeReadPixels;
    if (!readPixels)
        return;
    /* Reject at 32768, not above it: the largest accepted frame is 32767x32767,
     * so need = w * h * 4 stays below 2^32 and the multiply cannot wrap even a
     * 32-bit size_t. The dimensions also stay within int for the readback call.
     */
    if (w >= 32768 || h >= 32768)
        return;
    if (compositeFlush)
        compositeFlush();
    /* glReadPixels writes rows padded to GL_PACK_ALIGNMENT; force 4 so a client
     * that raised it to 8 cannot overrun the tightly packed w*h*4 buffer below
     * (each row is w*4 bytes, always 4-aligned).
     */
    if (compositePixelStorei)
        compositePixelStorei(0x0D05 /* GL_PACK_ALIGNMENT */, 4);
    /* Readback buffer is cached on the window (grown on demand) so an animating
     * widget does no per-frame allocation and the buffer is freed with the
     * window rather than leaked per thread. This runs on the thread holding the
     * current context, which for our single-threaded toolkits is the same
     * thread that owns the window, so the cache needs no extra lock (a fully
     * threaded client is the deferred T1/T2 work).
     */
    size_t need = (size_t) w * h * 4;
    unsigned char *buf = glxAcquireCompositeReadback(window, need);
    if (!buf)
        return;
    readPixels(0, 0, (int) w, (int) h, 0x1908 /* GL_RGBA */,
               0x1401 /* GL_UNSIGNED_BYTE */, buf);
    glxCompositeToWindow(window, buf, (int) w, (int) h);
}

/* Set the buffer-swap interval: 0 disables vsync (uncapped), 1 syncs to the
 * display. Maps to eglSwapInterval, which applies to the calling thread's
 * current surface, so honor the request only when drawable is the current one;
 * a call for a non-current drawable cannot be expressed in EGL and is ignored
 * (as is a provider that does not export eglSwapInterval, resolved softly
 * below).
 */
void glXSwapIntervalEXT(Display *dpy, GLXDrawable drawable, int interval)
{
    (void) dpy;
    if (!eglLoad() || !eglApi()->swapInterval)
        return;
    if (drawable != currentDrawable || !currentContext)
        return;
    eglApi()->swapInterval(eglDefaultDisplay(), interval);
}

/* GLX visual-list tokens that stand alone (no following value), used to walk a
 * stored attribute list by structure. Everything else is value-carrying.
 */
static Bool glxAttribIsBoolean(int attrib)
{
    switch (attrib) {
    case GLX_USE_GL:
    case GLX_RGBA:
    case GLX_DOUBLEBUFFER:
    case GLX_STEREO:
        return True;
    default:
        return False;
    }
}

int glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib, int *value)
{
    (void) dpy;
    if (!vis || !value)
        return GLX_BAD_VALUE;
    if (!eglLoad())
        return GLX_NO_EXTENSION;

    /* A few attributes are answered directly without an EGL round-trip. EGL
     * window/pbuffer surfaces are double-buffered, so GLX single-buffer configs
     * are not representable here; report double-buffered.
     */
    switch (attrib) {
    case GLX_USE_GL:
    case GLX_RGBA:
    case GLX_DOUBLEBUFFER:
        *value = 1;
        return 0;
    case GLX_STEREO:
        *value = 0;
        return 0;
    default:
        break;
    }

    VisualConfig vc;
    if (!lookupVisual(vis->visualid, &vc))
        return GLX_BAD_VISUAL;

    EGLint eglAttrib;
    if (!glxSizeAttribToEgl(attrib, &eglAttrib))
        return GLX_BAD_ATTRIBUTE;

    if (vc.lazy) {
        /* The config is not chosen until make-current, so there is no EGLConfig
         * to query. Answer the sized attributes from what the client asked
         * glXChooseVisual for; a token the client did not request reads as 0.
         * Walk the list by GLX structure (boolean tokens take one slot, valued
         * tokens two) so a value equal to some token is not misread.
         * Best-effort: no workload here calls glXGetConfig on a lazy visual.
         */
        *value = 0;
        int i = 0;
        while (i < vc.glxAttribCount && vc.glxAttribs[i] != None) {
            int tok = vc.glxAttribs[i];
            Bool boolean = glxAttribIsBoolean(tok);
            if (tok == attrib) {
                *value = boolean                       ? 1
                         : (i + 1 < vc.glxAttribCount) ? vc.glxAttribs[i + 1]
                                                       : 0;
                break;
            }
            i += boolean ? 1 : 2;
        }
        return 0;
    }

    EGLint eglValue = 0;
    if (!eglApi()->getConfigAttrib(eglDefaultDisplay(), vc.config, eglAttrib,
                                   &eglValue))
        return GLX_BAD_ATTRIBUTE;
    *value = eglValue;
    return 0;
}

GLXContext glXGetCurrentContext(void)
{
    return currentContext;
}

GLXDrawable glXGetCurrentDrawable(void)
{
    return currentDrawable;
}

Display *glXGetCurrentDisplay(void)
{
    return currentDisplay;
}

Bool glXIsDirect(Display *dpy, GLXContext ctx)
{
    (void) dpy;
    /* Everything is direct/in-process in this model, but honor the degrade
     * contract: no provider or no context means no direct context.
     */
    return eglIsAvailable() && ctx ? True : False;
}

void glXWaitGL(void)
{
    /* No-op: a GLX drawable bypasses the 2D pixman path, so there is no X
     * stream to synchronize against.
     */
}

void glXWaitX(void)
{
    /* No-op; see glXWaitGL. */
}

Bool glXQueryExtension(Display *dpy, int *errorBase, int *eventBase)
{
    (void) dpy;
    if (errorBase)
        *errorBase = 0;
    if (eventBase)
        *eventBase = 0;
    return eglLoad() ? True : False;
}

Bool glXQueryVersion(Display *dpy, int *major, int *minor)
{
    (void) dpy;
    if (!eglLoad())
        return False;
    if (major)
        *major = 1;
    if (minor)
        *minor = 4;
    return True;
}

const char *glXQueryExtensionsString(Display *dpy, int screen)
{
    (void) dpy;
    (void) screen;
    /* Advertise only ARB_create_context, which the FBConfig path honors.
     * glXSwapIntervalEXT is provided (resolvable via glXGetProcAddress) but the
     * GLX_EXT_swap_control string is not claimed, since that also implies the
     * per-drawable GLX_SWAP_INTERVAL_EXT/GLX_MAX_SWAP_INTERVAL_EXT queries this
     * layer does not track.
     */
    return eglLoad() ? "GLX_ARB_create_context GLX_ARB_create_context_profile"
                     : "";
}

const char *glXGetClientString(Display *dpy, int name)
{
    (void) dpy;
    /* Client-library metadata, legitimately available even without a provider
     * (this identifies the GLX client, not the rendering backend).
     */
    switch (name) {
    case GLX_VENDOR:
        return "libx11-compat";
    case GLX_VERSION:
        return "1.4";
    case GLX_EXTENSIONS:
        return glXQueryExtensionsString(dpy, 0);
    default:
        /* Unknown name: return an empty string, never NULL. Real clients
         * (glxinfo) strlen the result without a NULL check, so NULL is a crash.
         */
        return "";
    }
}

const char *glXQueryServerString(Display *dpy, int screen, int name)
{
    (void) screen;
    return glXGetClientString(dpy, name);
}

/* Our own glX* entry points, resolvable by name through glXGetProcAddress.
 * Every modern GLX client resolves glXCreateContextAttribsARB (and often the
 * FBConfig calls) dynamically rather than linking them, so they must be
 * discoverable here even when no provider is loaded, matching real GLX which
 * resolves its own client functions regardless of the server. Prototypes come
 * from GL/glx.h, so this table can reference every entry point regardless of
 * definition order.
 */
static const struct {
    const char *name;
    __GLXextFuncPtr func;
} glxProcTable[] = {
    {"glXChooseVisual", (__GLXextFuncPtr) glXChooseVisual},
    {"glXCreateContext", (__GLXextFuncPtr) glXCreateContext},
    {"glXDestroyContext", (__GLXextFuncPtr) glXDestroyContext},
    {"glXMakeCurrent", (__GLXextFuncPtr) glXMakeCurrent},
    {"glXMakeContextCurrent", (__GLXextFuncPtr) glXMakeContextCurrent},
    {"glXSwapBuffers", (__GLXextFuncPtr) glXSwapBuffers},
    {"glXSwapIntervalEXT", (__GLXextFuncPtr) glXSwapIntervalEXT},
    {"glXGetConfig", (__GLXextFuncPtr) glXGetConfig},
    {"glXGetCurrentContext", (__GLXextFuncPtr) glXGetCurrentContext},
    {"glXGetCurrentDrawable", (__GLXextFuncPtr) glXGetCurrentDrawable},
    {"glXGetCurrentDisplay", (__GLXextFuncPtr) glXGetCurrentDisplay},
    {"glXGetCurrentReadDrawable", (__GLXextFuncPtr) glXGetCurrentReadDrawable},
    {"glXIsDirect", (__GLXextFuncPtr) glXIsDirect},
    {"glXWaitGL", (__GLXextFuncPtr) glXWaitGL},
    {"glXWaitX", (__GLXextFuncPtr) glXWaitX},
    {"glXQueryExtension", (__GLXextFuncPtr) glXQueryExtension},
    {"glXQueryVersion", (__GLXextFuncPtr) glXQueryVersion},
    {"glXQueryExtensionsString", (__GLXextFuncPtr) glXQueryExtensionsString},
    {"glXGetClientString", (__GLXextFuncPtr) glXGetClientString},
    {"glXQueryServerString", (__GLXextFuncPtr) glXQueryServerString},
    {"glXGetProcAddress", (__GLXextFuncPtr) glXGetProcAddress},
    {"glXGetProcAddressARB", (__GLXextFuncPtr) glXGetProcAddressARB},
    {"glXChooseFBConfig", (__GLXextFuncPtr) glXChooseFBConfig},
    {"glXGetFBConfigs", (__GLXextFuncPtr) glXGetFBConfigs},
    {"glXGetFBConfigAttrib", (__GLXextFuncPtr) glXGetFBConfigAttrib},
    {"glXGetVisualFromFBConfig", (__GLXextFuncPtr) glXGetVisualFromFBConfig},
    {"glXCreateNewContext", (__GLXextFuncPtr) glXCreateNewContext},
    {"glXCreateContextAttribsARB",
     (__GLXextFuncPtr) glXCreateContextAttribsARB},
    {"glXCreateWindow", (__GLXextFuncPtr) glXCreateWindow},
    {"glXDestroyWindow", (__GLXextFuncPtr) glXDestroyWindow},
    {"glXCreatePbuffer", (__GLXextFuncPtr) glXCreatePbuffer},
    {"glXDestroyPbuffer", (__GLXextFuncPtr) glXDestroyPbuffer},
    {"glXQueryDrawable", (__GLXextFuncPtr) glXQueryDrawable},
};

__GLXextFuncPtr glXGetProcAddress(const unsigned char *procName)
{
    if (!procName)
        return NULL;
    const char *name = (const char *) procName;

    /* glX* client entry points resolve to our own implementations, provider or
     * not. A glX* name we do not implement returns NULL rather than falling
     * through to the provider: the EGL provider has no glX functions, so
     * handing it a glX* string could only return a foreign/mismatched pointer.
     */
    if (!strncmp(name, "glX", 3)) {
        for (size_t i = 0; i < sizeof(glxProcTable) / sizeof(glxProcTable[0]);
             i++) {
            if (!strcmp(name, glxProcTable[i].name))
                return glxProcTable[i].func;
        }
        return NULL;
    }

    /* Everything else (gl* entry points) comes from the provider. */
    if (!eglLoad())
        return NULL;
    __GLXextFuncPtr fn = (__GLXextFuncPtr) eglApi()->getProcAddress(name);
    if (!fn) {
        /* eglGetProcAddress on EGL 1.4 (and some 1.5) returns NULL for CORE GL
         * functions, only extensions, so a loader like GLEW/libepoxy/GLAD that
         * resolves glClear/glDrawArrays through glXGetProcAddress gets nothing.
         * Fall back to the process image, where the linked libGLESv2 (or gl4es)
         * exports the core entry points. On macOS that fallback is unsafe
         * unless gl4es is present: Apple's system OpenGL (pulled in via SDL)
         * exports gl* that crash when called with no CGL context, so gate on
         * gl4es there (the same rule the composite readback resolver uses).
         */
        Bool systemGlSafe = dlsym(RTLD_DEFAULT, "initialize_gl4es") != NULL;
#ifndef __APPLE__
        systemGlSafe = True;
#endif
        if (systemGlSafe)
            fn = (__GLXextFuncPtr) dlsym(RTLD_DEFAULT, name);
    }
    return fn;
}

__GLXextFuncPtr glXGetProcAddressARB(const unsigned char *procName)
{
    return glXGetProcAddress(procName);
}

/* FBConfig path. GLXFBConfig wraps an EGLConfig. glXChooseFBConfig takes a
 * token+value attribute list terminated by None (unlike glXChooseVisual's mixed
 * boolean/valued list).
 */
static int translateFBConfigAttribs(const int *glx, EGLint *egl, size_t cap)
{
    size_t n = 0;
    const int *p = glx;
    for (int i = 0; p && i < MAX_ATTRIB_TOKENS && *p != None; i++, p += 2) {
        EGLint eglTok;
        /* Sized token maps to an EGL attribute; drawable/render-type and
         * friends have no analog and are covered by EGL defaults.
         */
        if (glxSizeAttribToEgl(*p, &eglTok) &&
            !pushEglAttrib(egl, cap, &n, eglTok, p[1]))
            return -1;
    }
    return finishEglAttribs(egl, n, cap);
}

GLXFBConfig *glXChooseFBConfig(Display *dpy,
                               int screen,
                               const int *attribList,
                               int *nitems)
{
    (void) dpy;
    if (nitems)
        *nitems = 0;
    if (!eglLoad())
        return NULL;

    EGLint eglAttribs[64];
    int attribCount = translateFBConfigAttribs(attribList, eglAttribs, 64);
    if (attribCount < 0)
        return NULL;

    EGLConfig eglConfigs[32];
    EGLint numConfigs =
        chooseEglConfigs(eglAttribs, attribCount, eglConfigs, 32);
    if (numConfigs < 1)
        return NULL;

    GLXFBConfig *out = calloc((size_t) numConfigs, sizeof(*out));
    if (!out)
        return NULL;
    int written = 0;
    for (EGLint i = 0; i < numConfigs; i++) {
        GLXFBConfig handle = internFBConfig(eglConfigs[i], screen);
        if (handle)
            out[written++] = handle; /* stable handle; caller frees only out */
    }
    if (written == 0) {
        free(out);
        return NULL;
    }
    if (nitems)
        *nitems = written;
    return out;
}

GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements)
{
    static const int any[] = {None};
    return glXChooseFBConfig(dpy, screen, any, nelements);
}

int glXGetFBConfigAttrib(Display *dpy,
                         GLXFBConfig config,
                         int attribute,
                         int *value)
{
    (void) dpy;
    if (!config || !value)
        return GLX_BAD_VALUE;
    if (!eglIsAvailable())
        return GLX_NO_EXTENSION;

    switch (attribute) {
    case GLX_RENDER_TYPE:
        *value = GLX_RGBA_BIT;
        return 0;
    case GLX_DRAWABLE_TYPE:
        *value = GLX_WINDOW_BIT | GLX_PBUFFER_BIT;
        return 0;
    case GLX_X_RENDERABLE:
    case GLX_DOUBLEBUFFER:
        *value = 1;
        return 0;
    case GLX_CONFIG_CAVEAT:
    case GLX_TRANSPARENT_TYPE:
        /* No slow/non-conformant caveat and no transparency; both report
         * GLX_NONE so config-enumeration loops that read these do not see
         * BadAttribute.
         */
        *value = GLX_NONE;
        return 0;
    case GLX_X_VISUAL_TYPE:
        *value = GLX_TRUE_COLOR;
        return 0;
    case GLX_FBCONFIG_ID: {
        /* A stable per-config id apps use to cache/compare configs. */
        EGLint id = 0;
        if (!eglApi()->getConfigAttrib(eglDefaultDisplay(), config->egl,
                                       EGL_CONFIG_ID, &id))
            return GLX_BAD_ATTRIBUTE;
        *value = id;
        return 0;
    }
    default:
        break;
    }

    /* GLX_VISUAL_ID is FBConfig-only (glXGetConfig rejects it); the rest share
     * the common sized-attribute map.
     */
    EGLint eglAttrib;
    if (attribute == GLX_VISUAL_ID)
        eglAttrib = EGL_NATIVE_VISUAL_ID;
    else if (!glxSizeAttribToEgl(attribute, &eglAttrib))
        return GLX_BAD_ATTRIBUTE;

    EGLint eglValue = 0;
    if (!eglApi()->getConfigAttrib(eglDefaultDisplay(), config->egl, eglAttrib,
                                   &eglValue))
        return GLX_BAD_ATTRIBUTE;
    *value = eglValue;
    return 0;
}

XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config)
{
    if (!eglLoad() || !config)
        return NULL;
    /* The FBConfig already carries a chosen EGLConfig, so record it eagerly: a
     * glXCreateContext on this visual creates its context immediately rather
     * than deferring (the deferral is only for the classic glXChooseVisual
     * path). Reuse an existing eager visual for the same EGLConfig so an app
     * that maps configs to visuals repeatedly does not exhaust the table.
     */
    pthread_mutex_lock(&glxLock);
    VisualID vid = findEagerVisualLocked(config->egl);
    pthread_mutex_unlock(&glxLock);
    if (!vid) {
        vid = allocSyntheticVisualID();
        if (!rememberVisualConfig(vid, config->egl))
            return NULL;
    }
    return synthesizeVisual(dpy, config->screen, vid);
}

GLXContext glXCreateNewContext(Display *dpy,
                               GLXFBConfig config,
                               int renderType,
                               GLXContext shareList,
                               Bool direct)
{
    (void) dpy;
    (void) renderType;
    if (!eglLoad() || !config)
        return NULL;
    return createContextFromConfig(config->egl, config->screen, direct, 2,
                                   shareList, CTX_API_AUTO);
}

GLXContext glXCreateContextAttribsARB(Display *dpy,
                                      GLXFBConfig config,
                                      GLXContext shareList,
                                      Bool direct,
                                      const int *attribList)
{
    (void) dpy;
    if (!eglLoad() || !config)
        return NULL;

    /* Parse the requested version and profile. An explicit ES2 profile bit
     * forces a GLES context; a desktop core/compatibility profile forces
     * desktop GL, which only a desktop-capable provider (Mesa) can honor
     * (rejected on ANGLE). With no profile, AUTO matches classic
     * glXCreateContext (desktop on Mesa, GLES on ANGLE); a version >=3 selects
     * ES3 on the GLES path.
     */
    int major = 2;
    CtxApiPref pref = CTX_API_AUTO;
    for (int i = 0;
         attribList && i < MAX_ATTRIB_TOKENS && attribList[2 * i] != None;
         i++) {
        int token = attribList[2 * i];
        int val = attribList[2 * i + 1];
        if (token == GLX_CONTEXT_MAJOR_VERSION_ARB) {
            major = val;
        } else if (token == GLX_CONTEXT_PROFILE_MASK_ARB) {
            if (val & GLX_CONTEXT_ES2_PROFILE_BIT_EXT)
                pref = CTX_API_GLES;
            else if (val & (GLX_CONTEXT_CORE_PROFILE_BIT_ARB |
                            GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB)) {
                if (eglProviderClientApi() != EGL_CLIENT_API_DESKTOP)
                    return NULL; /* GLES-only provider cannot do desktop GL */
                pref = CTX_API_DESKTOP;
            }
        }
    }
    int clientVersion = major >= 3 ? 3 : 2;
    return createContextFromConfig(config->egl, config->screen, direct,
                                   clientVersion, shareList, pref);
}

/* GLX 1.3 on-screen drawable. A GLXWindow is modeled as the X window itself:
 * glXMakeCurrent then binds a surface to the real window via its geometry. This
 * is source-compatible (not a distinct GLX resource / separate XID), which is
 * all the compat layer needs.
 */
GLXWindow glXCreateWindow(Display *dpy,
                          GLXFBConfig config,
                          Window win,
                          const int *attribList)
{
    (void) dpy;
    (void) attribList; /* GLX_WINDOW has no attributes the layer models */
    if (!eglLoad() || !config || win == None || GET_XID_TYPE(win) != WINDOW)
        return None;
    return (GLXWindow) win;
}

void glXDestroyWindow(Display *dpy, GLXWindow win)
{
    (void) dpy;
    /* Release the GL surface but keep the X window: the window owns the XID and
     * outlives its GLXWindow. Guard on WINDOW type so a mis-passed pbuffer id
     * is not silently stripped of its surface.
     *
     * The cached CAMetalLayer view (WindowStruct.glxMetalView) is deliberately
     * left attached to the SDL window and torn down only at XDestroyWindow. So
     * a window whose GLXWindow is destroyed keeps being presented through Metal
     * (excluded from the 2D software present) until the X window itself goes
     * away. Reverting a live SDL window from a Metal view back to a software
     * surface is not something SDL supports cleanly, and no workload destroys a
     * GLXWindow only to resume Xlib 2D drawing on the same mapped window; add
     * that revert if one ever does.
     */
    if (win == None || !eglIsAvailable() || GET_XID_TYPE(win) != WINDOW)
        return;
    glxDrawableDestroyed(win);
}

/* GLX 1.3 pbuffer lifecycle. A pbuffer is an offscreen drawable; it maps to an
 * EGL pbuffer surface, keyed by a freshly allocated XID so it can be passed to
 * glXMakeCurrent like any other GLXDrawable. It carries no X window, so it is
 * pre-registered in the surface table and never goes through XGetGeometry.
 */
GLXPbuffer glXCreatePbuffer(Display *dpy,
                            GLXFBConfig config,
                            const int *attribList)
{
    (void) dpy;
    if (!eglLoad() || !config)
        return None;

    unsigned int width = 1, height = 1;
    const int *p = attribList;
    for (int i = 0; p && i < MAX_ATTRIB_TOKENS && *p != None; i++, p += 2) {
        if (p[1] == None) /* malformed: valued token with no value */
            break;
        if (*p == GLX_PBUFFER_WIDTH && p[1] > 0)
            width = (unsigned int) p[1];
        else if (*p == GLX_PBUFFER_HEIGHT && p[1] > 0)
            height = (unsigned int) p[1];
    }

    const EGLint pbufferAttribs[] = {EGL_WIDTH, (EGLint) width, EGL_HEIGHT,
                                     (EGLint) height, EGL_NONE};
    EGLSurface surface = eglApi()->createPbufferSurface(
        eglDefaultDisplay(), config->egl, pbufferAttribs);
    if (surface == EGL_NO_SURFACE)
        return None;

    GLXPbuffer id = ALLOC_XID();
    if (id == None) {
        eglApi()->destroySurface(eglDefaultDisplay(), surface);
        return None;
    }
    SET_XID_TYPE(id, GLX_PBUFFER_RESOURCE);
    /* A real GLX pbuffer is offscreen but must never be composited to a window
     * (it is not a WINDOW drawable); onScreen is moot, pass False.
     */
    if (registerDrawableSurface(id, surface, width, height, False) ==
        EGL_NO_SURFACE) {
        FREE_XID(id);
        return None;
    }
    return id;
}

void glXDestroyPbuffer(Display *dpy, GLXPbuffer pbuf)
{
    (void) dpy;
    /* Guard against a non-pbuffer XID: without this a live window/pixmap id
     * passed here would have its GLX surface torn down and its core XID freed.
     */
    if (pbuf == None || GET_XID_TYPE(pbuf) != GLX_PBUFFER_RESOURCE)
        return;
    /* Reuse the drawable teardown to drop the surface + registry entry, then
     * release the XID (a pbuffer, unlike a window, has no destroyWindow path).
     */
    if (eglIsAvailable())
        glxDrawableDestroyed(pbuf);
    FREE_XID(pbuf);
}

void glXQueryDrawable(Display *dpy,
                      GLXDrawable draw,
                      int attribute,
                      unsigned int *value)
{
    if (!value)
        return;
    *value = 0;
    if (!eglLoad() || draw == None)
        return;

    if (attribute != GLX_WIDTH && attribute != GLX_HEIGHT)
        return; /* other pbuffer attributes are not modeled */

    /* Prefer the live EGL surface's own size. For an on-screen window surface a
     * real provider reports the physical drawable in pixels, which is what the
     * client's GL viewport must match; on a HiDPI display that differs from the
     * window's logical X geometry. This unifies the pbuffer and window cases:
     * the surface is the single source of truth once it exists.
     */
    Bool isPbuffer = (GET_XID_TYPE(draw) == GLX_PBUFFER_RESOURCE);
    unsigned int width = 0, height = 0;
    Bool haveSize = False;
    const EglApi *egl = eglApi();

    /* Query the surface while holding glxLock: a concurrent destroy or resize
     * removes the entry from this registry under the same lock before it calls
     * destroySurface, so keeping the lock across the query stops the handle
     * from being freed under us. querySurface is a read-only provider call that
     * never reenters our lock, so this cannot deadlock.
     */
    pthread_mutex_lock(&glxLock);
    int idx = findDrawableLocked(draw);
    if (idx >= 0) {
        EGLSurface surface = drawableSurfaces[idx].surface;
        EGLint qw = 0, qh = 0;
        if (surface != EGL_NO_SURFACE && egl->querySurface &&
            egl->querySurface(eglDefaultDisplay(), surface, EGL_WIDTH, &qw) &&
            egl->querySurface(eglDefaultDisplay(), surface, EGL_HEIGHT, &qh) &&
            qw > 0 && qh > 0) {
            width = (unsigned int) qw;
            height = (unsigned int) qh;
            haveSize = True;
        } else {
            /* Provider could not report a size; fall back to what was stored at
             * surface creation (pbuffer creation size, or window logical size).
             */
            width = drawableSurfaces[idx].width;
            height = drawableSurfaces[idx].height;
            haveSize = (width > 0 && height > 0);
        }
    }
    pthread_mutex_unlock(&glxLock);

    if (!haveSize) {
        /* No registry entry. A pbuffer has no X geometry, so it is simply
         * unknown; a window may just not have been made current yet, so read
         * its current geometry (done outside the lock to avoid nesting X
         * calls).
         */
        if (isPbuffer)
            return;
        Window root;
        int x, y;
        unsigned int bw, depth;
        if (!XGetGeometry(dpy, draw, &root, &x, &y, &width, &height, &bw,
                          &depth))
            return;
    }
    *value = (attribute == GLX_WIDTH) ? width : height;
}

GLXDrawable glXGetCurrentReadDrawable(void)
{
    return currentReadDrawable;
}
