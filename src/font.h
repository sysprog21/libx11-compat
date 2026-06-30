#ifndef FONT_H
#define FONT_H

#include <X11/Xlib.h>
#include "sdl-ttf-compat.h"

extern void freeFontStorage(void);
Bool initFontStorage(void);

/* Open a TTF font for `familyHint` at `size`, sharing the same per-family
 * probe-path tables (sans / sans-bold / serif / monospace) the core font path
 * uses for XLoadQueryFont fallbacks. familyHint may be NULL (treated as "sans")
 * or any free-form family / XLFD substring - the heuristics inside font.c
 * decide which probe list applies.
 *
 * Returns a TTF_Font * the caller must TTF_CloseFont, or NULL when no probe
 * path is openable on this host.
 */
TTF_Font *compatFontOpenFamilyFallback(const char *familyHint, int size);
TTF_Font *compatFontOpenFamilyFallbackForChar(const char *familyHint,
                                              int size,
                                              Uint32 codepoint);

Bool compatFontIsClientUsable(Font fontXid);
XFontStruct *compatFontQueryRetained(Display *display, Font fontXid);
Bool compatFontRetainForGC(Font fontXid);
void compatFontReleaseForGC(Font fontXid);
int compatFontClose(Display *display, Font fontXid);

/* Drop cached text textures whose backing renderer is about to be destroyed;
 * callers in destroyWindow / closeRenderer paths must invoke this before
 * SDL_DestroyRenderer to keep the cache from holding a dangling pointer.
 */
void invalidateTextCacheForRenderer(SDL_Renderer *renderer);
/* Drop cached text entries for a Font XID being closed (XUnloadFont,
 * XFreeFont). The TTF_Font behind the entry will be torn down by the caller;
 * without this, a later cache hit would feed a freed font into a re-render
 * path.
 */
void invalidateTextCacheForFont(Font fontXid);
void freeTextCache(void);

#endif /* FONT_H */
