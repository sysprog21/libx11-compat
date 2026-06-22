#ifndef IMAGE_H
#define IMAGE_H

#include "sdl-compat.h"

void freeImageStorage(void);
void invalidatePutImageStagingTexture(SDL_Renderer *renderer);

/* Introspection for the XPutImage staging-texture cache. Tests use this
 * to verify that the cache observed a renderer change rather than
 * relying on SDL silently tolerating use-after-free of a stale handle.
 */
SDL_Renderer *getPutImageStagingTextureRenderer(void);

#endif /* IMAGE_H */
