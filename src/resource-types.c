#include "resource-types.h"
#include <stdlib.h>
#include <string.h>

#define FIRST_RESOURCE_ID ((XID) 0x100000)

typedef struct {
    Bool allocated;
    XID_Struct resource;
} ResourceSlot;

static ResourceSlot *resourceSlots = NULL;
static size_t resourceSlotCapacity = 0;
static XID nextResourceId = FIRST_RESOURCE_ID;

static XID_Struct invalidResource = {0, NULL};

static Bool ensureResourceSlot(size_t index)
{
    if (index < resourceSlotCapacity)
        return True;
    size_t newCapacity = resourceSlotCapacity ? resourceSlotCapacity : 1024;
    while (index >= newCapacity) {
        if (newCapacity > ((size_t) -1) / 2)
            return False;
        newCapacity *= 2;
    }
    ResourceSlot *newSlots =
        realloc(resourceSlots, newCapacity * sizeof(*newSlots));
    if (!newSlots)
        return False;
    memset(newSlots + resourceSlotCapacity, 0,
           (newCapacity - resourceSlotCapacity) * sizeof(*newSlots));
    resourceSlots = newSlots;
    resourceSlotCapacity = newCapacity;
    return True;
}

XID allocXidResource(void)
{
    if (nextResourceId < FIRST_RESOURCE_ID)
        return None;
    size_t index = (size_t) (nextResourceId - FIRST_RESOURCE_ID);
    if (!ensureResourceSlot(index))
        return None;
    XID id = nextResourceId++;
    resourceSlots[index].allocated = True;
    resourceSlots[index].resource.type = 0;
    resourceSlots[index].resource.dataPointer = NULL;
    return id;
}

void freeXidResource(XID id)
{
    if (id < FIRST_RESOURCE_ID)
        return;
    size_t index = (size_t) (id - FIRST_RESOURCE_ID);
    if (index >= resourceSlotCapacity)
        return;
    resourceSlots[index].allocated = False;
    resourceSlots[index].resource.type = 0;
    resourceSlots[index].resource.dataPointer = NULL;
}

XID_Struct *getXidStruct(XID id)
{
    if (id < FIRST_RESOURCE_ID)
        return &invalidResource;
    size_t index = (size_t) (id - FIRST_RESOURCE_ID);
    if (index >= resourceSlotCapacity || !resourceSlots[index].allocated)
        return &invalidResource;
    return &resourceSlots[index].resource;
}

void forEachXidResourceOfType(XResourceType type,
                              void (*visit)(void *data, void *ctx),
                              void *ctx)
{
    for (size_t i = 0; i < resourceSlotCapacity; i++) {
        if (resourceSlots[i].allocated &&
            resourceSlots[i].resource.type == type)
            visit(resourceSlots[i].resource.dataPointer, ctx);
    }
}
