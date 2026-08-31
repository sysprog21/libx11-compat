#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>
#include "../compat/xcb-compat-private.h"

#define CHECK(c, m)                       \
    do {                                  \
        if (!(c)) {                       \
            fprintf(stderr, "%s\n", (m)); \
            return 1;                     \
        }                                 \
    } while (0)

enum { ID_THREAD_COUNT = 8, IDS_PER_THREAD = 4096 };

typedef struct {
    xcb_connection_t *connection;
    uint32_t ids[IDS_PER_THREAD];
} IdThread;

static void *generateIds(void *opaque)
{
    IdThread *thread = opaque;
    for (size_t i = 0; i < IDS_PER_THREAD; i++)
        thread->ids[i] = xcb_generate_id(thread->connection);
    return NULL;
}

static int compareIds(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *) left;
    uint32_t b = *(const uint32_t *) right;
    return (a > b) - (a < b);
}

int main(void)
{
    int screenNumber = -1;
    xcb_connection_t *connection =
        xcb_connect_to_display_with_auth_info(NULL, NULL, &screenNumber);
    CHECK(connection && !xcb_connection_has_error(connection),
          "connection failed");
    CHECK(screenNumber == 0 && xcb_get_file_descriptor(connection) == -1,
          "connection metadata mismatch");
    CHECK(xcb_get_maximum_request_length(connection) == 65535,
          "request limit mismatch");
    const xcb_setup_t *setup = xcb_get_setup(connection);
    CHECK(setup && setup->status == 1 && setup->roots_len == 1,
          "invalid setup");
    static const char expectedVendor[] = "libx11-compat";
    CHECK(xcb_setup_vendor_length(setup) == (int) sizeof(expectedVendor) - 1 &&
              !memcmp(xcb_setup_vendor(setup), expectedVendor,
                      sizeof(expectedVendor) - 1),
          "invalid setup vendor");
    xcb_generic_iterator_t vendorEnd = xcb_setup_vendor_end(setup);
    CHECK(vendorEnd.data ==
                  xcb_setup_vendor(setup) + xcb_setup_vendor_length(setup) &&
              vendorEnd.index ==
                  (int) (sizeof(*setup) + xcb_setup_vendor_length(setup)),
          "invalid setup vendor end");
    CHECK(setup->resource_id_base && setup->resource_id_mask &&
              !(setup->resource_id_base & setup->resource_id_mask) &&
              (setup->resource_id_base | setup->resource_id_mask) <= 0x1fffffff,
          "invalid XID range");
    static const xcb_format_t expectedFormats[] = {
        {.depth = 1, .bits_per_pixel = 1, .scanline_pad = 32},
        {.depth = 16, .bits_per_pixel = 16, .scanline_pad = 32},
        {.depth = 24, .bits_per_pixel = 32, .scanline_pad = 32},
        {.depth = 32, .bits_per_pixel = 32, .scanline_pad = 32},
    };
    CHECK(xcb_setup_pixmap_formats_length(setup) ==
              (int) (sizeof(expectedFormats) / sizeof(expectedFormats[0])),
          "invalid pixmap format count");
    xcb_format_iterator_t formats = xcb_setup_pixmap_formats_iterator(setup);
    CHECK(formats.data == xcb_setup_pixmap_formats(setup),
          "pixmap format accessor mismatch");
    for (size_t i = 0; i < sizeof(expectedFormats) / sizeof(expectedFormats[0]);
         i++) {
        CHECK(formats.rem > 0 &&
                  formats.data->depth == expectedFormats[i].depth &&
                  formats.data->bits_per_pixel ==
                      expectedFormats[i].bits_per_pixel &&
                  formats.data->scanline_pad == expectedFormats[i].scanline_pad,
              "invalid pixmap format");
        xcb_format_next(&formats);
    }
    CHECK(!formats.rem && xcb_format_end(formats).data == formats.data,
          "pixmap format iterator termination failed");

    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    CHECK(screens.rem == 1 && screens.data && screens.data->root,
          "invalid screen");
    CHECK(screens.data->width_in_pixels && screens.data->height_in_pixels,
          "invalid dimensions");
    xcb_depth_iterator_t depths =
        xcb_screen_allowed_depths_iterator(screens.data);
    CHECK(depths.rem == 1 && depths.data->depth == screens.data->root_depth,
          "invalid depth");
    xcb_visualtype_iterator_t visuals = xcb_depth_visuals_iterator(depths.data);
    CHECK(visuals.data == xcb_depth_visuals(depths.data),
          "visual accessor mismatch");
    CHECK(visuals.rem == 1 &&
              visuals.data->visual_id == screens.data->root_visual,
          "invalid visual");
    xcb_generic_iterator_t visualEnd = xcb_visualtype_end(visuals);
    xcb_generic_iterator_t depthEnd = xcb_depth_end(depths);
    xcb_generic_iterator_t screenEnd = xcb_screen_end(screens);
    CHECK(
        !visualEnd.rem && visualEnd.data == xcb_depth_visuals(depths.data) + 1,
        "visual end mismatch");
    CHECK(!depthEnd.rem && depthEnd.data == visualEnd.data,
          "depth end mismatch");
    CHECK(!screenEnd.rem && screenEnd.data == depthEnd.data,
          "screen end mismatch");
    CHECK(xcb_depth_sizeof(depths.data) ==
                  (int) (sizeof(*depths.data) + sizeof(*visuals.data)) &&
              xcb_screen_sizeof(screens.data) ==
                  (int) ((char *) screenEnd.data - (char *) screens.data) &&
              xcb_setup_sizeof(setup) == (int) (8 + setup->length * 4u),
          "setup record size mismatch");
    xcb_visualtype_next(&visuals);
    xcb_depth_next(&depths);
    xcb_screen_next(&screens);
    CHECK(!visuals.rem && !depths.rem && !screens.rem &&
              visuals.data == visualEnd.data && depths.data == depthEnd.data &&
              screens.data == screenEnd.data,
          "iterator termination failed");
    struct {
        xcb_screen_t screen;
        xcb_depth_t firstDepth;
        xcb_visualtype_t firstVisuals[2];
        xcb_depth_t secondDepth;
        xcb_visualtype_t secondVisual;
    } synthetic = {
        .screen.allowed_depths_len = 2,
        .firstDepth.visuals_len = 2,
        .secondDepth.visuals_len = 1,
    };
    CHECK(xcb_depth_sizeof(&synthetic.firstDepth) ==
                  (int) (sizeof(synthetic.firstDepth) +
                         sizeof(synthetic.firstVisuals)) &&
              xcb_screen_sizeof(&synthetic.screen) == (int) sizeof(synthetic),
          "multi-depth record size mismatch");
    xcb_screen_iterator_t syntheticScreen = {
        .data = &synthetic.screen,
        .rem = 1,
        .index = 0,
    };
    xcb_screen_next(&syntheticScreen);
    CHECK(!syntheticScreen.rem &&
              syntheticScreen.data ==
                  (xcb_screen_t *) ((char *) &synthetic + sizeof(synthetic)),
          "multi-depth screen traversal mismatch");
    CHECK(!xcb_depth_sizeof(NULL) && !xcb_screen_sizeof(NULL) &&
              !xcb_setup_sizeof(NULL),
          "NULL record size mismatch");
    uint32_t first = xcb_generate_id(connection),
             second = xcb_generate_id(connection);
    CHECK(first != UINT32_MAX && second > first, "XID allocation failed");
    IdThread idThreads[ID_THREAD_COUNT] = {{0}};
    pthread_t workers[ID_THREAD_COUNT];
    for (size_t i = 0; i < ID_THREAD_COUNT; i++) {
        idThreads[i].connection = connection;
        CHECK(!pthread_create(&workers[i], NULL, generateIds, &idThreads[i]),
              "XID worker creation failed");
    }
    uint32_t *generated =
        malloc(sizeof(*generated) * ID_THREAD_COUNT * IDS_PER_THREAD);
    CHECK(generated, "XID result allocation failed");
    for (size_t i = 0; i < ID_THREAD_COUNT; i++) {
        CHECK(!pthread_join(workers[i], NULL), "XID worker join failed");
        memcpy(generated + i * IDS_PER_THREAD, idThreads[i].ids,
               sizeof(idThreads[i].ids));
    }
    qsort(generated, ID_THREAD_COUNT * IDS_PER_THREAD, sizeof(*generated),
          compareIds);
    for (size_t i = 0; i < ID_THREAD_COUNT * IDS_PER_THREAD; i++) {
        CHECK(generated[i] != UINT32_MAX,
              "concurrent XID allocation returned an error");
        CHECK(generated[i] >= setup->resource_id_base &&
                  ((generated[i] - setup->resource_id_base) &
                   ~setup->resource_id_mask) == 0,
              "generated XID is outside the advertised range");
        CHECK(i == 0 || generated[i] != generated[i - 1],
              "concurrent XID allocation returned a duplicate");
    }
    free(generated);
    xcbCompatSetNextSequence(connection, UINT32_MAX);
    CHECK(xcbCompatNextSequence(connection) == UINT32_MAX,
          "wrap start mismatch");
    CHECK(xcbCompatNextSequence(connection) == (uint64_t) UINT32_MAX + 1,
          "sequence wrap failed");
    xcbCompatSetNextSequence(connection, (uint64_t) UINT32_MAX + 1);
    xcb_void_cookie_t wrappedCookie = xcbCompatVoidCookie(
        connection, XCB_WINDOW, UINT32_C(0xdeadbeef), XCB_DESTROY_WINDOW, 1);
    CHECK(wrappedCookie.sequence == 0, "request cookie did not wrap to zero");
    xcb_generic_error_t *wrappedError =
        xcb_request_check(connection, wrappedCookie);
    CHECK(wrappedError && wrappedError->error_code == XCB_WINDOW &&
              wrappedError->full_sequence == 0,
          "zero sequence cookie was rejected after wrap");
    free(wrappedError);
    xcb_generic_error_t *stored = calloc(1, sizeof(*stored));
    CHECK(stored, "allocation failed");
    stored->error_code = XCB_WINDOW;
    xcbCompatStorePending(connection, 41, NULL, stored);
    xcb_void_cookie_t cookie = {.sequence = 41};
    xcb_generic_error_t *received = xcb_request_check(connection, cookie);
    CHECK(received && received->error_code == XCB_WINDOW,
          "checked error missing");
    free(received);
    CHECK(!xcb_request_check(connection, cookie), "error returned twice");
    xcb_generic_error_t *atomError = calloc(1, sizeof(*atomError));
    CHECK(atomError, "atom error allocation failed");
    atomError->error_code = XCB_ATOM;
    xcbCompatStorePending(connection, 42, NULL, atomError);
    cookie.sequence = 42;
    received = xcb_request_check(connection, cookie);
    CHECK(received && received->error_code == XCB_ATOM,
          "BadAtom was not returned");
    free(received);
    void *reply = calloc(1, 8);
    CHECK(reply, "reply allocation failed");
    xcbCompatStorePending(connection, 43, reply, NULL);
    CHECK(xcbCompatTakeReply(connection, 43, NULL) == reply,
          "stored reply was not returned");
    free(reply);
    xcbCompatStorePending(connection, 44, calloc(1, 8), NULL);
    xcb_discard_reply(connection, 44);
    xcb_discard_reply(connection, 44);
    uint64_t wide = ((uint64_t) 1 << 32) + 45;
    xcbCompatStorePending(connection, wide, calloc(1, 8), NULL);
    xcb_discard_reply64(connection, wide);
    wide = ((uint64_t) 1 << 32) + 46;
    xcbCompatStorePending(connection, wide, calloc(1, 8), NULL);
    xcbCompatSetNextSequence(connection, wide + 1);
    xcb_discard_reply(connection, 46);
    CHECK(!xcbCompatTakeReply(connection, wide, NULL),
          "32-bit discard did not widen across wrap");

    uint64_t *replacement = malloc(sizeof(*replacement));
    CHECK(replacement, "replacement allocation failed");
    *replacement = 2;
    xcbCompatStorePending(connection, 100, calloc(1, sizeof(*replacement)),
                          NULL);
    xcbCompatStorePending(connection, 100, replacement, NULL);
    replacement = xcbCompatTakeReply(connection, 100, NULL);
    CHECK(replacement && *replacement == 2, "pending replacement mismatch");
    free(replacement);

    enum { PENDING_STRESS_COUNT = 8192 };
    for (uint64_t i = 0; i < PENDING_STRESS_COUNT; i++) {
        uint64_t *value = malloc(sizeof(*value));
        CHECK(value, "pending stress allocation failed");
        *value = i;
        xcbCompatStorePending(connection, 1000 + i, value, NULL);
    }
    for (uint64_t i = PENDING_STRESS_COUNT; i > 0; i--) {
        uint64_t expected = i - 1;
        uint64_t *value = xcbCompatTakeReply(connection, 1000 + expected, NULL);
        CHECK(value && *value == expected, "pending stress lookup mismatch");
        free(value);
    }
    xcb_disconnect(connection);
    CHECK(xcb_connection_has_error(NULL) == XCB_CONN_ERROR,
          "NULL error mismatch");
    puts("test-xcb-setup: ok");
    return 0;
}
