/*
 * An override-redirect popup resized after mapping must grow the compositor's
 * xdg_popup geometry, not just its surface buffer. An xdg_popup's size is fixed
 * by its positioner at creation, so a plain SDL_SetWindowSize leaves the popup
 * clipped to its old rectangle: the exact "a large menu is cropped to a smaller
 * size with parts missing" report when Motif switches the armed pulldown from a
 * short menu to a taller one. Map a small popup, then XResizeWindow it larger.
 * Run under WAYLAND_DEBUG: the fix re-issues the positioner (set_size / a popup
 * reposition) at the new size, so the log must carry the resized dimensions.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <unistd.h>

static void pump(Display *d, int ms)
{
    for (int i = 0; i < ms / 25; i++) {
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
        }
        XFlush(d);
        usleep(25000);
    }
}

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "no display\n");
        return 1;
    }
    Window root = RootWindow(d, DefaultScreen(d));
    Window mw = XCreateSimpleWindow(d, root, 80, 80, 400, 300, 0, 0, 0x224488);
    XStoreName(d, mw, "resize-main");
    XSelectInput(d, mw, ExposureMask | StructureNotifyMask);
    XMapWindow(d, mw);
    XFlush(d);
    pump(d, 500);

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = 0xcc3333;
    Window popup =
        XCreateWindow(d, root, 90, 120, 80, 40, 0, CopyFromParent, InputOutput,
                      CopyFromParent, CWOverrideRedirect | CWBackPixel, &attrs);
    XStoreName(d, popup, "resize-popup");
    XSelectInput(d, popup, ExposureMask | StructureNotifyMask);
    XMapWindow(d, popup);
    XFlush(d);
    pump(d, 500);

    XResizeWindow(d, popup, 220, 180);
    XFlush(d);
    pump(d, 700);

    XWindowAttributes wa;
    if (XGetWindowAttributes(d, popup, &wa))
        printf("POPUP_WA wh=(%dx%d)\n", wa.width, wa.height);
    printf("POPUP_RESIZE_DONE\n");
    XCloseDisplay(d);
    return 0;
}
