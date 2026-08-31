#include <stdio.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
extern Window libx11CompatCreateWindowWithId(Display *,
                                             Window,
                                             Window,
                                             int,
                                             int,
                                             unsigned int,
                                             unsigned int,
                                             unsigned int,
                                             int,
                                             unsigned int,
                                             Visual *,
                                             unsigned long,
                                             XSetWindowAttributes *);
extern Bool isXidAllocated(XID);
extern Bool reserveXidResource(XID);
extern void xcbCompatFailNextReplyAllocationForTest(void);
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
    xcb_screen_t *s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    CHECK(!xcb_query_tree_children(NULL) &&
              !xcb_query_tree_children_length(NULL) &&
              !xcb_query_tree_children_end(NULL).data &&
              !xcb_query_tree_sizeof(NULL),
          "NULL query-tree accessor");
    uint32_t initialInvalidMaskValue = 0;
    xcb_void_cookie_t initialUnchecked = xcb_change_window_attributes(
        c, s->root, UINT32_C(1) << 31, &initialInvalidMaskValue);
    CHECK(!xcb_request_check(
              c, xcb_change_window_attributes_checked(c, s->root, 0, NULL)),
          "initialize pending table");
    xcb_generic_error_t *initialQueuedError =
        (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(initialQueuedError && initialQueuedError->error_code == XCB_VALUE &&
              initialQueuedError->full_sequence == initialUnchecked.sequence,
          "pending table growth preserves queued errors");
    free(initialQueuedError);
    xcb_window_t w = xcb_generate_id(c);
    uint32_t values[] = {0x112233, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_void_cookie_t bad;
    xcb_generic_error_t *e;
    xcb_void_cookie_t created = xcb_create_window_checked(
        c, XCB_COPY_FROM_PARENT, w, s->root, 7, 9, 80, 60, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    CHECK(!xcb_request_check(c, created), "checked create");
    xcb_get_geometry_reply_t *g =
        xcb_get_geometry_reply(c, xcb_get_geometry_unchecked(c, w), NULL);
    CHECK(g && g->x == 7 && g->y == 9 && g->width == 80 && g->height == 60,
          "geometry");
    free(g);
    xcb_get_window_attributes_reply_t *a = xcb_get_window_attributes_reply(
        c, xcb_get_window_attributes_unchecked(c, w), NULL);
    CHECK(a && a->your_event_mask == XCB_EVENT_MASK_STRUCTURE_NOTIFY,
          "attributes");
    free(a);
    xcb_query_tree_reply_t *t =
        xcb_query_tree_reply(c, xcb_query_tree_unchecked(c, w), NULL);
    CHECK(t && t->parent == s->root && xcb_query_tree_children_length(t) == 0,
          "tree");
    xcb_generic_iterator_t childrenEnd = xcb_query_tree_children_end(t);
    CHECK(xcb_query_tree_sizeof(t) == (int) sizeof(*t) &&
              childrenEnd.data == xcb_query_tree_children(t) &&
              childrenEnd.index == xcb_query_tree_sizeof(t),
          "tree layout");
    free(t);
    xcb_window_t uncheckedWindow = xcb_generate_id(c);
    xcb_void_cookie_t uncheckedCreate = xcb_create_window(
        c, XCB_COPY_FROM_PARENT, uncheckedWindow, s->root, 2, 3, 11, 12, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, NULL);
    g = xcb_get_geometry_reply(c, xcb_get_geometry(c, uncheckedWindow), NULL);
    CHECK(uncheckedCreate.sequence && g && g->x == 2 && g->y == 3 &&
              g->width == 11 && g->height == 12,
          "unchecked create window");
    free(g);
    xcb_destroy_window(c, uncheckedWindow);
    xcb_generic_error_t *queryError = NULL;
    a = xcb_get_window_attributes_reply(
        c, xcb_get_window_attributes(c, UINT32_C(0xdeadbeef)), &queryError);
    CHECK(!a && queryError && queryError->error_code == XCB_WINDOW &&
              queryError->resource_id == UINT32_C(0xdeadbeef) &&
              queryError->major_code == XCB_GET_WINDOW_ATTRIBUTES,
          "attributes reply BadWindow");
    free(queryError);
    xcb_get_window_attributes_cookie_t uncheckedAttributes =
        xcb_get_window_attributes_unchecked(c, UINT32_C(0xdeadbeef));
    queryError = (xcb_generic_error_t *) UINTPTR_MAX;
    a = xcb_get_window_attributes_reply(c, uncheckedAttributes, &queryError);
    CHECK(!a && !queryError, "unchecked attributes bypass reply error");
    queryError = (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(queryError && queryError->error_code == XCB_WINDOW &&
              queryError->full_sequence == uncheckedAttributes.sequence,
          "unchecked attributes queue error");
    free(queryError);
    xcb_get_geometry_cookie_t uncheckedGeometry =
        xcb_get_geometry_unchecked(c, UINT32_C(0xdeadbeef));
    queryError = (xcb_generic_error_t *) UINTPTR_MAX;
    g = xcb_get_geometry_reply(c, uncheckedGeometry, &queryError);
    CHECK(!g && !queryError, "unchecked geometry bypasses reply error");
    xcb_generic_error_t *uncheckedGeometryError =
        (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(
        uncheckedGeometryError &&
            uncheckedGeometryError->error_code == XCB_DRAWABLE &&
            uncheckedGeometryError->full_sequence == uncheckedGeometry.sequence,
        "unchecked geometry queues error");
    free(uncheckedGeometryError);
    queryError = NULL;
    g = xcb_get_geometry_reply(c, xcb_get_geometry(c, UINT32_C(0xdeadbeef)),
                               &queryError);
    CHECK(!g && queryError && queryError->error_code == XCB_DRAWABLE &&
              queryError->resource_id == UINT32_C(0xdeadbeef) &&
              queryError->major_code == XCB_GET_GEOMETRY,
          "geometry reply BadDrawable");
    free(queryError);
    queryError = NULL;
    t = xcb_query_tree_reply(c, xcb_query_tree(c, UINT32_C(0xdeadbeef)),
                             &queryError);
    CHECK(!t && queryError && queryError->error_code == XCB_WINDOW &&
              queryError->resource_id == UINT32_C(0xdeadbeef) &&
              queryError->major_code == XCB_QUERY_TREE,
          "query-tree reply BadWindow");
    free(queryError);
    xcb_query_tree_cookie_t uncheckedTree =
        xcb_query_tree_unchecked(c, UINT32_C(0xdeadbeef));
    queryError = (xcb_generic_error_t *) UINTPTR_MAX;
    t = xcb_query_tree_reply(c, uncheckedTree, &queryError);
    CHECK(!t && !queryError, "unchecked query tree bypasses reply error");
    queryError = (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(queryError && queryError->error_code == XCB_WINDOW &&
              queryError->full_sequence == uncheckedTree.sequence,
          "unchecked query tree queues error");
    free(queryError);

    xcb_window_t retryWindow = xcb_generate_id(c);
    bad = xcb_create_window_checked(c, 0, retryWindow, s->root, 0, 0, 4, 4, 1,
                                    XCB_WINDOW_CLASS_INPUT_ONLY,
                                    XCB_COPY_FROM_PARENT, 0, NULL);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_MATCH, "InputOnly border BadMatch");
    free(e);
    CHECK(!xcb_request_check(
              c, xcb_create_window_checked(c, 0, retryWindow, s->root, 0, 0, 4,
                                           4, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                                           XCB_COPY_FROM_PARENT, 0, NULL)),
          "retry caller ID after failed create");
    uint32_t inputOnlyBackground = 0;
    e = xcb_request_check(
        c, xcb_change_window_attributes_checked(
               c, retryWindow, XCB_CW_BACK_PIXEL, &inputOnlyBackground));
    CHECK(e && e->error_code == XCB_MATCH &&
              e->resource_id == XCB_CW_BACK_PIXEL &&
              e->major_code == XCB_CHANGE_WINDOW_ATTRIBUTES,
          "InputOnly background BadMatch");
    free(e);
    xcb_destroy_window(c, retryWindow);

    xcb_window_t validationWindow = xcb_generate_id(c);
    bad = xcb_create_window_checked(
        c, XCB_COPY_FROM_PARENT, validationWindow, s->root, 0, 0, 0, 4, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, NULL);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_VALUE && e->resource_id == 0,
          "create-window zero width BadValue");
    free(e);
    uint8_t invalidDepth = s->root_depth == 1 ? 16 : 1;
    bad = xcb_create_window_checked(
        c, invalidDepth, validationWindow, s->root, 0, 0, 4, 4, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, NULL);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_MATCH && e->resource_id == invalidDepth,
          "create-window depth BadMatch");
    free(e);
    bad = xcb_create_window_checked(
        c, XCB_COPY_FROM_PARENT, validationWindow, s->root, 0, 0, 4, 4, 0,
        XCB_WINDOW_CLASS_INPUT_OUTPUT, UINT32_C(0xdeadbeef), 0, NULL);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_MATCH &&
              e->resource_id == UINT32_C(0xdeadbeef),
          "create-window visual BadMatch");
    free(e);
    bad = xcb_create_window_checked(c, s->root_depth, validationWindow, s->root,
                                    0, 0, 4, 4, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                                    XCB_COPY_FROM_PARENT, 0, NULL);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_MATCH && e->resource_id == s->root_depth,
          "InputOnly depth BadMatch");
    free(e);
    CHECK(!xcb_request_check(
              c, xcb_create_window_checked(c, XCB_COPY_FROM_PARENT,
                                           validationWindow, s->root, 0, 0, 4,
                                           4, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                           XCB_COPY_FROM_PARENT, 0, NULL)),
          "reuse window ID after structural errors");
    xcb_destroy_window(c, validationWindow);
    bad = xcb_create_window_checked(c, XCB_COPY_FROM_PARENT, xcb_generate_id(c),
                                    UINT32_C(0xdeadbeef), 0, 0, 4, 4, 0,
                                    XCB_WINDOW_CLASS_COPY_FROM_PARENT,
                                    XCB_COPY_FROM_PARENT, 0, NULL);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_WINDOW &&
              e->resource_id == UINT32_C(0xdeadbeef),
          "inherited class invalid parent BadWindow");
    free(e);

    uint32_t invalidMaskValue = 0;
    xcb_window_t invalidMaskWindow = xcb_generate_id(c);
    e = xcb_request_check(
        c, xcb_create_window_checked(c, 0, invalidMaskWindow, s->root, 0, 0, 4,
                                     4, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                     XCB_COPY_FROM_PARENT, UINT32_C(1) << 31,
                                     &invalidMaskValue));
    CHECK(e && e->error_code == XCB_VALUE && e->major_code == XCB_CREATE_WINDOW,
          "create-window unknown mask");
    free(e);
    CHECK(!xcb_request_check(c, xcb_create_window_checked(
                                    c, 0, invalidMaskWindow, s->root, 0, 0, 4,
                                    4, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                    XCB_COPY_FROM_PARENT, 0, NULL)),
          "reuse window ID after unknown mask");
    e = xcb_request_check(
        c, xcb_change_window_attributes_checked(
               c, invalidMaskWindow, UINT32_C(1) << 31, &invalidMaskValue));
    CHECK(e && e->error_code == XCB_VALUE &&
              e->resource_id == (UINT32_C(1) << 31) &&
              e->major_code == XCB_CHANGE_WINDOW_ATTRIBUTES,
          "change-attributes unknown mask");
    free(e);
    uint32_t invalidGravity = 99;
    e = xcb_request_check(
        c, xcb_change_window_attributes_checked(
               c, invalidMaskWindow, XCB_CW_BIT_GRAVITY, &invalidGravity));
    CHECK(e && e->error_code == XCB_VALUE && e->resource_id == invalidGravity &&
              e->major_code == XCB_CHANGE_WINDOW_ATTRIBUTES,
          "change-attributes gravity BadValue");
    free(e);
    uint32_t invalidCursor = s->root;
    e = xcb_request_check(
        c, xcb_change_window_attributes_checked(c, invalidMaskWindow,
                                                XCB_CW_CURSOR, &invalidCursor));
    CHECK(e && e->error_code == XCB_CURSOR && e->resource_id == s->root &&
              e->major_code == XCB_CHANGE_WINDOW_ATTRIBUTES,
          "change-attributes BadCursor");
    free(e);
    e = xcb_request_check(
        c, xcb_configure_window_checked(c, invalidMaskWindow, UINT16_C(1) << 15,
                                        &invalidMaskValue));
    CHECK(e && e->error_code == XCB_VALUE &&
              e->resource_id == (UINT16_C(1) << 15) &&
              e->major_code == XCB_CONFIGURE_WINDOW,
          "configure-window unknown mask");
    free(e);
    xcb_void_cookie_t unchecked = xcb_change_window_attributes(
        c, invalidMaskWindow, UINT32_C(1) << 31, &invalidMaskValue);
    CHECK(!xcb_request_check(c, unchecked),
          "unchecked request is not request-checkable");
    xcb_generic_error_t *queuedError =
        (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(queuedError && queuedError->response_type == 0 &&
              queuedError->error_code == XCB_VALUE &&
              queuedError->resource_id == (UINT32_C(1) << 31) &&
              queuedError->major_code == XCB_CHANGE_WINDOW_ATTRIBUTES &&
              queuedError->full_sequence == unchecked.sequence,
          "unchecked error is queued as an event");
    free(queuedError);
    uint32_t invalidSibling[] = {UINT32_C(0xdeadbeef)};
    bad = xcb_configure_window_checked(c, w, XCB_CONFIG_WINDOW_SIBLING,
                                       invalidSibling);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_WINDOW &&
              e->resource_id == UINT32_C(0xdeadbeef) &&
              e->major_code == XCB_CONFIGURE_WINDOW,
          "configure sibling BadWindow metadata");
    free(e);
    xcb_destroy_window(c, invalidMaskWindow);

    Display *helperDisplay = XOpenDisplay(NULL);
    CHECK(helperDisplay, "helper display");
    xcb_window_t helperWindow = xcb_generate_id(c);
    CHECK(libx11CompatCreateWindowWithId(
              helperDisplay, helperWindow, DefaultRootWindow(helperDisplay), 0,
              0, 4, 4, 1, 0, InputOnly, CopyFromParent, 0, NULL) == None,
          "helper InputOnly border failure");
    CHECK(!isXidAllocated(helperWindow), "failed helper retained requested ID");
    CHECK(reserveXidResource(helperWindow), "reserve released helper ID");
    CHECK(libx11CompatCreateWindowWithId(helperDisplay, helperWindow,
                                         DefaultRootWindow(helperDisplay), 0, 0,
                                         4, 4, 0, 0, InputOnly, CopyFromParent,
                                         0, NULL) == helperWindow,
          "reuse released helper ID");
    XDestroyWindow(helperDisplay, helperWindow);
    XCloseDisplay(helperDisplay);
    bad = xcb_create_window_checked(c, 24, xcb_generate_id(c), 0xdeadbeef, 0, 0,
                                    1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                    s->root_visual, 0, NULL);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_WINDOW, "BadWindow");
    free(e);
    bad = xcb_create_window_checked(c, 24, 0x0f000000, s->root, 0, 0, 1, 1, 0,
                                    XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                    s->root_visual, 0, NULL);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_ID_CHOICE, "BadIDChoice");
    free(e);
    uint32_t changed[] = {XCB_GRAVITY_NORTH_EAST, XCB_GRAVITY_SOUTH_WEST, 1,
                          XCB_EVENT_MASK_PROPERTY_CHANGE};
    uint32_t changeMask = XCB_CW_BIT_GRAVITY | XCB_CW_WIN_GRAVITY |
                          XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    CHECK(!xcb_request_check(c, xcb_change_window_attributes_checked(
                                    c, w, changeMask, changed)),
          "change attributes");
    a = xcb_get_window_attributes_reply(c, xcb_get_window_attributes(c, w),
                                        NULL);
    CHECK(a && a->bit_gravity == XCB_GRAVITY_NORTH_EAST &&
              a->win_gravity == XCB_GRAVITY_SOUTH_WEST &&
              a->override_redirect &&
              a->your_event_mask == XCB_EVENT_MASK_PROPERTY_CHANGE,
          "changed attributes mismatch");
    free(a);

    uint32_t configured[] = {(uint32_t) (int32_t) -3, 11, 64, 48, 2};
    uint16_t configureMask =
        XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH |
        XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH;
    CHECK(!xcb_request_check(
              c, xcb_configure_window_checked(c, w, configureMask, configured)),
          "configure");
    g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), NULL);
    CHECK(g && g->x == -3 && g->y == 11 && g->width == 64 && g->height == 48 &&
              g->border_width == 2,
          "configured geometry mismatch");
    free(g);

    xcb_window_t container = xcb_generate_id(c);
    CHECK(
        !xcb_request_check(
            c, xcb_create_window_checked(
                   c, XCB_COPY_FROM_PARENT, container, s->root, 0, 0, 100, 100,
                   0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, 0, NULL)),
        "container create");
    CHECK(!xcb_request_check(
              c, xcb_reparent_window_checked(c, w, container, 4, 5)),
          "reparent");
    t = xcb_query_tree_reply(c, xcb_query_tree(c, w), NULL);
    CHECK(t && t->parent == container, "reparent parent mismatch");
    free(t);

    xcb_window_t sibling = xcb_generate_id(c);
    CHECK(
        !xcb_request_check(
            c, xcb_create_window_checked(
                   c, XCB_COPY_FROM_PARENT, sibling, container, 0, 0, 10, 10, 0,
                   XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, 0, NULL)),
        "sibling create");
    uint32_t siblingOnly[] = {sibling};
    e = xcb_request_check(c, xcb_configure_window_checked(
                                 c, w, XCB_CONFIG_WINDOW_SIBLING, siblingOnly));
    CHECK(e && e->error_code == XCB_MATCH && e->resource_id == w &&
              e->major_code == XCB_CONFIGURE_WINDOW,
          "sibling without stack-mode BadMatch");
    free(e);
    uint32_t selfStack[] = {w, XCB_STACK_MODE_ABOVE};
    e = xcb_request_check(
        c, xcb_configure_window_checked(
               c, w, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
               selfStack));
    CHECK(e && e->error_code == XCB_MATCH && e->resource_id == w,
          "self sibling BadMatch");
    free(e);
    uint32_t conditionalStack[] = {sibling, XCB_STACK_MODE_TOP_IF};
    CHECK(!xcb_request_check(
              c, xcb_configure_window_checked(
                     c, w,
                     XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
                     conditionalStack)),
          "conditional sibling stack configure");
    uint32_t stack[] = {sibling, XCB_STACK_MODE_ABOVE};
    CHECK(!xcb_request_check(
              c, xcb_configure_window_checked(
                     c, w,
                     XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE,
                     stack)),
          "stack configure");
    t = xcb_query_tree_reply(c, xcb_query_tree(c, container), NULL);
    CHECK(t && xcb_query_tree_children_length(t) == 2,
          "stacking changed child membership");
    CHECK(xcb_query_tree_children(t)[1] == w,
          "Above stacking did not move window to top");
    free(t);
    e = xcb_request_check(c, xcb_reparent_window_checked(c, w, w, 0, 0));
    CHECK(e && e->error_code == XCB_MATCH && e->resource_id == w &&
              e->major_code == XCB_REPARENT_WINDOW,
          "self reparent BadMatch");
    free(e);
    e = xcb_request_check(c,
                          xcb_reparent_window_checked(c, container, w, 0, 0));
    CHECK(e && e->error_code == XCB_MATCH && e->resource_id == container &&
              e->major_code == XCB_REPARENT_WINDOW,
          "cyclic reparent BadMatch");
    free(e);

    bad = xcb_configure_window_checked(c, 0xdeadbeef, XCB_CONFIG_WINDOW_X,
                                       configured);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_WINDOW, "configure BadWindow");
    free(e);
    uint32_t invalidSize[] = {0};
    bad = xcb_configure_window_checked(c, w, XCB_CONFIG_WINDOW_WIDTH,
                                       invalidSize);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_VALUE && e->resource_id == 0,
          "configure BadValue");
    free(e);
    uint32_t invalidStack[] = {255};
    bad = xcb_configure_window_checked(c, w, XCB_CONFIG_WINDOW_STACK_MODE,
                                       invalidStack);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_VALUE && e->resource_id == 255,
          "stack mode BadValue");
    free(e);
    bad = xcb_change_window_attributes_checked(c, 0xdeadbeef, XCB_CW_EVENT_MASK,
                                               changed);
    e = xcb_request_check(c, bad);
    CHECK(e && e->error_code == XCB_WINDOW, "attributes BadWindow");
    free(e);
    xcb_destroy_window(c, sibling);
    xcb_destroy_window(c, w);
    xcb_destroy_window(c, container);
    xcb_disconnect(c);

    c = xcb_connect(NULL, NULL);
    CHECK(c && !xcb_connection_has_error(c), "OOM test connection");
    s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcbCompatFailNextReplyAllocationForTest();
    t = xcb_query_tree_reply(c, xcb_query_tree(c, s->root), NULL);
    CHECK(!t && xcb_connection_has_error(c) == XCB_CONN_CLOSED_MEM_INSUFFICIENT,
          "reply allocation failure did not close connection");
    xcb_disconnect(c);
    puts("test-xcb-window: ok");
    return 0;
}
