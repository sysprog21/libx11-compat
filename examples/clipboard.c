/*
 * clipboard - interactive SDL clipboard bridge probe.
 *
 * Press C to seed SDL's clipboard, T to query CLIPBOARD TARGETS, V to read
 * UTF8_STRING text, and Esc to quit. Pass --once to print the TARGETS list
 * without opening an interactive loop.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include "sdl-compat.h"
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile bool quitRequested = false;

/* Handler only sets the flag; everything else (SDL, stdio, malloc) is not
 * async-signal-safe. The main loop checks the flag on the next event, so
 * Ctrl-C followed by any keystroke or WM event runs cleanup cleanly. */
static void onSignal(int signo)
{
    (void) signo;
    quitRequested = true;
}

static Bool isSelectionNotify(Display *display, XEvent *event, XPointer arg)
{
    (void) display;
    (void) arg;
    return event->type == SelectionNotify;
}

#define WIN_W 560
#define WIN_H 220

typedef struct {
    Display *display;
    Window window;
    GC gc;
    Atom clipboard;
    Atom targets;
    Atom utf8;
    Atom property;
    const char *status;
    char details[256];
} App;

static void convertSelection(App *app, Atom target)
{
    XConvertSelection(app->display, app->clipboard, target, app->property,
                      app->window, CurrentTime);
}

static int readProperty(App *app,
                        Atom reqType,
                        Atom *actualType,
                        int *actualFormat,
                        unsigned long *nItems,
                        unsigned char **data)
{
    unsigned long bytesAfter = 0;
    *data = NULL;
    /* long_length is in 32-bit units. LONG_MAX / 4 covers any plausible
     * clipboard payload in a single request (the X11 wire protocol caps a
     * request at ~16 MiB anyway), so bytesAfter==0 below stays a valid
     * "fully read" check without truncating a large UTF8_STRING. */
    int status = XGetWindowProperty(app->display, app->window, app->property, 0,
                                    LONG_MAX / 4, False, reqType, actualType,
                                    actualFormat, nItems, &bytesAfter, data);
    /* XGetWindowProperty allocates *data even when the helper's secondary
     * checks (success + bytesAfter==0 + non-NULL data) reject the result. */
    if (status != Success || bytesAfter != 0 || !*data) {
        if (*data) {
            XFree(*data);
            *data = NULL;
        }
        return 0;
    }
    return 1;
}

static void render(App *app)
{
    XClearWindow(app->display, app->window);
    XDrawString(app->display, app->window, app->gc, 20, 32,
                "libx11-compat clipboard", 23);
    XDrawString(app->display, app->window, app->gc, 20, 62,
                "C: copy sample text   T: query TARGETS   V: read text", 56);
    XDrawString(app->display, app->window, app->gc, 20, 92,
                "Esc: quit", 9);
    XDrawString(app->display, app->window, app->gc, 20, 132, app->status,
                (int) strlen(app->status));
    XDrawString(app->display, app->window, app->gc, 20, 162, app->details,
                (int) strlen(app->details));
    XFlush(app->display);
}

static void showTargets(App *app)
{
    convertSelection(app, app->targets);

    XEvent event;
    XIfEvent(app->display, &event, isSelectionNotify, NULL);
    if (event.xselection.property == None) {
        app->status = "TARGETS conversion failed";
        app->details[0] = '\0';
        return;
    }

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0;
    unsigned char *data = NULL;
    if (!readProperty(app, XA_ATOM, &actualType, &actualFormat, &nItems,
                      &data)) {
        app->status = "TARGETS property read failed";
        app->details[0] = '\0';
        return;
    }
    if (actualType != XA_ATOM || actualFormat != 32) {
        XFree(data);
        app->status = "TARGETS property read failed";
        app->details[0] = '\0';
        return;
    }

    Atom *atoms = (Atom *) data;
    app->status = "CLIPBOARD target atoms:";
    app->details[0] = '\0';
    for (unsigned long i = 0; i < nItems; i++) {
        char *name = XGetAtomName(app->display, atoms[i]);
        size_t used = strlen(app->details);
        snprintf(app->details + used, sizeof(app->details) - used, "%s%s",
                 used ? ", " : "", name ? name : "<unknown>");
        XFree(name);
    }
    XFree(data);
}

static void showClipboardText(App *app)
{
    convertSelection(app, app->utf8);

    XEvent event;
    XIfEvent(app->display, &event, isSelectionNotify, NULL);
    if (event.xselection.property == None) {
        app->status = "UTF8_STRING conversion failed";
        app->details[0] = '\0';
        return;
    }

    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nItems = 0;
    unsigned char *data = NULL;
    if (!readProperty(app, app->utf8, &actualType, &actualFormat, &nItems,
                      &data)) {
        app->status = "UTF8_STRING property read failed";
        app->details[0] = '\0';
        return;
    }
    if (actualType != app->utf8 || actualFormat != 8) {
        XFree(data);
        app->status = "UTF8_STRING property read failed";
        app->details[0] = '\0';
        return;
    }

    app->status = "CLIPBOARD text:";
    snprintf(app->details, sizeof(app->details), "%.*s", (int) nItems,
             (char *) data);
    XFree(data);
}

static int runOnce(App *app)
{
    XSetSelectionOwner(app->display, app->clipboard, None, CurrentTime);
    SDL_SetClipboardText("clipboard text from SDL");
    showTargets(app);
    if (app->details[0] == '\0') {
        fprintf(stderr, "%s\n", app->status);
        return 1;
    }
    printf("SDL clipboard text is available through XConvertSelection.\n");
    printf("Supported CLIPBOARD target atoms: %s\n", app->details);
    return 0;
}

static void seedClipboard(App *app)
{
    XSetSelectionOwner(app->display, app->clipboard, None, CurrentTime);
    SDL_SetClipboardText("clipboard text from SDL");
    app->status = "Copied sample text into SDL clipboard";
    snprintf(app->details, sizeof(app->details), "%s",
             "Press T to inspect targets or V to read the text back.");
}

static void cleanup(App *app)
{
    if (app->gc)
        XFreeGC(app->display, app->gc);
    if (app->window)
        XDestroyWindow(app->display, app->window);
    if (app->display)
        XCloseDisplay(app->display);
}

int main(int argc, char **argv)
{
    App app;
    memset(&app, 0, sizeof(app));
    app.status = "Press C to seed the SDL clipboard";

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    app.display = XOpenDisplay(NULL);
    if (!app.display) {
        fprintf(stderr, "XOpenDisplay failed\n");
        return 1;
    }

    int rc = 1;
    int screen = DefaultScreen(app.display);
    Window root = RootWindow(app.display, screen);
    app.window = XCreateSimpleWindow(app.display, root, 0, 0, WIN_W, WIN_H, 1,
                                     BlackPixel(app.display, screen),
                                     WhitePixel(app.display, screen));
    if (app.window == None) {
        fprintf(stderr, "XCreateSimpleWindow failed\n");
        goto done;
    }
    app.gc = XCreateGC(app.display, app.window, 0, NULL);
    if (!app.gc) {
        fprintf(stderr, "XCreateGC failed\n");
        goto done;
    }
    app.clipboard = XInternAtom(app.display, "CLIPBOARD", False);
    app.targets = XInternAtom(app.display, "TARGETS", False);
    app.utf8 = XInternAtom(app.display, "UTF8_STRING", False);
    app.property = XInternAtom(app.display, "CLIPBOARD_EXAMPLE", False);
    if (app.clipboard == None || app.targets == None || app.utf8 == None ||
        app.property == None) {
        fprintf(stderr, "XInternAtom failed\n");
        goto done;
    }

    if (argc > 1 && strcmp(argv[1], "--once") == 0) {
        rc = runOnce(&app);
        goto done;
    }

    XStoreName(app.display, app.window, "clipboard");
    Atom wmDelete = XInternAtom(app.display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app.display, app.window, &wmDelete, 1);
    XSelectInput(app.display, app.window, ExposureMask | KeyPressMask);
    XMapWindow(app.display, app.window);

    XEvent event;
    while (!quitRequested) {
        XNextEvent(app.display, &event);
        if (quitRequested)
            break;
        switch (event.type) {
        case Expose:
            if (event.xexpose.count == 0)
                render(&app);
            break;
        case KeyPress: {
            KeySym key = XLookupKeysym(&event.xkey, 0);
            if (key == XK_Escape) {
                rc = 0;
                goto done;
            } else if (key == XK_c || key == XK_C) {
                seedClipboard(&app);
            } else if (key == XK_t || key == XK_T) {
                showTargets(&app);
            } else if (key == XK_v || key == XK_V) {
                showClipboardText(&app);
            }
            render(&app);
            break;
        }
        case ClientMessage:
            if ((Atom) event.xclient.data.l[0] == wmDelete) {
                rc = 0;
                goto done;
            }
            break;
        }
    }
    rc = 0;

done:
    cleanup(&app);
    return rc;
}
