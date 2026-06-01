#ifndef _DRAWING_H_
#define _DRAWING_H_

#include <SDL2/SDL.h>
#include "resource-types.h"
#include "window.h"

#if SDL_BYTEORDER == SDL_BIG_ENDIAN || 1
#define DEFAULT_RED_MASK 0xFF000000
#define DEFAULT_GREEN_MASK 0x00FF0000
#define DEFAULT_BLUE_MASK 0x0000FF00
#define DEFAULT_ALPHA_MASK 0x000000FF
#else
#define DEFAULT_RED_MASK 0x000000FF
#define DEFAULT_GREEN_MASK 0x0000FF00
#define DEFAULT_BLUE_MASK 0x00FF0000
#define DEFAULT_ALPHA_MASK 0xFF000000
#endif
#define SDL_SURFACE_DEPTH 32

typedef struct {
    SDL_Texture *texture;
    unsigned int width;
    unsigned int height;
    unsigned int depth;
} PixmapStruct;

#define LOCK_SURFACE(surface)  \
    if (SDL_MUSTLOCK(surface)) \
    SDL_LockSurface(surface)
#define UNLOCK_SURFACE(surface) \
    if (SDL_MUSTLOCK(surface))  \
    SDL_UnlockSurface(surface)
#define GET_PIXMAP_STRUCT(pixmap) \
    (IS_TYPE(pixmap, PIXMAP) ? ((PixmapStruct *) GET_XID_VALUE(pixmap)) : NULL)
#define GET_PIXMAP_TEXTURE(pixmap) \
    (GET_PIXMAP_STRUCT(pixmap) ? GET_PIXMAP_STRUCT(pixmap)->texture : NULL)
#define GET_RENDERER(drawable, renderer)                                       \
    renderer = NULL;                                                           \
    if (IS_TYPE(drawable, WINDOW)) {                                           \
        renderer = getWindowRenderer(drawable);                                \
    } else if (IS_TYPE(drawable, PIXMAP)) {                                    \
        renderer = GET_WINDOW_STRUCT(SCREEN_WINDOW)->sdlRenderer;              \
        if (SDL_SetRenderTarget(renderer, GET_PIXMAP_TEXTURE(drawable)) !=     \
            0) {                                                               \
            fprintf(stderr,                                                    \
                    "SDL_SetRenderTarget failed while trying to get renderer " \
                    "in %s, %s, %d: %s\n",                                     \
                    __FILE__, __func__, __LINE__, SDL_GetError());             \
        }                                                                      \
        SDL_RenderSetViewport(renderer, NULL);                                 \
    } else {                                                                   \
        fprintf(stderr,                                                        \
                "Got unknown drawable type while trying to get renderer in "   \
                "%s, %s, %d\n",                                                \
                __FILE__, __func__, __LINE__);                                 \
    }

#ifdef DEBUG_WINDOWS
void drawWindowDebugView();
void drawDebugWindowSurfacePlanes();
#endif
void putPixel(SDL_Surface *surface,
              unsigned int x,
              unsigned int y,
              Uint32 pixel);
Uint32 getPixel(SDL_Surface *surface, unsigned int x, unsigned int y);
SDL_Renderer *getWindowRenderer(Window window);
SDL_Surface *getRenderSurface(SDL_Renderer *renderer);
SDL_Surface *getRenderSurfaceRect(SDL_Renderer *renderer,
                                  const SDL_Rect *source);
int getGcClipIterationCount(GC gc);
Bool setGcClipForIteration(SDL_Renderer *renderer, GC gc, int iteration);
void clearRendererClip(SDL_Renderer *renderer);
void drawWindowDataToScreen(void);

/* Single-slot (renderer, gc, generation) cache. The shim is single
 * threaded and tends to issue runs of draw calls against one renderer
 * with the same GC, so a one-deep cache catches most redundant pushes.
 * applySdlDrawState pushes the requested blend mode + color only when
 * the cached state diverges; invalidateGcStateCache is called from
 * XFreeGC so stale GC pointers don't fool a future generation match. */
void applySdlDrawState(SDL_Renderer *renderer,
                       GC gc,
                       SDL_BlendMode blendMode,
                       unsigned long color);
void invalidateGcStateCache(GC gc);
/* Call after any direct SDL_SetRenderDrawColor / SDL_SetRenderDrawBlendMode
 * that doesn't go through applySdlDrawState — otherwise the next cached
 * apply may see a "matching" cache entry and skip the SDL push even
 * though the renderer's real state has drifted. */
void invalidateSdlDrawStateCache(void);

#endif /* _DRAWING_H_ */
