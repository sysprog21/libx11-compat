#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xatom.h>
#include <X11/Xlib-xcb.h>

#define CHECK(c, m)                     \
    do {                                \
        if (!(c)) {                     \
            fprintf(stderr, "%s\n", m); \
            return 1;                   \
        }                               \
    } while (0)

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    CHECK(display, "open display");
    xcb_connection_t *connection = XGetXCBConnection(display);
    CHECK(connection && connection == XGetXCBConnection(display),
          "one wrapper per display");
    Display *otherDisplay = XOpenDisplay(NULL);
    CHECK(otherDisplay, "open second display");
    xcb_connection_t *otherConnection = XGetXCBConnection(otherDisplay);
    CHECK(otherConnection && otherConnection != connection,
          "distinct wrappers for two displays");
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0,
                                        0, 16, 16, 0, 0, 0);
    Atom property = XInternAtom(display, "X11_XCB_SHARED", False);
    const char value[] = "shared";
    XChangeProperty(display, window, property, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *) value, sizeof(value) - 1);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(
        connection,
        xcb_get_property(connection, 0, window, property, XCB_ATOM_STRING, 0,
                         sizeof(value)),
        NULL);
    CHECK(reply && xcb_get_property_value_length(reply) == sizeof(value) - 1 &&
              !memcmp(xcb_get_property_value(reply), value, sizeof(value) - 1),
          "Xlib write visible to XCB");
    free(reply);
    XSetEventQueueOwner(display, XCBOwnsEventQueue);
    XSetEventQueueOwner(display, XlibOwnsEventQueue);
    xcb_disconnect(otherConnection);
    XCloseDisplay(otherDisplay);
    xcb_disconnect(connection);
    XDestroyWindow(display, window);
    XCloseDisplay(display);

    display = XOpenDisplay(NULL);
    CHECK(display, "reopen display");
    connection = XGetXCBConnection(display);
    CHECK(connection, "second wrapper");
    xcb_screen_t *screen =
        xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
    xcb_gcontext_t gc = xcb_generate_id(connection);
    CHECK(!xcb_request_check(
              connection,
              xcb_create_gc_checked(connection, gc, screen->root, 0, NULL)),
          "create shared-display GC");
    xcb_connection_t *survivor = xcb_connect(NULL, NULL);
    CHECK(survivor && !xcb_connection_has_error(survivor),
          "open close-hook survivor");
    XCloseDisplay(display);
    CHECK(xcb_connection_has_error(connection) == XCB_CONN_ERROR,
          "display-first close invalidates wrapper");
    xcb_point_t point = {0, 0};
    xcb_generic_error_t *error = xcb_request_check(
        survivor,
        xcb_poly_point_checked(
            survivor, XCB_COORD_MODE_ORIGIN,
            xcb_setup_roots_iterator(xcb_get_setup(survivor)).data->root, gc, 1,
            &point));
    CHECK(error && error->error_code == XCB_G_CONTEXT,
          "display close removes owned GC entries");
    free(error);
    xcb_disconnect(connection);
    xcb_disconnect(survivor);
    puts("test-xlib-xcb: ok");
    return 0;
}
