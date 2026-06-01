#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <stddef.h>
#include <stdlib.h>
#include "events.h"

#define XSHM_EVENT_BASE 128

/* The MIT-SHM extension speeds up image transfer with shared memory. We have
 * no client/server boundary, so XGetSubImage / XPutImage are already the fast
 * path. These wrappers let callers that probe XShm fall back without crashing,
 * and route XShmGetImage/XShmPutImage to the regular image APIs. */

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
    /* We do not actually back shared pixmaps with a shared memory segment
     * (XShmCreatePixmap below creates a normal pixmap and ignores the SHM
     * data pointer). Advertising shared_pixmaps=True misleads callers into
     * relying on shared-memory semantics we do not honor. */
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
     * actual success sentinel instead. */
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
    if (!image)
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
    (void) shminfo;
    XImage *image =
        XCreateImage(dpy, visual, depth, format, 0,
                     data ? data : (shminfo ? shminfo->shmaddr : NULL), width,
                     height, 32, 0);
    if (image) {
        image->f.destroy_image = destroyShmImage;
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
