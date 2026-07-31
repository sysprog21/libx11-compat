/*
 * A cascade submenu must anchor to its parent popup, not to the app shell: the
 * child is created inside the parent popup's requested X11 geometry, so parent
 * selection must pick the parent popup and compute the child's offset from the
 * parent's requested origin. The validator confirms the child is a nested
 * xdg_popup of the parent popup surface and that its configured offset equals
 * the requested one.
 *
 * The parent popup is kept fully on screen. SDL3's Wayland backend cannot give
 * an xdg_popup role to a child whose parent popup lies off screen on a
 * non-constraining compositor (weston headless does not slide it back on), so
 * an off-screen parent would make the child silently fall back with no popup
 * role and mask the anchoring logic this test exercises. The compositor-slide
 * divergence is covered separately, where a constraining compositor slides the
 * parent on screen.
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

    int mainX = 80, mainY = 40, mainW = 400, mainH = 560;
    Window main = XCreateSimpleWindow(d, root, mainX, mainY, mainW, mainH, 0, 0,
                                      0x224488);
    XStoreName(d, main, "nested-main");
    XSelectInput(d, main, ExposureMask | StructureNotifyMask);
    XMapWindow(d, main);
    XFlush(d);
    pump(d, 800);

    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = 0xcc3333;
    int popX = mainX + 10, popY = mainY + 300, popW = 160, popH = 220;
    Window popup = XCreateWindow(d, root, popX, popY, popW, popH, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel, &attrs);
    XStoreName(d, popup, "nested-parent-popup");
    XSelectInput(d, popup, ExposureMask | StructureNotifyMask);
    XMapWindow(d, popup);
    XFlush(d);
    pump(d, 1000);

    attrs.background_pixel = 0x33aa55;
    int childX = popX + popW, childY = popY + 20;

    /* The child submenu must anchor to the parent popup's requested origin, so
     * its expected relative offset is its own X11 origin minus the parent's.
     * The validator compares this against the child xdg_popup's configure so an
     * offset computed from a compositor-slid parent origin gets caught.
     */
    printf("CHILD_REL=(%d,%d)\n", childX - popX, childY - popY);
    Window child = XCreateWindow(d, root, childX, childY, 120, 90, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel, &attrs);
    XStoreName(d, child, "nested-child-popup");
    XSelectInput(d, child, ExposureMask | StructureNotifyMask);
    XMapWindow(d, child);
    XFlush(d);
    pump(d, 1000);

    XDestroyWindow(d, popup);
    XFlush(d);
    pump(d, 500);

    XWindowAttributes wa;
    if (!XGetWindowAttributes(d, child, &wa) || wa.map_state != IsUnmapped) {
        fprintf(stderr, "nested child stayed mapped after parent destroy\n");
        return 2;
    }
    printf("NESTED_CHILD_UNMAPPED\n");
    printf("POPUP_NESTED_DONE\n");
    XCloseDisplay(d);
    return 0;
}
