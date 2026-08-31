#include <pthread.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <xcb/xcb.h>
#include "xcb-compat-private.h"
#include "../src/atoms.h"
#include "../src/drawing.h"
#include "../src/gc.h"
#include "../src/resource-types.h"
#include "../src/window.h"

typedef struct GcEntry {
    xcb_gcontext_t id;
    GC gc;
    Display *display;
    xcb_connection_t *owner;
    unsigned int depth;
    struct GcEntry *next;
} GcEntry;
static GcEntry *gcEntries;
static pthread_mutex_t gcMutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int failNextReplyAllocation;

void xcbCompatFailNextReplyAllocationForTest(void)
{
    atomic_store_explicit(&failNextReplyAllocation, 1, memory_order_release);
}

static GcEntry *findGcLocked(xcb_gcontext_t id)
{
    for (GcEntry *entry = gcEntries; entry; entry = entry->next)
        if (entry->id == id)
            return entry;
    return NULL;
}

void xcbCompatReleaseRequestResources(xcb_connection_t *connection)
{
    pthread_mutex_lock(&gcMutex);
    GcEntry **link = &gcEntries;
    while (*link) {
        GcEntry *entry = *link;
        if (entry->owner != connection) {
            link = &entry->next;
            continue;
        }
        *link = entry->next;
        XFreeGC(entry->display, entry->gc);
        free(entry);
    }
    pthread_mutex_unlock(&gcMutex);
}

static int isDrawable(xcb_drawable_t drawable)
{
    return IS_TYPE(drawable, DRAWABLE);
}

/* InputOnly windows are drawables the protocol refuses to render into, read
 * from or copy between: they carry no pixels. Xlib reports that as BadMatch,
 * and so must every request that reaches a drawing path.
 */
static int isInputOnlyDrawable(xcb_drawable_t drawable)
{
    return IS_TYPE(drawable, WINDOW) && IS_INPUT_ONLY(drawable);
}

static unsigned int drawableDepth(xcb_connection_t *c, xcb_drawable_t drawable)
{
    if (IS_TYPE(drawable, PIXMAP))
        return GET_PIXMAP_STRUCT(drawable)->depth;
    WindowStruct *window = GET_WINDOW_STRUCT(drawable);
    return window->depth == XCB_COPY_FROM_PARENT
               ? (unsigned int) DefaultDepth(xcbCompatDisplay(c),
                                             DefaultScreen(xcbCompatDisplay(c)))
               : (unsigned int) window->depth;
}

static unsigned int nextSequence(xcb_connection_t *c)
{
    return (unsigned int) xcbCompatNextSequence(c);
}

#define REQUIRE_REQUEST(connection, cookieType)  \
    do {                                         \
        if (!xcbCompatRequestReady(connection))  \
            return (cookieType) {.sequence = 0}; \
    } while (0)

static void storeReply(xcb_connection_t *c, unsigned int sequence, void *reply)
{
    if (!reply) {
        xcbCompatSetConnectionError(c, XCB_CONN_CLOSED_MEM_INSUFFICIENT);
        return;
    }
    ((xcb_generic_reply_t *) reply)->sequence = sequence;
    xcbCompatStorePending(c, sequence, reply, NULL);
}

static void *allocateReplyPayload(xcb_connection_t *c,
                                  size_t headerSize,
                                  uint64_t count,
                                  size_t itemSize,
                                  size_t *payloadSize)
{
    if (atomic_exchange_explicit(&failNextReplyAllocation, 0,
                                 memory_order_acq_rel)) {
        xcbCompatSetConnectionError(c, XCB_CONN_CLOSED_MEM_INSUFFICIENT);
        return NULL;
    }
    if (itemSize && count > (SIZE_MAX - headerSize) / itemSize) {
        xcbCompatSetConnectionError(c, XCB_CONN_CLOSED_MEM_INSUFFICIENT);
        return NULL;
    }
    size_t payload = (size_t) count * itemSize;
    void *reply = calloc(1, headerSize + payload);
    if (!reply)
        xcbCompatSetConnectionError(c, XCB_CONN_CLOSED_MEM_INSUFFICIENT);
    if (payloadSize)
        *payloadSize = payload;
    return reply;
}

static int requestPayloadFits(xcb_connection_t *c,
                              size_t headerSize,
                              uint64_t count,
                              size_t itemSize)
{
    uint64_t payload = count * itemSize;
    if (itemSize && payload / itemSize != count)
        return 0;
    if (payload > UINT64_MAX - 3u)
        return 0;
    uint64_t paddedPayload = (payload + 3u) & ~UINT64_C(3);
    uint64_t maximumBytes = (uint64_t) xcb_get_maximum_request_length(c) * 4u;
    return headerSize <= maximumBytes &&
           paddedPayload <= maximumBytes - headerSize;
}

static int isWindow(xcb_window_t window)
{
    return IS_TYPE(window, WINDOW);
}

#define XCB_CW_VALID_MASK                                                  \
    (XCB_CW_BACK_PIXMAP | XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXMAP |       \
     XCB_CW_BORDER_PIXEL | XCB_CW_BIT_GRAVITY | XCB_CW_WIN_GRAVITY |       \
     XCB_CW_BACKING_STORE | XCB_CW_BACKING_PLANES | XCB_CW_BACKING_PIXEL | \
     XCB_CW_OVERRIDE_REDIRECT | XCB_CW_SAVE_UNDER | XCB_CW_EVENT_MASK |    \
     XCB_CW_DONT_PROPAGATE | XCB_CW_COLORMAP | XCB_CW_CURSOR)

#define XCB_CONFIG_WINDOW_VALID_MASK                                       \
    (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | \
     XCB_CONFIG_WINDOW_HEIGHT | XCB_CONFIG_WINDOW_BORDER_WIDTH |           \
     XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE)

#define XCB_GC_VALID_MASK                                              \
    (XCB_GC_FUNCTION | XCB_GC_PLANE_MASK | XCB_GC_FOREGROUND |         \
     XCB_GC_BACKGROUND | XCB_GC_LINE_WIDTH | XCB_GC_LINE_STYLE |       \
     XCB_GC_CAP_STYLE | XCB_GC_JOIN_STYLE | XCB_GC_FILL_STYLE |        \
     XCB_GC_FILL_RULE | XCB_GC_TILE | XCB_GC_STIPPLE |                 \
     XCB_GC_TILE_STIPPLE_ORIGIN_X | XCB_GC_TILE_STIPPLE_ORIGIN_Y |     \
     XCB_GC_FONT | XCB_GC_SUBWINDOW_MODE | XCB_GC_GRAPHICS_EXPOSURES | \
     XCB_GC_CLIP_ORIGIN_X | XCB_GC_CLIP_ORIGIN_Y | XCB_GC_CLIP_MASK |  \
     XCB_GC_DASH_OFFSET | XCB_GC_DASH_LIST | XCB_GC_ARC_MODE)

xcb_intern_atom_cookie_t xcb_intern_atom(xcb_connection_t *c,
                                         uint8_t onlyIfExists,
                                         uint16_t nameLength,
                                         const char *name)
{
    REQUIRE_REQUEST(c, xcb_intern_atom_cookie_t);
    xcb_intern_atom_cookie_t cookie = {nextSequence(c)};
    char *copy = malloc((size_t) nameLength + 1);
    xcb_intern_atom_reply_t *reply = calloc(1, sizeof(*reply));
    if (copy && reply) {
        memcpy(copy, name, nameLength);
        copy[nameLength] = '\0';
        reply->response_type = 1;
        reply->atom = XInternAtom(xcbCompatDisplay(c), copy, onlyIfExists);
        storeReply(c, cookie.sequence, reply);
    } else {
        free(reply);
        xcbCompatSetConnectionError(c, XCB_CONN_CLOSED_MEM_INSUFFICIENT);
    }
    free(copy);
    return cookie;
}

xcb_intern_atom_cookie_t xcb_intern_atom_unchecked(xcb_connection_t *c,
                                                   uint8_t onlyIfExists,
                                                   uint16_t nameLength,
                                                   const char *name)
{
    return xcb_intern_atom(c, onlyIfExists, nameLength, name);
}

xcb_intern_atom_reply_t *xcb_intern_atom_reply(xcb_connection_t *c,
                                               xcb_intern_atom_cookie_t cookie,
                                               xcb_generic_error_t **error)
{
    return xcbCompatTakeReply(c, cookie.sequence, error);
}

static xcb_get_atom_name_cookie_t xcbGetAtomName(xcb_connection_t *c,
                                                 xcb_atom_t atom,
                                                 int checked)
{
    REQUIRE_REQUEST(c, xcb_get_atom_name_cookie_t);
    xcb_get_atom_name_cookie_t cookie = {nextSequence(c)};
    if (!isValidAtom(atom)) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_ATOM, atom,
                                    XCB_GET_ATOM_NAME, checked);
        return cookie;
    }
    char *name = XGetAtomName(xcbCompatDisplay(c), atom);
    size_t length = name ? strlen(name) : 0;
    size_t paddedLength = length <= SIZE_MAX - 3 ? (length + 3u) & ~3u : 0;
    xcb_get_atom_name_reply_t *reply =
        name && paddedLength >= length
            ? allocateReplyPayload(c, sizeof(*reply), paddedLength, 1, NULL)
            : NULL;
    if (reply) {
        reply->response_type = 1;
        reply->name_len = length;
        reply->length = (length + 3) / 4;
        memcpy(reply + 1, name, length);
    }
    free(name);
    storeReply(c, cookie.sequence, reply);
    return cookie;
}

xcb_get_atom_name_cookie_t xcb_get_atom_name(xcb_connection_t *c,
                                             xcb_atom_t atom)
{
    return xcbGetAtomName(c, atom, 1);
}

xcb_get_atom_name_cookie_t xcb_get_atom_name_unchecked(xcb_connection_t *c,
                                                       xcb_atom_t atom)
{
    return xcbGetAtomName(c, atom, 0);
}

xcb_get_atom_name_reply_t *xcb_get_atom_name_reply(
    xcb_connection_t *c,
    xcb_get_atom_name_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return xcbCompatTakeReply(c, cookie.sequence, error);
}
int xcb_get_atom_name_sizeof(const void *buffer)
{
    const xcb_get_atom_name_reply_t *reply = buffer;
    return reply ? (int) (sizeof(*reply) + reply->name_len) : 0;
}
char *xcb_get_atom_name_name(const xcb_get_atom_name_reply_t *reply)
{
    return reply ? (char *) (reply + 1) : NULL;
}
int xcb_get_atom_name_name_length(const xcb_get_atom_name_reply_t *reply)
{
    return reply ? reply->name_len : 0;
}
xcb_generic_iterator_t xcb_get_atom_name_name_end(
    const xcb_get_atom_name_reply_t *reply)
{
    xcb_generic_iterator_t end = {0};
    if (reply) {
        end.data = xcb_get_atom_name_name(reply) + reply->name_len;
        end.index = sizeof(*reply) + reply->name_len;
    }
    return end;
}

static void decodeAttributes(uint32_t mask,
                             const uint32_t *values,
                             XSetWindowAttributes *a)
{
    memset(a, 0, sizeof(*a));
    unsigned int n = 0;
#define TAKE(bit, field)            \
    do {                            \
        if (mask & (bit))           \
            a->field = values[n++]; \
    } while (0)
    TAKE(XCB_CW_BACK_PIXMAP, background_pixmap);
    TAKE(XCB_CW_BACK_PIXEL, background_pixel);
    TAKE(XCB_CW_BORDER_PIXMAP, border_pixmap);
    TAKE(XCB_CW_BORDER_PIXEL, border_pixel);
    TAKE(XCB_CW_BIT_GRAVITY, bit_gravity);
    TAKE(XCB_CW_WIN_GRAVITY, win_gravity);
    TAKE(XCB_CW_BACKING_STORE, backing_store);
    TAKE(XCB_CW_BACKING_PLANES, backing_planes);
    TAKE(XCB_CW_BACKING_PIXEL, backing_pixel);
    TAKE(XCB_CW_OVERRIDE_REDIRECT, override_redirect);
    TAKE(XCB_CW_SAVE_UNDER, save_under);
    TAKE(XCB_CW_EVENT_MASK, event_mask);
    TAKE(XCB_CW_DONT_PROPAGATE, do_not_propagate_mask);
    TAKE(XCB_CW_COLORMAP, colormap);
    TAKE(XCB_CW_CURSOR, cursor);
#undef TAKE
}

static uint8_t validateAttributes(uint32_t mask,
                                  const XSetWindowAttributes *attributes,
                                  unsigned int windowDepth,
                                  int inputOnly,
                                  uint32_t *errorValue)
{
    const uint32_t inputOnlyMask = XCB_CW_WIN_GRAVITY | XCB_CW_EVENT_MASK |
                                   XCB_CW_DONT_PROPAGATE |
                                   XCB_CW_OVERRIDE_REDIRECT | XCB_CW_CURSOR;
    if (inputOnly && (mask & ~inputOnlyMask)) {
        *errorValue = mask & ~inputOnlyMask;
        return XCB_MATCH;
    }
    if ((mask & XCB_CW_BIT_GRAVITY) &&
        attributes->bit_gravity > XCB_GRAVITY_STATIC) {
        *errorValue = attributes->bit_gravity;
        return XCB_VALUE;
    }
    if ((mask & XCB_CW_WIN_GRAVITY) &&
        attributes->win_gravity > XCB_GRAVITY_STATIC) {
        *errorValue = attributes->win_gravity;
        return XCB_VALUE;
    }
    if ((mask & XCB_CW_BACKING_STORE) &&
        attributes->backing_store > XCB_BACKING_STORE_ALWAYS) {
        *errorValue = attributes->backing_store;
        return XCB_VALUE;
    }
    if ((mask & XCB_CW_OVERRIDE_REDIRECT) &&
        attributes->override_redirect != False &&
        attributes->override_redirect != True) {
        *errorValue = attributes->override_redirect;
        return XCB_VALUE;
    }
    if ((mask & XCB_CW_SAVE_UNDER) && attributes->save_under != False &&
        attributes->save_under != True) {
        *errorValue = attributes->save_under;
        return XCB_VALUE;
    }
    const uint32_t validEventMask = (XCB_EVENT_MASK_OWNER_GRAB_BUTTON << 1) - 1;
    if ((mask & XCB_CW_EVENT_MASK) &&
        ((uint32_t) attributes->event_mask & ~validEventMask)) {
        *errorValue = attributes->event_mask;
        return XCB_VALUE;
    }
    const uint32_t validDoNotPropagate =
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
        XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_1_MOTION |
        XCB_EVENT_MASK_BUTTON_2_MOTION | XCB_EVENT_MASK_BUTTON_3_MOTION |
        XCB_EVENT_MASK_BUTTON_4_MOTION | XCB_EVENT_MASK_BUTTON_5_MOTION |
        XCB_EVENT_MASK_BUTTON_MOTION;
    if ((mask & XCB_CW_DONT_PROPAGATE) &&
        ((uint32_t) attributes->do_not_propagate_mask & ~validDoNotPropagate)) {
        *errorValue = attributes->do_not_propagate_mask;
        return XCB_VALUE;
    }
    const struct {
        uint32_t bit;
        Pixmap pixmap;
        int parentRelativeAllowed;
    } pixmaps[] = {
        {XCB_CW_BACK_PIXMAP, attributes->background_pixmap, 1},
        {XCB_CW_BORDER_PIXMAP, attributes->border_pixmap, 0},
    };
    for (size_t i = 0; i < sizeof(pixmaps) / sizeof(pixmaps[0]); i++) {
        if (!(mask & pixmaps[i].bit) || pixmaps[i].pixmap == XCB_NONE ||
            (pixmaps[i].parentRelativeAllowed &&
             pixmaps[i].pixmap == XCB_BACK_PIXMAP_PARENT_RELATIVE))
            continue;
        *errorValue = pixmaps[i].pixmap;
        if (!IS_TYPE(pixmaps[i].pixmap, PIXMAP))
            return XCB_PIXMAP;
        if (GET_PIXMAP_STRUCT(pixmaps[i].pixmap)->depth != windowDepth)
            return XCB_MATCH;
    }
    if ((mask & XCB_CW_COLORMAP) && attributes->colormap != XCB_NONE &&
        !IS_TYPE(attributes->colormap, COLORMAP)) {
        *errorValue = attributes->colormap;
        return XCB_COLORMAP;
    }
    if ((mask & XCB_CW_CURSOR) && attributes->cursor != XCB_NONE &&
        !IS_TYPE(attributes->cursor, CURSOR)) {
        *errorValue = attributes->cursor;
        return XCB_CURSOR;
    }
    return 0;
}

static xcb_void_cookie_t createWindow(xcb_connection_t *c,
                                      uint8_t depth,
                                      xcb_window_t wid,
                                      xcb_window_t parent,
                                      int16_t x,
                                      int16_t y,
                                      uint16_t width,
                                      uint16_t height,
                                      uint16_t borderWidth,
                                      uint16_t clazz,
                                      xcb_visualid_t visual,
                                      uint32_t mask,
                                      const uint32_t *values,
                                      int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = 0;
    uint32_t errorValue = wid;
    if (!isWindow(parent)) {
        error = XCB_WINDOW;
        errorValue = parent;
    }
    if (!error && getXidStruct(wid)->type != 0)
        error = XCB_ID_CHOICE;
    if (!error && (mask & ~XCB_CW_VALID_MASK)) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    if (!error && mask && !values) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    if (!error && clazz > XCB_WINDOW_CLASS_INPUT_ONLY) {
        error = XCB_VALUE;
        errorValue = clazz;
    }
    if (!error && !width) {
        error = XCB_VALUE;
        errorValue = width;
    }
    if (!error && !height) {
        error = XCB_VALUE;
        errorValue = height;
    }
    int parentInputOnly = !error && IS_INPUT_ONLY(parent);
    int inputOnly =
        clazz == XCB_WINDOW_CLASS_INPUT_ONLY ||
        (clazz == XCB_WINDOW_CLASS_COPY_FROM_PARENT && parentInputOnly);
    if (!error && clazz == XCB_WINDOW_CLASS_INPUT_OUTPUT && parentInputOnly) {
        error = XCB_MATCH;
        errorValue = parent;
    }
    if (!error && inputOnly && borderWidth != 0) {
        error = XCB_MATCH;
        errorValue = borderWidth;
    }
    unsigned int defaultDepth = 0;
    xcb_visualid_t defaultVisual = XCB_NONE;
    if (!error) {
        defaultDepth = (unsigned int) DefaultDepth(
            xcbCompatDisplay(c), DefaultScreen(xcbCompatDisplay(c)));
        defaultVisual = XVisualIDFromVisual(DefaultVisual(
            xcbCompatDisplay(c), DefaultScreen(xcbCompatDisplay(c))));
    }
    if (!error && inputOnly && depth != XCB_COPY_FROM_PARENT) {
        error = XCB_MATCH;
        errorValue = depth;
    }
    if (!error && inputOnly && visual != XCB_COPY_FROM_PARENT) {
        error = XCB_MATCH;
        errorValue = visual;
    }
    if (!error && !inputOnly && depth != XCB_COPY_FROM_PARENT &&
        depth != defaultDepth) {
        error = XCB_MATCH;
        errorValue = depth;
    }
    if (!error && !inputOnly && visual != XCB_COPY_FROM_PARENT &&
        visual != defaultVisual) {
        error = XCB_MATCH;
        errorValue = visual;
    }
    if (!error) {
        XSetWindowAttributes attributes;
        decodeAttributes(mask, values, &attributes);
        unsigned int windowDepth =
            depth == XCB_COPY_FROM_PARENT ? drawableDepth(c, parent) : depth;
        error = validateAttributes(mask, &attributes, windowDepth, inputOnly,
                                   &errorValue);
        if (error)
            goto done;
        Visual *xvisual = visual == XCB_COPY_FROM_PARENT
                              ? CopyFromParent
                              : DefaultVisual(xcbCompatDisplay(c), 0);

        /* Claim a client-chosen id last: every check above can still fail, and
         * an id reserved before them would have to be handed back. From here
         * the creator owns it and releases it itself if it fails.
         */
        if (!isXidAllocated(wid) && !reserveXidResource(wid))
            error = XCB_ID_CHOICE;
        else if (libx11CompatCreateWindowWithId(
                     xcbCompatDisplay(c), wid, parent, x, y, width, height,
                     borderWidth, depth, clazz, xvisual, mask,
                     &attributes) != wid)
            error = XCB_VALUE;
    }
done:
    return xcbCompatVoidCookie(c, error, errorValue, XCB_CREATE_WINDOW,
                               checked);
}

xcb_void_cookie_t xcb_create_window_checked(xcb_connection_t *c,
                                            uint8_t depth,
                                            xcb_window_t wid,
                                            xcb_window_t parent,
                                            int16_t x,
                                            int16_t y,
                                            uint16_t width,
                                            uint16_t height,
                                            uint16_t borderWidth,
                                            uint16_t clazz,
                                            xcb_visualid_t visual,
                                            uint32_t mask,
                                            const void *values)
{
    return createWindow(c, depth, wid, parent, x, y, width, height, borderWidth,
                        clazz, visual, mask, values, 1);
}
xcb_void_cookie_t xcb_create_window(xcb_connection_t *c,
                                    uint8_t depth,
                                    xcb_window_t wid,
                                    xcb_window_t parent,
                                    int16_t x,
                                    int16_t y,
                                    uint16_t width,
                                    uint16_t height,
                                    uint16_t borderWidth,
                                    uint16_t clazz,
                                    xcb_visualid_t visual,
                                    uint32_t mask,
                                    const void *values)
{
    return createWindow(c, depth, wid, parent, x, y, width, height, borderWidth,
                        clazz, visual, mask, values, 0);
}

#define WINDOW_VOID(name, checked_name, xfunc, opcode)                       \
    static xcb_void_cookie_t name##Impl(xcb_connection_t *c, xcb_window_t w, \
                                        int checked)                         \
    {                                                                        \
        REQUIRE_REQUEST(c, xcb_void_cookie_t);                               \
        uint8_t e = isWindow(w) ? 0 : XCB_WINDOW;                            \
        if (!e)                                                              \
            xfunc(xcbCompatDisplay(c), w);                                   \
        return xcbCompatVoidCookie(c, e, w, opcode, checked);                \
    }                                                                        \
    xcb_void_cookie_t name(xcb_connection_t *c, xcb_window_t w)              \
    {                                                                        \
        return name##Impl(c, w, 0);                                          \
    }                                                                        \
    xcb_void_cookie_t checked_name(xcb_connection_t *c, xcb_window_t w)      \
    {                                                                        \
        return name##Impl(c, w, 1);                                          \
    }
WINDOW_VOID(xcb_map_window, xcb_map_window_checked, XMapWindow, XCB_MAP_WINDOW)
WINDOW_VOID(xcb_unmap_window,
            xcb_unmap_window_checked,
            XUnmapWindow,
            XCB_UNMAP_WINDOW)
WINDOW_VOID(xcb_destroy_window,
            xcb_destroy_window_checked,
            XDestroyWindow,
            XCB_DESTROY_WINDOW)

static xcb_get_window_attributes_cookie_t
getWindowAttributes(xcb_connection_t *c, xcb_window_t w, int checked)
{
    REQUIRE_REQUEST(c, xcb_get_window_attributes_cookie_t);
    xcb_get_window_attributes_cookie_t cookie = {nextSequence(c)};
    if (!isWindow(w)) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_WINDOW, w,
                                    XCB_GET_WINDOW_ATTRIBUTES, checked);
        return cookie;
    }
    xcb_get_window_attributes_reply_t *reply = calloc(1, sizeof(*reply));
    XWindowAttributes a;
    if (reply && XGetWindowAttributes(xcbCompatDisplay(c), w, &a)) {
        reply->response_type = 1;
        reply->visual = XVisualIDFromVisual(a.visual);
        reply->_class = a.class;
        reply->bit_gravity = a.bit_gravity;
        reply->win_gravity = a.win_gravity;
        reply->backing_store = a.backing_store;
        reply->backing_planes = a.backing_planes;
        reply->backing_pixel = a.backing_pixel;
        reply->save_under = a.save_under;
        reply->map_is_installed = a.map_installed;
        reply->map_state = a.map_state;
        reply->override_redirect = a.override_redirect;
        reply->colormap = a.colormap;
        reply->all_event_masks = a.all_event_masks;
        reply->your_event_mask = a.your_event_mask;
        reply->do_not_propagate_mask = a.do_not_propagate_mask;
    } else {
        free(reply);
        reply = NULL;
    }
    storeReply(c, cookie.sequence, reply);
    return cookie;
}
xcb_get_window_attributes_cookie_t xcb_get_window_attributes(
    xcb_connection_t *c,
    xcb_window_t w)
{
    return getWindowAttributes(c, w, 1);
}
xcb_get_window_attributes_cookie_t xcb_get_window_attributes_unchecked(
    xcb_connection_t *c,
    xcb_window_t w)
{
    return getWindowAttributes(c, w, 0);
}
xcb_get_window_attributes_reply_t *xcb_get_window_attributes_reply(
    xcb_connection_t *c,
    xcb_get_window_attributes_cookie_t cookie,
    xcb_generic_error_t **e)
{
    return xcbCompatTakeReply(c, cookie.sequence, e);
}

static xcb_get_geometry_cookie_t getGeometry(xcb_connection_t *c,
                                             xcb_drawable_t d,
                                             int checked)
{
    REQUIRE_REQUEST(c, xcb_get_geometry_cookie_t);
    xcb_get_geometry_cookie_t cookie = {nextSequence(c)};
    if (!isDrawable(d)) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_DRAWABLE, d,
                                    XCB_GET_GEOMETRY, checked);
        return cookie;
    }
    Window root;
    int x, y;
    unsigned int width, height, border, depth;
    xcb_get_geometry_reply_t *reply = calloc(1, sizeof(*reply));
    if (reply && XGetGeometry(xcbCompatDisplay(c), d, &root, &x, &y, &width,
                              &height, &border, &depth)) {
        reply->response_type = 1;
        reply->root = root;
        reply->x = x;
        reply->y = y;
        reply->width = width;
        reply->height = height;
        reply->border_width = border;
        reply->depth = depth;
    } else {
        free(reply);
        reply = NULL;
    }
    storeReply(c, cookie.sequence, reply);
    return cookie;
}
xcb_get_geometry_cookie_t xcb_get_geometry(xcb_connection_t *c,
                                           xcb_drawable_t d)
{
    return getGeometry(c, d, 1);
}
xcb_get_geometry_cookie_t xcb_get_geometry_unchecked(xcb_connection_t *c,
                                                     xcb_drawable_t d)
{
    return getGeometry(c, d, 0);
}
xcb_get_geometry_reply_t *xcb_get_geometry_reply(
    xcb_connection_t *c,
    xcb_get_geometry_cookie_t cookie,
    xcb_generic_error_t **e)
{
    return xcbCompatTakeReply(c, cookie.sequence, e);
}

static xcb_query_tree_cookie_t queryTree(xcb_connection_t *c,
                                         xcb_window_t w,
                                         int checked)
{
    REQUIRE_REQUEST(c, xcb_query_tree_cookie_t);
    xcb_query_tree_cookie_t cookie = {nextSequence(c)};
    if (!isWindow(w)) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_WINDOW, w,
                                    XCB_QUERY_TREE, checked);
        return cookie;
    }
    Window root, parent, *children = NULL;
    unsigned int count = 0;
    int ok =
        XQueryTree(xcbCompatDisplay(c), w, &root, &parent, &children, &count);
    xcb_query_tree_reply_t *reply =
        ok ? allocateReplyPayload(c, sizeof(*reply), count,
                                  sizeof(xcb_window_t), NULL)
           : NULL;
    if (reply) {
        reply->response_type = 1;
        reply->root = root;
        reply->parent = parent;
        reply->children_len = count;
        reply->length = count;
        for (unsigned int i = 0; i < count; i++)
            ((xcb_window_t *) (reply + 1))[i] = children[i];
    }
    free(children);
    storeReply(c, cookie.sequence, reply);
    return cookie;
}
xcb_query_tree_cookie_t xcb_query_tree(xcb_connection_t *c, xcb_window_t w)
{
    return queryTree(c, w, 1);
}
xcb_query_tree_cookie_t xcb_query_tree_unchecked(xcb_connection_t *c,
                                                 xcb_window_t w)
{
    return queryTree(c, w, 0);
}
xcb_query_tree_reply_t *xcb_query_tree_reply(xcb_connection_t *c,
                                             xcb_query_tree_cookie_t cookie,
                                             xcb_generic_error_t **e)
{
    return xcbCompatTakeReply(c, cookie.sequence, e);
}
int xcb_query_tree_sizeof(const void *buffer)
{
    const xcb_query_tree_reply_t *reply = buffer;
    return reply ? (int) (sizeof(*reply) +
                          (size_t) reply->children_len * sizeof(xcb_window_t))
                 : 0;
}
xcb_window_t *xcb_query_tree_children(const xcb_query_tree_reply_t *r)
{
    return r ? (xcb_window_t *) (r + 1) : NULL;
}
int xcb_query_tree_children_length(const xcb_query_tree_reply_t *r)
{
    return r ? r->children_len : 0;
}
xcb_generic_iterator_t xcb_query_tree_children_end(
    const xcb_query_tree_reply_t *reply)
{
    xcb_generic_iterator_t end = {0};
    if (reply) {
        end.data = xcb_query_tree_children(reply) + reply->children_len;
        end.index = xcb_query_tree_sizeof(reply);
    }
    return end;
}

static xcb_void_cookie_t changeProperty(xcb_connection_t *c,
                                        uint8_t mode,
                                        xcb_window_t window,
                                        xcb_atom_t property,
                                        xcb_atom_t type,
                                        uint8_t format,
                                        uint32_t length,
                                        const void *data,
                                        int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = 0;
    uint32_t errorValue = window;
    if (!isWindow(window))
        error = XCB_WINDOW;
    if (!error && !isValidAtom(property)) {
        error = XCB_ATOM;
        errorValue = property;
    }
    if (!error && !isValidAtom(type)) {
        error = XCB_ATOM;
        errorValue = type;
    }
    if (!error && mode > XCB_PROP_MODE_APPEND) {
        error = XCB_VALUE;
        errorValue = mode;
    }
    if (!error && format != 8 && format != 16 && format != 32) {
        error = XCB_VALUE;
        errorValue = format;
    }
    if (!error && !requestPayloadFits(c, sizeof(xcb_change_property_request_t),
                                      length, format / 8u)) {
        error = XCB_LENGTH;
        errorValue = length;
    }
    if (!error && length && !data) {
        error = XCB_VALUE;
        errorValue = length;
    }
    unsigned long *wide = NULL;
    const unsigned char *xdata = data;
    if (!error && format == 32 && length) {
        wide = calloc(length, sizeof(*wide));
        if (!wide)
            error = XCB_ALLOC;
        for (uint32_t i = 0; wide && i < length; i++)
            wide[i] = ((const uint32_t *) data)[i];
        xdata = (const unsigned char *) wide;
    }
    if (!error && !XChangeProperty(xcbCompatDisplay(c), window, property, type,
                                   format, mode, xdata, length))
        error = XCB_MATCH;
    free(wide);
    return xcbCompatVoidCookie(c, error, errorValue, XCB_CHANGE_PROPERTY,
                               checked);
}

xcb_void_cookie_t xcb_change_property(xcb_connection_t *c,
                                      uint8_t mode,
                                      xcb_window_t window,
                                      xcb_atom_t property,
                                      xcb_atom_t type,
                                      uint8_t format,
                                      uint32_t length,
                                      const void *data)
{
    return changeProperty(c, mode, window, property, type, format, length, data,
                          0);
}
xcb_void_cookie_t xcb_change_property_checked(xcb_connection_t *c,
                                              uint8_t mode,
                                              xcb_window_t window,
                                              xcb_atom_t property,
                                              xcb_atom_t type,
                                              uint8_t format,
                                              uint32_t length,
                                              const void *data)
{
    return changeProperty(c, mode, window, property, type, format, length, data,
                          1);
}

static xcb_get_property_cookie_t getProperty(xcb_connection_t *c,
                                             uint8_t delete,
                                             xcb_window_t window,
                                             xcb_atom_t property,
                                             xcb_atom_t type,
                                             uint32_t offset,
                                             uint32_t length,
                                             int checked)
{
    REQUIRE_REQUEST(c, xcb_get_property_cookie_t);
    xcb_get_property_cookie_t cookie = {nextSequence(c)};
    if (!isWindow(window)) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_WINDOW, window,
                                    XCB_GET_PROPERTY, checked);
        return cookie;
    }
    if (!isValidAtom(property) ||
        (type != XCB_GET_PROPERTY_TYPE_ANY && !isValidAtom(type))) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_ATOM,
                                    !isValidAtom(property) ? property : type,
                                    XCB_GET_PROPERTY, checked);
        return cookie;
    }
    Atom actualType;
    int format;
    unsigned long count, after;
    unsigned char *value = NULL;
    int status = XGetWindowProperty(xcbCompatDisplay(c), window, property,
                                    offset, length, delete, type, &actualType,
                                    &format, &count, &after, &value);

    /* The wire pads a reply payload to a 4-byte boundary and length counts
     * those words, so an 8- or 16-bit property whose bytes do not divide by
     * four needs the padding allocated too. A client that trusts length and
     * reads length * 4 bytes would otherwise run off the end of the block.
     */
    size_t itemSize = format ? (unsigned int) format / 8 : 0;
    uint64_t valueBytes = (uint64_t) count * itemSize;
    uint64_t paddedBytes =
        valueBytes <= UINT64_MAX - 3 ? (valueBytes + 3u) & ~UINT64_C(3) : 0;
    size_t bytes = (size_t) valueBytes;
    xcb_get_property_reply_t *reply =
        status == Success && paddedBytes >= valueBytes
            ? allocateReplyPayload(c, sizeof(*reply), paddedBytes, 1, NULL)
            : NULL;
    if (reply) {
        reply->response_type = 1;
        reply->format = format;
        reply->type = actualType;
        reply->bytes_after = after;
        reply->value_len = count;
        reply->length = (uint32_t) (paddedBytes / 4);
        if (format == 32) {
            for (unsigned long i = 0; i < count; i++)
                ((uint32_t *) (reply + 1))[i] = ((unsigned long *) value)[i];
        } else if (bytes) {
            memcpy(reply + 1, value, bytes);
        }
    }
    free(value);
    if (status == Success)
        storeReply(c, cookie.sequence, reply);
    else
        xcbCompatStoreProtocolError(c, cookie.sequence, (uint8_t) status,
                                    window, XCB_GET_PROPERTY, checked);
    return cookie;
}
xcb_get_property_cookie_t xcb_get_property(xcb_connection_t *c,
                                           uint8_t delete,
                                           xcb_window_t window,
                                           xcb_atom_t property,
                                           xcb_atom_t type,
                                           uint32_t offset,
                                           uint32_t length)
{
    return getProperty(c, delete, window, property, type, offset, length, 1);
}
xcb_get_property_cookie_t xcb_get_property_unchecked(xcb_connection_t *c,
                                                     uint8_t delete,
                                                     xcb_window_t window,
                                                     xcb_atom_t property,
                                                     xcb_atom_t type,
                                                     uint32_t offset,
                                                     uint32_t length)
{
    return getProperty(c, delete, window, property, type, offset, length, 0);
}
xcb_get_property_reply_t *xcb_get_property_reply(
    xcb_connection_t *c,
    xcb_get_property_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return xcbCompatTakeReply(c, cookie.sequence, error);
}
static int propertyValueBytes(const xcb_get_property_reply_t *reply)
{
    if (!reply)
        return 0;
    uint64_t bytes = (uint64_t) reply->value_len * (reply->format / 8u);
    return bytes <= INT_MAX ? (int) bytes : -1;
}
int xcb_get_property_sizeof(const void *buffer)
{
    const xcb_get_property_reply_t *reply = buffer;
    int bytes = propertyValueBytes(reply);
    if (!reply || bytes < 0)
        return 0;
    uint64_t padded = ((uint64_t) bytes + 3u) & ~UINT64_C(3);
    return padded <= INT_MAX - sizeof(*reply)
               ? (int) sizeof(*reply) + (int) padded
               : 0;
}
void *xcb_get_property_value(const xcb_get_property_reply_t *reply)
{
    return reply ? (void *) (reply + 1) : NULL;
}
int xcb_get_property_value_length(const xcb_get_property_reply_t *reply)
{
    int bytes = propertyValueBytes(reply);
    return bytes >= 0 ? bytes : 0;
}
xcb_generic_iterator_t xcb_get_property_value_end(
    const xcb_get_property_reply_t *reply)
{
    xcb_generic_iterator_t end = {0};
    int bytes = propertyValueBytes(reply);
    if (reply && bytes >= 0) {
        end.data = (char *) xcb_get_property_value(reply) + bytes;
        end.index = sizeof(*reply) + bytes;
    }
    return end;
}

static xcb_void_cookie_t deleteProperty(xcb_connection_t *c,
                                        xcb_window_t window,
                                        xcb_atom_t property,
                                        int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = isWindow(window) ? 0 : XCB_WINDOW;
    if (!error && !isValidAtom(property))
        error = XCB_ATOM;
    if (!error)
        XDeleteProperty(xcbCompatDisplay(c), window, property);
    return xcbCompatVoidCookie(c, error, error == XCB_ATOM ? property : window,
                               XCB_DELETE_PROPERTY, checked);
}

static xcb_list_properties_cookie_t listProperties(xcb_connection_t *c,
                                                   xcb_window_t window,
                                                   int checked)
{
    REQUIRE_REQUEST(c, xcb_list_properties_cookie_t);
    xcb_list_properties_cookie_t cookie = {nextSequence(c)};
    if (!isWindow(window)) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_WINDOW, window,
                                    XCB_LIST_PROPERTIES, checked);
        return cookie;
    }
    int count = 0;
    Atom *atoms = XListProperties(xcbCompatDisplay(c), window, &count);
    if (count < 0) {
        free(atoms);
        storeReply(c, cookie.sequence, NULL);
        return cookie;
    }
    size_t bytes = 0;
    xcb_list_properties_reply_t *reply = allocateReplyPayload(
        c, sizeof(*reply), (uint64_t) count, sizeof(xcb_atom_t), &bytes);
    if (reply) {
        reply->response_type = 1;
        reply->atoms_len = (uint16_t) count;
        reply->length = (uint32_t) count;
        for (int i = 0; i < count; i++)
            ((xcb_atom_t *) (reply + 1))[i] = (xcb_atom_t) atoms[i];
    }
    free(atoms);
    storeReply(c, cookie.sequence, reply);
    return cookie;
}

xcb_list_properties_cookie_t xcb_list_properties(xcb_connection_t *c,
                                                 xcb_window_t window)
{
    return listProperties(c, window, 1);
}

int xcb_list_properties_sizeof(const void *buffer)
{
    const xcb_list_properties_reply_t *reply = buffer;
    return reply
               ? (int) (sizeof(*reply) + reply->atoms_len * sizeof(xcb_atom_t))
               : 0;
}

xcb_list_properties_cookie_t xcb_list_properties_unchecked(xcb_connection_t *c,
                                                           xcb_window_t window)
{
    return listProperties(c, window, 0);
}

xcb_list_properties_reply_t *xcb_list_properties_reply(
    xcb_connection_t *c,
    xcb_list_properties_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return xcbCompatTakeReply(c, cookie.sequence, error);
}

xcb_atom_t *xcb_list_properties_atoms(const xcb_list_properties_reply_t *reply)
{
    return reply ? (xcb_atom_t *) (reply + 1) : NULL;
}

int xcb_list_properties_atoms_length(const xcb_list_properties_reply_t *reply)
{
    return reply ? reply->atoms_len : 0;
}

xcb_generic_iterator_t xcb_list_properties_atoms_end(
    const xcb_list_properties_reply_t *reply)
{
    xcb_generic_iterator_t iterator = {0};
    if (reply) {
        iterator.data = xcb_list_properties_atoms(reply) + reply->atoms_len;
        iterator.index = sizeof(*reply) + reply->atoms_len * sizeof(xcb_atom_t);
    }
    return iterator;
}

static xcb_void_cookie_t setSelectionOwner(xcb_connection_t *c,
                                           xcb_window_t owner,
                                           xcb_atom_t selection,
                                           xcb_timestamp_t time,
                                           int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = owner != XCB_NONE && !isWindow(owner) ? XCB_WINDOW : 0;
    if (!error && !isValidAtom(selection))
        error = XCB_ATOM;
    if (!error)
        XSetSelectionOwner(xcbCompatDisplay(c), selection, owner, time);
    return xcbCompatVoidCookie(c, error, error == XCB_ATOM ? selection : owner,
                               XCB_SET_SELECTION_OWNER, checked);
}

xcb_void_cookie_t xcb_set_selection_owner(xcb_connection_t *c,
                                          xcb_window_t owner,
                                          xcb_atom_t selection,
                                          xcb_timestamp_t time)
{
    return setSelectionOwner(c, owner, selection, time, 0);
}

xcb_void_cookie_t xcb_set_selection_owner_checked(xcb_connection_t *c,
                                                  xcb_window_t owner,
                                                  xcb_atom_t selection,
                                                  xcb_timestamp_t time)
{
    return setSelectionOwner(c, owner, selection, time, 1);
}

static xcb_get_selection_owner_cookie_t getSelectionOwner(xcb_connection_t *c,
                                                          xcb_atom_t selection,
                                                          int checked)
{
    REQUIRE_REQUEST(c, xcb_get_selection_owner_cookie_t);
    xcb_get_selection_owner_cookie_t cookie = {nextSequence(c)};
    if (!isValidAtom(selection)) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_ATOM, selection,
                                    XCB_GET_SELECTION_OWNER, checked);
        return cookie;
    }
    xcb_get_selection_owner_reply_t *reply = calloc(1, sizeof(*reply));
    if (reply) {
        reply->response_type = 1;
        reply->owner = XGetSelectionOwner(xcbCompatDisplay(c), selection);
    }
    storeReply(c, cookie.sequence, reply);
    return cookie;
}

xcb_get_selection_owner_cookie_t xcb_get_selection_owner(xcb_connection_t *c,
                                                         xcb_atom_t selection)
{
    return getSelectionOwner(c, selection, 1);
}

xcb_get_selection_owner_cookie_t xcb_get_selection_owner_unchecked(
    xcb_connection_t *c,
    xcb_atom_t selection)
{
    return getSelectionOwner(c, selection, 0);
}

xcb_get_selection_owner_reply_t *xcb_get_selection_owner_reply(
    xcb_connection_t *c,
    xcb_get_selection_owner_cookie_t cookie,
    xcb_generic_error_t **error)
{
    return xcbCompatTakeReply(c, cookie.sequence, error);
}

static xcb_void_cookie_t convertSelection(xcb_connection_t *c,
                                          xcb_window_t requestor,
                                          xcb_atom_t selection,
                                          xcb_atom_t target,
                                          xcb_atom_t property,
                                          xcb_timestamp_t time,
                                          int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = isWindow(requestor) ? 0 : XCB_WINDOW;
    uint32_t errorValue = requestor;
    if (!error && !isValidAtom(selection)) {
        error = XCB_ATOM;
        errorValue = selection;
    }
    if (!error && !isValidAtom(target)) {
        error = XCB_ATOM;
        errorValue = target;
    }
    if (!error && property != XCB_NONE && !isValidAtom(property)) {
        error = XCB_ATOM;
        errorValue = property;
    }
    if (!error)
        XConvertSelection(xcbCompatDisplay(c), selection, target, property,
                          requestor, time);
    return xcbCompatVoidCookie(c, error, errorValue, XCB_CONVERT_SELECTION,
                               checked);
}

xcb_void_cookie_t xcb_convert_selection(xcb_connection_t *c,
                                        xcb_window_t requestor,
                                        xcb_atom_t selection,
                                        xcb_atom_t target,
                                        xcb_atom_t property,
                                        xcb_timestamp_t time)
{
    return convertSelection(c, requestor, selection, target, property, time, 0);
}

xcb_void_cookie_t xcb_convert_selection_checked(xcb_connection_t *c,
                                                xcb_window_t requestor,
                                                xcb_atom_t selection,
                                                xcb_atom_t target,
                                                xcb_atom_t property,
                                                xcb_timestamp_t time)
{
    return convertSelection(c, requestor, selection, target, property, time, 1);
}
xcb_void_cookie_t xcb_delete_property(xcb_connection_t *c,
                                      xcb_window_t w,
                                      xcb_atom_t p)
{
    return deleteProperty(c, w, p, 0);
}
xcb_void_cookie_t xcb_delete_property_checked(xcb_connection_t *c,
                                              xcb_window_t w,
                                              xcb_atom_t p)
{
    return deleteProperty(c, w, p, 1);
}

static xcb_void_cookie_t changeAttributes(xcb_connection_t *c,
                                          xcb_window_t window,
                                          uint32_t mask,
                                          const uint32_t *values,
                                          int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = !isWindow(window) ? XCB_WINDOW : 0;
    uint32_t errorValue = window;
    if (!error && (mask & ~XCB_CW_VALID_MASK)) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    if (!error && mask && !values) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    if (!error) {
        XSetWindowAttributes attributes;
        decodeAttributes(mask, values, &attributes);
        error = validateAttributes(mask, &attributes, drawableDepth(c, window),
                                   IS_INPUT_ONLY(window), &errorValue);
        if (!error && !XChangeWindowAttributes(xcbCompatDisplay(c), window,
                                               mask, &attributes))
            error = XCB_VALUE;
    }
    return xcbCompatVoidCookie(c, error, errorValue,
                               XCB_CHANGE_WINDOW_ATTRIBUTES, checked);
}
xcb_void_cookie_t xcb_change_window_attributes(xcb_connection_t *c,
                                               xcb_window_t window,
                                               uint32_t mask,
                                               const void *values)
{
    return changeAttributes(c, window, mask, values, 0);
}
xcb_void_cookie_t xcb_change_window_attributes_checked(xcb_connection_t *c,
                                                       xcb_window_t window,
                                                       uint32_t mask,
                                                       const void *values)
{
    return changeAttributes(c, window, mask, values, 1);
}

static void decodeConfigure(uint16_t mask,
                            const uint32_t *values,
                            XWindowChanges *changes)
{
    memset(changes, 0, sizeof(*changes));
    unsigned int index = 0;
#define TAKE(bit, field, type)                       \
    do {                                             \
        if (mask & (bit))                            \
            changes->field = (type) values[index++]; \
    } while (0)
    TAKE(XCB_CONFIG_WINDOW_X, x, int16_t);
    TAKE(XCB_CONFIG_WINDOW_Y, y, int16_t);
    TAKE(XCB_CONFIG_WINDOW_WIDTH, width, uint16_t);
    TAKE(XCB_CONFIG_WINDOW_HEIGHT, height, uint16_t);
    TAKE(XCB_CONFIG_WINDOW_BORDER_WIDTH, border_width, uint16_t);
    TAKE(XCB_CONFIG_WINDOW_SIBLING, sibling, xcb_window_t);
    TAKE(XCB_CONFIG_WINDOW_STACK_MODE, stack_mode, uint8_t);
#undef TAKE
}

static xcb_void_cookie_t xcbConfigureWindow(xcb_connection_t *c,
                                            xcb_window_t window,
                                            uint16_t mask,
                                            const uint32_t *values,
                                            int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = !isWindow(window) ? XCB_WINDOW : 0;
    uint32_t errorValue = window;
    if (!error && (mask & ~XCB_CONFIG_WINDOW_VALID_MASK)) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    if (!error && mask && !values) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    XWindowChanges changes;
    if (!error) {
        decodeConfigure(mask, values, &changes);
        if (mask & XCB_CONFIG_WINDOW_WIDTH && changes.width <= 0) {
            error = XCB_VALUE;
            errorValue = changes.width;
        } else if (mask & XCB_CONFIG_WINDOW_HEIGHT && changes.height <= 0) {
            error = XCB_VALUE;
            errorValue = changes.height;
        } else if (mask & XCB_CONFIG_WINDOW_STACK_MODE &&
                   changes.stack_mode > XCB_STACK_MODE_OPPOSITE) {
            error = XCB_VALUE;
            errorValue = changes.stack_mode;
        } else if ((mask & XCB_CONFIG_WINDOW_SIBLING) &&
                   !isWindow(changes.sibling)) {
            error = XCB_WINDOW;
            errorValue = changes.sibling;
        } else if ((mask & XCB_CONFIG_WINDOW_SIBLING) &&
                   !(mask & XCB_CONFIG_WINDOW_STACK_MODE)) {
            error = XCB_MATCH;
            errorValue = window;
        } else if ((mask & XCB_CONFIG_WINDOW_SIBLING) &&
                   (changes.sibling == window ||
                    GET_PARENT(changes.sibling) != GET_PARENT(window))) {
            error = XCB_MATCH;
            errorValue = window;
        }
    }
    if (!error) {
        if (!XConfigureWindow(xcbCompatDisplay(c), window, mask, &changes))
            error = XCB_MATCH;
    }
    return xcbCompatVoidCookie(c, error, errorValue, XCB_CONFIGURE_WINDOW,
                               checked);
}
xcb_void_cookie_t xcb_configure_window(xcb_connection_t *c,
                                       xcb_window_t window,
                                       uint16_t mask,
                                       const void *values)
{
    return xcbConfigureWindow(c, window, mask, values, 0);
}
xcb_void_cookie_t xcb_configure_window_checked(xcb_connection_t *c,
                                               xcb_window_t window,
                                               uint16_t mask,
                                               const void *values)
{
    return xcbConfigureWindow(c, window, mask, values, 1);
}

static xcb_void_cookie_t reparentWindow(xcb_connection_t *c,
                                        xcb_window_t window,
                                        xcb_window_t parent,
                                        int16_t x,
                                        int16_t y,
                                        int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    int windowValid = isWindow(window);
    int parentValid = isWindow(parent);
    uint8_t error = !windowValid ? XCB_WINDOW : 0;
    uint32_t errorResource = window;
    if (!error && !parentValid) {
        error = XCB_WINDOW;
        errorResource = parent;
    }
    if (!error && !XReparentWindow(xcbCompatDisplay(c), window, parent, x, y))
        error = XCB_MATCH;
    return xcbCompatVoidCookie(c, error, errorResource, XCB_REPARENT_WINDOW,
                               checked);
}
xcb_void_cookie_t xcb_reparent_window(xcb_connection_t *c,
                                      xcb_window_t window,
                                      xcb_window_t parent,
                                      int16_t x,
                                      int16_t y)
{
    return reparentWindow(c, window, parent, x, y, 0);
}
xcb_void_cookie_t xcb_reparent_window_checked(xcb_connection_t *c,
                                              xcb_window_t window,
                                              xcb_window_t parent,
                                              int16_t x,
                                              int16_t y)
{
    return reparentWindow(c, window, parent, x, y, 1);
}

/* Accept exactly the depths the setup record advertises, rather than repeating
 * the list src/pixmap.c keeps: a client picks its depth from those formats, so
 * anything else is a BadValue and the two sets cannot drift apart.
 */
static int supportedPixmapDepth(xcb_connection_t *c, uint8_t depth)
{
    const xcb_setup_t *setup = xcb_get_setup(c);
    if (!setup)
        return 0;
    const xcb_format_t *formats = xcb_setup_pixmap_formats(setup);
    int count = xcb_setup_pixmap_formats_length(setup);
    for (int i = 0; i < count; i++)
        if (formats[i].depth == depth)
            return 1;
    return 0;
}

static xcb_void_cookie_t createPixmap(xcb_connection_t *c,
                                      uint8_t depth,
                                      xcb_pixmap_t pid,
                                      xcb_drawable_t drawable,
                                      uint16_t width,
                                      uint16_t height,
                                      int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = !isDrawable(drawable) ? XCB_DRAWABLE : 0;
    uint32_t errorValue = drawable;
    if (!error && getXidStruct(pid)->type != 0) {
        error = XCB_ID_CHOICE;
        errorValue = pid;
    }
    if (!error && !width) {
        error = XCB_VALUE;
        errorValue = width;
    }
    if (!error && !height) {
        error = XCB_VALUE;
        errorValue = height;
    }
    if (!error && !supportedPixmapDepth(c, depth)) {
        error = XCB_VALUE;
        errorValue = depth;
    }

    /* Past the argument checks the only way the shared path still fails is a
     * failed allocation, which the protocol reports as BadAlloc rather than
     * blaming one of the arguments. Claim a client-chosen id last, once nothing
     * else can reject the request; the creator owns it from here and releases
     * it itself if it fails.
     */
    if (!error && !isXidAllocated(pid) && !reserveXidResource(pid)) {
        error = XCB_ID_CHOICE;
        errorValue = pid;
    }
    if (!error &&
        libx11CompatCreatePixmapWithId(xcbCompatDisplay(c), pid, drawable,
                                       width, height, depth) == XCB_NONE) {
        error = XCB_ALLOC;
        errorValue = pid;
    }
    return xcbCompatVoidCookie(c, error, errorValue, XCB_CREATE_PIXMAP,
                               checked);
}

xcb_void_cookie_t xcb_create_pixmap(xcb_connection_t *c,
                                    uint8_t depth,
                                    xcb_pixmap_t pid,
                                    xcb_drawable_t drawable,
                                    uint16_t width,
                                    uint16_t height)
{
    return createPixmap(c, depth, pid, drawable, width, height, 0);
}
xcb_void_cookie_t xcb_create_pixmap_checked(xcb_connection_t *c,
                                            uint8_t depth,
                                            xcb_pixmap_t pid,
                                            xcb_drawable_t drawable,
                                            uint16_t width,
                                            uint16_t height)
{
    return createPixmap(c, depth, pid, drawable, width, height, 1);
}

static xcb_void_cookie_t freePixmap(xcb_connection_t *c,
                                    xcb_pixmap_t pixmap,
                                    int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = IS_TYPE(pixmap, PIXMAP) ? 0 : XCB_PIXMAP;
    if (!error)
        XFreePixmap(xcbCompatDisplay(c), pixmap);
    return xcbCompatVoidCookie(c, error, pixmap, XCB_FREE_PIXMAP, checked);
}
xcb_void_cookie_t xcb_free_pixmap(xcb_connection_t *c, xcb_pixmap_t pixmap)
{
    return freePixmap(c, pixmap, 0);
}
xcb_void_cookie_t xcb_free_pixmap_checked(xcb_connection_t *c,
                                          xcb_pixmap_t pixmap)
{
    return freePixmap(c, pixmap, 1);
}

static void decodeGcValues(uint32_t mask,
                           const uint32_t *list,
                           XGCValues *values)
{
    memset(values, 0, sizeof(*values));
    unsigned int n = 0;
#define TAKE(bit, field, type)                \
    do {                                      \
        if (mask & (bit))                     \
            values->field = (type) list[n++]; \
    } while (0)
    TAKE(XCB_GC_FUNCTION, function, int);
    TAKE(XCB_GC_PLANE_MASK, plane_mask, unsigned long);
    TAKE(XCB_GC_FOREGROUND, foreground, unsigned long);
    TAKE(XCB_GC_BACKGROUND, background, unsigned long);
    TAKE(XCB_GC_LINE_WIDTH, line_width, int);
    TAKE(XCB_GC_LINE_STYLE, line_style, int);
    TAKE(XCB_GC_CAP_STYLE, cap_style, int);
    TAKE(XCB_GC_JOIN_STYLE, join_style, int);
    TAKE(XCB_GC_FILL_STYLE, fill_style, int);
    TAKE(XCB_GC_FILL_RULE, fill_rule, int);
    TAKE(XCB_GC_TILE, tile, Pixmap);
    TAKE(XCB_GC_STIPPLE, stipple, Pixmap);
    TAKE(XCB_GC_TILE_STIPPLE_ORIGIN_X, ts_x_origin, int);
    TAKE(XCB_GC_TILE_STIPPLE_ORIGIN_Y, ts_y_origin, int);
    TAKE(XCB_GC_FONT, font, Font);
    TAKE(XCB_GC_SUBWINDOW_MODE, subwindow_mode, int);
    TAKE(XCB_GC_GRAPHICS_EXPOSURES, graphics_exposures, Bool);
    TAKE(XCB_GC_CLIP_ORIGIN_X, clip_x_origin, int);
    TAKE(XCB_GC_CLIP_ORIGIN_Y, clip_y_origin, int);
    TAKE(XCB_GC_CLIP_MASK, clip_mask, Pixmap);
    TAKE(XCB_GC_DASH_OFFSET, dash_offset, int);
    TAKE(XCB_GC_DASH_LIST, dashes, char);
    TAKE(XCB_GC_ARC_MODE, arc_mode, int);
#undef TAKE
}

static uint8_t validateGcValues(uint32_t mask,
                                const XGCValues *values,
                                unsigned int gcDepth,
                                uint32_t *errorValue)
{
#define BAD_VALUE(bit, field, limit)                          \
    do {                                                      \
        if ((mask & (bit)) &&                                 \
            (values->field < 0 || values->field > (limit))) { \
            *errorValue = (uint32_t) values->field;           \
            return XCB_VALUE;                                 \
        }                                                     \
    } while (0)
    BAD_VALUE(XCB_GC_FUNCTION, function, XCB_GX_SET);
    BAD_VALUE(XCB_GC_LINE_STYLE, line_style, XCB_LINE_STYLE_DOUBLE_DASH);
    BAD_VALUE(XCB_GC_CAP_STYLE, cap_style, XCB_CAP_STYLE_PROJECTING);
    BAD_VALUE(XCB_GC_JOIN_STYLE, join_style, XCB_JOIN_STYLE_BEVEL);
    BAD_VALUE(XCB_GC_FILL_STYLE, fill_style, XCB_FILL_STYLE_OPAQUE_STIPPLED);
    BAD_VALUE(XCB_GC_FILL_RULE, fill_rule, XCB_FILL_RULE_WINDING);
    BAD_VALUE(XCB_GC_SUBWINDOW_MODE, subwindow_mode,
              XCB_SUBWINDOW_MODE_INCLUDE_INFERIORS);
    BAD_VALUE(XCB_GC_ARC_MODE, arc_mode, XCB_ARC_MODE_PIE_SLICE);
#undef BAD_VALUE
    if ((mask & XCB_GC_GRAPHICS_EXPOSURES) &&
        values->graphics_exposures != False &&
        values->graphics_exposures != True) {
        *errorValue = (uint32_t) values->graphics_exposures;
        return XCB_VALUE;
    }
    if ((mask & XCB_GC_DASH_LIST) && values->dashes == 0) {
        *errorValue = 0;
        return XCB_VALUE;
    }
    const struct {
        uint32_t bit;
        Pixmap pixmap;
        unsigned int requiredDepth;
        int noneAllowed;
    } pixmaps[] = {
        {XCB_GC_TILE, values->tile, gcDepth, 0},
        {XCB_GC_STIPPLE, values->stipple, 1, 0},
        {XCB_GC_CLIP_MASK, values->clip_mask, 1, 1},
    };
    for (size_t i = 0; i < sizeof(pixmaps) / sizeof(pixmaps[0]); i++) {
        if (!(mask & pixmaps[i].bit) ||
            (pixmaps[i].noneAllowed && pixmaps[i].pixmap == XCB_NONE))
            continue;
        *errorValue = pixmaps[i].pixmap;
        if (!IS_TYPE(pixmaps[i].pixmap, PIXMAP))
            return XCB_PIXMAP;
        if (GET_PIXMAP_STRUCT(pixmaps[i].pixmap)->depth !=
            pixmaps[i].requiredDepth)
            return XCB_MATCH;
    }
    if (mask & XCB_GC_FONT) {
        *errorValue = values->font;
        if (!IS_TYPE(values->font, FONT))
            return XCB_FONT;
    }
    return 0;
}

static xcb_void_cookie_t createGc(xcb_connection_t *c,
                                  xcb_gcontext_t cid,
                                  xcb_drawable_t drawable,
                                  uint32_t mask,
                                  const uint32_t *list,
                                  int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = !isDrawable(drawable) ? XCB_DRAWABLE : 0;
    uint32_t errorValue = drawable;
    if (!error && isInputOnlyDrawable(drawable))
        error = XCB_MATCH;
    if (!error && getXidStruct(cid)->type != 0) {
        error = XCB_ID_CHOICE;
        errorValue = cid;
    }
    if (!error && (mask & ~XCB_GC_VALID_MASK)) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    if (!error && mask && !list) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    XGCValues values;
    unsigned int gcDepth = 0;
    if (!error) {
        gcDepth = drawableDepth(c, drawable);
        decodeGcValues(mask, list, &values);
        error = validateGcValues(mask, &values, gcDepth, &errorValue);
    }

    /* Claim a client-chosen id last, once nothing else can reject the request;
     * the creator owns it from here and releases it itself if it fails.
     */
    if (!error && !isXidAllocated(cid) && !reserveXidResource(cid)) {
        error = XCB_ID_CHOICE;
        errorValue = cid;
    }
    if (!error) {
        GC gc = libx11CompatCreateGCWithId(xcbCompatDisplay(c), cid, drawable,
                                           mask, &values);
        GcEntry *entry = gc ? calloc(1, sizeof(*entry)) : NULL;
        if (!entry) {
            if (gc)
                XFreeGC(xcbCompatDisplay(c), gc);
            error = XCB_ALLOC;
        } else {
            entry->id = cid;
            entry->gc = gc;
            entry->display = xcbCompatDisplay(c);
            entry->owner = c;
            entry->depth = gcDepth;
            pthread_mutex_lock(&gcMutex);
            entry->next = gcEntries;
            gcEntries = entry;
            pthread_mutex_unlock(&gcMutex);
        }
    }
    return xcbCompatVoidCookie(c, error, errorValue, XCB_CREATE_GC, checked);
}

#define GC_CREATE_WRAPPER(name, checkedValue)                        \
    xcb_void_cookie_t name(xcb_connection_t *c, xcb_gcontext_t cid,  \
                           xcb_drawable_t drawable, uint32_t mask,   \
                           const void *list)                         \
    {                                                                \
        return createGc(c, cid, drawable, mask, list, checkedValue); \
    }
GC_CREATE_WRAPPER(xcb_create_gc, 0)
GC_CREATE_WRAPPER(xcb_create_gc_checked, 1)

static xcb_void_cookie_t changeGc(xcb_connection_t *c,
                                  xcb_gcontext_t id,
                                  uint32_t mask,
                                  const uint32_t *list,
                                  int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    pthread_mutex_lock(&gcMutex);
    GcEntry *entry = findGcLocked(id);
    GC gc = entry ? entry->gc : NULL;
    uint8_t error = entry ? 0 : XCB_G_CONTEXT;
    uint32_t errorValue = id;
    if (!error && (mask & ~XCB_GC_VALID_MASK)) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    if (!error && mask && !list) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    if (!error) {
        XGCValues values;
        decodeGcValues(mask, list, &values);
        error = validateGcValues(mask, &values, entry->depth, &errorValue);
        if (!error && !XChangeGC(xcbCompatDisplay(c), gc, mask, &values))
            error = XCB_VALUE;
    }
    pthread_mutex_unlock(&gcMutex);
    return xcbCompatVoidCookie(c, error, errorValue, XCB_CHANGE_GC, checked);
}
#define GC_CHANGE_WRAPPER(name, checkedValue)                      \
    xcb_void_cookie_t name(xcb_connection_t *c, xcb_gcontext_t gc, \
                           uint32_t mask, const void *list)        \
    {                                                              \
        return changeGc(c, gc, mask, list, checkedValue);          \
    }
GC_CHANGE_WRAPPER(xcb_change_gc, 0)
GC_CHANGE_WRAPPER(xcb_change_gc_checked, 1)

static xcb_void_cookie_t copyGc(xcb_connection_t *c,
                                xcb_gcontext_t source,
                                xcb_gcontext_t destination,
                                uint32_t mask,
                                int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    pthread_mutex_lock(&gcMutex);
    GcEntry *srcEntry = findGcLocked(source);
    GcEntry *dstEntry = findGcLocked(destination);
    GC src = srcEntry ? srcEntry->gc : NULL;
    GC dst = dstEntry ? dstEntry->gc : NULL;
    uint8_t error = !srcEntry || !dstEntry ? XCB_G_CONTEXT : 0;
    uint32_t errorValue = !srcEntry ? source : destination;
    if (!error && srcEntry->depth != dstEntry->depth)
        error = XCB_MATCH;
    if (!error && (mask & ~XCB_GC_VALID_MASK)) {
        error = XCB_VALUE;
        errorValue = mask;
    }
    if (!error && !XCopyGC(xcbCompatDisplay(c), src, mask, dst))
        error = XCB_VALUE;
    pthread_mutex_unlock(&gcMutex);
    return xcbCompatVoidCookie(c, error, errorValue, XCB_COPY_GC, checked);
}
xcb_void_cookie_t xcb_copy_gc(xcb_connection_t *c,
                              xcb_gcontext_t src,
                              xcb_gcontext_t dst,
                              uint32_t mask)
{
    return copyGc(c, src, dst, mask, 0);
}
xcb_void_cookie_t xcb_copy_gc_checked(xcb_connection_t *c,
                                      xcb_gcontext_t src,
                                      xcb_gcontext_t dst,
                                      uint32_t mask)
{
    return copyGc(c, src, dst, mask, 1);
}

static xcb_void_cookie_t freeGc(xcb_connection_t *c,
                                xcb_gcontext_t id,
                                int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    pthread_mutex_lock(&gcMutex);
    GcEntry **link = &gcEntries;
    while (*link && (*link)->id != id)
        link = &(*link)->next;
    uint8_t error = *link ? 0 : XCB_G_CONTEXT;
    if (!error) {
        GcEntry *entry = *link;
        *link = entry->next;
        XFreeGC(entry->display, entry->gc);
        free(entry);
    }
    pthread_mutex_unlock(&gcMutex);
    return xcbCompatVoidCookie(c, error, id, XCB_FREE_GC, checked);
}
xcb_void_cookie_t xcb_free_gc(xcb_connection_t *c, xcb_gcontext_t gc)
{
    return freeGc(c, gc, 0);
}
xcb_void_cookie_t xcb_free_gc_checked(xcb_connection_t *c, xcb_gcontext_t gc)
{
    return freeGc(c, gc, 1);
}

static uint8_t lockDrawingResources(xcb_connection_t *c,
                                    xcb_drawable_t drawable,
                                    xcb_gcontext_t gc,
                                    GC *xgc)
{
    *xgc = NULL;
    if (!isDrawable(drawable))
        return XCB_DRAWABLE;
    if (isInputOnlyDrawable(drawable))
        return XCB_MATCH;
    pthread_mutex_lock(&gcMutex);
    GcEntry *entry = findGcLocked(gc);
    *xgc = entry ? entry->gc : NULL;
    if (entry && entry->depth == drawableDepth(c, drawable))
        return 0;
    if (entry) {
        pthread_mutex_unlock(&gcMutex);
        *xgc = NULL;
        return XCB_MATCH;
    }
    pthread_mutex_unlock(&gcMutex);
    return XCB_G_CONTEXT;
}

static xcb_void_cookie_t clearArea(xcb_connection_t *c,
                                   uint8_t exposures,
                                   xcb_window_t window,
                                   int16_t x,
                                   int16_t y,
                                   uint16_t width,
                                   uint16_t height,
                                   int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    uint8_t error = isWindow(window) ? 0 : XCB_WINDOW;
    if (!error)
        XClearArea(xcbCompatDisplay(c), window, x, y, width, height, exposures);
    return xcbCompatVoidCookie(c, error, window, XCB_CLEAR_AREA, checked);
}
#define CLEAR_WRAPPER(name, checkedValue)                             \
    xcb_void_cookie_t name(xcb_connection_t *c, uint8_t exposures,    \
                           xcb_window_t window, int16_t x, int16_t y, \
                           uint16_t width, uint16_t height)           \
    {                                                                 \
        return clearArea(c, exposures, window, x, y, width, height,   \
                         checkedValue);                               \
    }
CLEAR_WRAPPER(xcb_clear_area, 0)
CLEAR_WRAPPER(xcb_clear_area_checked, 1)

static xcb_void_cookie_t copyArea(xcb_connection_t *c,
                                  xcb_drawable_t source,
                                  xcb_drawable_t destination,
                                  xcb_gcontext_t gc,
                                  int16_t sourceX,
                                  int16_t sourceY,
                                  int16_t destinationX,
                                  int16_t destinationY,
                                  uint16_t width,
                                  uint16_t height,
                                  int checked,
                                  int copyPlane,
                                  uint32_t plane)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    GC xgc = NULL;
    int sourceValid = isDrawable(source);
    int destinationValid = isDrawable(destination);
    uint8_t error = !sourceValid || !destinationValid
                        ? XCB_DRAWABLE
                        : lockDrawingResources(c, destination, gc, &xgc);
    if (!error && isInputOnlyDrawable(source))
        error = XCB_MATCH;
    uint32_t errorValue = !sourceValid             ? source
                          : !destinationValid      ? destination
                          : error == XCB_G_CONTEXT ? gc
                                                   : source;
    if (!error && !copyPlane &&
        drawableDepth(c, source) != drawableDepth(c, destination)) {
        error = XCB_MATCH;
        errorValue = destination;
    }

    /* CopyPlane names a single bit, and that bit has to exist in the source: a
     * plane above the source depth selects nothing at all. A depth of 32 or
     * more admits every bit, and shifting by the width of the type would be
     * undefined, so that case skips the range test.
     */
    if (!error && copyPlane) {
        unsigned int sourceDepth = drawableDepth(c, source);
        if (!plane || (plane & (plane - 1)) ||
            (sourceDepth < 32 && plane >= (UINT32_C(1) << sourceDepth))) {
            error = XCB_VALUE;
            errorValue = plane;
        }
    }
    if (!error) {
        if (copyPlane)
            XCopyPlane(xcbCompatDisplay(c), source, destination, xgc, sourceX,
                       sourceY, width, height, destinationX, destinationY,
                       plane);
        else
            XCopyArea(xcbCompatDisplay(c), source, destination, xgc, sourceX,
                      sourceY, width, height, destinationX, destinationY);
    }
    if (xgc)
        pthread_mutex_unlock(&gcMutex);
    return xcbCompatVoidCookie(c, error, errorValue,
                               copyPlane ? XCB_COPY_PLANE : XCB_COPY_AREA,
                               checked);
}
#define COPY_AREA_WRAPPER(name, checkedValue)                                  \
    xcb_void_cookie_t name(xcb_connection_t *c, xcb_drawable_t src,            \
                           xcb_drawable_t dst, xcb_gcontext_t gc, int16_t sx,  \
                           int16_t sy, int16_t dx, int16_t dy, uint16_t width, \
                           uint16_t height)                                    \
    {                                                                          \
        return copyArea(c, src, dst, gc, sx, sy, dx, dy, width, height,        \
                        checkedValue, 0, 0);                                   \
    }
COPY_AREA_WRAPPER(xcb_copy_area, 0)
COPY_AREA_WRAPPER(xcb_copy_area_checked, 1)

#define COPY_PLANE_WRAPPER(name, checkedValue)                                 \
    xcb_void_cookie_t name(xcb_connection_t *c, xcb_drawable_t src,            \
                           xcb_drawable_t dst, xcb_gcontext_t gc, int16_t sx,  \
                           int16_t sy, int16_t dx, int16_t dy, uint16_t width, \
                           uint16_t height, uint32_t plane)                    \
    {                                                                          \
        return copyArea(c, src, dst, gc, sx, sy, dx, dy, width, height,        \
                        checkedValue, 1, plane);                               \
    }
COPY_PLANE_WRAPPER(xcb_copy_plane, 0)
COPY_PLANE_WRAPPER(xcb_copy_plane_checked, 1)

typedef enum {
    DRAW_POINTS,
    DRAW_LINES,
    DRAW_SEGMENTS,
    DRAW_RECTANGLES,
    DRAW_ARCS
} DrawKind;

static xcb_void_cookie_t drawPrimitives(xcb_connection_t *c,
                                        xcb_drawable_t drawable,
                                        xcb_gcontext_t gc,
                                        uint8_t mode,
                                        uint32_t count,
                                        const void *items,
                                        DrawKind kind,
                                        uint8_t opcode,
                                        int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    _Static_assert(sizeof(xcb_point_t) == sizeof(XPoint), "point ABI");
    _Static_assert(sizeof(xcb_segment_t) == sizeof(XSegment), "segment ABI");
    _Static_assert(sizeof(xcb_rectangle_t) == sizeof(XRectangle),
                   "rectangle ABI");
    _Static_assert(sizeof(xcb_arc_t) == sizeof(XArc), "arc ABI");
    size_t itemSize = kind == DRAW_POINTS || kind == DRAW_LINES
                          ? sizeof(xcb_point_t)
                      : kind == DRAW_SEGMENTS   ? sizeof(xcb_segment_t)
                      : kind == DRAW_RECTANGLES ? sizeof(xcb_rectangle_t)
                                                : sizeof(xcb_arc_t);
    GC xgc = NULL;
    uint8_t error =
        requestPayloadFits(c, sizeof(xcb_poly_point_request_t), count, itemSize)
            ? lockDrawingResources(c, drawable, gc, &xgc)
            : XCB_LENGTH;
    uint32_t errorValue = error == XCB_G_CONTEXT ? gc
                          : error == XCB_LENGTH  ? count
                                                 : drawable;
    if (!error && count && !items) {
        error = XCB_VALUE;
        errorValue = count;
    }
    if (!error && (kind == DRAW_POINTS || kind == DRAW_LINES) &&
        mode > XCB_COORD_MODE_PREVIOUS) {
        error = XCB_VALUE;
        errorValue = mode;
    }
    if (!error) {
        switch (kind) {
        case DRAW_POINTS:
            XDrawPoints(xcbCompatDisplay(c), drawable, xgc, (XPoint *) items,
                        count, mode);
            break;
        case DRAW_LINES:
            XDrawLines(xcbCompatDisplay(c), drawable, xgc, (XPoint *) items,
                       count, mode);
            break;
        case DRAW_SEGMENTS:
            XDrawSegments(xcbCompatDisplay(c), drawable, xgc,
                          (XSegment *) items, count);
            break;
        case DRAW_RECTANGLES:
            XDrawRectangles(xcbCompatDisplay(c), drawable, xgc,
                            (XRectangle *) items, count);
            break;
        case DRAW_ARCS:
            XDrawArcs(xcbCompatDisplay(c), drawable, xgc, (XArc *) items,
                      count);
            break;
        }
    }
    if (xgc)
        pthread_mutex_unlock(&gcMutex);
    return xcbCompatVoidCookie(c, error, errorValue, opcode, checked);
}

#define POINTS_WRAPPER(name, kindValue, opcodeValue, checkedValue)            \
    xcb_void_cookie_t name(xcb_connection_t *c, uint8_t mode,                 \
                           xcb_drawable_t drawable, xcb_gcontext_t gc,        \
                           uint32_t count, const xcb_point_t *items)          \
    {                                                                         \
        return drawPrimitives(c, drawable, gc, mode, count, items, kindValue, \
                              opcodeValue, checkedValue);                     \
    }
POINTS_WRAPPER(xcb_poly_point, DRAW_POINTS, XCB_POLY_POINT, 0)
POINTS_WRAPPER(xcb_poly_point_checked, DRAW_POINTS, XCB_POLY_POINT, 1)
POINTS_WRAPPER(xcb_poly_line, DRAW_LINES, XCB_POLY_LINE, 0)
POINTS_WRAPPER(xcb_poly_line_checked, DRAW_LINES, XCB_POLY_LINE, 1)

#define FIXED_WRAPPER(name, itemType, kindValue, opcodeValue, checkedValue) \
    xcb_void_cookie_t name(xcb_connection_t *c, xcb_drawable_t drawable,    \
                           xcb_gcontext_t gc, uint32_t count,               \
                           const itemType *items)                           \
    {                                                                       \
        return drawPrimitives(c, drawable, gc, 0, count, items, kindValue,  \
                              opcodeValue, checkedValue);                   \
    }
FIXED_WRAPPER(xcb_poly_segment,
              xcb_segment_t,
              DRAW_SEGMENTS,
              XCB_POLY_SEGMENT,
              0)
FIXED_WRAPPER(xcb_poly_segment_checked,
              xcb_segment_t,
              DRAW_SEGMENTS,
              XCB_POLY_SEGMENT,
              1)
FIXED_WRAPPER(xcb_poly_rectangle,
              xcb_rectangle_t,
              DRAW_RECTANGLES,
              XCB_POLY_RECTANGLE,
              0)
FIXED_WRAPPER(xcb_poly_rectangle_checked,
              xcb_rectangle_t,
              DRAW_RECTANGLES,
              XCB_POLY_RECTANGLE,
              1)
FIXED_WRAPPER(xcb_poly_arc, xcb_arc_t, DRAW_ARCS, XCB_POLY_ARC, 0)
FIXED_WRAPPER(xcb_poly_arc_checked, xcb_arc_t, DRAW_ARCS, XCB_POLY_ARC, 1)

/* ImageText8 and ImageText16 differ only in the character width, so one body
 * takes both: wide selects the Xlib entry point, the string type and the
 * opcode, the same way copyArea covers CopyPlane.
 */
static xcb_void_cookie_t imageText(xcb_connection_t *c,
                                   uint8_t length,
                                   xcb_drawable_t drawable,
                                   xcb_gcontext_t gc,
                                   int16_t x,
                                   int16_t y,
                                   const void *string,
                                   int wide,
                                   int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    _Static_assert(sizeof(xcb_char2b_t) == sizeof(XChar2b), "char2b ABI");
    GC xgc;
    uint8_t error = lockDrawingResources(c, drawable, gc, &xgc);
    if (!error && length && !string)
        error = XCB_VALUE;
    if (!error && wide)
        XDrawImageString16(xcbCompatDisplay(c), drawable, xgc, x, y,
                           (XChar2b *) string, length);
    else if (!error)
        XDrawImageString(xcbCompatDisplay(c), drawable, xgc, x, y, string,
                         length);
    if (xgc)
        pthread_mutex_unlock(&gcMutex);
    return xcbCompatVoidCookie(c, error, error == XCB_G_CONTEXT ? gc : drawable,
                               wide ? XCB_IMAGE_TEXT_16 : XCB_IMAGE_TEXT_8,
                               checked);
}

#define TEXT_WRAPPER(name, stringType, wideValue, checkedValue)            \
    xcb_void_cookie_t name(xcb_connection_t *c, uint8_t length,            \
                           xcb_drawable_t drawable, xcb_gcontext_t gc,     \
                           int16_t x, int16_t y, stringType *string)       \
    {                                                                      \
        return imageText(c, length, drawable, gc, x, y, string, wideValue, \
                         checkedValue);                                    \
    }
TEXT_WRAPPER(xcb_image_text_8, const char, 0, 0)
TEXT_WRAPPER(xcb_image_text_8_checked, const char, 0, 1)
TEXT_WRAPPER(xcb_image_text_16, const xcb_char2b_t, 1, 0)
TEXT_WRAPPER(xcb_image_text_16_checked, const xcb_char2b_t, 1, 1)

static int imagePayloadSize(uint16_t width,
                            uint16_t height,
                            uint8_t depth,
                            size_t *rowBytes,
                            size_t *totalBytes)
{
    unsigned int bitsPerPixel = depth <= 1    ? 1
                                : depth <= 8  ? 8
                                : depth <= 16 ? 16
                                : depth <= 32 ? 32
                                              : 0;
    if (!bitsPerPixel)
        return 0;
    uint64_t rowBits = (uint64_t) width * bitsPerPixel;
    uint64_t row = ((rowBits + 31) & ~UINT64_C(31)) / 8;
    uint64_t total = row * height;
    if (row > SIZE_MAX || total > SIZE_MAX || total > UINT32_MAX)
        return 0;
    *rowBytes = (size_t) row;
    *totalBytes = (size_t) total;
    return 1;
}

static xcb_void_cookie_t putImage(xcb_connection_t *c,
                                  uint8_t format,
                                  xcb_drawable_t drawable,
                                  xcb_gcontext_t gc,
                                  uint16_t width,
                                  uint16_t height,
                                  int16_t destinationX,
                                  int16_t destinationY,
                                  uint8_t leftPad,
                                  uint8_t depth,
                                  uint32_t dataLength,
                                  const uint8_t *data,
                                  int checked)
{
    REQUIRE_REQUEST(c, xcb_void_cookie_t);
    GC xgc;
    uint8_t error = lockDrawingResources(c, drawable, gc, &xgc);
    uint32_t errorValue = error == XCB_G_CONTEXT ? gc : drawable;
    size_t rowBytes = 0, bytes = 0;
    if (!error && format != XCB_IMAGE_FORMAT_Z_PIXMAP) {
        error = XCB_VALUE;
        errorValue = format;
    }
    if (!error && leftPad != 0) {
        error = XCB_VALUE;
        errorValue = leftPad;
    }
    if (!error && !width) {
        error = XCB_VALUE;
        errorValue = width;
    }
    if (!error && !height) {
        error = XCB_VALUE;
        errorValue = height;
    }
    if (!error && depth != drawableDepth(c, drawable)) {
        error = XCB_MATCH;
        errorValue = drawable;
    }
    if (!error && !imagePayloadSize(width, height, depth, &rowBytes, &bytes)) {
        error = XCB_LENGTH;
        errorValue = dataLength;
    }
    if (!error && !requestPayloadFits(c, sizeof(xcb_put_image_request_t),
                                      dataLength, 1)) {
        error = XCB_LENGTH;
        errorValue = dataLength;
    }
    if (!error && dataLength < bytes) {
        error = XCB_VALUE;
        errorValue = dataLength;
    }
    if (!error && !data) {
        error = XCB_VALUE;
        errorValue = dataLength;
    }
    if (!error) {
        char *copy = malloc(bytes);
        if (!copy)
            error = XCB_ALLOC;
        else {
            memcpy(copy, data, bytes);
            XImage *image = XCreateImage(
                xcbCompatDisplay(c),
                DefaultVisual(xcbCompatDisplay(c),
                              DefaultScreen(xcbCompatDisplay(c))),
                depth, ZPixmap, 0, copy, width, height, 32, (int) rowBytes);
            if (!image) {
                free(copy);
                error = XCB_ALLOC;
            } else {
                XPutImage(xcbCompatDisplay(c), drawable, xgc, image, 0, 0,
                          destinationX, destinationY, width, height);
                XDestroyImage(image);
            }
        }
    }
    if (xgc)
        pthread_mutex_unlock(&gcMutex);
    return xcbCompatVoidCookie(c, error, errorValue, XCB_PUT_IMAGE, checked);
}
#define PUT_IMAGE_WRAPPER(name, checkedValue)                                  \
    xcb_void_cookie_t name(xcb_connection_t *c, uint8_t format,                \
                           xcb_drawable_t drawable, xcb_gcontext_t gc,         \
                           uint16_t width, uint16_t height, int16_t x,         \
                           int16_t y, uint8_t leftPad, uint8_t depth,          \
                           uint32_t dataLength, const uint8_t *data)           \
    {                                                                          \
        return putImage(c, format, drawable, gc, width, height, x, y, leftPad, \
                        depth, dataLength, data, checkedValue);                \
    }
PUT_IMAGE_WRAPPER(xcb_put_image, 0)
PUT_IMAGE_WRAPPER(xcb_put_image_checked, 1)

static xcb_get_image_cookie_t getImage(xcb_connection_t *c,
                                       uint8_t format,
                                       xcb_drawable_t drawable,
                                       int16_t x,
                                       int16_t y,
                                       uint16_t width,
                                       uint16_t height,
                                       uint32_t planeMask,
                                       int checked)
{
    REQUIRE_REQUEST(c, xcb_get_image_cookie_t);
    xcb_get_image_cookie_t cookie = {nextSequence(c)};
    if (!isDrawable(drawable)) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_DRAWABLE, drawable,
                                    XCB_GET_IMAGE, checked);
        return cookie;
    }
    if (isInputOnlyDrawable(drawable)) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_MATCH, drawable,
                                    XCB_GET_IMAGE, checked);
        return cookie;
    }
    if (format != XCB_IMAGE_FORMAT_Z_PIXMAP) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_VALUE, format,
                                    XCB_GET_IMAGE, checked);
        return cookie;
    }
    if (!width || !height) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_VALUE,
                                    !width ? width : height, XCB_GET_IMAGE,
                                    checked);
        return cookie;
    }

    /* Core GetImage requires the rectangle to lie wholly inside the drawable.
     * XGetImage zero-fills whatever hangs over the edge, so without this the
     * client would get a successful reply full of invented pixels.
     */
    Window root;
    int drawableX, drawableY;
    unsigned int drawableWidth, drawableHeight, borderWidth, depth;
    if (!XGetGeometry(xcbCompatDisplay(c), drawable, &root, &drawableX,
                      &drawableY, &drawableWidth, &drawableHeight, &borderWidth,
                      &depth) ||
        x < 0 || y < 0 || (unsigned int) x + width > drawableWidth ||
        (unsigned int) y + height > drawableHeight) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_MATCH, drawable,
                                    XCB_GET_IMAGE, checked);
        return cookie;
    }
    XImage *image = XGetImage(xcbCompatDisplay(c), drawable, x, y, width,
                              height, planeMask, ZPixmap);
    if (!image) {
        xcbCompatStoreProtocolError(c, cookie.sequence, XCB_MATCH, drawable,
                                    XCB_GET_IMAGE, checked);
        return cookie;
    }
    if (image->bytes_per_line < 0) {
        XDestroyImage(image);
        xcbCompatSetConnectionError(c, XCB_CONN_CLOSED_MEM_INSUFFICIENT);
        return cookie;
    }
    uint64_t imageBytes =
        (uint64_t) (unsigned int) image->bytes_per_line * height;
    if (imageBytes > SIZE_MAX || imageBytes / 4 > UINT32_MAX) {
        XDestroyImage(image);
        xcbCompatSetConnectionError(c, XCB_CONN_CLOSED_MEM_INSUFFICIENT);
        return cookie;
    }
    size_t bytes = 0;
    xcb_get_image_reply_t *reply =
        allocateReplyPayload(c, sizeof(*reply), imageBytes, 1, &bytes);
    if (reply) {
        reply->response_type = 1;
        reply->depth = image->depth;
        reply->visual =
            isWindow(drawable)
                ? XVisualIDFromVisual(DefaultVisual(
                      xcbCompatDisplay(c), DefaultScreen(xcbCompatDisplay(c))))
                : XCB_NONE;
        reply->length = bytes / 4;
        if (bytes)
            memcpy(reply + 1, image->data, bytes);
    }
    XDestroyImage(image);
    storeReply(c, cookie.sequence, reply);
    return cookie;
}
xcb_get_image_cookie_t xcb_get_image(xcb_connection_t *c,
                                     uint8_t format,
                                     xcb_drawable_t drawable,
                                     int16_t x,
                                     int16_t y,
                                     uint16_t width,
                                     uint16_t height,
                                     uint32_t planeMask)
{
    return getImage(c, format, drawable, x, y, width, height, planeMask, 1);
}
xcb_get_image_cookie_t xcb_get_image_unchecked(xcb_connection_t *c,
                                               uint8_t format,
                                               xcb_drawable_t drawable,
                                               int16_t x,
                                               int16_t y,
                                               uint16_t width,
                                               uint16_t height,
                                               uint32_t planeMask)
{
    return getImage(c, format, drawable, x, y, width, height, planeMask, 0);
}
xcb_get_image_reply_t *xcb_get_image_reply(xcb_connection_t *c,
                                           xcb_get_image_cookie_t cookie,
                                           xcb_generic_error_t **error)
{
    return xcbCompatTakeReply(c, cookie.sequence, error);
}
int xcb_get_image_sizeof(const void *buffer)
{
    const xcb_get_image_reply_t *reply = buffer;
    uint64_t bytes = reply ? (uint64_t) reply->length * 4u : 0;
    return reply && bytes <= INT_MAX - sizeof(*reply)
               ? (int) sizeof(*reply) + (int) bytes
               : 0;
}
uint8_t *xcb_get_image_data(const xcb_get_image_reply_t *reply)
{
    return reply ? (uint8_t *) (reply + 1) : NULL;
}
int xcb_get_image_data_length(const xcb_get_image_reply_t *reply)
{
    uint64_t bytes = reply ? (uint64_t) reply->length * 4u : 0;
    return bytes <= INT_MAX ? (int) bytes : 0;
}
xcb_generic_iterator_t xcb_get_image_data_end(
    const xcb_get_image_reply_t *reply)
{
    xcb_generic_iterator_t iterator = {0};
    uint64_t bytes = reply ? (uint64_t) reply->length * 4u : 0;
    if (reply && bytes <= INT_MAX - sizeof(*reply)) {
        iterator.data = xcb_get_image_data(reply) + (size_t) bytes;
        iterator.index = sizeof(*reply) + (int) bytes;
    }
    return iterator;
}
