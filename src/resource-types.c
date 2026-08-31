#include "resource-types.h"
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>

/* The X protocol has a client allocate resource ids as base | (n & mask), so
 * the advertised base and mask must not share a bit and every id must fit the
 * protocol's 29 bits. Spanning exactly [base, base | mask] means any id a
 * client derives that way is one this table can hold.
 */
#define FIRST_RESOURCE_ID ((XID) XID_RESOURCE_BASE)
#define RESOURCE_ID_COUNT ((size_t) XID_RESOURCE_MASK + 1)
#define RESOURCE_CHUNK_SHIFT 10
#define RESOURCE_CHUNK_SIZE ((size_t) 1 << RESOURCE_CHUNK_SHIFT)
#define RESOURCE_CHUNK_COUNT (RESOURCE_ID_COUNT / RESOURCE_CHUNK_SIZE)
#define RESOURCE_DIRECTORY_SHIFT 9
#define RESOURCE_DIRECTORY_SIZE ((size_t) 1 << RESOURCE_DIRECTORY_SHIFT)
#define RESOURCE_DIRECTORY_COUNT \
    (RESOURCE_CHUNK_COUNT / RESOURCE_DIRECTORY_SIZE)

typedef struct {
    _Atomic Bool allocated;
    XID_Struct resource;
} ResourceSlot;

typedef struct {
    _Atomic(ResourceSlot *) chunks[RESOURCE_DIRECTORY_SIZE];
} ResourceDirectory;

static _Atomic(ResourceDirectory *)
    resourceDirectories[RESOURCE_DIRECTORY_COUNT];
static pthread_mutex_t resourceMutex = PTHREAD_MUTEX_INITIALIZER;
static XID nextResourceId = FIRST_RESOURCE_ID;
static XID_Struct invalidResource = {0, NULL};

static ResourceSlot *resourceSlot(XID id)
{
    if (id < FIRST_RESOURCE_ID || id - FIRST_RESOURCE_ID >= RESOURCE_ID_COUNT)
        return NULL;
    size_t index = (size_t) (id - FIRST_RESOURCE_ID);
    size_t chunkIndex = index >> RESOURCE_CHUNK_SHIFT;
    ResourceDirectory *directory = atomic_load_explicit(
        &resourceDirectories[chunkIndex >> RESOURCE_DIRECTORY_SHIFT],
        memory_order_acquire);
    if (!directory)
        return NULL;
    ResourceSlot *chunk = atomic_load_explicit(
        &directory->chunks[chunkIndex & (RESOURCE_DIRECTORY_SIZE - 1)],
        memory_order_acquire);
    return chunk ? &chunk[index & (RESOURCE_CHUNK_SIZE - 1)] : NULL;
}

static ResourceSlot *ensureResourceSlot(XID id)
{
    size_t index = (size_t) (id - FIRST_RESOURCE_ID);
    size_t chunkIndex = index >> RESOURCE_CHUNK_SHIFT;
    size_t directoryIndex = chunkIndex >> RESOURCE_DIRECTORY_SHIFT;
    ResourceDirectory *directory = atomic_load_explicit(
        &resourceDirectories[directoryIndex], memory_order_relaxed);
    if (!directory) {
        directory = calloc(1, sizeof(*directory));
        if (!directory)
            return NULL;
        atomic_store_explicit(&resourceDirectories[directoryIndex], directory,
                              memory_order_release);
    }
    _Atomic(ResourceSlot *) *chunkSlot =
        &directory->chunks[chunkIndex & (RESOURCE_DIRECTORY_SIZE - 1)];
    ResourceSlot *chunk = atomic_load_explicit(chunkSlot, memory_order_relaxed);
    if (!chunk) {
        chunk = calloc(RESOURCE_CHUNK_SIZE, sizeof(*chunk));
        if (!chunk)
            return NULL;
        atomic_store_explicit(chunkSlot, chunk, memory_order_release);
    }
    return &chunk[index & (RESOURCE_CHUNK_SIZE - 1)];
}

/* Caller holds resourceMutex. A freshly claimed slot carries no resource yet.
 */
static void claimSlotLocked(ResourceSlot *slot)
{
    slot->resource.type = 0;
    slot->resource.dataPointer = NULL;
    atomic_store_explicit(&slot->allocated, True, memory_order_release);
}

XID allocXidResource(void)
{
    pthread_mutex_lock(&resourceMutex);

    /* Step over ids a client reserved for itself: the cursor only moves
     * forward, so a reserved id ahead of it must not be handed out twice.
     */
    while (nextResourceId - FIRST_RESOURCE_ID < RESOURCE_ID_COUNT) {
        XID id = nextResourceId++;
        ResourceSlot *slot = ensureResourceSlot(id);
        if (!slot)
            break;
        if (!atomic_load_explicit(&slot->allocated, memory_order_acquire)) {
            claimSlotLocked(slot);
            pthread_mutex_unlock(&resourceMutex);
            return id;
        }
    }
    pthread_mutex_unlock(&resourceMutex);
    return None;
}

Bool reserveXidResource(XID id)
{
    /* The range check needs no lock: both bounds are constants. */
    if (id < FIRST_RESOURCE_ID || id - FIRST_RESOURCE_ID >= RESOURCE_ID_COUNT)
        return False;

    /* A client names its own resources, so the id can sit anywhere in the
     * advertised range, including above ids handed out so far.
     */
    pthread_mutex_lock(&resourceMutex);
    ResourceSlot *slot = ensureResourceSlot(id);
    Bool free =
        slot && !atomic_load_explicit(&slot->allocated, memory_order_acquire);
    if (free)
        claimSlotLocked(slot);
    pthread_mutex_unlock(&resourceMutex);
    return free;
}

void freeXidResource(XID id)
{
    pthread_mutex_lock(&resourceMutex);
    ResourceSlot *slot = resourceSlot(id);
    if (!slot) {
        pthread_mutex_unlock(&resourceMutex);
        return;
    }
    atomic_store_explicit(&slot->allocated, False, memory_order_release);
    slot->resource.type = 0;
    slot->resource.dataPointer = NULL;
    pthread_mutex_unlock(&resourceMutex);
}

XID_Struct *getXidStruct(XID id)
{
    ResourceSlot *slot = resourceSlot(id);
    if (!slot || !atomic_load_explicit(&slot->allocated, memory_order_acquire))
        return &invalidResource;
    return &slot->resource;
}

Bool isXidAllocated(XID id)
{
    ResourceSlot *slot = resourceSlot(id);
    return slot && atomic_load_explicit(&slot->allocated, memory_order_acquire);
}

void forEachXidResourceOfType(XResourceType type,
                              void (*visit)(void *data, void *ctx),
                              void *ctx)
{
    /* Collect first, then visit with the lock dropped. Holding it across the
     * callback would serialize correctly but makes every visitor a potential
     * deadlock: they run at renderer teardown and are free to call back into
     * anything, including this table. Snapshotting keeps the read of each slot
     * atomic with respect to freeXidResource without that exposure.
     *
     * Walking the chunks that exist, rather than the id range they cover, keeps
     * this proportional to live resources: a client-chosen id can sit anywhere
     * in the advertised space.
     */
    void **matches = NULL;
    size_t count = 0, capacity = 0;
    pthread_mutex_lock(&resourceMutex);
    for (size_t d = 0; d < RESOURCE_DIRECTORY_COUNT; d++) {
        ResourceDirectory *directory =
            atomic_load_explicit(&resourceDirectories[d], memory_order_acquire);
        if (!directory)
            continue;
        for (size_t c = 0; c < RESOURCE_DIRECTORY_SIZE; c++) {
            ResourceSlot *chunk = atomic_load_explicit(&directory->chunks[c],
                                                       memory_order_acquire);
            if (!chunk)
                continue;
            for (size_t i = 0; i < RESOURCE_CHUNK_SIZE; i++) {
                ResourceSlot *slot = &chunk[i];
                if (!atomic_load_explicit(&slot->allocated,
                                          memory_order_acquire) ||
                    slot->resource.type != type)
                    continue;
                if (count == capacity) {
                    size_t grown = capacity ? capacity * 2 : 64;
                    void **bigger = realloc(matches, grown * sizeof(*matches));
                    if (!bigger) {
                        /* Out of memory mid-sweep: visit what was collected and
                         * drop the rest rather than lose the lock discipline or
                         * abort a teardown path.
                         */
                        goto visit;
                    }
                    matches = bigger;
                    capacity = grown;
                }
                matches[count++] = slot->resource.dataPointer;
            }
        }
    }
visit:
    pthread_mutex_unlock(&resourceMutex);
    for (size_t i = 0; i < count; i++)
        visit(matches[i], ctx);
    free(matches);
}
