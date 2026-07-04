/*
 * Minimal but validating fake EGL provider for exercising src/glx.c without
 * ANGLE or Mesa.
 *
 * libx11-compat dlopens a libEGL and resolves a fixed set of egl* entry points
 * (see src/egl-wrapper.c). A real provider needs a GPU backend; to validate the
 * GLX->EGL translation, context/surface bookkeeping, and attribute mapping in a
 * provider-less CI, this stub implements those entry points with in-memory
 * state. It renders nothing.
 *
 * It is deliberately NOT permissive: eglChooseConfig parses the translated EGL
 * attribute list and stores the requested per-channel sizes on the config, so
 * eglGetConfigAttrib reads them back. That way a mistranslation in src/glx.c (a
 * swapped channel token, a dropped EGL_RENDERABLE_TYPE/EGL_SURFACE_TYPE tail)
 * is caught rather than masked. Context/surface handles are tracked in live
 * sets so leaks and use-after-free in the layer above surface as failures; the
 * test queries the balance through the exported fake_egl_* helpers.
 *
 * Built as a loadable object, pointed at via LIBX11_COMPAT_EGL by
 * tests/test-glx-provider.c. Modeled GLES-only (eglBindAPI rejects desktop GL),
 * matching the ANGLE capability the layer must handle. Single-threaded test
 * use.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_OPENGL_API 0x30A2
#define EGL_CLIENT_APIS 0x308D
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
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
#define EGL_NATIVE_VISUAL_ID 0x302E
#define EGL_SUCCESS 0x3000
#define EGL_NO_CONTEXT ((EGLContext) 0)
#define EGL_NO_SURFACE ((EGLSurface) 0)
#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057

#define EGL_CONFIG_ID 0x3028
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define FAKE_NATIVE_VISUAL_ID 0x21
#define MAX_CONFIGS 16
#define MAX_LIVE 64

typedef struct {
    EGLint red, green, blue, alpha, depth, stencil, samples, sampleBuffers;
    int used;
} FakeConfig;

/* Single source of truth mapping an EGL channel-size token to its FakeConfig
 * field, shared by eglChooseConfig (store) and eglGetConfigAttrib (read) so the
 * two directions cannot drift apart.
 */
static const struct {
    EGLint token;
    size_t offset;
} channelFields[] = {
    {EGL_RED_SIZE, offsetof(FakeConfig, red)},
    {EGL_GREEN_SIZE, offsetof(FakeConfig, green)},
    {EGL_BLUE_SIZE, offsetof(FakeConfig, blue)},
    {EGL_ALPHA_SIZE, offsetof(FakeConfig, alpha)},
    {EGL_DEPTH_SIZE, offsetof(FakeConfig, depth)},
    {EGL_STENCIL_SIZE, offsetof(FakeConfig, stencil)},
    {EGL_SAMPLES, offsetof(FakeConfig, samples)},
    {EGL_SAMPLE_BUFFERS, offsetof(FakeConfig, sampleBuffers)},
};

/* Return the FakeConfig field for an EGL channel token, or NULL if the token is
 * not a per-channel size.
 */
static EGLint *channelField(FakeConfig *cfg, EGLint token)
{
    for (size_t i = 0; i < sizeof(channelFields) / sizeof(channelFields[0]);
         i++)
        if (channelFields[i].token == token)
            return (EGLint *) ((char *) cfg + channelFields[i].offset);
    return NULL;
}

static int displayToken;
static FakeConfig configs[MAX_CONFIGS];
static int configCount;
static int lastConfig = -1;

/* Live-handle sets so create/destroy imbalance and stale handles are caught. */
static intptr_t liveContexts[MAX_LIVE];
static int contextCount;
static intptr_t liveSurfaces[MAX_LIVE];
static int surfaceCount;
static intptr_t nextHandle = 1;

static EGLContext currentContext;
static EGLSurface currentDraw;
static EGLSurface currentRead;
static long swapCount;
static long
    surfacesCreated; /* monotonic: total ever created, for resize checks */
static long releaseCount;

static int addLive(intptr_t *set, int *count, intptr_t handle)
{
    if (*count >= MAX_LIVE)
        return 0;
    set[(*count)++] = handle;
    return 1;
}

static int removeLive(intptr_t *set, int *count, intptr_t handle)
{
    for (int i = 0; i < *count; i++) {
        if (set[i] == handle) {
            set[i] = set[--(*count)];
            return 1;
        }
    }
    return 0;
}

static int isLive(const intptr_t *set, int count, intptr_t handle)
{
    for (int i = 0; i < count; i++)
        if (set[i] == handle)
            return 1;
    return 0;
}

EGLDisplay eglGetDisplay(EGLNativeDisplayType native)
{
    (void) native;
    return &displayToken;
}

EGLBoolean eglInitialize(EGLDisplay dpy, EGLint *major, EGLint *minor)
{
    (void) dpy;
    if (getenv("FAKE_EGL_FAIL_INITIALIZE"))
        return EGL_FALSE;
    if (major)
        *major = 1;
    if (minor)
        *minor = 5;
    return EGL_TRUE;
}

EGLBoolean eglTerminate(EGLDisplay dpy)
{
    (void) dpy;
    return EGL_TRUE;
}

EGLBoolean eglBindAPI(EGLenum api)
{
    /* GLES-only, like ANGLE: reject desktop GL so the capability probe lands on
     * the GLES path.
     */
    return api == EGL_OPENGL_ES_API ? EGL_TRUE : EGL_FALSE;
}

const char *eglQueryString(EGLDisplay dpy, EGLint name)
{
    (void) dpy;
    switch (name) {
    case EGL_CLIENT_APIS:
        return "OpenGL_ES";
    case EGL_VENDOR:
        return "fake-egl";
    case EGL_VERSION:
        return "1.5 fake";
    default:
        return "";
    }
}

/* Parse the translated EGL attribute list into a config, recording the
 * requested channel sizes. Requires the EGL_RENDERABLE_TYPE and
 * EGL_SURFACE_TYPE tail src/glx.c appends; without it the translation is
 * malformed and the config request is refused.
 */
EGLBoolean eglChooseConfig(EGLDisplay dpy,
                           const EGLint *attribs,
                           EGLConfig *out,
                           EGLint configSize,
                           EGLint *numConfig)
{
    (void) dpy;
    if (numConfig)
        *numConfig = 0;
    if (configCount >= MAX_CONFIGS)
        return EGL_FALSE;

    FakeConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    int sawRenderable = 0, sawSurface = 0;
    /* Bounded like the layer's own parser: a malformed (odd-length) list from a
     * translation bug must fail cleanly, not walk off the end of the array.
     */
    const EGLint *p = attribs;
    for (int i = 0; p && *p != EGL_NONE && i < 256; i++, p += 2) {
        EGLint *field = channelField(&cfg, *p);
        if (field)
            *field = p[1];
        else if (*p == EGL_RENDERABLE_TYPE)
            sawRenderable = 1;
        else if (*p == EGL_SURFACE_TYPE)
            sawSurface = 1;
    }
    if (!sawRenderable || !sawSurface)
        return EGL_FALSE; /* src/glx.c always appends both; a miss is a bug */

    if (configSize < 1)
        return EGL_TRUE; /* query-count form: report zero via numConfig */

    cfg.used = 1;
    configs[configCount] = cfg;
    if (out)
        out[0] = &configs[configCount];
    lastConfig = configCount;
    configCount++;
    if (numConfig)
        *numConfig = 1;
    return EGL_TRUE;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay dpy,
                              EGLConfig config,
                              EGLint attrib,
                              EGLint *value)
{
    (void) dpy;
    FakeConfig *cfg = config;
    if (!cfg || !cfg->used || !value)
        return EGL_FALSE;
    if (attrib == EGL_NATIVE_VISUAL_ID) {
        *value = FAKE_NATIVE_VISUAL_ID;
        return EGL_TRUE;
    }
    if (attrib == EGL_CONFIG_ID) {
        /* Stable non-zero id per config (its slot), like a real provider. */
        *value = (EGLint) (cfg - configs) + 1;
        return EGL_TRUE;
    }
    EGLint *field = channelField(cfg, attrib);
    if (!field)
        return EGL_FALSE;
    *value = *field;
    return EGL_TRUE;
}

static EGLContext lastShareContext = EGL_NO_CONTEXT;
static EGLint lastClientVersion;

EGLContext eglCreateContext(EGLDisplay dpy,
                            EGLConfig config,
                            EGLContext share,
                            const EGLint *attribs)
{
    (void) dpy;
    FakeConfig *cfg = config;
    if (!cfg || !cfg->used)
        return EGL_NO_CONTEXT;
    /* Record what the layer passed so a test can confirm context sharing and
     * the requested client version were plumbed through, not dropped.
     */
    lastShareContext = share;
    lastClientVersion = 0;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE; a += 2)
        if (a[0] == EGL_CONTEXT_CLIENT_VERSION)
            lastClientVersion = a[1];
    intptr_t handle = nextHandle++;
    if (!addLive(liveContexts, &contextCount, handle))
        return EGL_NO_CONTEXT;
    return (EGLContext) handle;
}

/* Test introspection: the share context and client version of the last
 * eglCreateContext, so a test can prove the GLX context-creation entry points
 * forwarded them instead of dropping the share list or version.
 */
int fake_egl_last_share_nonnull(void)
{
    return lastShareContext != EGL_NO_CONTEXT;
}
int fake_egl_last_client_version(void)
{
    return (int) lastClientVersion;
}

EGLBoolean eglDestroyContext(EGLDisplay dpy, EGLContext ctx)
{
    (void) dpy;
    if (currentContext == ctx)
        return EGL_FALSE;
    return removeLive(liveContexts, &contextCount, (intptr_t) ctx) ? EGL_TRUE
                                                                   : EGL_FALSE;
}

/* Per-surface dimensions so eglQuerySurface can report a real size, the way a
 * real provider does. Keyed by the surface handle; a linear scan is fine at
 * this scale. Entries are left in place on destroy (handles are never reused).
 */
static struct {
    intptr_t handle;
    EGLint w, h;
} surfaceDims[MAX_LIVE];
static int surfaceDimCount;

/* Index of the surfaceDims entry for a handle, or -1 if absent. */
static int findSurfaceDim(intptr_t handle)
{
    for (int i = 0; i < surfaceDimCount; i++)
        if (surfaceDims[i].handle == handle)
            return i;
    return -1;
}

static EGLSurface makeSurface(EGLConfig config, EGLint w, EGLint h)
{
    FakeConfig *cfg = config;
    if (!cfg || !cfg->used)
        return EGL_NO_SURFACE;
    intptr_t handle = nextHandle++;
    if (!addLive(liveSurfaces, &surfaceCount, handle))
        return EGL_NO_SURFACE;
    if (surfaceDimCount < MAX_LIVE) {
        surfaceDims[surfaceDimCount].handle = handle;
        surfaceDims[surfaceDimCount].w = w;
        surfaceDims[surfaceDimCount].h = h;
        surfaceDimCount++;
    }
    surfacesCreated++;
    return (EGLSurface) handle;
}

EGLSurface eglCreateWindowSurface(EGLDisplay dpy,
                                  EGLConfig config,
                                  EGLNativeWindowType win,
                                  const EGLint *attribs)
{
    (void) dpy;
    (void) win;
    (void) attribs;
    /* A real window surface takes its size from the native window; the fake has
     * no window, so report 0x0 (headless tests use pbuffers, which carry a
     * size).
     */
    return makeSurface(config, 0, 0);
}

EGLSurface eglCreatePbufferSurface(EGLDisplay dpy,
                                   EGLConfig config,
                                   const EGLint *attribs)
{
    (void) dpy;
    EGLint w = 0, h = 0;
    for (const EGLint *a = attribs; a && a[0] != EGL_NONE; a += 2) {
        if (a[0] == EGL_WIDTH)
            w = a[1];
        else if (a[0] == EGL_HEIGHT)
            h = a[1];
    }
    return makeSurface(config, w, h);
}

EGLBoolean eglQuerySurface(EGLDisplay dpy,
                           EGLSurface surface,
                           EGLint attribute,
                           EGLint *value)
{
    (void) dpy;
    if (!value || !isLive(liveSurfaces, surfaceCount, (intptr_t) surface))
        return EGL_FALSE;
    if (attribute != EGL_WIDTH && attribute != EGL_HEIGHT)
        return EGL_FALSE;
    int idx = findSurfaceDim((intptr_t) surface);
    if (idx < 0)
        return EGL_FALSE;
    *value = (attribute == EGL_WIDTH) ? surfaceDims[idx].w : surfaceDims[idx].h;
    return EGL_TRUE;
}

EGLBoolean eglDestroySurface(EGLDisplay dpy, EGLSurface surface)
{
    (void) dpy;
    if (currentDraw == surface)
        currentDraw = EGL_NO_SURFACE;
    if (currentRead == surface)
        currentRead = EGL_NO_SURFACE;
    /* Drop the dimension entry too, or the table (sized to live surfaces) would
     * fill up after MAX_LIVE total creations and starve eglQuerySurface. Swap
     * with the last entry, the same compaction removeLive uses.
     */
    int idx = findSurfaceDim((intptr_t) surface);
    if (idx >= 0)
        surfaceDims[idx] = surfaceDims[--surfaceDimCount];
    return removeLive(liveSurfaces, &surfaceCount, (intptr_t) surface)
               ? EGL_TRUE
               : EGL_FALSE;
}

EGLBoolean eglMakeCurrent(EGLDisplay dpy,
                          EGLSurface draw,
                          EGLSurface read,
                          EGLContext ctx)
{
    (void) dpy;
    if (!ctx && !draw && !read) {
        currentContext = EGL_NO_CONTEXT;
        currentDraw = EGL_NO_SURFACE;
        currentRead = EGL_NO_SURFACE;
        releaseCount++;
        return EGL_TRUE; /* release */
    }
    if (!isLive(liveContexts, contextCount, (intptr_t) ctx))
        return EGL_FALSE;
    if (!isLive(liveSurfaces, surfaceCount, (intptr_t) draw))
        return EGL_FALSE;
    if (read && !isLive(liveSurfaces, surfaceCount, (intptr_t) read))
        return EGL_FALSE;
    currentContext = ctx;
    currentDraw = draw;
    currentRead = read;
    return EGL_TRUE;
}

EGLBoolean eglSwapBuffers(EGLDisplay dpy, EGLSurface surface)
{
    (void) dpy;
    if (!isLive(liveSurfaces, surfaceCount, (intptr_t) surface))
        return EGL_FALSE;
    swapCount++;
    return EGL_TRUE;
}

EGLBoolean eglSwapInterval(EGLDisplay dpy, EGLint interval)
{
    (void) dpy;
    (void) interval;
    return EGL_TRUE;
}

EGLint eglGetError(void)
{
    return EGL_SUCCESS;
}

EGLFuncPointer eglGetProcAddress(const char *name)
{
    /* Resolve only a token symbol so the layer's proc-address path is testable
     * without a real GL. Anything else is unknown.
     */
    if (name && !strcmp(name, "glClear"))
        return (EGLFuncPointer) eglGetError;
    return NULL;
}

/* Test-only introspection (not part of EGL). The provider test dlopens this
 * same object to read them, so context balance and swap dispatch become
 * assertions. Contexts and surfaces are reported separately because the layer
 * currently leaks the per-drawable pbuffer surface by design (a documented
 * Phase 1 limitation), so only the context balance should return to zero.
 */
int fake_egl_live_contexts(void)
{
    return contextCount;
}

int fake_egl_live_surfaces(void)
{
    return surfaceCount;
}

long fake_egl_swap_count(void)
{
    return swapCount;
}

long fake_egl_surfaces_created(void)
{
    return surfacesCreated;
}

long fake_egl_release_count(void)
{
    return releaseCount;
}

int fake_egl_current_draw_equals_read(void)
{
    return currentDraw == currentRead;
}

int fake_egl_has_current_read(void)
{
    return currentRead != EGL_NO_SURFACE;
}

/* Report the per-channel sizes the most recent eglChooseConfig actually
 * received on the EGL side. This checks src/glx.c's ENCODE direction
 * independently of its decode, so a swapped channel token in its shared
 * translation table (which a GLX->EGL->GLX round-trip would hide) is caught.
 */
void fake_egl_last_config_channels(int *red,
                                   int *green,
                                   int *blue,
                                   int *alpha,
                                   int *depth,
                                   int *stencil)
{
    FakeConfig empty;
    memset(&empty, 0, sizeof(empty));
    FakeConfig *cfg = lastConfig >= 0 ? &configs[lastConfig] : &empty;
    if (red)
        *red = cfg->red;
    if (green)
        *green = cfg->green;
    if (blue)
        *blue = cfg->blue;
    if (alpha)
        *alpha = cfg->alpha;
    if (depth)
        *depth = cfg->depth;
    if (stencil)
        *stencil = cfg->stencil;
}
