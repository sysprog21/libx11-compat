#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <X11/Xlib-xcb.h>
#include "../compat/xcb-compat-private.h"

#define CHECK(c, m)                     \
    do {                                \
        if (!(c)) {                     \
            fprintf(stderr, "%s\n", m); \
            return 1;                   \
        }                               \
    } while (0)

typedef struct {
    xcb_connection_t *connection;
    xcb_generic_event_t *result;
} EventWaiter;

static void *waitForEvent(void *opaque)
{
    EventWaiter *waiter = opaque;
    waiter->result = xcb_wait_for_event(waiter->connection);
    return NULL;
}

/* Wait until the connection reports the expected number of blocked waiters.
 * The started flag alone only proves a thread entered waitForEvent, not that it
 * reached the blocking call, so a sleep here would be a timing guess.
 */
static void waitForWaiters(xcb_connection_t *connection, unsigned int expected)
{
    while (xcbCompatEventWaiters(connection) < expected)
        sched_yield();
}

static xcb_generic_event_t *convert(Display *display,
                                    xcb_connection_t *connection,
                                    XEvent *event)
{
    XPutBackEvent(display, event);
    return xcb_poll_for_queued_event(connection);
}

static xcb_generic_event_t *pollForType(xcb_connection_t *connection,
                                        uint8_t type)
{
    for (int i = 0; i < 16; i++) {
        xcb_generic_event_t *event = xcb_poll_for_queued_event(connection);
        if (!event || (event->response_type & 0x7f) == type)
            return event;
        free(event);
    }
    return NULL;
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    CHECK(display, "open");
    xcb_connection_t *connection = XGetXCBConnection(display);
    Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 1,
                                        2, 30, 40, 0, 0, 0);
    XSetEventQueueOwner(display, XCBOwnsEventQueue);
    XEvent x;
    memset(&x, 0, sizeof(x));
    x.xexpose.type = Expose;
    x.xexpose.display = display;
    x.xexpose.window = window;
    x.xexpose.send_event = True;
    x.xexpose.serial = 0x12345;
    x.xexpose.x = 3;
    x.xexpose.y = 4;
    x.xexpose.width = 5;
    x.xexpose.height = 6;
    XPutBackEvent(display, &x);
    CHECK(XEventsQueued(display, QueuedAlready) == 0,
          "Xlib cannot consume XCB-owned queue");
    xcb_expose_event_t *expose =
        (xcb_expose_event_t *) xcb_poll_for_queued_event(connection);
    CHECK(expose && expose->response_type == (XCB_EXPOSE | 0x80) &&
              expose->sequence == 0x2345 && expose->window == window &&
              expose->x == 3 && expose->height == 6,
          "expose conversion");
    free(expose);

    memset(&x, 0, sizeof(x));
    x.xgraphicsexpose.type = GraphicsExpose;
    x.xgraphicsexpose.drawable = window;
    x.xgraphicsexpose.x = 7;
    x.xgraphicsexpose.y = 8;
    x.xgraphicsexpose.width = 9;
    x.xgraphicsexpose.height = 10;
    x.xgraphicsexpose.count = 2;
    x.xgraphicsexpose.major_code = XCB_COPY_AREA;
    x.xgraphicsexpose.minor_code = 11;
    xcb_graphics_exposure_event_t *graphicsExpose =
        (xcb_graphics_exposure_event_t *) convert(display, connection, &x);
    CHECK(graphicsExpose &&
              graphicsExpose->response_type == XCB_GRAPHICS_EXPOSURE &&
              graphicsExpose->drawable == window && graphicsExpose->x == 7 &&
              graphicsExpose->y == 8 && graphicsExpose->width == 9 &&
              graphicsExpose->height == 10 && graphicsExpose->count == 2 &&
              graphicsExpose->major_opcode == XCB_COPY_AREA &&
              graphicsExpose->minor_opcode == 11,
          "graphics-expose conversion");
    free(graphicsExpose);

    memset(&x, 0, sizeof(x));
    x.xnoexpose.type = NoExpose;
    x.xnoexpose.drawable = window;
    x.xnoexpose.major_code = XCB_COPY_PLANE;
    x.xnoexpose.minor_code = 12;
    xcb_no_exposure_event_t *noExpose =
        (xcb_no_exposure_event_t *) convert(display, connection, &x);
    CHECK(noExpose && noExpose->response_type == XCB_NO_EXPOSURE &&
              noExpose->drawable == window &&
              noExpose->major_opcode == XCB_COPY_PLANE &&
              noExpose->minor_opcode == 12,
          "no-expose conversion");
    free(noExpose);

    memset(&x, 0, sizeof(x));
    x.xconfigure.type = ConfigureNotify;
    x.xconfigure.window = window;
    x.xconfigure.event = window;
    x.xconfigure.width = 31;
    x.xconfigure.height = 41;
    xcb_configure_notify_event_t *configure =
        (xcb_configure_notify_event_t *) convert(display, connection, &x);
    CHECK(configure && configure->window == window && configure->width == 31 &&
              configure->height == 41,
          "configure conversion");
    free(configure);

    Window root = DefaultRootWindow(display);
    memset(&x, 0, sizeof(x));
    x.xcreatewindow.type = CreateNotify;
    x.xcreatewindow.parent = root;
    x.xcreatewindow.window = window;
    x.xcreatewindow.x = -3;
    x.xcreatewindow.y = 4;
    x.xcreatewindow.width = 31;
    x.xcreatewindow.height = 41;
    x.xcreatewindow.border_width = 2;
    x.xcreatewindow.override_redirect = True;
    xcb_create_notify_event_t *create =
        (xcb_create_notify_event_t *) convert(display, connection, &x);
    CHECK(create && create->response_type == XCB_CREATE_NOTIFY &&
              create->parent == root && create->window == window &&
              create->x == -3 && create->height == 41 &&
              create->border_width == 2 && create->override_redirect,
          "create conversion");
    free(create);

    memset(&x, 0, sizeof(x));
    x.xmap.type = MapNotify;
    x.xmap.event = root;
    x.xmap.window = window;
    x.xmap.override_redirect = True;
    xcb_map_notify_event_t *map =
        (xcb_map_notify_event_t *) convert(display, connection, &x);
    CHECK(map && map->event == root && map->window == window &&
              map->override_redirect,
          "map conversion");
    free(map);

    memset(&x, 0, sizeof(x));
    x.xunmap.type = UnmapNotify;
    x.xunmap.event = root;
    x.xunmap.window = window;
    x.xunmap.from_configure = True;
    xcb_unmap_notify_event_t *unmap =
        (xcb_unmap_notify_event_t *) convert(display, connection, &x);
    CHECK(unmap && unmap->event == root && unmap->window == window &&
              unmap->from_configure,
          "unmap conversion");
    free(unmap);

    memset(&x, 0, sizeof(x));
    x.xreparent.type = ReparentNotify;
    x.xreparent.event = root;
    x.xreparent.window = window;
    x.xreparent.parent = root;
    x.xreparent.x = 8;
    x.xreparent.y = 9;
    x.xreparent.override_redirect = True;
    xcb_reparent_notify_event_t *reparent =
        (xcb_reparent_notify_event_t *) convert(display, connection, &x);
    CHECK(reparent && reparent->event == root && reparent->window == window &&
              reparent->parent == root && reparent->x == 8 &&
              reparent->y == 9 && reparent->override_redirect,
          "reparent conversion");
    free(reparent);

    memset(&x, 0, sizeof(x));
    x.xgravity.type = GravityNotify;
    x.xgravity.event = root;
    x.xgravity.window = window;
    x.xgravity.x = -7;
    x.xgravity.y = 12;
    xcb_gravity_notify_event_t *gravity =
        (xcb_gravity_notify_event_t *) convert(display, connection, &x);
    CHECK(gravity && gravity->event == root && gravity->window == window &&
              gravity->x == -7 && gravity->y == 12,
          "gravity conversion");
    free(gravity);

    memset(&x, 0, sizeof(x));
    x.xmaprequest.type = MapRequest;
    x.xmaprequest.parent = root;
    x.xmaprequest.window = window;
    xcb_map_request_event_t *mapRequest =
        (xcb_map_request_event_t *) convert(display, connection, &x);
    CHECK(mapRequest && mapRequest->parent == root &&
              mapRequest->window == window,
          "map-request conversion");
    free(mapRequest);

    memset(&x, 0, sizeof(x));
    x.xconfigurerequest.type = ConfigureRequest;
    x.xconfigurerequest.parent = root;
    x.xconfigurerequest.window = window;
    x.xconfigurerequest.above = root;
    x.xconfigurerequest.x = -11;
    x.xconfigurerequest.y = 12;
    x.xconfigurerequest.width = 45;
    x.xconfigurerequest.height = 46;
    x.xconfigurerequest.border_width = 3;
    x.xconfigurerequest.detail = Below;
    x.xconfigurerequest.value_mask = CWX | CWHeight | CWStackMode;
    xcb_configure_request_event_t *configureRequest =
        (xcb_configure_request_event_t *) convert(display, connection, &x);
    CHECK(configureRequest && configureRequest->parent == root &&
              configureRequest->window == window &&
              configureRequest->sibling == root && configureRequest->x == -11 &&
              configureRequest->height == 46 &&
              configureRequest->stack_mode == XCB_STACK_MODE_BELOW &&
              configureRequest->value_mask ==
                  (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_HEIGHT |
                   XCB_CONFIG_WINDOW_STACK_MODE),
          "configure-request conversion");
    free(configureRequest);

    memset(&x, 0, sizeof(x));
    x.xresizerequest.type = ResizeRequest;
    x.xresizerequest.window = window;
    x.xresizerequest.width = 47;
    x.xresizerequest.height = 48;
    xcb_resize_request_event_t *resize =
        (xcb_resize_request_event_t *) convert(display, connection, &x);
    CHECK(resize && resize->window == window && resize->width == 47 &&
              resize->height == 48,
          "resize-request conversion");
    free(resize);

    memset(&x, 0, sizeof(x));
    x.xcirculate.type = CirculateNotify;
    x.xcirculate.event = root;
    x.xcirculate.window = window;
    x.xcirculate.place = PlaceOnBottom;
    xcb_circulate_notify_event_t *circulate =
        (xcb_circulate_notify_event_t *) convert(display, connection, &x);
    CHECK(circulate && circulate->event == root &&
              circulate->window == window &&
              circulate->place == XCB_PLACE_ON_BOTTOM,
          "circulate conversion");
    free(circulate);

    memset(&x, 0, sizeof(x));
    x.xcirculaterequest.type = CirculateRequest;
    x.xcirculaterequest.parent = root;
    x.xcirculaterequest.window = window;
    x.xcirculaterequest.place = PlaceOnTop;
    xcb_circulate_request_event_t *circulateRequest =
        (xcb_circulate_request_event_t *) convert(display, connection, &x);
    CHECK(circulateRequest && circulateRequest->event == root &&
              circulateRequest->window == window &&
              circulateRequest->place == XCB_PLACE_ON_TOP,
          "circulate-request conversion");
    free(circulateRequest);

    XSelectInput(display, window, StructureNotifyMask);
    CHECK(!xcb_request_check(connection,
                             xcb_map_window_checked(connection, window)),
          "checked map request");
    map = (xcb_map_notify_event_t *) pollForType(connection, XCB_MAP_NOTIFY);
    CHECK(map && map->response_type == XCB_MAP_NOTIFY && map->event == window &&
              map->window == window,
          "map request event");
    free(map);
    CHECK(!xcb_request_check(connection,
                             xcb_unmap_window_checked(connection, window)),
          "checked unmap request");
    unmap =
        (xcb_unmap_notify_event_t *) pollForType(connection, XCB_UNMAP_NOTIFY);
    CHECK(unmap && unmap->response_type == XCB_UNMAP_NOTIFY &&
              unmap->event == window && unmap->window == window &&
              !unmap->from_configure,
          "unmap request event");
    free(unmap);

    memset(&x, 0, sizeof(x));
    x.xproperty.type = PropertyNotify;
    x.xproperty.window = window;
    x.xproperty.atom = XCB_ATOM_WM_NAME;
    x.xproperty.state = PropertyDelete;
    xcb_property_notify_event_t *property =
        (xcb_property_notify_event_t *) convert(display, connection, &x);
    CHECK(property && property->atom == XCB_ATOM_WM_NAME &&
              property->state == XCB_PROPERTY_DELETE,
          "property conversion");
    free(property);

    memset(&x, 0, sizeof(x));
    x.xselectionclear.type = SelectionClear;
    x.xselectionclear.window = window;
    x.xselectionclear.selection = XCB_ATOM_PRIMARY;
    x.xselectionclear.time = 2345;
    xcb_selection_clear_event_t *selectionClear =
        (xcb_selection_clear_event_t *) convert(display, connection, &x);
    CHECK(selectionClear &&
              selectionClear->response_type == XCB_SELECTION_CLEAR &&
              selectionClear->owner == window &&
              selectionClear->selection == XCB_ATOM_PRIMARY &&
              selectionClear->time == 2345,
          "selection-clear conversion");
    free(selectionClear);

    memset(&x, 0, sizeof(x));
    x.xselectionrequest.type = SelectionRequest;
    x.xselectionrequest.owner = window;
    x.xselectionrequest.requestor = root;
    x.xselectionrequest.selection = XCB_ATOM_PRIMARY;
    x.xselectionrequest.target = XCB_ATOM_STRING;
    x.xselectionrequest.property = XCB_ATOM_WM_NAME;
    x.xselectionrequest.time = 3456;
    xcb_selection_request_event_t *selectionRequest =
        (xcb_selection_request_event_t *) convert(display, connection, &x);
    CHECK(selectionRequest &&
              selectionRequest->response_type == XCB_SELECTION_REQUEST &&
              selectionRequest->owner == window &&
              selectionRequest->requestor == root &&
              selectionRequest->selection == XCB_ATOM_PRIMARY &&
              selectionRequest->target == XCB_ATOM_STRING &&
              selectionRequest->property == XCB_ATOM_WM_NAME &&
              selectionRequest->time == 3456,
          "selection-request conversion");
    free(selectionRequest);

    memset(&x, 0, sizeof(x));
    x.xselection.type = SelectionNotify;
    x.xselection.requestor = window;
    x.xselection.selection = XCB_ATOM_PRIMARY;
    x.xselection.target = XCB_ATOM_STRING;
    x.xselection.property = XCB_ATOM_NONE;
    x.xselection.time = 4567;
    xcb_selection_notify_event_t *selectionNotify =
        (xcb_selection_notify_event_t *) convert(display, connection, &x);
    CHECK(selectionNotify &&
              selectionNotify->response_type == XCB_SELECTION_NOTIFY &&
              selectionNotify->requestor == window &&
              selectionNotify->selection == XCB_ATOM_PRIMARY &&
              selectionNotify->target == XCB_ATOM_STRING &&
              selectionNotify->property == XCB_ATOM_NONE &&
              selectionNotify->time == 4567,
          "selection-notify conversion");
    free(selectionNotify);

    memset(&x, 0, sizeof(x));
    x.xcolormap.type = ColormapNotify;
    x.xcolormap.window = window;
    x.xcolormap.colormap = DefaultColormap(display, DefaultScreen(display));
    x.xcolormap.new = True;
    x.xcolormap.state = ColormapInstalled;
    xcb_colormap_notify_event_t *colormap =
        (xcb_colormap_notify_event_t *) convert(display, connection, &x);
    CHECK(colormap && colormap->response_type == XCB_COLORMAP_NOTIFY &&
              colormap->window == window &&
              colormap->colormap ==
                  DefaultColormap(display, DefaultScreen(display)) &&
              colormap->_new && colormap->state == XCB_COLORMAP_STATE_INSTALLED,
          "colormap conversion");
    free(colormap);

    memset(&x, 0, sizeof(x));
    x.xmapping.type = MappingNotify;
    x.xmapping.window = window;
    x.xmapping.request = MappingKeyboard;
    x.xmapping.first_keycode = 17;
    x.xmapping.count = 23;
    xcb_mapping_notify_event_t *mapping =
        (xcb_mapping_notify_event_t *) convert(display, connection, &x);
    CHECK(mapping && mapping->response_type == XCB_MAPPING_NOTIFY &&
              mapping->request == XCB_MAPPING_KEYBOARD &&
              mapping->first_keycode == 17 && mapping->count == 23,
          "mapping conversion");
    free(mapping);

    memset(&x, 0, sizeof(x));
    x.xclient.type = ClientMessage;
    x.xclient.window = window;
    x.xclient.message_type = XCB_ATOM_WM_NAME;
    x.xclient.format = 8;
    memcpy(x.xclient.data.b, "client-message-data!", 20);
    xcb_client_message_event_t *client =
        (xcb_client_message_event_t *) convert(display, connection, &x);
    CHECK(client && client->window == window && client->format == 8 &&
              !memcmp(client->data.data8, "client-message-data!", 20),
          "client conversion");
    free(client);

    memset(&x, 0, sizeof(x));
    x.xfocus.type = FocusIn;
    x.xfocus.window = window;
    x.xfocus.mode = NotifyGrab;
    x.xfocus.detail = NotifyPointer;
    xcb_focus_in_event_t *focus =
        (xcb_focus_in_event_t *) convert(display, connection, &x);
    CHECK(focus && focus->event == window &&
              focus->mode == XCB_NOTIFY_MODE_GRAB &&
              focus->detail == XCB_NOTIFY_DETAIL_POINTER,
          "focus conversion");
    free(focus);
    memset(&x, 0, sizeof(x));
    x.xkeymap.type = KeymapNotify;
    x.xkeymap.serial = 0x34567;
    x.xkeymap.window = window;
    for (size_t i = 0; i < sizeof(x.xkeymap.key_vector); i++)
        x.xkeymap.key_vector[i] = (char) (i * 7u + 3u);
    xcb_keymap_notify_event_t *keymap =
        (xcb_keymap_notify_event_t *) convert(display, connection, &x);
    CHECK(keymap && keymap->response_type == XCB_KEYMAP_NOTIFY &&
              !memcmp(keymap->keys, x.xkeymap.key_vector + 1,
                      sizeof(keymap->keys)) &&
              ((xcb_generic_event_t *) keymap)->full_sequence == 0x34567,
          "keymap conversion");
    free(keymap);
    memset(&x, 0, sizeof(x));
    x.xkey.type = KeyPress;
    x.xkey.window = window;
    x.xkey.root = DefaultRootWindow(display);
    x.xkey.keycode = 42;
    x.xkey.x = 7;
    x.xkey.state = ShiftMask;
    xcb_key_press_event_t *key =
        (xcb_key_press_event_t *) convert(display, connection, &x);
    CHECK(key && key->event == window && key->detail == 42 &&
              key->event_x == 7 && key->state == XCB_MOD_MASK_SHIFT,
          "key conversion");
    free(key);
    memset(&x, 0, sizeof(x));
    x.xbutton.type = ButtonPress;
    x.xbutton.window = window;
    x.xbutton.button = 3;
    xcb_button_press_event_t *button =
        (xcb_button_press_event_t *) convert(display, connection, &x);
    CHECK(button && button->event == window && button->detail == 3,
          "button conversion");
    free(button);
    memset(&x, 0, sizeof(x));
    x.xmotion.type = MotionNotify;
    x.xmotion.window = window;
    x.xmotion.is_hint = NotifyHint;
    x.xmotion.x_root = 19;
    xcb_motion_notify_event_t *motion =
        (xcb_motion_notify_event_t *) convert(display, connection, &x);
    CHECK(motion && motion->event == window &&
              motion->detail == XCB_MOTION_HINT && motion->root_x == 19,
          "motion conversion");
    free(motion);
    memset(&x, 0, sizeof(x));
    x.xcrossing.type = EnterNotify;
    x.xcrossing.window = window;
    x.xcrossing.root = root;
    x.xcrossing.subwindow = window;
    x.xcrossing.time = 1234;
    x.xcrossing.x = -2;
    x.xcrossing.y = 3;
    x.xcrossing.x_root = 18;
    x.xcrossing.y_root = 19;
    x.xcrossing.mode = NotifyGrab;
    x.xcrossing.detail = NotifyInferior;
    x.xcrossing.same_screen = True;
    x.xcrossing.focus = True;
    x.xcrossing.state = ControlMask;
    xcb_enter_notify_event_t *enter =
        (xcb_enter_notify_event_t *) convert(display, connection, &x);
    CHECK(enter && enter->response_type == XCB_ENTER_NOTIFY &&
              enter->event == window && enter->root == root &&
              enter->child == window && enter->time == 1234 &&
              enter->event_x == -2 && enter->root_y == 19 &&
              enter->mode == XCB_NOTIFY_MODE_GRAB &&
              enter->detail == XCB_NOTIFY_DETAIL_INFERIOR &&
              enter->state == XCB_MOD_MASK_CONTROL &&
              enter->same_screen_focus == 3,
          "enter conversion");
    free(enter);
    x.xcrossing.type = LeaveNotify;
    x.xcrossing.focus = False;
    xcb_leave_notify_event_t *leave =
        (xcb_leave_notify_event_t *) convert(display, connection, &x);
    CHECK(leave && leave->response_type == XCB_LEAVE_NOTIFY &&
              leave->same_screen_focus == 1,
          "leave conversion");
    free(leave);
    memset(&x, 0, sizeof(x));
    x.xvisibility.type = VisibilityNotify;
    x.xvisibility.window = window;
    x.xvisibility.state = VisibilityPartiallyObscured;
    xcb_visibility_notify_event_t *visibility =
        (xcb_visibility_notify_event_t *) convert(display, connection, &x);
    CHECK(visibility && visibility->response_type == XCB_VISIBILITY_NOTIFY &&
              visibility->window == window &&
              visibility->state == XCB_VISIBILITY_PARTIALLY_OBSCURED,
          "visibility conversion");
    free(visibility);
    memset(&x, 0, sizeof(x));
    x.xdestroywindow.type = DestroyNotify;
    x.xdestroywindow.event = DefaultRootWindow(display);
    x.xdestroywindow.window = window;
    xcb_destroy_notify_event_t *destroy =
        (xcb_destroy_notify_event_t *) convert(display, connection, &x);
    CHECK(destroy && destroy->event == DefaultRootWindow(display) &&
              destroy->window == window,
          "destroy conversion");
    free(destroy);
    CHECK(!xcb_poll_for_event(connection), "empty poll");
    CHECK(!xcb_poll_for_special_event(connection, NULL), "special empty");
    memset(&x, 0, sizeof(x));
    x.xfocus.type = FocusOut;
    x.xfocus.window = window;
    XPutBackEvent(display, &x);
    xcb_generic_event_t *waited = xcb_wait_for_event(connection);
    CHECK(waited && waited->response_type == XCB_FOCUS_OUT, "blocking wait");
    free(waited);
    XSetEventQueueOwner(display, XlibOwnsEventQueue);
    memset(&x, 0, sizeof(x));
    x.xfocus.type = FocusIn;
    x.xfocus.window = window;
    XPutBackEvent(display, &x);
    CHECK(XEventsQueued(display, QueuedAlready) == 1,
          "Xlib sees Xlib-owned queue");
    XSetEventQueueOwner(display, XCBOwnsEventQueue);
    waited = xcb_poll_for_queued_event(connection);
    CHECK(waited && waited->response_type == XCB_FOCUS_IN,
          "Xlib-to-XCB switch preserves event");
    free(waited);
    memset(&x, 0, sizeof(x));
    x.xfocus.type = FocusOut;
    x.xfocus.window = window;
    XPutBackEvent(display, &x);
    XSetEventQueueOwner(display, XlibOwnsEventQueue);
    CHECK(XEventsQueued(display, QueuedAlready) == 1,
          "XCB-to-Xlib switch preserves event");
    XEvent consumed;
    XNextEvent(display, &consumed);
    CHECK(consumed.type == FocusOut, "Xlib consumes after owner switch");
    XSelectInput(display, window, NoEventMask);
    CHECK(!xcb_request_check(connection,
                             xcb_destroy_window_checked(connection, window)),
          "checked destroy request");
    xcb_disconnect(connection);
    XCloseDisplay(display);

    display = XOpenDisplay(NULL);
    CHECK(display, "open display-close test");
    connection = XGetXCBConnection(display);
    CHECK(connection, "display-close connection");
    EventWaiter waiter = {.connection = connection};
    pthread_t thread;
    CHECK(!pthread_create(&thread, NULL, waitForEvent, &waiter),
          "display-close waiter creation");
    waitForWaiters(connection, 1);
    XCloseDisplay(display);
    CHECK(!pthread_join(thread, NULL), "display-close waiter join");
    CHECK(!waiter.result &&
              xcb_connection_has_error(connection) == XCB_CONN_ERROR,
          "display close wakes XCB waiter");
    xcb_disconnect(connection);

    connection = xcb_connect(NULL, NULL);
    CHECK(connection && !xcb_connection_has_error(connection),
          "open disconnect test");
    EventWaiter errorWaiter = {.connection = connection};
    pthread_t errorThread;
    CHECK(!pthread_create(&errorThread, NULL, waitForEvent, &errorWaiter),
          "unchecked-error waiter creation");
    waitForWaiters(connection, 1);
    xcb_void_cookie_t unchecked =
        xcb_destroy_window(connection, UINT32_C(0xdeadbeef));
    CHECK(!pthread_join(errorThread, NULL), "unchecked-error waiter join");
    xcb_generic_error_t *uncheckedError =
        (xcb_generic_error_t *) errorWaiter.result;
    CHECK(uncheckedError && uncheckedError->response_type == 0 &&
              uncheckedError->error_code == XCB_WINDOW &&
              uncheckedError->full_sequence == unchecked.sequence,
          "unchecked error wakes XCB waiter");
    free(uncheckedError);
    enum { DISCONNECT_WAITER_COUNT = 4 };
    EventWaiter disconnectWaiters[DISCONNECT_WAITER_COUNT] = {0};
    pthread_t disconnectThreads[DISCONNECT_WAITER_COUNT];
    for (size_t i = 0; i < DISCONNECT_WAITER_COUNT; i++) {
        disconnectWaiters[i].connection = connection;
        CHECK(!pthread_create(&disconnectThreads[i], NULL, waitForEvent,
                              &disconnectWaiters[i]),
              "disconnect waiter creation");
    }
    waitForWaiters(connection, DISCONNECT_WAITER_COUNT);
    xcb_disconnect(connection);
    for (size_t i = 0; i < DISCONNECT_WAITER_COUNT; i++) {
        CHECK(!pthread_join(disconnectThreads[i], NULL),
              "disconnect waiter join");
        CHECK(!disconnectWaiters[i].result, "disconnect wakes XCB waiters");
    }
    puts("test-xcb-events: ok");
    return 0;
}
