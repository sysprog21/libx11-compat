#include <pthread.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <xcb/xcb.h>
#include "xcb-compat-private.h"
#include "../src/display.h"
#include "../src/events.h"
#include "../src/resource-types.h"



typedef struct PendingReply {
    uint64_t sequence;
    void *reply;
    xcb_generic_error_t *error;
    struct PendingReply *next;
} PendingReply;

typedef struct QueuedError {
    xcb_generic_error_t *error;
    struct QueuedError *next;
} QueuedError;

typedef struct {
    xcb_setup_t setup;
    char vendor[16];
    xcb_format_t formats[4];
    xcb_screen_t screen;
    xcb_depth_t depth;
    xcb_visualtype_t visual;
} SetupWire;

_Static_assert(offsetof(SetupWire, formats) == 56, "XCB format wire offset");
_Static_assert(offsetof(SetupWire, screen) == 88, "XCB screen wire offset");
_Static_assert(offsetof(SetupWire, depth) == 128, "XCB depth wire offset");
_Static_assert(offsetof(SetupWire, visual) == 136, "XCB visual wire offset");

struct xcb_connection_t {
    Display *display;
    int error;
    pthread_mutex_t mutex;
    pthread_cond_t eventCond;
    unsigned int eventUsers;
    unsigned int closeCallbacks;
    int closing;
    uint64_t nextSequence;
    PendingReply **pendingBuckets;
    size_t pendingBucketCount;
    size_t pendingCount;
    QueuedError *errorHead;
    QueuedError *errorTail;
    SetupWire wire;
    int ownsDisplay;
    int queueOwner;
    struct xcb_connection_t *next;
};

static pthread_mutex_t connectionsMutex = PTHREAD_MUTEX_INITIALIZER;
static xcb_connection_t *connections;

__attribute__((weak)) void xcbCompatReleaseRequestResources(
    xcb_connection_t *connection)
{
    (void) connection;
}

/* Callers hold connectionsMutex. A display maps to at most one connection. */
static xcb_connection_t *connectionForDisplayLocked(Display *display)
{
    for (xcb_connection_t *c = connections; c; c = c->next)
        if (c->display == display)
            return c;
    return NULL;
}

static int eventQueueOwner(Display *display)
{
    int owner = 0;
    pthread_mutex_lock(&connectionsMutex);
    xcb_connection_t *c = connectionForDisplayLocked(display);
    if (c) {
        pthread_mutex_lock(&c->mutex);
        owner = c->queueOwner;
        pthread_mutex_unlock(&c->mutex);
    }
    pthread_mutex_unlock(&connectionsMutex);
    return owner;
}

static int eventWaitCancelled(Display *display)
{
    int cancelled = 0;
    pthread_mutex_lock(&connectionsMutex);
    xcb_connection_t *c = connectionForDisplayLocked(display);
    if (c) {
        pthread_mutex_lock(&c->mutex);
        cancelled = c->closing || c->errorHead != NULL;
        pthread_mutex_unlock(&c->mutex);
    }
    pthread_mutex_unlock(&connectionsMutex);
    return cancelled;
}

/* Release everything a dead connection still holds. Caller holds c->mutex.
 * Nothing can reap these once the connection is in error: xcb_request_check and
 * the reply accessors both refuse a failed connection.
 */
static void drainPendingLocked(xcb_connection_t *c)
{
    for (size_t i = 0; i < c->pendingBucketCount; i++) {
        PendingReply *pending = c->pendingBuckets[i];
        while (pending) {
            PendingReply *next = pending->next;
            free(pending->reply);
            free(pending->error);
            free(pending);
            pending = next;
        }
        c->pendingBuckets[i] = NULL;
    }
    c->pendingCount = 0;
    while (c->errorHead) {
        QueuedError *queued = c->errorHead;
        c->errorHead = queued->next;
        free(queued->error);
        free(queued);
    }
    c->errorTail = NULL;
}

static void displayClosed(Display *display)
{
    pthread_mutex_lock(&connectionsMutex);
    xcb_connection_t *connection = connectionForDisplayLocked(display);
    if (connection) {
        pthread_mutex_lock(&connection->mutex);
        connection->closeCallbacks++;
        pthread_mutex_unlock(&connection->mutex);
    }
    pthread_mutex_unlock(&connectionsMutex);
    if (!connection)
        return;

    pthread_mutex_lock(&connection->mutex);
    connection->closing = 1;
    connection->error = XCB_CONN_ERROR;
    while (connection->eventUsers)
        pthread_cond_wait(&connection->eventCond, &connection->mutex);
    connection->display = NULL;
    connection->ownsDisplay = 0;

    /* The wrapper itself stays registered and errored, because a client that
     * kept the XGetXCBConnection pointer must get a failed connection rather
     * than a dangling one. What it can no longer reach goes now.
     */
    drainPendingLocked(connection);
    pthread_mutex_unlock(&connection->mutex);

    xcbCompatReleaseRequestResources(connection);
    pthread_mutex_lock(&connection->mutex);
    connection->closeCallbacks--;
    pthread_cond_broadcast(&connection->eventCond);
    pthread_mutex_unlock(&connection->mutex);
}

static void installDisplayHooks(void)
{
    libx11CompatSetDisplayCloseHook(displayClosed);
    libx11CompatSetEventQueueOwnerHook(eventQueueOwner);
    libx11CompatSetEventWaitCancelledHook(eventWaitCancelled);
}

static void registerConnection(xcb_connection_t *c)
{
    installDisplayHooks();
    pthread_mutex_lock(&connectionsMutex);
    c->next = connections;
    connections = c;
    pthread_mutex_unlock(&connectionsMutex);
}

/* Publish c for its display unless another thread got there first, in which
 * case the winner is returned and c is left for the caller to discard. The
 * lookup and the insertion happen under one acquisition, so a display can never
 * end up with two connection wrappers.
 */
static xcb_connection_t *registerConnectionForDisplay(xcb_connection_t *c)
{
    installDisplayHooks();
    pthread_mutex_lock(&connectionsMutex);
    xcb_connection_t *existing = connectionForDisplayLocked(c->display);
    if (!existing) {
        c->next = connections;
        connections = c;
    }
    pthread_mutex_unlock(&connectionsMutex);
    return existing;
}

static void unregisterConnection(xcb_connection_t *c)
{
    pthread_mutex_lock(&connectionsMutex);
    xcb_connection_t **link = &connections;
    while (*link && *link != c)
        link = &(*link)->next;
    if (*link)
        *link = c->next;
    pthread_mutex_unlock(&connectionsMutex);
}

static void initializeSetup(xcb_connection_t *connection)
{
    Display *display = connection->display;
    SetupWire *wire = &connection->wire;
    xcb_setup_t *setup = &wire->setup;
    xcb_screen_t *screen = &wire->screen;
    Visual *visual = DefaultVisual(display, DefaultScreen(display));
    int depth = DefaultDepth(display, DefaultScreen(display));
    memset(wire, 0, sizeof(*wire));
    setup->status = 1;
    setup->protocol_major_version = 11;
    setup->length = (sizeof(*wire) - 8) / 4;
    setup->release_number = 1;
    setup->resource_id_base = XID_RESOURCE_BASE;
    setup->resource_id_mask = XID_RESOURCE_MASK;
    setup->vendor_len = sizeof("libx11-compat") - 1;
    setup->maximum_request_length = 65535;
    setup->roots_len = 1;
    setup->pixmap_formats_len = 4;
    setup->image_byte_order = ImageByteOrder(display);
    setup->bitmap_format_bit_order = BitmapBitOrder(display);
    setup->bitmap_format_scanline_unit = BitmapUnit(display);
    setup->bitmap_format_scanline_pad = BitmapPad(display);
    int minimumKeycode, maximumKeycode;
    XDisplayKeycodes(display, &minimumKeycode, &maximumKeycode);
    setup->min_keycode = minimumKeycode;
    setup->max_keycode = maximumKeycode;
    memcpy(wire->vendor, "libx11-compat", setup->vendor_len);
    static const xcb_format_t formats[] = {
        {.depth = 1, .bits_per_pixel = 1, .scanline_pad = 32},
        {.depth = 16, .bits_per_pixel = 16, .scanline_pad = 32},
        {.depth = 24, .bits_per_pixel = 32, .scanline_pad = 32},
        {.depth = 32, .bits_per_pixel = 32, .scanline_pad = 32},
    };
    memcpy(wire->formats, formats, sizeof(formats));
    screen->root = DefaultRootWindow(display);
    screen->default_colormap = DefaultColormap(display, DefaultScreen(display));
    screen->white_pixel = WhitePixel(display, DefaultScreen(display));
    screen->black_pixel = BlackPixel(display, DefaultScreen(display));
    screen->width_in_pixels = DisplayWidth(display, DefaultScreen(display));
    screen->height_in_pixels = DisplayHeight(display, DefaultScreen(display));
    screen->width_in_millimeters =
        DisplayWidthMM(display, DefaultScreen(display));
    screen->height_in_millimeters =
        DisplayHeightMM(display, DefaultScreen(display));
    screen->min_installed_maps = screen->max_installed_maps = 1;
    screen->root_visual = XVisualIDFromVisual(visual);
    screen->backing_stores = DoesBackingStore(DefaultScreenOfDisplay(display));
    screen->save_unders = DoesSaveUnders(DefaultScreenOfDisplay(display));
    screen->root_depth = depth;
    screen->allowed_depths_len = 1;
    wire->depth.depth = depth;
    wire->depth.visuals_len = 1;
    wire->visual.visual_id = XVisualIDFromVisual(visual);
    wire->visual._class = visual->class;
    wire->visual.bits_per_rgb_value = visual->bits_per_rgb;
    wire->visual.colormap_entries = visual->map_entries;
    wire->visual.red_mask = visual->red_mask;
    wire->visual.green_mask = visual->green_mask;
    wire->visual.blue_mask = visual->blue_mask;
}

static xcb_connection_t *allocConnection(void)
{
    xcb_connection_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    if (pthread_mutex_init(&c->mutex, NULL) != 0) {
        free(c);
        return NULL;
    }
    if (pthread_cond_init(&c->eventCond, NULL) != 0) {
        pthread_mutex_destroy(&c->mutex);
        free(c);
        return NULL;
    }
    c->nextSequence = 1;
    return c;
}

static void freeConnection(xcb_connection_t *c)
{
    pthread_cond_destroy(&c->eventCond);
    pthread_mutex_destroy(&c->mutex);
    free(c);
}

xcb_connection_t *xcb_connect(const char *name, int *screen)
{
    xcb_connection_t *c = allocConnection();
    if (!c)
        return NULL;
    if (screen)
        *screen = 0;
    c->display = XOpenDisplay(name);
    if (!c->display) {
        c->error = XCB_CONN_ERROR;
        return c;
    }
    c->ownsDisplay = 1;
    initializeSetup(c);
    registerConnection(c);
    return c;
}

xcb_connection_t *xcbCompatConnectionForDisplay(Display *display)
{
    if (!display)
        return NULL;
    pthread_mutex_lock(&connectionsMutex);
    xcb_connection_t *existing = connectionForDisplayLocked(display);
    pthread_mutex_unlock(&connectionsMutex);
    if (existing)
        return existing;
    xcb_connection_t *c = allocConnection();
    if (!c)
        return NULL;
    c->display = display;
    initializeSetup(c);

    /* Another thread may have bound this display while the setup record was
     * being built, so publish and re-check as one step and keep the winner.
     */
    existing = registerConnectionForDisplay(c);
    if (existing) {
        freeConnection(c);
        return existing;
    }
    return c;
}

/* Number of threads currently blocked inside an event wait on this connection.
 * Exposed for tests, which otherwise have no way to tell a thread that is about
 * to block from one that already is.
 */
unsigned int xcbCompatEventWaiters(xcb_connection_t *c)
{
    if (!c)
        return 0;
    pthread_mutex_lock(&c->mutex);
    unsigned int waiters = c->eventUsers;
    pthread_mutex_unlock(&c->mutex);
    return waiters;
}

void xcbCompatSetQueueOwner(xcb_connection_t *c, int owner)
{
    if (c) {
        pthread_mutex_lock(&c->mutex);
        c->queueOwner = owner != 0;
        pthread_mutex_unlock(&c->mutex);
    }
}

xcb_connection_t *xcb_connect_to_display_with_auth_info(const char *display,
                                                        xcb_auth_info_t *auth,
                                                        int *screen)
{
    (void) auth;
    return xcb_connect(display, screen);
}

int xcb_connection_has_error(xcb_connection_t *c)
{
    if (!c)
        return XCB_CONN_ERROR;
    pthread_mutex_lock(&c->mutex);
    int error = c->error;
    pthread_mutex_unlock(&c->mutex);
    return error;
}

void xcbCompatSetConnectionError(xcb_connection_t *c, int error)
{
    if (!c || !error)
        return;
    pthread_mutex_lock(&c->mutex);
    if (!c->error)
        c->error = error;
    pthread_mutex_unlock(&c->mutex);
}
int xcbCompatRequestReady(xcb_connection_t *c)
{
    if (!c)
        return 0;
    pthread_mutex_lock(&c->mutex);
    int ready = c->display && !c->error && !c->closing;
    pthread_mutex_unlock(&c->mutex);
    return ready;
}
int xcb_get_file_descriptor(xcb_connection_t *c)
{
    (void) c;
    return -1;
}
const xcb_setup_t *xcb_get_setup(xcb_connection_t *c)
{
    return (!c || c->error) ? NULL : &c->wire.setup;
}
uint32_t xcb_get_maximum_request_length(xcb_connection_t *c)
{
    const xcb_setup_t *s = xcb_get_setup(c);
    return s ? s->maximum_request_length : 0;
}
uint32_t xcb_generate_id(xcb_connection_t *c)
{
    return (!c || c->error) ? UINT32_MAX : allocXidResource();
}

typedef struct {
    int type;
    Display *display;
    uint64_t serial;
    uint8_t synthetic, detail, mode, format, sameScreen, focus,
        overrideRedirect;
    uint32_t window, event, root, child, sibling, parent, owner, requestor,
        selection, target, property, colormap, atom, messageType, time, state;
    int16_t x, y, rootX, rootY;
    uint16_t unsignedX, unsignedY, width, height, borderWidth, count,
        minorOpcode;
    uint8_t fromConfigure, newColormap, majorOpcode, firstKeycode, keys[32],
        data[20];
} NormalizedEvent;

static void normalizeEvent(const XEvent *x, NormalizedEvent *n)
{
    memset(n, 0, sizeof(*n));
    n->type = x->type;
    n->display = x->xany.display;
    n->serial = x->xany.serial;
    n->synthetic = x->xany.send_event;
    switch (x->type) {
    case KeyPress:
    case KeyRelease:
        n->detail = x->xkey.keycode;
        n->time = x->xkey.time;
        n->root = x->xkey.root;
        n->window = x->xkey.window;
        n->child = x->xkey.subwindow;
        n->rootX = x->xkey.x_root;
        n->rootY = x->xkey.y_root;
        n->x = x->xkey.x;
        n->y = x->xkey.y;
        n->state = x->xkey.state;
        n->sameScreen = x->xkey.same_screen;
        break;
    case ButtonPress:
    case ButtonRelease:
        n->detail = x->xbutton.button;
        n->time = x->xbutton.time;
        n->root = x->xbutton.root;
        n->window = x->xbutton.window;
        n->child = x->xbutton.subwindow;
        n->rootX = x->xbutton.x_root;
        n->rootY = x->xbutton.y_root;
        n->x = x->xbutton.x;
        n->y = x->xbutton.y;
        n->state = x->xbutton.state;
        n->sameScreen = x->xbutton.same_screen;
        break;
    case MotionNotify:
        n->detail = x->xmotion.is_hint;
        n->time = x->xmotion.time;
        n->root = x->xmotion.root;
        n->window = x->xmotion.window;
        n->child = x->xmotion.subwindow;
        n->rootX = x->xmotion.x_root;
        n->rootY = x->xmotion.y_root;
        n->x = x->xmotion.x;
        n->y = x->xmotion.y;
        n->state = x->xmotion.state;
        n->sameScreen = x->xmotion.same_screen;
        break;
    case EnterNotify:
    case LeaveNotify:
        n->detail = x->xcrossing.detail;
        n->time = x->xcrossing.time;
        n->root = x->xcrossing.root;
        n->window = x->xcrossing.window;
        n->child = x->xcrossing.subwindow;
        n->rootX = x->xcrossing.x_root;
        n->rootY = x->xcrossing.y_root;
        n->x = x->xcrossing.x;
        n->y = x->xcrossing.y;
        n->state = x->xcrossing.state;
        n->mode = x->xcrossing.mode;
        n->sameScreen = x->xcrossing.same_screen;
        n->focus = x->xcrossing.focus;
        break;
    case FocusIn:
    case FocusOut:
        n->detail = x->xfocus.detail;
        n->window = x->xfocus.window;
        n->mode = x->xfocus.mode;
        break;
    case KeymapNotify:
        n->window = x->xkeymap.window;
        memcpy(n->keys, x->xkeymap.key_vector, sizeof(n->keys));
        break;
    case Expose:
        n->window = x->xexpose.window;
        n->x = x->xexpose.x;
        n->y = x->xexpose.y;
        n->width = x->xexpose.width;
        n->height = x->xexpose.height;
        n->count = x->xexpose.count;
        break;
    case GraphicsExpose:
        n->window = x->xgraphicsexpose.drawable;
        n->unsignedX = x->xgraphicsexpose.x;
        n->unsignedY = x->xgraphicsexpose.y;
        n->width = x->xgraphicsexpose.width;
        n->height = x->xgraphicsexpose.height;
        n->count = x->xgraphicsexpose.count;
        n->majorOpcode = x->xgraphicsexpose.major_code;
        n->minorOpcode = x->xgraphicsexpose.minor_code;
        break;
    case NoExpose:
        n->window = x->xnoexpose.drawable;
        n->majorOpcode = x->xnoexpose.major_code;
        n->minorOpcode = x->xnoexpose.minor_code;
        break;
    case VisibilityNotify:
        n->window = x->xvisibility.window;
        n->state = x->xvisibility.state;
        break;
    case ConfigureNotify:
        n->event = x->xconfigure.event;
        n->window = x->xconfigure.window;
        n->sibling = x->xconfigure.above;
        n->x = x->xconfigure.x;
        n->y = x->xconfigure.y;
        n->width = x->xconfigure.width;
        n->height = x->xconfigure.height;
        n->borderWidth = x->xconfigure.border_width;
        n->overrideRedirect = x->xconfigure.override_redirect;
        break;
    case CreateNotify:
        n->parent = x->xcreatewindow.parent;
        n->window = x->xcreatewindow.window;
        n->x = x->xcreatewindow.x;
        n->y = x->xcreatewindow.y;
        n->width = x->xcreatewindow.width;
        n->height = x->xcreatewindow.height;
        n->borderWidth = x->xcreatewindow.border_width;
        n->overrideRedirect = x->xcreatewindow.override_redirect;
        break;
    case MapNotify:
        n->event = x->xmap.event;
        n->window = x->xmap.window;
        n->overrideRedirect = x->xmap.override_redirect;
        break;
    case UnmapNotify:
        n->event = x->xunmap.event;
        n->window = x->xunmap.window;
        n->fromConfigure = x->xunmap.from_configure;
        break;
    case ReparentNotify:
        n->event = x->xreparent.event;
        n->window = x->xreparent.window;
        n->parent = x->xreparent.parent;
        n->x = x->xreparent.x;
        n->y = x->xreparent.y;
        n->overrideRedirect = x->xreparent.override_redirect;
        break;
    case GravityNotify:
        n->event = x->xgravity.event;
        n->window = x->xgravity.window;
        n->x = x->xgravity.x;
        n->y = x->xgravity.y;
        break;
    case MapRequest:
        n->parent = x->xmaprequest.parent;
        n->window = x->xmaprequest.window;
        break;
    case ConfigureRequest:
        n->parent = x->xconfigurerequest.parent;
        n->window = x->xconfigurerequest.window;
        n->sibling = x->xconfigurerequest.above;
        n->x = x->xconfigurerequest.x;
        n->y = x->xconfigurerequest.y;
        n->width = x->xconfigurerequest.width;
        n->height = x->xconfigurerequest.height;
        n->borderWidth = x->xconfigurerequest.border_width;
        n->detail = x->xconfigurerequest.detail;
        n->state = x->xconfigurerequest.value_mask;
        break;
    case ResizeRequest:
        n->window = x->xresizerequest.window;
        n->width = x->xresizerequest.width;
        n->height = x->xresizerequest.height;
        break;
    case CirculateNotify:
        n->event = x->xcirculate.event;
        n->window = x->xcirculate.window;
        n->detail = x->xcirculate.place;
        break;
    case CirculateRequest:
        n->event = x->xcirculaterequest.parent;
        n->window = x->xcirculaterequest.window;
        n->detail = x->xcirculaterequest.place;
        break;
    case PropertyNotify:
        n->window = x->xproperty.window;
        n->atom = x->xproperty.atom;
        n->time = x->xproperty.time;
        n->state = x->xproperty.state;
        break;
    case SelectionClear:
        n->time = x->xselectionclear.time;
        n->owner = x->xselectionclear.window;
        n->selection = x->xselectionclear.selection;
        break;
    case SelectionRequest:
        n->time = x->xselectionrequest.time;
        n->owner = x->xselectionrequest.owner;
        n->requestor = x->xselectionrequest.requestor;
        n->selection = x->xselectionrequest.selection;
        n->target = x->xselectionrequest.target;
        n->property = x->xselectionrequest.property;
        break;
    case SelectionNotify:
        n->time = x->xselection.time;
        n->requestor = x->xselection.requestor;
        n->selection = x->xselection.selection;
        n->target = x->xselection.target;
        n->property = x->xselection.property;
        break;
    case ColormapNotify:
        n->window = x->xcolormap.window;
        n->colormap = x->xcolormap.colormap;
        n->newColormap = x->xcolormap.new;
        n->state = x->xcolormap.state;
        break;
    case MappingNotify:
        n->window = x->xmapping.window;
        n->detail = x->xmapping.request;
        n->firstKeycode = x->xmapping.first_keycode;
        n->count = x->xmapping.count;
        break;
    case DestroyNotify:
        n->event = x->xdestroywindow.event;
        n->window = x->xdestroywindow.window;
        break;
    case ClientMessage:
        n->format = x->xclient.format;
        n->window = x->xclient.window;
        n->messageType = x->xclient.message_type;
        memcpy(n->data, x->xclient.data.b, sizeof(n->data));
        break;
    }
}

static xcb_generic_event_t *convertEvent(xcb_connection_t *c, const XEvent *x)
{
    _Static_assert(sizeof(xcb_raw_generic_event_t) == 32,
                   "XCB core event wire size");
    _Static_assert(offsetof(xcb_generic_event_t, full_sequence) == 32,
                   "XCB full sequence trailer offset");
    xcb_generic_event_t *generic = calloc(1, sizeof(*generic));
    if (!generic)
        return NULL;
    NormalizedEvent n;
    normalizeEvent(x, &n);
    generic->response_type = (uint8_t) n.type | (n.synthetic ? 0x80 : 0);
    generic->sequence = (uint16_t) n.serial;
    generic->full_sequence = (uint32_t) n.serial;
    switch (n.type) {
    case KeyPress:
    case KeyRelease:
    case ButtonPress:
    case ButtonRelease:
    case MotionNotify: {
        xcb_key_press_event_t *e = (xcb_key_press_event_t *) generic;
        e->detail = n.detail;
        e->time = n.time;
        e->root = n.root;
        e->event = n.window;
        e->child = n.child;
        e->root_x = n.rootX;
        e->root_y = n.rootY;
        e->event_x = n.x;
        e->event_y = n.y;
        e->state = n.state;
        e->same_screen = n.sameScreen;
        break;
    }
    case EnterNotify:
    case LeaveNotify: {
        xcb_enter_notify_event_t *e = (xcb_enter_notify_event_t *) generic;
        e->detail = n.detail;
        e->time = n.time;
        e->root = n.root;
        e->event = n.window;
        e->child = n.child;
        e->root_x = n.rootX;
        e->root_y = n.rootY;
        e->event_x = n.x;
        e->event_y = n.y;
        e->state = n.state;
        e->mode = n.mode;
        e->same_screen_focus = (n.sameScreen ? 1u : 0u) | (n.focus ? 2u : 0u);
        break;
    }
    case FocusIn:
    case FocusOut: {
        xcb_focus_in_event_t *e = (xcb_focus_in_event_t *) generic;
        e->detail = n.detail;
        e->event = n.window;
        e->mode = n.mode;
        break;
    }
    case KeymapNotify: {
        xcb_keymap_notify_event_t *e = (xcb_keymap_notify_event_t *) generic;
        memcpy(e->keys, n.keys + 1, sizeof(e->keys));
        break;
    }
    case Expose: {
        xcb_expose_event_t *e = (xcb_expose_event_t *) generic;
        e->window = n.window;
        e->x = n.x;
        e->y = n.y;
        e->width = n.width;
        e->height = n.height;
        e->count = n.count;
        break;
    }
    case GraphicsExpose: {
        xcb_graphics_exposure_event_t *e =
            (xcb_graphics_exposure_event_t *) generic;
        e->drawable = n.window;
        e->x = n.unsignedX;
        e->y = n.unsignedY;
        e->width = n.width;
        e->height = n.height;
        e->minor_opcode = n.minorOpcode;
        e->count = n.count;
        e->major_opcode = n.majorOpcode;
        break;
    }
    case NoExpose: {
        xcb_no_exposure_event_t *e = (xcb_no_exposure_event_t *) generic;
        e->drawable = n.window;
        e->minor_opcode = n.minorOpcode;
        e->major_opcode = n.majorOpcode;
        break;
    }
    case VisibilityNotify: {
        xcb_visibility_notify_event_t *e =
            (xcb_visibility_notify_event_t *) generic;
        e->window = n.window;
        e->state = n.state;
        break;
    }
    case ConfigureNotify: {
        xcb_configure_notify_event_t *e =
            (xcb_configure_notify_event_t *) generic;
        e->event = n.event;
        e->window = n.window;
        e->above_sibling = n.sibling;
        e->x = n.x;
        e->y = n.y;
        e->width = n.width;
        e->height = n.height;
        e->border_width = n.borderWidth;
        e->override_redirect = n.overrideRedirect;
        break;
    }
    case CreateNotify: {
        xcb_create_notify_event_t *e = (xcb_create_notify_event_t *) generic;
        e->parent = n.parent;
        e->window = n.window;
        e->x = n.x;
        e->y = n.y;
        e->width = n.width;
        e->height = n.height;
        e->border_width = n.borderWidth;
        e->override_redirect = n.overrideRedirect;
        break;
    }
    case MapNotify: {
        xcb_map_notify_event_t *e = (xcb_map_notify_event_t *) generic;
        e->event = n.event;
        e->window = n.window;
        e->override_redirect = n.overrideRedirect;
        break;
    }
    case UnmapNotify: {
        xcb_unmap_notify_event_t *e = (xcb_unmap_notify_event_t *) generic;
        e->event = n.event;
        e->window = n.window;
        e->from_configure = n.fromConfigure;
        break;
    }
    case ReparentNotify: {
        xcb_reparent_notify_event_t *e =
            (xcb_reparent_notify_event_t *) generic;
        e->event = n.event;
        e->window = n.window;
        e->parent = n.parent;
        e->x = n.x;
        e->y = n.y;
        e->override_redirect = n.overrideRedirect;
        break;
    }
    case GravityNotify: {
        xcb_gravity_notify_event_t *e = (xcb_gravity_notify_event_t *) generic;
        e->event = n.event;
        e->window = n.window;
        e->x = n.x;
        e->y = n.y;
        break;
    }
    case MapRequest: {
        xcb_map_request_event_t *e = (xcb_map_request_event_t *) generic;
        e->parent = n.parent;
        e->window = n.window;
        break;
    }
    case ConfigureRequest: {
        xcb_configure_request_event_t *e =
            (xcb_configure_request_event_t *) generic;
        e->stack_mode = n.detail;
        e->parent = n.parent;
        e->window = n.window;
        e->sibling = n.sibling;
        e->x = n.x;
        e->y = n.y;
        e->width = n.width;
        e->height = n.height;
        e->border_width = n.borderWidth;
        e->value_mask = n.state;
        break;
    }
    case ResizeRequest: {
        xcb_resize_request_event_t *e = (xcb_resize_request_event_t *) generic;
        e->window = n.window;
        e->width = n.width;
        e->height = n.height;
        break;
    }
    case CirculateNotify:
    case CirculateRequest: {
        xcb_circulate_notify_event_t *e =
            (xcb_circulate_notify_event_t *) generic;
        e->event = n.event;
        e->window = n.window;
        e->place = n.detail;
        break;
    }
    case PropertyNotify: {
        xcb_property_notify_event_t *e =
            (xcb_property_notify_event_t *) generic;
        e->window = n.window;
        e->atom = n.atom;
        e->time = n.time;
        e->state = n.state;
        break;
    }
    case SelectionClear: {
        xcb_selection_clear_event_t *e =
            (xcb_selection_clear_event_t *) generic;
        e->time = n.time;
        e->owner = n.owner;
        e->selection = n.selection;
        break;
    }
    case SelectionRequest: {
        xcb_selection_request_event_t *e =
            (xcb_selection_request_event_t *) generic;
        e->time = n.time;
        e->owner = n.owner;
        e->requestor = n.requestor;
        e->selection = n.selection;
        e->target = n.target;
        e->property = n.property;
        break;
    }
    case SelectionNotify: {
        xcb_selection_notify_event_t *e =
            (xcb_selection_notify_event_t *) generic;
        e->time = n.time;
        e->requestor = n.requestor;
        e->selection = n.selection;
        e->target = n.target;
        e->property = n.property;
        break;
    }
    case ColormapNotify: {
        xcb_colormap_notify_event_t *e =
            (xcb_colormap_notify_event_t *) generic;
        e->window = n.window;
        e->colormap = n.colormap;
        e->_new = n.newColormap;
        e->state = n.state;
        break;
    }
    case MappingNotify: {
        xcb_mapping_notify_event_t *e = (xcb_mapping_notify_event_t *) generic;
        e->request = n.detail;
        e->first_keycode = n.firstKeycode;
        e->count = n.count;
        break;
    }
    case DestroyNotify: {
        xcb_destroy_notify_event_t *e = (xcb_destroy_notify_event_t *) generic;
        e->event = n.event;
        e->window = n.window;
        break;
    }
    case ClientMessage: {
        xcb_client_message_event_t *e = (xcb_client_message_event_t *) generic;
        e->format = n.format;
        e->window = n.window;
        e->type = n.messageType;
        memcpy(e->data.data8, n.data, sizeof(e->data.data8));
        break;
    }
    default:
        break;
    }
    (void) c;
    return generic;
}

static xcb_generic_event_t *takeQueuedErrorLocked(xcb_connection_t *c)
{
    QueuedError *queued = c->errorHead;
    if (!queued)
        return NULL;
    c->errorHead = queued->next;
    if (!c->errorHead)
        c->errorTail = NULL;
    xcb_generic_event_t *error = (xcb_generic_event_t *) queued->error;
    free(queued);
    return error;
}

static xcb_generic_event_t *takeEvent(xcb_connection_t *c, int mode)
{
    if (!c)
        return NULL;
    pthread_mutex_lock(&c->mutex);
    xcb_generic_event_t *queuedError = takeQueuedErrorLocked(c);
    if (queuedError) {
        pthread_mutex_unlock(&c->mutex);
        return queuedError;
    }
    if (!c->display || c->error || c->closing) {
        pthread_mutex_unlock(&c->mutex);
        return NULL;
    }
    Display *display = c->display;
    c->eventUsers++;
    pthread_mutex_unlock(&c->mutex);

    libx11CompatBeginXcbEventAccess();
    XEvent event = {0};
    int haveEvent = mode < 0 || XEventsQueued(display, mode) != 0;
    while (haveEvent) {
        XNextEvent(display, &event);
        pthread_mutex_lock(&c->mutex);
        int interrupted = c->closing;
        if (event.type == 0 && !interrupted)
            queuedError = takeQueuedErrorLocked(c);
        pthread_mutex_unlock(&c->mutex);
        if (queuedError || interrupted || event.type != 0)
            break;
    }
    libx11CompatEndXcbEventAccess();

    pthread_mutex_lock(&c->mutex);
    int closing = c->closing;
    pthread_mutex_unlock(&c->mutex);
    xcb_generic_event_t *converted =
        haveEvent && !closing && !queuedError ? convertEvent(c, &event) : NULL;

    pthread_mutex_lock(&c->mutex);
    if (haveEvent && !closing && !queuedError && !converted)
        c->error = XCB_CONN_CLOSED_MEM_INSUFFICIENT;
    c->eventUsers--;
    pthread_cond_broadcast(&c->eventCond);
    pthread_mutex_unlock(&c->mutex);
    return queuedError ? queuedError : converted;
}

xcb_generic_event_t *xcb_poll_for_event(xcb_connection_t *c)
{
    return takeEvent(c, QueuedAfterReading);
}

xcb_generic_event_t *xcb_poll_for_queued_event(xcb_connection_t *c)
{
    return takeEvent(c, QueuedAlready);
}

xcb_generic_event_t *xcb_wait_for_event(xcb_connection_t *c)
{
    return takeEvent(c, -1);
}

xcb_generic_event_t *xcb_poll_for_special_event(xcb_connection_t *c,
                                                xcb_special_event_t *special)
{
    (void) c;
    (void) special;
    return NULL;
}

char *xcb_setup_vendor(const xcb_setup_t *s)
{
    return s ? (char *) (s + 1) : NULL;
}

int xcb_setup_vendor_length(const xcb_setup_t *s)
{
    return s ? s->vendor_len : 0;
}

xcb_generic_iterator_t xcb_setup_vendor_end(const xcb_setup_t *s)
{
    xcb_generic_iterator_t end = {0};
    if (s) {
        end.data = xcb_setup_vendor(s) + s->vendor_len;
        end.index = sizeof(*s) + s->vendor_len;
    }
    return end;
}

xcb_format_t *xcb_setup_pixmap_formats(const xcb_setup_t *s)
{
    if (!s)
        return NULL;
    size_t vendor = (s->vendor_len + 3u) & ~3u;
    return (xcb_format_t *) ((char *) (s + 1) + vendor);
}

int xcb_setup_pixmap_formats_length(const xcb_setup_t *s)
{
    return s ? s->pixmap_formats_len : 0;
}

xcb_format_iterator_t xcb_setup_pixmap_formats_iterator(const xcb_setup_t *s)
{
    xcb_format_iterator_t i = {0};
    if (s) {
        i.data = xcb_setup_pixmap_formats(s);
        i.rem = s->pixmap_formats_len;
        i.index = (char *) i.data - (char *) s;
    }
    return i;
}

void xcb_format_next(xcb_format_iterator_t *i)
{
    if (!i || i->rem <= 0)
        return;
    i->data++;
    i->index += sizeof(*i->data);
    i->rem--;
}

xcb_generic_iterator_t xcb_format_end(xcb_format_iterator_t i)
{
    while (i.rem > 0)
        xcb_format_next(&i);
    xcb_generic_iterator_t end = {
        .data = i.data,
        .rem = 0,
        .index = i.index,
    };
    return end;
}

xcb_screen_iterator_t xcb_setup_roots_iterator(const xcb_setup_t *s)
{
    xcb_screen_iterator_t i = {0};
    if (!s)
        return i;
    size_t vendor = (s->vendor_len + 3u) & ~3u;
    i.data = (xcb_screen_t *) ((char *) (s + 1) + vendor +
                               s->pixmap_formats_len * sizeof(xcb_format_t));
    i.rem = s->roots_len;
    i.index = (char *) i.data - (char *) s;
    return i;
}
int xcb_setup_roots_length(const xcb_setup_t *s)
{
    return s ? s->roots_len : 0;
}
xcb_depth_iterator_t xcb_screen_allowed_depths_iterator(const xcb_screen_t *s)
{
    xcb_depth_iterator_t i = {0};
    if (s) {
        i.data = (xcb_depth_t *) (s + 1);
        i.rem = s->allowed_depths_len;
        i.index = sizeof(*s);
    }
    return i;
}
int xcb_screen_allowed_depths_length(const xcb_screen_t *s)
{
    return s ? s->allowed_depths_len : 0;
}
xcb_visualtype_iterator_t xcb_depth_visuals_iterator(const xcb_depth_t *d)
{
    xcb_visualtype_iterator_t i = {0};
    if (d) {
        i.data = (xcb_visualtype_t *) (d + 1);
        i.rem = d->visuals_len;
        i.index = sizeof(*d);
    }
    return i;
}
xcb_visualtype_t *xcb_depth_visuals(const xcb_depth_t *d)
{
    return d ? (xcb_visualtype_t *) (d + 1) : NULL;
}
int xcb_depth_sizeof(const void *buffer)
{
    if (!buffer)
        return 0;
    const xcb_depth_t *depth = buffer;
    uint64_t size = sizeof(*depth) +
                    (uint64_t) depth->visuals_len * sizeof(xcb_visualtype_t);
    return size <= INT_MAX ? (int) size : 0;
}
int xcb_depth_visuals_length(const xcb_depth_t *d)
{
    return d ? d->visuals_len : 0;
}
void xcb_depth_next(xcb_depth_iterator_t *i)
{
    if (!i || i->rem <= 0)
        return;
    size_t bytes =
        sizeof(*i->data) + i->data->visuals_len * sizeof(xcb_visualtype_t);
    i->data = (xcb_depth_t *) ((char *) i->data + bytes);
    i->index += bytes;
    i->rem--;
}
xcb_generic_iterator_t xcb_depth_end(xcb_depth_iterator_t i)
{
    while (i.rem > 0)
        xcb_depth_next(&i);
    xcb_generic_iterator_t end = {
        .data = i.data,
        .rem = 0,
        .index = i.index,
    };
    return end;
}
void xcb_visualtype_next(xcb_visualtype_iterator_t *i)
{
    if (!i || i->rem <= 0)
        return;
    i->data++;
    i->index += sizeof(*i->data);
    i->rem--;
}
xcb_generic_iterator_t xcb_visualtype_end(xcb_visualtype_iterator_t i)
{
    while (i.rem > 0)
        xcb_visualtype_next(&i);
    xcb_generic_iterator_t end = {
        .data = i.data,
        .rem = 0,
        .index = i.index,
    };
    return end;
}
int xcb_screen_sizeof(const void *buffer)
{
    if (!buffer)
        return 0;
    const xcb_screen_t *screen = buffer;
    const xcb_depth_t *depth = (const xcb_depth_t *) (screen + 1);
    uint64_t size = sizeof(*screen);
    for (uint8_t i = 0; i < screen->allowed_depths_len; i++) {
        int depthSize = xcb_depth_sizeof(depth);
        if (!depthSize || size + (uint64_t) depthSize > INT_MAX)
            return 0;
        size += (uint64_t) depthSize;
        depth = (const xcb_depth_t *) ((const char *) depth + depthSize);
    }
    return (int) size;
}
void xcb_screen_next(xcb_screen_iterator_t *i)
{
    if (!i || i->rem <= 0)
        return;
    xcb_depth_iterator_t depths = xcb_screen_allowed_depths_iterator(i->data);
    xcb_generic_iterator_t end = xcb_depth_end(depths);
    size_t bytes = (char *) end.data - (char *) i->data;
    i->data = (xcb_screen_t *) ((char *) i->data + bytes);
    i->index += bytes;
    i->rem--;
}
xcb_generic_iterator_t xcb_screen_end(xcb_screen_iterator_t i)
{
    while (i.rem > 0)
        xcb_screen_next(&i);
    xcb_generic_iterator_t end = {
        .data = i.data,
        .rem = 0,
        .index = i.index,
    };
    return end;
}
int xcb_setup_sizeof(const void *buffer)
{
    if (!buffer)
        return 0;
    const xcb_setup_t *setup = buffer;
    uint64_t vendorSize = ((uint64_t) setup->vendor_len + 3u) & ~UINT64_C(3);
    uint64_t size = sizeof(*setup) + vendorSize +
                    (uint64_t) setup->pixmap_formats_len * sizeof(xcb_format_t);
    if (size > INT_MAX)
        return 0;
    const xcb_screen_t *screen =
        (const xcb_screen_t *) ((const char *) setup + (size_t) size);
    for (uint8_t i = 0; i < setup->roots_len; i++) {
        int screenSize = xcb_screen_sizeof(screen);
        if (!screenSize || size + (uint64_t) screenSize > INT_MAX)
            return 0;
        size += (uint64_t) screenSize;
        screen = (const xcb_screen_t *) ((const char *) screen + screenSize);
    }
    return (int) size;
}
int xcb_flush(xcb_connection_t *c)
{
    if (!c || c->error)
        return 0;
    XFlush(c->display);
    return 1;
}

Display *xcbCompatDisplay(xcb_connection_t *c)
{
    return c ? c->display : NULL;
}

xcb_void_cookie_t xcbCompatVoidCookie(xcb_connection_t *c,
                                      uint8_t errorCode,
                                      uint32_t resource,
                                      uint8_t opcode,
                                      int checked)
{
    if (!xcbCompatRequestReady(c))
        return (xcb_void_cookie_t) {.sequence = 0};
    uint64_t sequence = xcbCompatNextSequence(c);
    xcb_void_cookie_t cookie = {.sequence = (unsigned int) sequence};
    if (errorCode)
        xcbCompatStoreProtocolError(c, sequence, errorCode, resource, opcode,
                                    checked);
    else if (checked)
        xcbCompatStorePending(c, sequence, NULL, NULL);
    return cookie;
}

void xcbCompatStoreProtocolError(xcb_connection_t *c,
                                 uint64_t sequence,
                                 uint8_t code,
                                 uint32_t resource,
                                 uint8_t opcode,
                                 int checked)
{
    xcb_generic_error_t *error = calloc(1, sizeof(*error));
    if (!error) {
        xcbCompatSetConnectionError(c, XCB_CONN_CLOSED_MEM_INSUFFICIENT);
        return;
    }
    error->error_code = code;
    error->sequence = sequence;
    error->full_sequence = sequence;
    error->resource_id = resource;
    error->major_code = opcode;
    if (checked) {
        xcbCompatStorePending(c, sequence, NULL, error);
    } else {
        QueuedError *queued = calloc(1, sizeof(*queued));
        if (!queued) {
            free(error);
            xcbCompatSetConnectionError(c, XCB_CONN_CLOSED_MEM_INSUFFICIENT);
        } else {
            queued->error = error;
            pthread_mutex_lock(&c->mutex);
            if (c->errorTail)
                c->errorTail->next = queued;
            else
                c->errorHead = queued;
            c->errorTail = queued;
            pthread_cond_broadcast(&c->eventCond);
            pthread_mutex_unlock(&c->mutex);
        }
    }
}

uint64_t xcbCompatNextSequence(xcb_connection_t *c)
{
    pthread_mutex_lock(&c->mutex);
    uint64_t sequence = c->nextSequence++;
    pthread_mutex_unlock(&c->mutex);
    return sequence;
}
void xcbCompatSetNextSequence(xcb_connection_t *c, uint64_t sequence)
{
    pthread_mutex_lock(&c->mutex);
    c->nextSequence = sequence;
    pthread_mutex_unlock(&c->mutex);
}
static size_t pendingBucket(uint64_t sequence, size_t bucketCount)
{
    return (size_t) (sequence ^ (sequence >> 32)) & (bucketCount - 1);
}

static PendingReply **findPending(xcb_connection_t *c, uint64_t sequence)
{
    PendingReply **link =
        &c->pendingBuckets[pendingBucket(sequence, c->pendingBucketCount)];
    while (*link && (*link)->sequence != sequence)
        link = &(*link)->next;
    return link;
}

static int growPendingTable(xcb_connection_t *c)
{
    size_t bucketCount = c->pendingBucketCount ? c->pendingBucketCount * 2 : 64;
    if (bucketCount < c->pendingBucketCount ||
        bucketCount > SIZE_MAX / sizeof(*c->pendingBuckets))
        return 0;
    PendingReply **buckets = calloc(bucketCount, sizeof(*buckets));
    if (!buckets)
        return 0;
    for (size_t i = 0; i < c->pendingBucketCount; i++) {
        PendingReply *pending = c->pendingBuckets[i];
        while (pending) {
            PendingReply *next = pending->next;
            size_t bucket = pendingBucket(pending->sequence, bucketCount);
            pending->next = buckets[bucket];
            buckets[bucket] = pending;
            pending = next;
        }
    }
    free(c->pendingBuckets);
    c->pendingBuckets = buckets;
    c->pendingBucketCount = bucketCount;
    return 1;
}

static uint64_t widenSequence(xcb_connection_t *c, uint32_t sequence)
{
    uint64_t last = c->nextSequence - 1;
    uint64_t widened = (last & ~((uint64_t) UINT32_MAX)) | sequence;
    if (widened > last && widened >= ((uint64_t) 1 << 32))
        widened -= (uint64_t) 1 << 32;
    return widened;
}
void xcbCompatStorePending(xcb_connection_t *c,
                           uint64_t sequence,
                           void *reply,
                           xcb_generic_error_t *error)
{
    PendingReply *pending = calloc(1, sizeof(*pending));
    if (!pending) {
        free(reply);
        free(error);
        pthread_mutex_lock(&c->mutex);
        c->error = XCB_CONN_CLOSED_MEM_INSUFFICIENT;
        pthread_mutex_unlock(&c->mutex);
        return;
    }
    pending->sequence = sequence;
    pending->reply = reply;
    pending->error = error;
    pthread_mutex_lock(&c->mutex);
    if (!c->pendingBucketCount && !growPendingTable(c)) {
        c->error = XCB_CONN_CLOSED_MEM_INSUFFICIENT;
        pthread_mutex_unlock(&c->mutex);
        free(pending->reply);
        free(pending->error);
        free(pending);
        return;
    }
    if (c->pendingCount >= c->pendingBucketCount - c->pendingBucketCount / 4)
        growPendingTable(c);
    PendingReply **link = findPending(c, sequence);
    if (*link) {
        PendingReply *old = *link;
        pending->next = old->next;
        *link = pending;
        free(old->reply);
        free(old->error);
        free(old);
    } else {
        *link = pending;
        c->pendingCount++;
    }
    pthread_mutex_unlock(&c->mutex);
}
xcb_generic_error_t *xcb_request_check(xcb_connection_t *c,
                                       xcb_void_cookie_t cookie)
{
    if (!c)
        return NULL;
    pthread_mutex_lock(&c->mutex);
    if (!c->pendingBucketCount) {
        pthread_mutex_unlock(&c->mutex);
        return NULL;
    }
    PendingReply **link = findPending(c, widenSequence(c, cookie.sequence));
    if (!*link) {
        pthread_mutex_unlock(&c->mutex);
        return NULL;
    }
    PendingReply *pending = *link;
    *link = pending->next;
    c->pendingCount--;
    pthread_mutex_unlock(&c->mutex);
    xcb_generic_error_t *error = pending->error;
    free(pending->reply);
    free(pending);
    return error;
}

void *xcbCompatTakeReply(xcb_connection_t *c,
                         uint64_t sequence,
                         xcb_generic_error_t **error)
{
    if (error)
        *error = NULL;
    if (!c)
        return NULL;
    pthread_mutex_lock(&c->mutex);
    if (!c->pendingBucketCount) {
        pthread_mutex_unlock(&c->mutex);
        return NULL;
    }
    PendingReply **link = findPending(c, sequence);
    PendingReply *pending = *link;
    if (pending) {
        *link = pending->next;
        c->pendingCount--;
    }
    pthread_mutex_unlock(&c->mutex);
    if (!pending)
        return NULL;
    void *reply = pending->reply;
    if (error)
        *error = pending->error;
    else
        free(pending->error);
    free(pending);
    return reply;
}
static void discardReply(xcb_connection_t *c, uint64_t sequence)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->mutex);
    if (!c->pendingBucketCount) {
        pthread_mutex_unlock(&c->mutex);
        return;
    }
    PendingReply **link = findPending(c, sequence);
    PendingReply *pending = *link;
    if (pending) {
        *link = pending->next;
        c->pendingCount--;
    }
    pthread_mutex_unlock(&c->mutex);
    if (pending) {
        free(pending->reply);
        free(pending->error);
        free(pending);
    }
}
void xcb_discard_reply(xcb_connection_t *c, unsigned int sequence)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->mutex);
    uint64_t widened = widenSequence(c, sequence);
    pthread_mutex_unlock(&c->mutex);
    discardReply(c, widened);
}
void xcb_discard_reply64(xcb_connection_t *c, uint64_t sequence)
{
    discardReply(c, sequence);
}
void xcb_disconnect(xcb_connection_t *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->mutex);
    c->closing = 1;
    c->error = XCB_CONN_ERROR;
    while (c->eventUsers)
        pthread_cond_wait(&c->eventCond, &c->mutex);
    pthread_mutex_unlock(&c->mutex);

    unregisterConnection(c);
    pthread_mutex_lock(&c->mutex);
    while (c->closeCallbacks)
        pthread_cond_wait(&c->eventCond, &c->mutex);
    Display *display = c->display;
    int ownsDisplay = c->ownsDisplay;
    c->display = NULL;
    c->ownsDisplay = 0;
    pthread_mutex_unlock(&c->mutex);

    xcbCompatReleaseRequestResources(c);
    if (display && ownsDisplay)
        XCloseDisplay(display);
    pthread_mutex_lock(&c->mutex);
    drainPendingLocked(c);
    pthread_mutex_unlock(&c->mutex);
    free(c->pendingBuckets);
    pthread_cond_destroy(&c->eventCond);
    pthread_mutex_destroy(&c->mutex);
    free(c);
}
