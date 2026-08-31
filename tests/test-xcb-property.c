#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#define CHECK(c, m)                     \
    do {                                \
        if (!(c)) {                     \
            fprintf(stderr, "%s\n", m); \
            return 1;                   \
        }                               \
    } while (0)
int main(void)
{
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    CHECK(c && !xcb_connection_has_error(c), "connect");
    CHECK(
        !xcb_get_atom_name_name(NULL) && !xcb_get_atom_name_name_length(NULL) &&
            !xcb_get_atom_name_name_end(NULL).data &&
            !xcb_get_atom_name_sizeof(NULL) && !xcb_get_property_value(NULL) &&
            !xcb_get_property_value_length(NULL) &&
            !xcb_get_property_value_end(NULL).data &&
            !xcb_get_property_sizeof(NULL),
        "NULL reply accessor");
    xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t w = xcb_generate_id(c);
    CHECK(!xcb_request_check(
              c, xcb_create_window_checked(c, 24, w, s->root, 0, 0, 20, 20, 0,
                                           XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                           s->root_visual, 0, NULL)),
          "create");
    const char name[] = "XCB_TEST_PROPERTY";
    xcb_intern_atom_reply_t *ia = xcb_intern_atom_reply(
        c, xcb_intern_atom_unchecked(c, 0, sizeof(name) - 1, name), NULL);
    CHECK(ia, "intern");
    xcb_atom_t atom = ia->atom;
    free(ia);
    xcb_get_atom_name_reply_t *an =
        xcb_get_atom_name_reply(c, xcb_get_atom_name_unchecked(c, atom), NULL);
    CHECK(an && xcb_get_atom_name_name_length(an) == (int) sizeof(name) - 1 &&
              !memcmp(xcb_get_atom_name_name(an), name, sizeof(name) - 1),
          "atom name");
    xcb_generic_iterator_t atomNameEnd = xcb_get_atom_name_name_end(an);
    CHECK(
        xcb_get_atom_name_sizeof(an) ==
                (int) (sizeof(*an) + sizeof(name) - 1) &&
            atomNameEnd.data == xcb_get_atom_name_name(an) + sizeof(name) - 1 &&
            atomNameEnd.index == xcb_get_atom_name_sizeof(an),
        "atom name layout");
    free(an);
    xcb_generic_error_t *error = NULL;
    an = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, UINT32_MAX), &error);
    CHECK(!an && error && error->error_code == XCB_ATOM &&
              error->resource_id == UINT32_MAX &&
              error->major_code == XCB_GET_ATOM_NAME,
          "atom name BadAtom");
    free(error);
    xcb_get_atom_name_cookie_t uncheckedAtomName =
        xcb_get_atom_name_unchecked(c, UINT32_MAX);
    error = (xcb_generic_error_t *) UINTPTR_MAX;
    an = xcb_get_atom_name_reply(c, uncheckedAtomName, &error);
    CHECK(!an && !error, "unchecked atom name bypasses reply error");
    error = (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(error && error->error_code == XCB_ATOM &&
              error->full_sequence == uncheckedAtomName.sequence,
          "unchecked atom name queues error");
    free(error);
    const char first[] = "abcd", second[] = "ef";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom, XCB_ATOM_STRING, 8,
                        4, first);
    xcb_change_property(c, XCB_PROP_MODE_APPEND, w, atom, XCB_ATOM_STRING, 8, 2,
                        second);
    xcb_get_property_reply_t *p = xcb_get_property_reply(
        c,
        xcb_get_property_unchecked(c, 0, w, atom, XCB_GET_PROPERTY_TYPE_ANY, 0,
                                   1),
        NULL);
    CHECK(p && xcb_get_property_value_length(p) == 4 && p->bytes_after == 2 &&
              !memcmp(xcb_get_property_value(p), "abcd", 4),
          "truncated property");
    xcb_generic_iterator_t propertyEnd = xcb_get_property_value_end(p);
    CHECK(xcb_get_property_sizeof(p) == (int) (sizeof(*p) + 4) &&
              propertyEnd.data == (char *) xcb_get_property_value(p) + 4 &&
              propertyEnd.index == xcb_get_property_sizeof(p),
          "property layout");
    free(p);

    /* Six bytes of an 8-bit property pad to eight on the wire. length counts
     * those words, so a client that reads length * 4 bytes must stay inside the
     * reply block.
     */
    p = xcb_get_property_reply(
        c,
        xcb_get_property_unchecked(c, 0, w, atom, XCB_GET_PROPERTY_TYPE_ANY, 0,
                                   16),
        NULL);
    CHECK(p && xcb_get_property_value_length(p) == 6 && p->length == 2 &&
              xcb_get_property_sizeof(p) == (int) (sizeof(*p) + 8) &&
              !memcmp(xcb_get_property_value(p), "abcdef", 6),
          "unaligned property payload is padded to its advertised length");
    free(p);
    xcb_get_property_cookie_t uncheckedProperty = xcb_get_property_unchecked(
        c, 0, w, UINT32_MAX, XCB_GET_PROPERTY_TYPE_ANY, 0, 1);
    error = (xcb_generic_error_t *) UINTPTR_MAX;
    p = xcb_get_property_reply(c, uncheckedProperty, &error);
    CHECK(!p && !error, "unchecked property bypasses reply error");
    error = (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(error && error->error_code == XCB_ATOM &&
              error->full_sequence == uncheckedProperty.sequence,
          "unchecked property queues error");
    free(error);
    const char prefix[] = "01";
    CHECK(!xcb_request_check(
              c, xcb_change_property_checked(c, XCB_PROP_MODE_PREPEND, w, atom,
                                             XCB_ATOM_STRING, 8, 2, prefix)),
          "prepend");
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 8),
        NULL);
    CHECK(p && xcb_get_property_value_length(p) == 8 &&
              !memcmp(xcb_get_property_value(p), "01abcdef", 8),
          "prepend value");
    free(p);
    error = xcb_request_check(
        c, xcb_change_property_checked(c, XCB_PROP_MODE_APPEND, w, atom,
                                       XCB_ATOM_INTEGER, 16, 1, prefix));
    CHECK(error && error->error_code == XCB_MATCH, "append type mismatch");
    free(error);
    error =
        xcb_request_check(c, xcb_change_property_checked(
                                 c, XCB_PROP_MODE_REPLACE, w, atom,
                                 XCB_ATOM_STRING, 8, UINT32_C(262144), prefix));
    CHECK(error && error->error_code == XCB_LENGTH &&
              error->major_code == XCB_CHANGE_PROPERTY,
          "property oversized element count");
    free(error);
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom, XCB_ATOM_INTEGER, 0, 8), NULL);
    CHECK(p && p->type == XCB_ATOM_STRING && p->format == 8 &&
              p->value_len == 0 && p->bytes_after == 8,
          "get type mismatch");
    free(p);
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom, XCB_ATOM_STRING, 2, 8), &error);
    CHECK(p && !error && p->value_len == 0 && p->bytes_after == 0,
          "offset at end");
    free(p);
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom, XCB_ATOM_STRING, 3, 8), &error);
    CHECK(!p && error && error->error_code == XCB_VALUE, "offset beyond end");
    free(error);
    error = NULL;
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom, XCB_ATOM_STRING, 1, 8), &error);
    CHECK(p && !error && xcb_get_property_value_length(p) == 4 &&
              !memcmp(xcb_get_property_value(p), "cdef", 4),
          "offset at final unit");
    free(p);
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 1, w, atom, XCB_ATOM_STRING, 0, 8), NULL);
    CHECK(p && p->bytes_after == 0 && p->value_len == 8,
          "delete-on-read reply");
    free(p);
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 8),
        NULL);
    CHECK(p && p->type == XCB_ATOM_NONE, "delete-on-read removal");
    free(p);
    uint16_t shorts[] = {0x1234, 0xabcd};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom, XCB_ATOM_INTEGER, 16,
                        2, shorts);
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom, XCB_ATOM_INTEGER, 0, 8), NULL);
    CHECK(p && p->format == 16 && p->value_len == 2 &&
              !memcmp(xcb_get_property_value(p), shorts, sizeof(shorts)),
          "16-bit property");
    free(p);
    uint32_t words[] = {0x12345678, 0x90abcdef};
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom, XCB_ATOM_CARDINAL,
                        32, 2, words);
    xcb_list_properties_reply_t *list =
        xcb_list_properties_reply(c, xcb_list_properties(c, w), NULL);
    CHECK(list && xcb_list_properties_atoms_length(list) == 1 &&
              xcb_list_properties_atoms(list)[0] == atom &&
              xcb_list_properties_sizeof(list) ==
                  (int) (sizeof(*list) + sizeof(xcb_atom_t)) &&
              xcb_list_properties_atoms_end(list).index ==
                  (int) (sizeof(*list) + sizeof(xcb_atom_t)),
          "list properties");
    free(list);
    xcb_get_selection_owner_reply_t *owner = xcb_get_selection_owner_reply(
        c, xcb_get_selection_owner(c, atom), NULL);
    CHECK(owner && owner->owner == XCB_NONE, "initial selection owner");
    free(owner);
    CHECK(!xcb_request_check(
              c, xcb_set_selection_owner_checked(c, w, atom, XCB_CURRENT_TIME)),
          "set selection owner");
    owner = xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, atom),
                                          NULL);
    CHECK(owner && owner->owner == w, "get selection owner");
    free(owner);
    CHECK(!xcb_request_check(
              c, xcb_convert_selection_checked(c, w, atom, XCB_ATOM_STRING,
                                               atom, XCB_CURRENT_TIME)),
          "convert selection");
    xcb_selection_request_event_t *selectionRequest =
        (xcb_selection_request_event_t *) xcb_poll_for_queued_event(c);
    CHECK(selectionRequest &&
              selectionRequest->response_type == XCB_SELECTION_REQUEST &&
              selectionRequest->owner == w &&
              selectionRequest->requestor == w &&
              selectionRequest->selection == atom &&
              selectionRequest->target == XCB_ATOM_STRING &&
              selectionRequest->property == atom,
          "selection request event");
    free(selectionRequest);
    CHECK(!xcb_request_check(c, xcb_set_selection_owner_checked(
                                    c, XCB_NONE, atom, XCB_CURRENT_TIME)),
          "clear selection owner");
    xcb_selection_clear_event_t *selectionClear =
        (xcb_selection_clear_event_t *) xcb_poll_for_queued_event(c);
    CHECK(selectionClear &&
              selectionClear->response_type == XCB_SELECTION_CLEAR &&
              selectionClear->owner == w && selectionClear->selection == atom,
          "selection clear event");
    free(selectionClear);
    CHECK(!xcb_request_check(
              c, xcb_convert_selection_checked(c, w, atom, XCB_ATOM_STRING,
                                               atom, XCB_CURRENT_TIME)),
          "convert unowned selection");
    xcb_selection_notify_event_t *selectionNotify =
        (xcb_selection_notify_event_t *) xcb_poll_for_queued_event(c);
    CHECK(selectionNotify &&
              selectionNotify->response_type == XCB_SELECTION_NOTIFY &&
              selectionNotify->requestor == w &&
              selectionNotify->selection == atom &&
              selectionNotify->target == XCB_ATOM_STRING &&
              selectionNotify->property == XCB_NONE,
          "selection notify event");
    free(selectionNotify);
    error = xcb_request_check(c, xcb_set_selection_owner_checked(
                                     c, 0xdeadbeef, atom, XCB_CURRENT_TIME));
    CHECK(error && error->error_code == XCB_WINDOW,
          "selection owner BadWindow");
    free(error);
    error = xcb_request_check(
        c, xcb_set_selection_owner_checked(c, w, 0xdeadbeef, XCB_CURRENT_TIME));
    CHECK(error && error->error_code == XCB_ATOM &&
              error->resource_id == UINT32_C(0xdeadbeef),
          "selection owner BadAtom");
    free(error);
    error = xcb_request_check(
        c, xcb_delete_property_checked(c, w, UINT32_C(0xdeadbeef)));
    CHECK(error && error->error_code == XCB_ATOM &&
              error->resource_id == UINT32_C(0xdeadbeef) &&
              error->major_code == XCB_DELETE_PROPERTY,
          "delete property BadAtom metadata");
    free(error);
    error = xcb_request_check(
        c, xcb_change_property_checked(c, XCB_PROP_MODE_REPLACE, w,
                                       UINT32_C(0xdeadbeef), XCB_ATOM_STRING, 8,
                                       0, NULL));
    CHECK(error && error->error_code == XCB_ATOM &&
              error->resource_id == UINT32_C(0xdeadbeef) &&
              error->major_code == XCB_CHANGE_PROPERTY,
          "change property BadAtom metadata");
    free(error);
    error =
        xcb_request_check(c, xcb_change_property_checked(
                                 c, 99, w, atom, XCB_ATOM_STRING, 8, 0, NULL));
    CHECK(error && error->error_code == XCB_VALUE && error->resource_id == 99 &&
              error->major_code == XCB_CHANGE_PROPERTY,
          "change property mode metadata");
    free(error);
    error = xcb_request_check(
        c, xcb_change_property_checked(c, XCB_PROP_MODE_REPLACE, w, atom,
                                       XCB_ATOM_STRING, 7, 0, NULL));
    CHECK(error && error->error_code == XCB_VALUE && error->resource_id == 7 &&
              error->major_code == XCB_CHANGE_PROPERTY,
          "change property format metadata");
    free(error);
    error = NULL;
    owner = xcb_get_selection_owner_reply(
        c, xcb_get_selection_owner(c, 0xdeadbeef), &error);
    CHECK(!owner && error && error->error_code == XCB_ATOM,
          "get selection owner BadAtom");
    free(error);
    error = NULL;
    list = xcb_list_properties_reply(c, xcb_list_properties(c, 0xdeadbeef),
                                     &error);
    CHECK(!list && error && error->error_code == XCB_WINDOW,
          "list properties BadWindow");
    free(error);
    xcb_list_properties_cookie_t uncheckedList =
        xcb_list_properties_unchecked(c, UINT32_C(0xdeadbeef));
    error = (xcb_generic_error_t *) UINTPTR_MAX;
    list = xcb_list_properties_reply(c, uncheckedList, &error);
    CHECK(!list && !error, "unchecked list error bypasses reply API");
    xcb_generic_error_t *queuedError =
        (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(queuedError && queuedError->response_type == 0 &&
              queuedError->error_code == XCB_WINDOW &&
              queuedError->resource_id == UINT32_C(0xdeadbeef) &&
              queuedError->full_sequence == uncheckedList.sequence,
          "unchecked list error is queued");
    free(queuedError);
    xcb_get_selection_owner_cookie_t uncheckedOwner =
        xcb_get_selection_owner_unchecked(c, UINT32_C(0xdeadbeef));
    error = (xcb_generic_error_t *) UINTPTR_MAX;
    owner = xcb_get_selection_owner_reply(c, uncheckedOwner, &error);
    CHECK(!owner && !error, "unchecked selection error bypasses reply API");
    queuedError = (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(queuedError && queuedError->error_code == XCB_ATOM &&
              queuedError->full_sequence == uncheckedOwner.sequence,
          "unchecked selection error is queued");
    free(queuedError);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, atom, XCB_ATOM_CARDINAL,
                        32, 2, words);
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom, XCB_ATOM_CARDINAL, 0, 8), NULL);
    CHECK(p && p->format == 32 && p->value_len == 2 &&
              !memcmp(xcb_get_property_value(p), words, sizeof(words)),
          "32-bit property");
    free(p);
    xcb_get_property_reply_t oversizedProperty = {
        .format = 32,
        .value_len = UINT32_MAX,
    };
    CHECK(!xcb_get_property_sizeof(&oversizedProperty) &&
              !xcb_get_property_value_length(&oversizedProperty) &&
              !xcb_get_property_value_end(&oversizedProperty).data,
          "oversized property layout");
    xcb_delete_property(c, w, atom);
    p = xcb_get_property_reply(
        c, xcb_get_property(c, 0, w, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 8),
        NULL);
    CHECK(p && p->type == XCB_ATOM_NONE, "delete");
    free(p);
    xcb_destroy_window(c, w);
    xcb_disconnect(c);
    puts("test-xcb-property: ok");
    return 0;
}
