#include <X11/extensions/Xinerama.h>
#include <X11/Xlibint.h>
#include <limits.h>
#include <stdlib.h>

Bool XineramaQueryExtension(Display *dpy,
                            int *event_base_return,
                            int *error_base_return)
{
    (void) dpy;
    if (event_base_return)
        *event_base_return = 0;
    if (error_base_return)
        *error_base_return = 0;
    return True;
}

Status XineramaQueryVersion(Display *dpy,
                            int *major_version_return,
                            int *minor_version_return)
{
    (void) dpy;
    if (major_version_return)
        *major_version_return = 1;
    if (minor_version_return)
        *minor_version_return = 1;
    return True;
}

Bool XineramaIsActive(Display *dpy)
{
    /* Match XineramaQueryScreens: report inactive when no screen is available
     * so a client can't get True here and then crash on the NULL it returns. */
    return dpy && dpy->nscreens > 0;
}

XineramaScreenInfo *XineramaQueryScreens(Display *dpy, int *number_return)
{
    if (number_return)
        *number_return = 0;
    if (!dpy || dpy->nscreens <= 0)
        return NULL;

    XineramaScreenInfo *info =
        calloc((size_t) dpy->nscreens, sizeof(*info));
    if (!info)
        return NULL;

    for (int i = 0; i < dpy->nscreens; i++) {
        Screen *screen = &dpy->screens[i];
        info[i].screen_number = i;
        info[i].x_org = 0;
        info[i].y_org = 0;
        /* XineramaScreenInfo's width/height are `short` per the X11 ABI; clamp
         * at SHRT_MAX so a 4K+ screen reports the ceiling instead of silently
         * wrapping to a negative or tiny value. */
        info[i].width = screen->width > SHRT_MAX ? SHRT_MAX
                                                 : (short) screen->width;
        info[i].height = screen->height > SHRT_MAX ? SHRT_MAX
                                                   : (short) screen->height;
    }
    if (number_return)
        *number_return = dpy->nscreens;
    return info;
}
