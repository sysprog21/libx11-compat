/*
 * Public GLX API surface for libx11-compat.
 *
 * This is a hand-trimmed, source-compatible subset of the X.Org GL/glx.h.
 * libx11-compat does not run the GLX wire protocol; the entry points declared
 * here are implemented in src/glx.c as a thin translation onto EGL. ABI is not
 * a concern here, only source-level API compat, so the opaque handle tags and
 * token values follow the standard names Xlib GLX clients expect at compile
 * time.
 *
 * Only the tokens and entry points the compat layer actually honors are listed.
 * Adding a symbol here also means implementing it in src/glx.c and adding it to
 * tests/api-symbols.txt.
 */

#ifndef LIBX11_COMPAT_GL_GLX_H
#define LIBX11_COMPAT_GL_GLX_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handle types. The Rec structs are defined privately in src/glx.c. */
typedef struct __GLXcontextRec *GLXContext;
typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef XID GLXDrawable;
typedef XID GLXPixmap;
typedef XID GLXWindow;
typedef XID GLXPbuffer;
typedef XID GLXContextID;

/* Function-pointer return type for glXGetProcAddress. Kept independent of
 * GL/gl.h so a GLES2 client does not have to pull desktop gl.h just to link.
 */
typedef void (*__GLXextFuncPtr)(void);

/* glXChooseVisual / glXGetConfig attribute tokens. */
#define GLX_USE_GL 1
#define GLX_BUFFER_SIZE 2
#define GLX_LEVEL 3
#define GLX_RGBA 4
#define GLX_DOUBLEBUFFER 5
#define GLX_STEREO 6
#define GLX_AUX_BUFFERS 7
#define GLX_RED_SIZE 8
#define GLX_GREEN_SIZE 9
#define GLX_BLUE_SIZE 10
#define GLX_ALPHA_SIZE 11
#define GLX_DEPTH_SIZE 12
#define GLX_STENCIL_SIZE 13
#define GLX_ACCUM_RED_SIZE 14
#define GLX_ACCUM_GREEN_SIZE 15
#define GLX_ACCUM_BLUE_SIZE 16
#define GLX_ACCUM_ALPHA_SIZE 17

/* Error returns from glXGetConfig. */
#define GLX_BAD_SCREEN 1
#define GLX_BAD_ATTRIBUTE 2
#define GLX_NO_EXTENSION 3
#define GLX_BAD_VISUAL 4
#define GLX_BAD_CONTEXT 5
#define GLX_BAD_VALUE 6
#define GLX_BAD_ENUM 7

/* GLX 1.3 FBConfig tokens. */
#define GLX_CONFIG_CAVEAT 0x0020
#define GLX_X_VISUAL_TYPE 0x0022
#define GLX_TRANSPARENT_TYPE 0x0023
#define GLX_DRAWABLE_TYPE 0x8010
#define GLX_RENDER_TYPE 0x8011
#define GLX_X_RENDERABLE 0x8012
#define GLX_FBCONFIG_ID 0x8013
#define GLX_RGBA_TYPE 0x8014
#define GLX_VISUAL_ID 0x800B
#define GLX_WINDOW_BIT 0x0001
#define GLX_PIXMAP_BIT 0x0002
#define GLX_PBUFFER_BIT 0x0004
#define GLX_RGBA_BIT 0x0001
#define GLX_COLOR_INDEX_BIT 0x0002
#define GLX_TRUE_COLOR 0x8002
#define GLX_DIRECT_COLOR 0x8003
#define GLX_NONE 0x8000
#define GLX_SLOW_CONFIG 0x8001
#define GLX_SAMPLE_BUFFERS 0x186a0
#define GLX_SAMPLES 0x186a1

/* GLX 1.3 pbuffer / drawable-query tokens. */
#define GLX_PRESERVED_CONTENTS 0x801B
#define GLX_LARGEST_PBUFFER 0x801C
#define GLX_WIDTH 0x801D
#define GLX_HEIGHT 0x801E
#define GLX_MAX_PBUFFER_WIDTH 0x8016
#define GLX_MAX_PBUFFER_HEIGHT 0x8017
#define GLX_MAX_PBUFFER_PIXELS 0x8018
#define GLX_PBUFFER_HEIGHT 0x8040
#define GLX_PBUFFER_WIDTH 0x8041

/* glXGetClientString / glXQueryServerString / glXQueryExtensionsString. */
#define GLX_VENDOR 0x1
#define GLX_VERSION 0x2
#define GLX_EXTENSIONS 0x3

/* GLX_ARB_create_context / _profile tokens (used to request a GLES/core context
 * through the FBConfig path).
 */
#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_FLAGS_ARB 0x2094
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001
#define GLX_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB 0x00000002
#define GLX_CONTEXT_ES2_PROFILE_BIT_EXT 0x00000004
#define GLX_CONTEXT_ES_PROFILE_BIT_EXT 0x00000004

extern XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList);
extern GLXContext glXCreateContext(Display *dpy,
                                   XVisualInfo *vis,
                                   GLXContext shareList,
                                   Bool direct);
extern void glXDestroyContext(Display *dpy, GLXContext ctx);
extern Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx);
extern Bool glXMakeContextCurrent(Display *dpy,
                                  GLXDrawable draw,
                                  GLXDrawable read,
                                  GLXContext ctx);
extern void glXSwapBuffers(Display *dpy, GLXDrawable drawable);
extern void glXSwapIntervalEXT(Display *dpy,
                               GLXDrawable drawable,
                               int interval);
extern int glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib, int *value);
extern GLXContext glXGetCurrentContext(void);
extern GLXDrawable glXGetCurrentDrawable(void);
extern Display *glXGetCurrentDisplay(void);
extern Bool glXIsDirect(Display *dpy, GLXContext ctx);
extern void glXWaitGL(void);
extern void glXWaitX(void);
extern Bool glXQueryExtension(Display *dpy, int *errorBase, int *eventBase);
extern Bool glXQueryVersion(Display *dpy, int *major, int *minor);
extern const char *glXQueryExtensionsString(Display *dpy, int screen);
extern const char *glXGetClientString(Display *dpy, int name);
extern const char *glXQueryServerString(Display *dpy, int screen, int name);
extern __GLXextFuncPtr glXGetProcAddress(const unsigned char *procName);
extern __GLXextFuncPtr glXGetProcAddressARB(const unsigned char *procName);

/* GLX 1.3 FBConfig path. */
extern GLXFBConfig *glXChooseFBConfig(Display *dpy,
                                      int screen,
                                      const int *attribList,
                                      int *nitems);
extern GLXFBConfig *glXGetFBConfigs(Display *dpy, int screen, int *nelements);
extern int glXGetFBConfigAttrib(Display *dpy,
                                GLXFBConfig config,
                                int attribute,
                                int *value);
extern XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config);
extern GLXContext glXCreateNewContext(Display *dpy,
                                      GLXFBConfig config,
                                      int renderType,
                                      GLXContext shareList,
                                      Bool direct);
extern GLXContext glXCreateContextAttribsARB(Display *dpy,
                                             GLXFBConfig config,
                                             GLXContext shareList,
                                             Bool direct,
                                             const int *attribList);

/* GLX 1.3 on-screen drawable + pbuffer + query. */
extern GLXWindow glXCreateWindow(Display *dpy,
                                 GLXFBConfig config,
                                 Window win,
                                 const int *attribList);
extern void glXDestroyWindow(Display *dpy, GLXWindow win);
extern GLXPbuffer glXCreatePbuffer(Display *dpy,
                                   GLXFBConfig config,
                                   const int *attribList);
extern void glXDestroyPbuffer(Display *dpy, GLXPbuffer pbuf);
extern void glXQueryDrawable(Display *dpy,
                             GLXDrawable draw,
                             int attribute,
                             unsigned int *value);
extern GLXDrawable glXGetCurrentReadDrawable(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBX11_COMPAT_GL_GLX_H */
