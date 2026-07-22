#ifndef IMAGE_H
#define IMAGE_H

#include "sdl-compat.h"

void freeImageStorage(void);
void invalidatePutImageStagingTexture(SDL_Renderer *renderer);

/* Introspection for the XPutImage staging-texture cache. Tests use this to
 * verify that the cache observed a renderer change rather than relying on SDL
 * silently tolerating use-after-free of a stale handle.
 */
SDL_Renderer *getPutImageStagingTextureRenderer(void);

/* Test hook: number of XPutImage calls that handed caller rows directly to the
 * RGBA8888 staging texture instead of swizzling through the scratch buffer.
 */
extern unsigned long x11compat_xputimage_direct_uploads;

#endif /* IMAGE_H */
