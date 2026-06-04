#include <stdio.h>
#include <stdlib.h>

#include <X11/Xlib.h>
#include <X11/xpm.h>

static char *tiny_xpm[] = {
    "4 4 2 1", ". c #000000", "o c #ffffff", ".oo.", "o..o", "o..o", ".oo.",
};

int main(void)
{
    if (!getenv("SDL_VIDEODRIVER"))
        setenv("SDL_VIDEODRIVER", "dummy", 1);

    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "XOpenDisplay failed\n");
        return 1;
    }

    Window root = DefaultRootWindow(display);
    Pixmap pixmap = None;
    Pixmap mask = None;
    XpmAttributes attrs;
    attrs.valuemask = 0;

    int rc = XpmCreatePixmapFromData(display, root, tiny_xpm, &pixmap, &mask,
                                     &attrs);
    if (rc != XpmSuccess || pixmap == None) {
        fprintf(stderr, "XpmCreatePixmapFromData failed: %d\n", rc);
        XCloseDisplay(display);
        return 1;
    }

    XFreePixmap(display, pixmap);
    if (mask != None)
        XFreePixmap(display, mask);
    XCloseDisplay(display);
    printf("test_libxpm_link: ok\n");
    return 0;
}
