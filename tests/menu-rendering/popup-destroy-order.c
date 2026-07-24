/* Lifecycle guard: destroying the parent of a live SDL3 xdg_popup must unmap
 * the popup through the normal path before tearing down native surfaces. Map a
 * main window, map an override-redirect popup anchored inside it, then
 * XDestroyWindow the parent FIRST while the popup is still realized. SDL owns
 * the popup as a child of the parent surface, so the parent teardown must not
 * leave it logically mapped with a destroyed SDL window.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <unistd.h>

static void pump(Display *d, int ticks)
{
    for (int i = 0; i < ticks; i++) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
        }
        usleep(20000);
    }
}

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "no display\n");
        return 1;
    }
    int s = DefaultScreen(d);
    Window root = RootWindow(d, s);

    Window main =
        XCreateSimpleWindow(d, root, 80, 80, 400, 300, 0, 0, 0x224488);
    XSelectInput(d, main, ExposureMask | StructureNotifyMask);
    XMapWindow(d, main);
    XFlush(d);
    pump(d, 20);

    XSetWindowAttributes a;
    a.override_redirect = True;
    a.background_pixel = 0xcc3333;
    Window popup = XCreateWindow(d, root, 90, 120, 160, 120, 0, CopyFromParent,
                                 InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel, &a);
    XSelectInput(d, popup, StructureNotifyMask);
    XMapWindow(d, popup);
    XFlush(d);
    pump(d, 20);

    /* Parent first, popup still mapped: the guarded order. */
    XDestroyWindow(d, main);
    XSync(d, False);

    Bool got_unmap_notify = False;
    for (int i = 0; i < 10 && !got_unmap_notify; i++) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type == UnmapNotify && e.xunmap.window == popup)
                got_unmap_notify = True;
        }
        usleep(20000);
    }
    if (!got_unmap_notify) {
        fprintf(stderr,
                "popup never received UnmapNotify after parent destroy\n");
        XCloseDisplay(d);
        return 1;
    }

    XWindowAttributes wa;
    if (!XGetWindowAttributes(d, popup, &wa)) {
        fprintf(stderr, "XGetWindowAttributes(popup) failed\n");
        XCloseDisplay(d);
        return 1;
    }
    if (wa.map_state != IsUnmapped) {
        fprintf(stderr,
                "popup still mapped after parent destroy (map_state=%d)\n",
                wa.map_state);
        XCloseDisplay(d);
        return 1;
    }

    XMapWindow(d, popup);
    XSync(d, False);
    pump(d, 10);
    if (!XGetWindowAttributes(d, popup, &wa)) {
        fprintf(stderr, "XGetWindowAttributes(remapped popup) failed\n");
        XCloseDisplay(d);
        return 1;
    }
    if (wa.map_state != IsViewable) {
        fprintf(stderr, "popup did not remap after parent destroy\n");
        XCloseDisplay(d);
        return 1;
    }

    XDestroyWindow(d, popup);
    XSync(d, False);

    XCloseDisplay(d);
    printf("DESTROY_ORDER_OK\n");
    return 0;
}
