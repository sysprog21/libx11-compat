#include "replay-target.h"
#include "sdl-compat.h"
#include <limits.h>
#include <stdlib.h>
#include <stdint.h>
#include "util.h"

static SDL_atomic_t targetWindowId;
static SDL_atomic_t latestWindowId;
static SDL_atomic_t latestRootX;
static SDL_atomic_t latestRootY;
static SDL_atomic_t targetRootX;
static SDL_atomic_t targetRootY;
static SDL_atomic_t firstWindowId;
static SDL_atomic_t firstRootX;
static SDL_atomic_t firstRootY;
/* Seqlock counter guarding two writer-mutated groups: the active target triple
 * (targetWindowId, targetRootX, targetRootY) and the first/latest candidate
 * snapshots (id, rootX, rootY, area). Writers bump it to odd before mutating a
 * group and back to even afterward, so a reader that observes a stable even
 * value across its loads has a coherent snapshot. Both groups share one
 * counter: window offers are rare, so an occasional cross-group reader retry is
 * cheaper than maintaining a second counter.
 */
static SDL_atomic_t targetSeq;
static SDL_atomic_t lastPointerX;
static SDL_atomic_t lastPointerY;
static SDL_atomic_t lastPointerRootX;
static SDL_atomic_t lastPointerRootY;
static SDL_atomic_t hasPointer;

/* Window areas for the target-selection heuristic. firstArea and latestArea are
 * written inside the seqlock brackets in replayTargetOfferWindow and read back
 * under the same seqlock retry in the select loops, so they stay paired with
 * their ids and roots. targetArea is only used on the main-thread offer path
 * (plus a replay-thread write in replayTargetSelectStored); it is a best-effort
 * scalar whose stale read at worst reorders a retarget the next offer corrects.
 */
static unsigned long targetArea = 0;
static unsigned long latestArea = 0;
static unsigned long firstArea = 0;
static unsigned long targetAreaHighWater = 0;

typedef struct {
    Uint32 id;
    int rootX;
    int rootY;
    unsigned long area;
    unsigned long order;
} ReplayTargetEntry;

static ReplayTargetEntry *knownTargets;
static size_t knownTargetCount;
static size_t knownTargetCap;
static unsigned long nextTargetOrder = 1;

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

/* Bump the seq counter by one. Writers call this to bracket their writes to the
 * target triple; the parity convention is "odd = write in progress" so a reader
 * observing an odd value (or a change across its before/after reads) knows to
 * retry.
 */
static void seqBumpLocked(void)
{
    SDL_AtomicAdd(&targetSeq, 1);
}

/* Writer serialization for the seqlock. A seqlock tolerates many lock-free
 * readers but only one writer at a time: two writers bumping targetSeq
 * concurrently can leave it even mid-write and hand a reader a mixed snapshot.
 * Offers and forgets run on the main thread while a "target first|latest"
 * command retargets from the replay thread, so every write bracket below runs
 * under this mutex. Readers stay lock-free.
 */
static SDL_mutex *targetWriteMutex = NULL;
static SDL_SpinLock targetWriteMutexInitLock = 0;

static void targetWriteLock(void)
{
    if (!targetWriteMutex) {
        SDL_AtomicLock(&targetWriteMutexInitLock);
        if (!targetWriteMutex)
            targetWriteMutex = SDL_CreateMutex();
        SDL_AtomicUnlock(&targetWriteMutexInitLock);
    }
    if (targetWriteMutex)
        SDL_LockMutex(targetWriteMutex);
}

static void targetWriteUnlock(void)
{
    if (targetWriteMutex)
        SDL_UnlockMutex(targetWriteMutex);
}

static ReplayTargetEntry *findKnownTarget(Uint32 id)
{
    for (size_t i = 0; i < knownTargetCount; i++) {
        if (knownTargets[i].id == id)
            return &knownTargets[i];
    }
    return NULL;
}

static Bool rememberKnownTarget(Uint32 id,
                                int rootX,
                                int rootY,
                                unsigned long area,
                                Bool *isNewOut)
{
    ReplayTargetEntry *entry = findKnownTarget(id);
    if (entry) {
        entry->rootX = rootX;
        entry->rootY = rootY;
        entry->area = area;
        *isNewOut = False;
        return True;
    }

    if (knownTargetCount == knownTargetCap) {
        size_t newCap = knownTargetCap ? knownTargetCap * 2 : 8;
        ReplayTargetEntry *grown =
            realloc(knownTargets, newCap * sizeof(*knownTargets));
        if (!grown)
            return False;
        knownTargets = grown;
        knownTargetCap = newCap;
    }

    entry = &knownTargets[knownTargetCount++];
    *entry = (ReplayTargetEntry) {
        .id = id,
        .rootX = rootX,
        .rootY = rootY,
        .area = area,
        .order = nextTargetOrder++,
    };
    *isNewOut = True;
    return True;
}

static void publishFirstLatestLocked(void)
{
    ReplayTargetEntry *first = NULL, *latest = NULL;
    for (size_t i = 0; i < knownTargetCount; i++) {
        ReplayTargetEntry *entry = &knownTargets[i];
        if (!first || entry->order < first->order)
            first = entry;
        if (!latest || entry->order > latest->order)
            latest = entry;
    }

    SDL_AtomicSet(&firstWindowId, first ? (int) first->id : 0);
    SDL_AtomicSet(&firstRootX, first ? first->rootX : 0);
    SDL_AtomicSet(&firstRootY, first ? first->rootY : 0);
    firstArea = first ? first->area : 0;
    SDL_AtomicSet(&latestWindowId, latest ? (int) latest->id : 0);
    SDL_AtomicSet(&latestRootX, latest ? latest->rootX : 0);
    SDL_AtomicSet(&latestRootY, latest ? latest->rootY : 0);
    latestArea = latest ? latest->area : 0;
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
    targetWriteLock();
    Bool isNew = False;
    if (!rememberKnownTarget(sdlWindowId, rootX, rootY, area, &isNew)) {
        targetWriteUnlock();
        return;
    }
    seqBumpLocked();
    publishFirstLatestLocked();
    seqBumpLocked();

    Uint32 currentTarget = (Uint32) SDL_AtomicGet(&targetWindowId);
    Bool takeTarget = isNew || currentTarget == sdlWindowId;
    if (takeTarget && targetArea != 0 && area < targetArea &&
        currentTarget != sdlWindowId) {
        takeTarget = False;
    } else if (takeTarget && targetArea == 0 && targetAreaHighWater > 0) {
        unsigned long floor =
            (targetAreaHighWater * REPLAY_TARGET_HIGH_WATER_NUMERATOR) /
            REPLAY_TARGET_HIGH_WATER_DENOMINATOR;
        if (area < floor) {
            LOG("replay-target: deferring candidate id=%u (%dx%d, area=%lu) "
                "below high-water floor %lu\n",
                sdlWindowId, width, height, area, floor);
            takeTarget = False;
        }
    }

    if (takeTarget) {
        targetArea = area;
        if (area > targetAreaHighWater)
            targetAreaHighWater = area;
        rebasePointerToTargetRoot(rootX, rootY);
        /* Bracket the triple write with seq bumps so readers see either the old
         * triple or the new one, never a mix.
         */
        seqBumpLocked();
        SDL_AtomicSet(&targetRootX, rootX);
        SDL_AtomicSet(&targetRootY, rootY);
        SDL_AtomicSet(&targetWindowId, (int) sdlWindowId);
        seqBumpLocked();
        LOG("replay-target: target window id=%u at %+d%+d (%dx%d)\n",
            sdlWindowId, rootX, rootY, width, height);
    }
    targetWriteUnlock();
}

/* Retarget onto an already-resolved (id, root, area). Caller must hold
 * targetWriteMutex so this write bracket cannot overlap another writer.
 */
static Bool replayTargetSelectStored(Uint32 sdlWindowId,
                                     int rootX,
                                     int rootY,
                                     unsigned long area)
{
    if (sdlWindowId == 0)
        return False;
    targetArea = area;
    rebasePointerToTargetRoot(rootX, rootY);
    seqBumpLocked();
    SDL_AtomicSet(&targetRootX, rootX);
    SDL_AtomicSet(&targetRootY, rootY);
    SDL_AtomicSet(&targetWindowId, (int) sdlWindowId);
    seqBumpLocked();
    return True;
}

/* Read one candidate snapshot (first or latest) and retarget onto it. Runs
 * under the writer mutex so the id, roots, and area are read coherently and the
 * retarget cannot race a concurrent offer or forget. replayTargetSelectStored
 * expects the mutex already held.
 */
static Bool replayTargetSelectSnapshot(SDL_atomic_t *idAtom,
                                       SDL_atomic_t *rootXAtom,
                                       SDL_atomic_t *rootYAtom,
                                       const unsigned long *areaField)
{
    targetWriteLock();
    Uint32 id = (Uint32) SDL_AtomicGet(idAtom);
    int rx = SDL_AtomicGet(rootXAtom), ry = SDL_AtomicGet(rootYAtom);
    unsigned long area = *areaField;
    Bool ok = replayTargetSelectStored(id, rx, ry, area);
    targetWriteUnlock();
    return ok;
}

Bool replayTargetSelectFirst(void)
{
    return replayTargetSelectSnapshot(&firstWindowId, &firstRootX, &firstRootY,
                                      &firstArea);
}

Bool replayTargetSelectLatest(void)
{
    return replayTargetSelectSnapshot(&latestWindowId, &latestRootX,
                                      &latestRootY, &latestArea);
}

void replayTargetForgetWindow(Uint32 sdlWindowId)
{
    if (sdlWindowId == 0)
        return;
    targetWriteLock();
    for (size_t i = 0; i < knownTargetCount; i++) {
        if (knownTargets[i].id == sdlWindowId) {
            knownTargets[i] = knownTargets[knownTargetCount - 1];
            knownTargetCount--;
            break;
        }
    }
    seqBumpLocked();
    publishFirstLatestLocked();
    seqBumpLocked();
    if ((Uint32) SDL_AtomicGet(&targetWindowId) == sdlWindowId) {
        seqBumpLocked();
        SDL_AtomicSet(&targetWindowId, 0);
        targetArea = 0;
        seqBumpLocked();
        LOG("replay-target: target window id=%u retired (high-water %lu "
            "kept)\n",
            sdlWindowId, targetAreaHighWater);
    }
    targetWriteUnlock();
}

Uint32 replayTargetWindowId(void)
{
    return (Uint32) SDL_AtomicGet(&targetWindowId);
}

Uint32 replayTargetLatestWindowId(void)
{
    return (Uint32) SDL_AtomicGet(&latestWindowId);
}

void replayTargetRootToLocal(int rootX, int rootY, int *localX, int *localY)
{
    /* Best-effort single-field translation. Callers that need the id and the
     * local coords as a coherent pair must go through
     * replayTargetTranslateRoot() instead.
     */
    *localX = rootX - SDL_AtomicGet(&targetRootX);
    *localY = rootY - SDL_AtomicGet(&targetRootY);
}

Bool replayTargetTranslateRoot(int rootX,
                               int rootY,
                               Uint32 *winId,
                               int *localX,
                               int *localY)
{
    /* Seqlock read with a small retry budget: the writer side bumps an even
     * counter to odd before mutating the triple and back to even after. A
     * reader observes a coherent snapshot iff the counter is even and unchanged
     * across the before/after loads.
     */
    for (int attempt = 0; attempt < 8; attempt++) {
        int seqBefore = SDL_AtomicGet(&targetSeq);
        if (seqBefore & 1)
            continue;
        Uint32 id = (Uint32) SDL_AtomicGet(&targetWindowId);
        int rx = SDL_AtomicGet(&targetRootX), ry = SDL_AtomicGet(&targetRootY);
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
        int rx = SDL_AtomicGet(&targetRootX), ry = SDL_AtomicGet(&targetRootY);
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
    /* Read targetRoot via the seqlock so a concurrent retarget does not combine
     * the new (x,y) local with the previous target's root origin. If no
     * coherent snapshot is reachable, fall back to a single best-effort read of
     * the current values; staying at (0, 0) after a busy writer would otherwise
     * pin lastPointerRoot at the local coords and the next
     * rebasePointerToTargetRoot() would convert a local coordinate as if it
     * were root-relative.
     */
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
