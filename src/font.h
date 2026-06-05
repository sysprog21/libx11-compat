#ifndef FONT_H
#define FONT_H

#include <X11/Xlib.h>
#include <SDL2/SDL.h>

extern void freeFontStorage(void);
Bool initFontStorage(void);

Bool compatFontIsClientUsable(Font fontXid);
Bool compatFontRetainForGC(Font fontXid);
void compatFontReleaseForGC(Font fontXid);
int compatFontClose(Display *display, Font fontXid);

/* Drop cached text textures whose backing renderer is about to be
 * destroyed; callers in destroyWindow / closeRenderer paths must invoke
 * this before SDL_DestroyRenderer to keep the cache from holding a
 * dangling pointer. */
void invalidateTextCacheForRenderer(SDL_Renderer *renderer);
/* Drop cached text entries for a Font XID being closed (XUnloadFont,
 * XFreeFont). The TTF_Font behind the entry will be torn down by the
 * caller; without this, a later cache hit would feed a freed font into
 * a re-render path. */
void invalidateTextCacheForFont(Font fontXid);
void freeTextCache(void);

#endif /* FONT_H */
