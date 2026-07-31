/* A Motif-style override-redirect popup must be anchored to its parent surface,
 * not spawned as a detached top-level. This maps a main window and then an
 * override-redirect popup whose top-left falls inside the main window, the way
 * a pulldown drops off a menu bar. Run under WAYLAND_DEBUG and grep the
 * protocol: with the fix the popup is an xdg_popup with a positioner parented
 * to the main window's xdg_surface; without it the popup is a second
 * xdg_toplevel the compositor places wherever it likes.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "no display\n");
        return 1;
    }
    int s = DefaultScreen(d);
    Window root = RootWindow(d, s);

    int mainX = 80, mainY = 80, mainW = 400, mainH = 300;
    Window main = XCreateSimpleWindow(d, root, mainX, mainY, mainW, mainH, 0, 0,
                                      0x224488);
    XStoreName(d, main, "menu-main");
    XSelectInput(d, main, ExposureMask | StructureNotifyMask);
    XMapWindow(d, main);
    XFlush(d);

    int popX = mainX + 10, popY = mainY + 40, popW = 160, popH = 120;
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = 0xcc3333;
    Window popup = XCreateWindow(d, root, popX, popY, popW, popH, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel, &attrs);
    XStoreName(d, popup, "menu-popup");
    XSelectInput(d, popup, ExposureMask);

    int mapped = 0;
    for (int i = 0; i < 120; i++) {
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
        }
        if (i == 20 && !mapped) {
            XMapWindow(d, popup);
            mapped = 1;
        }
        XFlush(d);
        usleep(25000);
    }
    XCloseDisplay(d);
    printf("POPUP_ANCHOR_DONE\n");
    return 0;
}
