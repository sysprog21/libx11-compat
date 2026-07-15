#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/shm.h>
#include "events.h"

#define XSHM_EVENT_BASE 128

/* MIT-SHM exists to skip copying image bytes across the X11 client/server
 * socket. libx11-compat has no such socket: the "server" runs in the client's
 * own address space, which collapses every part of the extension:
 *
 * - XShmAttach records the caller's mapped segment so XShmPutImage can derive
 *   the ShmCompletion event's shmseg and offset. The client usually already
 *   shmat'd the segment; this library only maps it itself when shmaddr is NULL.
 * - XShmPutImage routes pixels through XPutImage. A verbatim upload of the
 *   segment would only pay off for images already in the renderer's texture
 *   format, but this layer's visual advertises ARGB while the streaming texture
 *   is RGBA8888, so every real client still needs the XPutImage swizzle anyway.
 * - shared_pixmaps stays False (see XShmQueryVersion): pixmaps are GPU textures
 *   and cannot be backed by a CPU shm segment with live coherence.
 *
 * The wrappers below route XShmGetImage/XShmPutImage through the regular image
 * APIs. XShmQueryExtension reporting True stays coherent with the generic
 * XQueryExtension("MIT-SHM") probe in src/extension.c.
 */

typedef struct ShmAttachment {
    XShmSegmentInfo *info;
    char *addr;
    size_t size;
    Bool attachedHere;
    int inUse;          /* readers copying a library-owned segment right now */
    Bool pendingDetach; /* XShmDetach raced a reader; unmap once inUse hits 0 */
    struct ShmAttachment *next;
} ShmAttachment;

static ShmAttachment *shmAttachments;
static SDL_mutex *shmAttachmentLock;
static SDL_SpinLock shmAttachmentLockInitLock;

static SDL_mutex *ensureShmAttachmentLock(void)
{
    SDL_AtomicLock(&shmAttachmentLockInitLock);
    if (!shmAttachmentLock)
        shmAttachmentLock = SDL_CreateMutex();
    SDL_mutex *lock = shmAttachmentLock;
    SDL_AtomicUnlock(&shmAttachmentLockInitLock);
    return lock;
}

static SDL_mutex *lockShmAttachments(void)
{
    SDL_mutex *lock = ensureShmAttachmentLock();
    if (lock)
        SDL_LockMutex(lock);
    return lock;
}

static void unlockShmAttachments(SDL_mutex *lock)
{
    if (lock)
        SDL_UnlockMutex(lock);
}

static int destroyShmImage(XImage *image)
{
    free(image);
    return 1;
}

static ShmAttachment *findAttachment(XShmSegmentInfo *info)
{
    for (ShmAttachment *entry = shmAttachments; entry; entry = entry->next) {
        if (entry->info == info)
            return entry;
    }
    return NULL;
}

/* Pin a library-owned (attachedHere) attachment while a reader copies it, so a
 * concurrent XShmDetach defers its shmdt until the copy finishes instead of
 * unmapping the segment out from under it. Client-supplied mappings are the
 * client's lifetime to manage, so they are left unpinned. Caller holds the lock
 * and passes the entry it already looked up (or NULL); returns the entry to
 * hand back to unpinAttachment, or NULL if nothing pinned.
 */
static ShmAttachment *pinAttachmentLocked(ShmAttachment *entry)
{
    if (entry && entry->attachedHere) {
        entry->inUse++;
        return entry;
    }
    return NULL;
}

/* Release a pin taken by pinAttachmentLocked and, if XShmDetach raced this
 * reader, complete the deferred unmap once the last reader leaves.
 */
static void unpinAttachment(ShmAttachment *entry)
{
    if (!entry)
        return;
    SDL_mutex *lock = lockShmAttachments();
    if (--entry->inUse == 0 && entry->pendingDetach) {
        if (entry->attachedHere && entry->addr)
            shmdt(entry->addr);
        free(entry);
    }
    unlockShmAttachments(lock);
}

static Bool queueShmCompletion(Display *dpy,
                               Drawable d,
                               unsigned long shmseg,
                               unsigned long offset)
{
    XEvent *event = calloc(1, sizeof(XEvent));
    if (!event)
        return False;
    XShmCompletionEvent *completion = (XShmCompletionEvent *) event;
    completion->type = XSHM_EVENT_BASE + ShmCompletion;

    /* serial stays 0 (calloc). Real Xlib fills the request serial, but no
     * in-tree client keys completion handling off it.
     */
    completion->send_event = False;
    completion->display = dpy;
    completion->drawable = d;
    completion->major_code = X_ShmPutImage;
    completion->minor_code = 0;
    completion->shmseg = shmseg;
    completion->offset = offset;
    if (!enqueueEvent(dpy, d, event)) {
        free(event);
        return False;
    }
    return True;
}

Bool XShmQueryExtension(Display *dpy)
{
    (void) dpy;
    return True;
}

int XShmGetEventBase(Display *dpy)
{
    (void) dpy;
    return XSHM_EVENT_BASE;
}

Status XShmQueryVersion(Display *dpy,
                        int *major_version,
                        int *minor_version,
                        Bool *shared_pixmaps)
{
    (void) dpy;
    if (major_version)
        *major_version = 1;
    if (minor_version)
        *minor_version = 2;

    /* XShmCreatePixmap below creates a normal pixmap and ignores the SHM data
     * pointer, so the implementation does not actually back shared pixmaps with
     * a shared memory segment. Advertising shared_pixmaps=True would mislead
     * callers into relying on shared-memory semantics this shim does not honor.
     */
    if (shared_pixmaps)
        *shared_pixmaps = False;
    return 1;
}

int XShmPixmapFormat(Display *dpy)
{
    (void) dpy;
    return ZPixmap;
}

Status XShmAttach(Display *dpy, XShmSegmentInfo *shminfo)
{
    (void) dpy;
    if (!shminfo)
        return 0;

    SDL_mutex *lock = lockShmAttachments();
    ShmAttachment *entry = findAttachment(shminfo);
    if (entry) {
        shminfo->shmseg = (ShmSeg) (uintptr_t) shminfo;
        unlockShmAttachments(lock);
        return 1;
    }
    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        unlockShmAttachments(lock);
        return 0;
    }
    entry->info = shminfo;
    if (shminfo->shmid >= 0) {
        struct shmid_ds ds;
        if (shmctl(shminfo->shmid, IPC_STAT, &ds) == 0)
            entry->size = ds.shm_segsz;
    }
    if (shminfo->shmaddr) {
        entry->addr = shminfo->shmaddr;
        entry->attachedHere = False;
    } else if (shminfo->shmid >= 0) {
        void *addr =
            shmat(shminfo->shmid, NULL, shminfo->readOnly ? SHM_RDONLY : 0);
        if (addr == (void *) -1) {
            free(entry);
            unlockShmAttachments(lock);
            return 0;
        }
        shminfo->shmaddr = addr;
        entry->addr = addr;
        entry->attachedHere = True;
    }
    entry->next = shmAttachments;
    shmAttachments = entry;
    shminfo->shmseg = (ShmSeg) (uintptr_t) shminfo;
    unlockShmAttachments(lock);
    return 1;
}

Status XShmDetach(Display *dpy, XShmSegmentInfo *shminfo)
{
    (void) dpy;
    SDL_mutex *lock = lockShmAttachments();
    ShmAttachment **link = &shmAttachments;
    while (*link) {
        ShmAttachment *entry = *link;
        if (entry->info == shminfo) {
            *link = entry->next;
            Bool ownMap = entry->attachedHere && entry->addr;
            if (ownMap)
                entry->info->shmaddr = NULL;
            if (entry->inUse > 0) {
                /* A reader is mid-copy on this library-owned segment. Unlink it
                 * now but leave the mapping in place; unpinAttachment unmaps
                 * and frees once the last reader finishes, avoiding a
                 * use-after-unmap.
                 */
                entry->pendingDetach = True;
            } else {
                if (ownMap)
                    shmdt(entry->addr);
                free(entry);
            }
            unlockShmAttachments(lock);
            return 1;
        }
        link = &entry->next;
    }
    unlockShmAttachments(lock);
    return 1;
}

Bool XShmPutImage(Display *dpy,
                  Drawable d,
                  GC gc,
                  XImage *image,
                  int src_x,
                  int src_y,
                  int dst_x,
                  int dst_y,
                  unsigned int width,
                  unsigned int height,
                  Bool send_event)
{
    XShmSegmentInfo *info = image ? (XShmSegmentInfo *) image->obdata : NULL;
    unsigned long shmseg = 0, offset = 0;

    /* Snapshot the completion-event identifiers under the lock and pin a
     * library-owned segment so an XShmDetach that arrives while this copy is in
     * flight defers its unmap, then drop the lock before rendering. Releasing
     * it first keeps a client error handler that reenters XShmDetach from
     * deadlocking or freeing the entry underneath the render. A put that starts
     * after XShmDetach is use-after-detach, undefined here as in real Xlib.
     */
    SDL_mutex *lock = lockShmAttachments();
    ShmAttachment *entry = findAttachment(info);
    if (entry && image && image->data) {
        shmseg = info->shmseg;
        offset =
            (unsigned long) ((uintptr_t) image->data - (uintptr_t) entry->addr);
    }
    ShmAttachment *pinned = pinAttachmentLocked(entry);
    unlockShmAttachments(lock);

    /* XPutImage returns 1 on success and -1 on failure -- never 0 -- so
     * comparing against zero treats every failure as a success. Match the
     * actual success sentinel instead.
     */
    Bool put_ok = XPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y,
                            width, height) == 1;
    unpinAttachment(pinned);
    if (!put_ok)
        return False;
    if (send_event && !queueShmCompletion(dpy, d, shmseg, offset))
        return False;
    return True;
}

Bool XShmGetImage(Display *dpy,
                  Drawable d,
                  XImage *image,
                  int x,
                  int y,
                  unsigned long plane_mask)
{
    /* XGetSubImage writes through image->data via XPutPixel, which silently
     * no-ops when data is NULL, so it would report success without copying.
     * Reject that up front.
     */
    if (!image || (image->width && image->height && !image->data))
        return False;

    /* XGetSubImage writes the copied-back pixels into image->data, so a
     * read-only shared segment would fault. readOnly lives in the client's
     * shminfo and this library never changes it, so check it lock-free and
     * before the attachment lookup: this also rejects a read-only image that
     * was never XShmAttach'd, which no entry would catch.
     */
    XShmSegmentInfo *info = (XShmSegmentInfo *) image->obdata;
    if (info && info->readOnly)
        return False;

    SDL_mutex *lock = lockShmAttachments();
    ShmAttachment *pinned = pinAttachmentLocked(findAttachment(info));
    unlockShmAttachments(lock);

    Bool ok = XGetSubImage(dpy, d, x, y, image->width, image->height,
                           plane_mask, image->format, image, 0, 0)
                  ? True
                  : False;
    unpinAttachment(pinned);
    return ok;
}

XImage *XShmCreateImage(Display *dpy,
                        Visual *visual,
                        unsigned int depth,
                        int format,
                        char *data,
                        XShmSegmentInfo *shminfo,
                        unsigned int width,
                        unsigned int height)
{
    /* The caller owns image->data. In the canonical MIT-SHM sequence the client
     * sets image->data = shminfo->shmaddr = shmat(...) itself, so bind
     * whichever address is already present. A client that fills
     * shminfo->shmaddr only after this call gets a NULL-data image; that is not
     * the documented flow.
     */
    XImage *image =
        XCreateImage(dpy, visual, depth, format, 0,
                     data ? data : (shminfo ? shminfo->shmaddr : NULL), width,
                     height, 32, 0);
    if (image) {
        image->f.destroy_image = destroyShmImage;
        image->obdata = (XPointer) shminfo;
    }
    return image;
}

Pixmap XShmCreatePixmap(Display *dpy,
                        Drawable d,
                        char *data,
                        XShmSegmentInfo *shminfo,
                        unsigned int width,
                        unsigned int height,
                        unsigned int depth)
{
    (void) data;
    (void) shminfo;
    return XCreatePixmap(dpy, d, width, height, depth);
}
