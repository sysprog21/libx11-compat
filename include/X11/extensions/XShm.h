#ifndef _XSHM_H_
#define _XSHM_H_

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/ipc.h>

#define X_ShmQueryVersion 0
#define X_ShmAttach 1
#define X_ShmDetach 2
#define X_ShmPutImage 3
#define X_ShmGetImage 4
#define X_ShmCreatePixmap 5

#define ShmCompletion 0
#define ShmNumberEvents (ShmCompletion + 1)

typedef unsigned long ShmSeg;

typedef struct {
    ShmSeg shmseg;
    int shmid;
    char *shmaddr;
    Bool readOnly;
} XShmSegmentInfo;

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Drawable drawable;
    int major_code;
    int minor_code;
    ShmSeg shmseg;
    unsigned long offset;
} XShmCompletionEvent;

extern Bool XShmQueryExtension(Display *dpy);
extern int XShmGetEventBase(Display *dpy);
extern Status XShmQueryVersion(Display *dpy,
                               int *major_version,
                               int *minor_version,
                               Bool *shared_pixmaps);
extern int XShmPixmapFormat(Display *dpy);
extern Status XShmAttach(Display *dpy, XShmSegmentInfo *shminfo);
extern Status XShmDetach(Display *dpy, XShmSegmentInfo *shminfo);
extern Bool XShmPutImage(Display *dpy,
                         Drawable d,
                         GC gc,
                         XImage *image,
                         int src_x,
                         int src_y,
                         int dst_x,
                         int dst_y,
                         unsigned int width,
                         unsigned int height,
                         Bool send_event);
extern Bool XShmGetImage(Display *dpy,
                         Drawable d,
                         XImage *image,
                         int x,
                         int y,
                         unsigned long plane_mask);
extern XImage *XShmCreateImage(Display *dpy,
                               Visual *visual,
                               unsigned int depth,
                               int format,
                               char *data,
                               XShmSegmentInfo *shminfo,
                               unsigned int width,
                               unsigned int height);
extern Pixmap XShmCreatePixmap(Display *dpy,
                               Drawable d,
                               char *data,
                               XShmSegmentInfo *shminfo,
                               unsigned int width,
                               unsigned int height,
                               unsigned int depth);

#endif /* _XSHM_H_ */
