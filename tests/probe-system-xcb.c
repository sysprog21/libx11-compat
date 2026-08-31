/* Deterministic core-XCB reference probe. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

static void fail(const char *message)
{
    fprintf(stderr, "probe-system-xcb: %s\n", message);
    exit(1);
}

static xcb_screen_t *firstScreen(const xcb_setup_t *setup)
{
    xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(setup);
    return iterator.rem ? iterator.data : NULL;
}

int main(void)
{
    int preferredScreen = 0;
    xcb_connection_t *connection = xcb_connect(NULL, &preferredScreen);
    if (!connection || xcb_connection_has_error(connection))
        fail("cannot connect");

    const xcb_setup_t *setup = xcb_get_setup(connection);
    if (!setup)
        fail("connection has no setup");
    xcb_screen_t *screen = firstScreen(setup);
    if (!screen)
        fail("setup has no screen");

    printf("setup status=%u byte_order=%u screens=%u roots=%d\n", setup->status,
           setup->image_byte_order, setup->roots_len,
           xcb_setup_roots_length(setup));
    printf("screen root_depth=%u width=%u height=%u\n", screen->root_depth,
           screen->width_in_pixels, screen->height_in_pixels);

    const char atomName[] = "LIBX11_COMPAT_XCB_PROBE";
    xcb_intern_atom_cookie_t atomCookie =
        xcb_intern_atom(connection, 0, sizeof(atomName) - 1, atomName);
    xcb_intern_atom_reply_t *atomReply =
        xcb_intern_atom_reply(connection, atomCookie, NULL);
    if (!atomReply || atomReply->atom == XCB_ATOM_NONE)
        fail("cannot intern atom");
    xcb_atom_t atom = atomReply->atom;
    free(atomReply);

    xcb_get_atom_name_cookie_t nameCookie = xcb_get_atom_name(connection, atom);
    xcb_get_atom_name_reply_t *nameReply =
        xcb_get_atom_name_reply(connection, nameCookie, NULL);
    if (!nameReply)
        fail("cannot get atom name");
    printf("atom name=%.*s length=%d\n",
           xcb_get_atom_name_name_length(nameReply),
           xcb_get_atom_name_name(nameReply),
           xcb_get_atom_name_name_length(nameReply));
    free(nameReply);

    xcb_window_t window = xcb_generate_id(connection);
    uint32_t values[] = {
        screen->white_pixel,
        XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE};
    xcb_void_cookie_t createCookie = xcb_create_window_checked(
        connection, XCB_COPY_FROM_PARENT, window, screen->root, 10, 20, 160, 90,
        0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    xcb_generic_error_t *error = xcb_request_check(connection, createCookie);
    if (error)
        fail("create window failed");

    const char payload[] = "xcb-probe";
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, window, atom,
                        XCB_ATOM_STRING, 8, sizeof(payload) - 1, payload);

    xcb_get_window_attributes_reply_t *attributes =
        xcb_get_window_attributes_reply(
            connection, xcb_get_window_attributes(connection, window), NULL);
    if (!attributes)
        fail("cannot get attributes");
    printf("attributes class=%u gravity=%u map_state=%u override=%u\n",
           attributes->_class, attributes->win_gravity, attributes->map_state,
           attributes->override_redirect);
    free(attributes);

    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(
        connection, xcb_get_geometry(connection, window), NULL);
    if (!geometry)
        fail("cannot get geometry");
    printf("geometry x=%d y=%d width=%u height=%u border=%u depth=%u\n",
           geometry->x, geometry->y, geometry->width, geometry->height,
           geometry->border_width, geometry->depth);
    free(geometry);

    xcb_query_tree_reply_t *tree = xcb_query_tree_reply(
        connection, xcb_query_tree(connection, window), NULL);
    if (!tree)
        fail("cannot query tree");
    printf("tree parent_is_root=%u children=%d\n", tree->parent == screen->root,
           xcb_query_tree_children_length(tree));
    free(tree);

    xcb_get_property_reply_t *property = xcb_get_property_reply(
        connection,
        xcb_get_property(connection, 0, window, atom, XCB_GET_PROPERTY_TYPE_ANY,
                         0, 64),
        NULL);
    if (!property)
        fail("cannot get property");
    printf("property format=%u length=%d after=%u value=%.*s\n",
           property->format, xcb_get_property_value_length(property),
           property->bytes_after, xcb_get_property_value_length(property),
           (const char *) xcb_get_property_value(property));
    free(property);

    xcb_map_window(connection, window);
    xcb_flush(connection);
    for (int index = 0; index < 2; index++) {
        xcb_generic_event_t *event = xcb_wait_for_event(connection);
        if (!event)
            fail("property/map operation produced no event");
        printf("event index=%d type=%u synthetic=%u\n", index,
               event->response_type & 0x7f, !!(event->response_type & 0x80));
        free(event);
    }

    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
    return 0;
}
