#include "replay-target.h"
#include <SDL2/SDL_atomic.h>
#include <limits.h>
#include <stdint.h>
#include "util.h"

static SDL_atomic_t targetWindowId;
static SDL_atomic_t targetRootX;
static SDL_atomic_t targetRootY;
/* Seqlock counter: writers bump it to odd before mutating the (id, rootX,
 * rootY) triple and back to even afterward, so a reader that observes a
 * stable even value before and after its loads has a coherent snapshot. */
static SDL_atomic_t targetSeq;
static SDL_atomic_t lastPointerX;
static SDL_atomic_t lastPointerY;
static SDL_atomic_t lastPointerRootX;
static SDL_atomic_t lastPointerRootY;
static SDL_atomic_t hasPointer;

/* Read/written only from the main thread while top-level windows map/unmap. */
static unsigned long targetArea = 0;
static unsigned long targetAreaHighWater = 0;

#define REPLAY_TARGET_HIGH_WATER_NUMERATOR 1
#define REPLAY_TARGET_HIGH_WATER_DENOMINATOR 2

static int clampInt64ToInt(int64_t value)
{
    if (value > INT_MAX)
        return INT_MAX;
    if (value < INT_MIN)
        return INT_MIN;
    return (int) value;
}

static void rebasePointerToTargetRoot(int rootX, int rootY)
{
    if (!SDL_AtomicGet(&hasPointer))
        return;
    int pointerRootX = SDL_AtomicGet(&lastPointerRootX);
    int pointerRootY = SDL_AtomicGet(&lastPointerRootY);
    SDL_AtomicSet(&lastPointerX, pointerRootX - rootX);
    SDL_AtomicSet(&lastPointerY, pointerRootY - rootY);
}

/* Bump the seq counter by one. Writers call this to bracket their writes
 * to the target triple; the parity convention is "odd = write in
 * progress" so a reader observing an odd value (or a change across its
 * before/after reads) knows to retry. */
static void seqBumpLocked(void)
{
    SDL_AtomicAdd(&targetSeq, 1);
}

void replayTargetOfferWindow(Uint32 sdlWindowId,
                             int rootX,
                             int rootY,
                             int width,
                             int height)
{
    unsigned long area = (unsigned long) (width > 0 ? width : 0) *
                         (unsigned long) (height > 0 ? height : 0);
    if (sdlWindowId == 0)
        return;

    Uint32 currentTarget = (Uint32) SDL_AtomicGet(&targetWindowId);
    if (targetArea != 0 && area < targetArea && currentTarget != sdlWindowId)
        return;

    if (targetArea == 0 && targetAreaHighWater > 0) {
        unsigned long floor =
            (targetAreaHighWater * REPLAY_TARGET_HIGH_WATER_NUMERATOR) /
            REPLAY_TARGET_HIGH_WATER_DENOMINATOR;
        if (area < floor) {
            LOG("replay-target: deferring candidate id=%u (%dx%d, area=%lu) "
                "below high-water floor %lu\n",
                sdlWindowId, width, height, area, floor);
            return;
        }
    }

    targetArea = area;
    if (area > targetAreaHighWater)
        targetAreaHighWater = area;
    rebasePointerToTargetRoot(rootX, rootY);
    /* Bracket the triple write with seq bumps so readers see either the
     * old triple or the new one, never a mix. */
    seqBumpLocked();
    SDL_AtomicSet(&targetRootX, rootX);
    SDL_AtomicSet(&targetRootY, rootY);
    SDL_AtomicSet(&targetWindowId, (int) sdlWindowId);
    seqBumpLocked();
    LOG("replay-target: target window id=%u at %+d%+d (%dx%d)\n", sdlWindowId,
        rootX, rootY, width, height);
}

void replayTargetForgetWindow(Uint32 sdlWindowId)
{
    if (sdlWindowId == 0)
        return;
    if ((Uint32) SDL_AtomicGet(&targetWindowId) != sdlWindowId)
        return;
    seqBumpLocked();
    SDL_AtomicSet(&targetWindowId, 0);
    targetArea = 0;
    seqBumpLocked();
    LOG("replay-target: target window id=%u retired (high-water %lu kept)\n",
        sdlWindowId, targetAreaHighWater);
}

Uint32 replayTargetWindowId(void)
{
    return (Uint32) SDL_AtomicGet(&targetWindowId);
}

void replayTargetRootToLocal(int rootX, int rootY, int *localX, int *localY)
{
    /* Best-effort single-field translation. Callers that need the id and
     * the local coords as a coherent pair must go through
     * replayTargetTranslateRoot() instead. */
    *localX = rootX - SDL_AtomicGet(&targetRootX);
    *localY = rootY - SDL_AtomicGet(&targetRootY);
}

Bool replayTargetTranslateRoot(int rootX,
                               int rootY,
                               Uint32 *winId,
                               int *localX,
                               int *localY)
{
    /* Seqlock read with a small retry budget: the writer side bumps an
     * even counter to odd before mutating the triple and back to even
     * after. A reader observes a coherent snapshot iff the counter is
     * even and unchanged across the before/after loads. */
    for (int attempt = 0; attempt < 8; attempt++) {
        int seqBefore = SDL_AtomicGet(&targetSeq);
        if (seqBefore & 1)
            continue;
        Uint32 id = (Uint32) SDL_AtomicGet(&targetWindowId);
        int rx = SDL_AtomicGet(&targetRootX);
        int ry = SDL_AtomicGet(&targetRootY);
        int seqAfter = SDL_AtomicGet(&targetSeq);
        if (seqBefore != seqAfter)
            continue;
        if (id == 0)
            return False;
        if (winId)
            *winId = id;
        if (localX)
            *localX = rootX - rx;
        if (localY)
            *localY = rootY - ry;
        return True;
    }
    return False;
}

Bool replayTargetTranslateLocal(int localX, int localY, int *rootX, int *rootY)
{
    for (int attempt = 0; attempt < 8; attempt++) {
        int seqBefore = SDL_AtomicGet(&targetSeq);
        if (seqBefore & 1)
            continue;
        Uint32 id = (Uint32) SDL_AtomicGet(&targetWindowId);
        int rx = SDL_AtomicGet(&targetRootX);
        int ry = SDL_AtomicGet(&targetRootY);
        int seqAfter = SDL_AtomicGet(&targetSeq);
        if (seqBefore != seqAfter)
            continue;
        if (id == 0)
            return False;
        if (rootX)
            *rootX = clampInt64ToInt((int64_t) rx + (int64_t) localX);
        if (rootY)
            *rootY = clampInt64ToInt((int64_t) ry + (int64_t) localY);
        return True;
    }
    return False;
}

void replayTargetRememberPointer(int x, int y)
{
    /* Read targetRoot via the seqlock so a concurrent retarget does not
     * combine the new (x,y) local with the previous target's root
     * origin. If no coherent snapshot is reachable, fall back to a
     * single best-effort read of the current values; staying at (0, 0)
     * after a busy writer would otherwise pin lastPointerRoot at the
     * local coords and the next rebasePointerToTargetRoot() would
     * convert a local coordinate as if it were root-relative. */
    int rx = 0, ry = 0;
    Bool gotSnapshot = False;
    for (int attempt = 0; attempt < 8 && !gotSnapshot; attempt++) {
        int seqBefore = SDL_AtomicGet(&targetSeq);
        if (seqBefore & 1)
            continue;
        rx = SDL_AtomicGet(&targetRootX);
        ry = SDL_AtomicGet(&targetRootY);
        int seqAfter = SDL_AtomicGet(&targetSeq);
        if (seqBefore == seqAfter)
            gotSnapshot = True;
    }
    if (!gotSnapshot) {
        rx = SDL_AtomicGet(&targetRootX);
        ry = SDL_AtomicGet(&targetRootY);
    }
    SDL_AtomicSet(&lastPointerX, x);
    SDL_AtomicSet(&lastPointerY, y);
    SDL_AtomicSet(&lastPointerRootX, rx + x);
    SDL_AtomicSet(&lastPointerRootY, ry + y);
    SDL_AtomicSet(&hasPointer, 1);
}

Bool replayTargetReadPointer(int *x, int *y)
{
    if (!SDL_AtomicGet(&hasPointer))
        return False;
    if (x)
        *x = SDL_AtomicGet(&lastPointerX);
    if (y)
        *y = SDL_AtomicGet(&lastPointerY);
    return True;
}
