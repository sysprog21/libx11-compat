/*
 * An anchored popup's recorded absolute origin (ws->x/y, used for every pointer
 * event's root translation) must equal where the compositor actually placed the
 * popup, not where the client requested it. A constraint-enforcing compositor
 * slides or flips a popup that would fall off screen; if ws->x/y keeps the
 * requested position, pointer coordinates for a grabbed submenu land off by the
 * slide, so the highlight drifts from the cursor.
 *
 * This maps a tall parent and anchors an override-redirect popup deep inside
 * it, deep enough that a compositor which honors xdg_positioner constraints
 * slides it up. It prints the requested anchor, the parent origin, and the
 * readback of the popup's absolute origin via XTranslateCoordinates(popup,
 * root). The harness compares the readback against parent + the compositor's
 * actual xdg_popup.configure offset (from WAYLAND_DEBUG), so it validates the
 * invariant on a non-constraining compositor (no slide) and a constraining one
 * (slide) alike. weston headless does not constrain; wilco does.
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

    int mainX = 80, mainY = 80, mainW = 400, mainH = 960;
    Window mw = XCreateSimpleWindow(d, root, mainX, mainY, mainW, mainH, 0, 0,
                                    0x224488);
    XStoreName(d, mw, "slide-main");
    XSelectInput(d, mw, ExposureMask | StructureNotifyMask);
    XMapWindow(d, mw);
    XFlush(d);
    pump(d, 800);

    int popX = mainX + 10, popY = mainY + 890, popW = 160, popH = 220;
    XSetWindowAttributes attrs;
    attrs.override_redirect = True;
    attrs.background_pixel = 0xcc3333;
    Window popup = XCreateWindow(d, root, popX, popY, popW, popH, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel, &attrs);
    XStoreName(d, popup, "slide-popup");
    XSelectInput(d, popup, ExposureMask | StructureNotifyMask);
    XMapWindow(d, popup);
    XFlush(d);
    pump(d, 1000);

    int rx = -1, ry = -1, px = -1, py = -1;
    Window child = None;
    XTranslateCoordinates(d, popup, root, 0, 0, &rx, &ry, &child);

    /* Report the parent origin the library actually recorded, not the requested
     * constants, so the harness compares against the real anchor base.
     */
    XTranslateCoordinates(d, mw, root, 0, 0, &px, &py, &child);
    printf("PARENT_ORIGIN=(%d,%d)\n", px, py);
    printf("REQUESTED_ANCHOR=(%d,%d)\n", popX, popY);
    printf("READBACK_ABS_ORIGIN=(%d,%d)\n", rx, ry);
    printf("POPUP_SLIDE_DONE\n");
    XCloseDisplay(d);
    return 0;
}
