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
#include "../src/resource-types.h"
#include "../src/window.h"

static _Atomic int failNextReplyAllocation;

void xcbCompatFailNextReplyAllocationForTest(void)
{
    atomic_store_explicit(&failNextReplyAllocation, 1, memory_order_release);
}

static int isDrawable(xcb_drawable_t drawable)
{
    return IS_TYPE(drawable, DRAWABLE);
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
