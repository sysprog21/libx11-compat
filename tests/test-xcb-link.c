#include <stdio.h>
#include <xcb/xcb.h>

int main(void)
{
    int screenNumber = -1;
    xcb_connection_t *connection = xcb_connect(NULL, &screenNumber);
    if (!connection || xcb_connection_has_error(connection)) {
        fprintf(stderr, "test-xcb-link: connection failed\n");
        return 1;
    }
    const xcb_setup_t *setup = xcb_get_setup(connection);
    if (!setup) {
        fprintf(stderr, "test-xcb-link: invalid setup\n");
        xcb_disconnect(connection);
        return 1;
    }
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    if (setup->status != 1 || screenNumber != 0 || screens.rem != 1 ||
        !screens.data || screens.data->root == XCB_WINDOW_NONE) {
        fprintf(stderr, "test-xcb-link: invalid setup\n");
        xcb_disconnect(connection);
        return 1;
    }
    if (!xcb_flush(connection)) {
        fprintf(stderr, "test-xcb-link: flush failed\n");
        xcb_disconnect(connection);
        return 1;
    }
    xcb_disconnect(connection);
    puts("test-xcb-link: ok");
    return 0;
}
