#ifndef LIBX11_COMPAT_REPLAY_TARGET_H
#define LIBX11_COMPAT_REPLAY_TARGET_H

#include <X11/Xlib.h>
#include <SDL2/SDL_stdinc.h>

/* Shared target state for in-process replay, XTest injection, snapshots,
 * and replay-driven resize. The target is captured on the main thread when
 * top-level SDL windows are mapped; background replay/XTest callers then use
 * the cached SDL id without walking the live X window tree.
 */
void replayTargetOfferWindow(Uint32 sdlWindowId,
                             int rootX,
                             int rootY,
                             int width,
                             int height);
void replayTargetForgetWindow(Uint32 sdlWindowId);
Uint32 replayTargetWindowId(void);
void replayTargetRootToLocal(int rootX, int rootY, int *localX, int *localY);
/* Coherent snapshot of the (target id, target root) triple. Returns False
 * when no target is currently registered. Callers that need both the id
 * and the local-coord translation should prefer this over the separate
 * replayTargetWindowId / replayTargetRootToLocal pair, since the latter
 * pair can read a half-retargeted state where id and root come from
 * different generations. */
Bool replayTargetTranslateRoot(int rootX,
                               int rootY,
                               Uint32 *winId,
                               int *localX,
                               int *localY);
void replayTargetRememberPointer(int x, int y);
Bool replayTargetReadPointer(int *x, int *y);

#endif
