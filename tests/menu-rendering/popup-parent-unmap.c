/* P1 lifecycle guard: unmapping a popup's parent must tear the anchored popup
 * down through the normal unmap path, not leave it logically mapped with a dead
 * SDL surface. Map a main window and an override-redirect popup anchored inside
 * it, then XUnmapWindow the parent while the popup is still mapped, and remap
 * the parent. Run under AddressSanitizer: the popup's owned SDL surface is
 * freed when the parent unmaps, so a stale reference or a re-map that cannot
 * recreate it would surface here.
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

    /* Unmap the parent while the popup is still mapped: the popup must be
     * unmapped through the full path, not stranded.
     */
    XUnmapWindow(d, main);
    XSync(d, False);

    /* Confirm the parent-unmap sweep drove the popup through the real unmap
     * path, not just avoided a crash. Two independent signals: the popup must
     * receive its own UnmapNotify (the sweep posted it), and it must end up
     * IsUnmapped. A stranded popup would keep getting neither and stay
     * IsViewable.
     */
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
                "popup never received UnmapNotify after parent unmap\n");
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
                "popup still mapped after parent unmap (map_state=%d)\n",
                wa.map_state);
        XCloseDisplay(d);
        return 1;
    }

    /* Remapping the parent must work cleanly afterward. */
    XMapWindow(d, main);
    XSync(d, False);
    pump(d, 10);

    XCloseDisplay(d);
    printf("PARENT_UNMAP_OK\n");
    return 0;
}
