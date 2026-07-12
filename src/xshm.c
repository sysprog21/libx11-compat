#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <stddef.h>
#include <stdlib.h>
#include "events.h"

#define XSHM_EVENT_BASE 128

/* MIT-SHM exists to skip copying image bytes across the X11 client/server
 * socket. libx11-compat has no such socket: the "server" runs in the client's
 * own address space, which collapses every part of the extension:
 *
 * - XShmAttach is a no-op. The client already shmat'd the segment and set
 *   shminfo->shmaddr; image->data points at that mapping, already readable
 * here. There is nothing for the "server" to attach to.
 * - XShmPutImage cannot be zero-copy. XPutImage swizzles each pixel from the
 *   X-canonical byte order into SDL's texture format and forces opaque pixels
 *   visible before SDL_UpdateTexture, which does a raw per-row memcpy. So
 *   image->data is not a general upload source; that per-pixel transform is
 *   mandatory work, not a wire copy MIT-SHM elides, so XPutImage is the fast
 *   path.
 * - shared_pixmaps stays False (see XShmQueryVersion): pixmaps are GPU textures
 *   and cannot be backed by a CPU shm segment with live coherence.
 *
 * The wrappers below route XShmGetImage/XShmPutImage through the regular image
 * APIs. XShmQueryExtension reporting True stays coherent with the generic
 * XQueryExtension("MIT-SHM") probe in src/extension.c.
 */

static int destroyShmImage(XImage *image)
{
    free(image);
    return 1;
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
    (void) shminfo;
    return 1;
}

Status XShmDetach(Display *dpy, XShmSegmentInfo *shminfo)
{
    (void) dpy;
    (void) shminfo;
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
    /* XPutImage returns 1 on success and -1 on failure -- never 0 -- so
     * comparing against zero treats every failure as a success. Match the
     * actual success sentinel instead.
     */
    if (XPutImage(dpy, d, gc, image, src_x, src_y, dst_x, dst_y, width,
                  height) != 1) {
        return False;
    }
    if (send_event) {
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
        if (!enqueueEvent(dpy, d, event)) {
            free(event);
            return False;
        }
    }
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
     * no-ops when data is NULL, so it would report success without copying. A
     * no-op XShmAttach can leave image->data NULL if the client fills shmaddr
     * only after XShmCreateImage, so reject that up front.
     */
    if (!image || (image->width && image->height && !image->data))
        return False;
    return XGetSubImage(dpy, d, x, y, image->width, image->height, plane_mask,
                        image->format, image, 0, 0)
               ? True
               : False;
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
     * whichever the caller has already provided. Since XShmAttach is a no-op, a
     * client that fills shminfo->shmaddr only after this call gets a NULL-data
     * image; that is not the documented flow.
     */
    (void) shminfo;
    XImage *image =
        XCreateImage(dpy, visual, depth, format, 0,
                     data ? data : (shminfo ? shminfo->shmaddr : NULL), width,
                     height, 32, 0);
    if (image)
        image->f.destroy_image = destroyShmImage;
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
