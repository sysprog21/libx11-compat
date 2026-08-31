#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <xcb/xcb.h>

#define CHECK(c, m)                     \
    do {                                \
        if (!(c)) {                     \
            fprintf(stderr, "%s\n", m); \
            return 1;                   \
        }                               \
    } while (0)

static uint64_t checksum(const xcb_get_image_reply_t *reply)
{
    const uint32_t *pixels = (const uint32_t *) xcb_get_image_data(reply);
    size_t count = (size_t) xcb_get_image_data_length(reply) / 4;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < count; i++) {
        uint32_t pixel = pixels[i] & 0x00ffffff;
        for (int byte = 0; byte < 3; byte++) {
            hash ^= (pixel >> (byte * 8)) & 0xff;
            hash *= UINT64_C(1099511628211);
        }
    }
    return hash;
}

int main(void)
{
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    CHECK(c && !xcb_connection_has_error(c), "connect");
    CHECK(!xcb_get_image_data(NULL) && !xcb_get_image_data_length(NULL) &&
              !xcb_get_image_data_end(NULL).data && !xcb_get_image_sizeof(NULL),
          "NULL image accessor");
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_pixmap_t source = xcb_generate_id(c), target = xcb_generate_id(c);
    CHECK(!xcb_request_check(
              c, xcb_create_pixmap_checked(c, screen->root_depth, source,
                                           screen->root, 16, 16)),
          "create source");
    CHECK(!xcb_request_check(
              c, xcb_create_pixmap_checked(c, screen->root_depth, target,
                                           screen->root, 16, 16)),
          "create target");
    xcb_gcontext_t gc = xcb_generate_id(c), copied = xcb_generate_id(c);
    uint32_t values[] = {0x00ffffff, 0x00000000};
    uint32_t mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND;
    CHECK(!xcb_request_check(
              c, xcb_create_gc_checked(c, gc, source, mask, values)),
          "create gc");
    CHECK(!xcb_request_check(
              c, xcb_create_gc_checked(c, copied, target, mask, values)),
          "create copied gc");
    xcb_point_t onePoint = {0, 0};
    xcb_generic_error_t *error = xcb_request_check(
        c, xcb_poly_point_checked(c, XCB_COORD_MODE_ORIGIN, source, gc, 65536,
                                  &onePoint));
    CHECK(error && error->error_code == XCB_LENGTH &&
              error->major_code == XCB_POLY_POINT,
          "oversized checked primitive request");
    free(error);
    xcb_void_cookie_t uncheckedPrimitive =
        xcb_poly_point(c, XCB_COORD_MODE_ORIGIN, source, gc, 65536, &onePoint);
    error = (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(error && error->error_code == XCB_LENGTH &&
              error->major_code == XCB_POLY_POINT &&
              error->full_sequence == uncheckedPrimitive.sequence,
          "oversized unchecked primitive request");
    free(error);
    uint32_t onePixel = 0;
    error = xcb_request_check(
        c, xcb_put_image_checked(c, XCB_IMAGE_FORMAT_Z_PIXMAP, source, gc,
                                 UINT16_MAX, UINT16_MAX, 0, 0, 0,
                                 screen->root_depth, UINT32_MAX,
                                 (const uint8_t *) &onePixel));
    CHECK(error && error->error_code == XCB_LENGTH &&
              error->major_code == XCB_PUT_IMAGE,
          "put-image overflowing payload");
    free(error);
    error = xcb_request_check(
        c, xcb_put_image_checked(c, XCB_IMAGE_FORMAT_Z_PIXMAP, source, gc, 1, 1,
                                 0, 0, 0, screen->root_depth, UINT32_MAX,
                                 (const uint8_t *) &onePixel));
    CHECK(error && error->error_code == XCB_LENGTH &&
              error->major_code == XCB_PUT_IMAGE,
          "put-image oversized request");
    free(error);
    error = xcb_request_check(
        c, xcb_put_image_checked(c, XCB_IMAGE_FORMAT_Z_PIXMAP, source, gc, 0, 1,
                                 0, 0, 0, screen->root_depth, 0, NULL));
    CHECK(error && error->error_code == XCB_VALUE &&
              error->major_code == XCB_PUT_IMAGE,
          "put-image zero width");
    free(error);
    uint32_t pixels[16 * 16];
    for (unsigned int y = 0; y < 16; y++)
        for (unsigned int x = 0; x < 16; x++)
            pixels[y * 16 + x] = ((x * 13) << 16) | ((y * 11) << 8) | (x + y);
    CHECK(!xcb_request_check(
              c, xcb_put_image_checked(c, XCB_IMAGE_FORMAT_Z_PIXMAP, source, gc,
                                       16, 16, 0, 0, 0, screen->root_depth,
                                       sizeof(pixels), (uint8_t *) pixels)),
          "put image");
    xcb_copy_area(c, source, target, copied, 0, 0, 0, 0, 16, 16);
    xcb_no_exposure_event_t *noExpose =
        (xcb_no_exposure_event_t *) xcb_poll_for_queued_event(c);
    CHECK(noExpose && noExpose->response_type == XCB_NO_EXPOSURE &&
              noExpose->drawable == target &&
              noExpose->major_opcode == XCB_COPY_AREA &&
              noExpose->minor_opcode == 0,
          "copy-area no-exposure event");
    free(noExpose);
    xcb_get_image_reply_t *image =
        xcb_get_image_reply(c,
                            xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP, target,
                                          0, 0, 16, 16, UINT32_MAX),
                            NULL);
    CHECK(image && xcb_get_image_data_length(image) == 16 * 16 * 4,
          "get image");
    xcb_generic_iterator_t imageEnd = xcb_get_image_data_end(image);
    CHECK(xcb_get_image_sizeof(image) == (int) (sizeof(*image) + 16 * 16 * 4) &&
              imageEnd.data == xcb_get_image_data(image) +
                                   xcb_get_image_data_length(image) &&
              imageEnd.index == xcb_get_image_sizeof(image),
          "image layout");
    CHECK(image->visual == XCB_NONE, "pixmap image visual");
    uint64_t hash = checksum(image);
    CHECK(hash == UINT64_C(0x293005f544a302e3), "drawing checksum mismatch");
    printf("drawing-checksum=%016" PRIx64 "\n", hash);
    free(image);
    xcb_copy_area(c, source, target, copied, -2, 0, 0, 0, 4, 4);
    xcb_graphics_exposure_event_t *graphicsExpose =
        (xcb_graphics_exposure_event_t *) xcb_poll_for_queued_event(c);
    CHECK(graphicsExpose &&
              graphicsExpose->response_type == XCB_GRAPHICS_EXPOSURE &&
              graphicsExpose->drawable == target && graphicsExpose->x == 0 &&
              graphicsExpose->y == 0 && graphicsExpose->width == 2 &&
              graphicsExpose->height == 4 && graphicsExpose->count == 0 &&
              graphicsExpose->major_opcode == XCB_COPY_AREA &&
              graphicsExpose->minor_opcode == 0,
          "copy-area graphics-exposure event");
    free(graphicsExpose);
    xcb_get_image_cookie_t uncheckedImage =
        xcb_get_image_unchecked(c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                UINT32_C(0xdeadbeef), 0, 0, 1, 1, UINT32_MAX);
    error = NULL;
    image = xcb_get_image_reply(c, uncheckedImage, &error);
    CHECK(!image && !error, "unchecked image error bypasses reply API");
    xcb_generic_error_t *queuedError =
        (xcb_generic_error_t *) xcb_poll_for_queued_event(c);
    CHECK(queuedError && queuedError->response_type == 0 &&
              queuedError->error_code == XCB_DRAWABLE &&
              queuedError->resource_id == UINT32_C(0xdeadbeef) &&
              queuedError->full_sequence == uncheckedImage.sequence,
          "unchecked image error is queued");
    free(queuedError);
    xcb_pixmap_t narrowPixmap = xcb_generate_id(c);
    xcb_gcontext_t narrowGc = xcb_generate_id(c);
    CHECK(!xcb_request_check(c, xcb_create_pixmap_checked(c, 16, narrowPixmap,
                                                          screen->root, 1, 1)),
          "create 16-bit pixmap");
    CHECK(!xcb_request_check(
              c, xcb_create_gc_checked(c, narrowGc, narrowPixmap, 0, NULL)),
          "create 16-bit pixmap GC");
    const uint8_t narrowPixels[4] = {0};
    CHECK(!xcb_request_check(
              c, xcb_put_image_checked(c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                       narrowPixmap, narrowGc, 1, 1, 0, 0, 0,
                                       16, sizeof(narrowPixels), narrowPixels)),
          "put padded 16-bit image");
    error = xcb_request_check(
        c, xcb_put_image_checked(c, XCB_IMAGE_FORMAT_Z_PIXMAP, narrowPixmap,
                                 narrowGc, 1, 1, 0, 0, 0, 8,
                                 sizeof(narrowPixels), narrowPixels));
    CHECK(error && error->error_code == XCB_MATCH &&
              error->resource_id == narrowPixmap &&
              error->major_code == XCB_PUT_IMAGE,
          "put image depth BadMatch");
    free(error);
    image =
        xcb_get_image_reply(c,
                            xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                          narrowPixmap, 0, 0, 1, 1, UINT32_MAX),
                            NULL);
    CHECK(image && image->depth == 16 && image->visual == XCB_NONE &&
              xcb_get_image_data_length(image) == sizeof(narrowPixels),
          "16-bit image wire stride");
    free(image);
    xcb_get_image_reply_t oversizedImage = {.length = UINT32_MAX};
    CHECK(!xcb_get_image_sizeof(&oversizedImage) &&
              !xcb_get_image_data_length(&oversizedImage) &&
              !xcb_get_image_data_end(&oversizedImage).data,
          "oversized image layout");
    error = NULL;
    image =
        xcb_get_image_reply(c,
                            xcb_get_image(c, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                          narrowPixmap, 0, 0, 0, 1, UINT32_MAX),
                            &error);
    CHECK(!image && error && error->error_code == XCB_VALUE &&
              error->resource_id == 0 && error->major_code == XCB_GET_IMAGE,
          "get image zero-width BadValue");
    free(error);
    xcb_point_t narrowPoint = {0, 0};
    error = xcb_request_check(
        c, xcb_poly_point_checked(c, XCB_COORD_MODE_ORIGIN, narrowPixmap, gc, 1,
                                  &narrowPoint));
    CHECK(error && error->error_code == XCB_MATCH &&
              error->resource_id == narrowPixmap &&
              error->major_code == XCB_POLY_POINT,
          "cross-depth drawing BadMatch");
    free(error);
    error = xcb_request_check(
        c, xcb_copy_gc_checked(c, narrowGc, gc, XCB_GC_FOREGROUND));
    CHECK(error && error->error_code == XCB_MATCH && error->resource_id == gc &&
              error->major_code == XCB_COPY_GC,
          "cross-depth copy-GC BadMatch");
    free(error);
    uint32_t narrowTile = narrowPixmap;
    error = xcb_request_check(
        c, xcb_change_gc_checked(c, gc, XCB_GC_TILE, &narrowTile));
    CHECK(error && error->error_code == XCB_MATCH &&
              error->resource_id == narrowPixmap &&
              error->major_code == XCB_CHANGE_GC,
          "cross-depth GC tile BadMatch");
    free(error);
    error =
        xcb_request_check(c, xcb_copy_area_checked(c, source, narrowPixmap,
                                                   narrowGc, 0, 0, 0, 0, 1, 1));
    CHECK(error && error->error_code == XCB_MATCH &&
              error->resource_id == narrowPixmap &&
              error->major_code == XCB_COPY_AREA,
          "cross-depth copy-area BadMatch");
    free(error);
    xcb_free_gc(c, narrowGc);
    xcb_free_pixmap(c, narrowPixmap);
    xcb_point_t points[] = {{1, 1}, {2, 3}, {4, 2}};
    xcb_poly_point(c, XCB_COORD_MODE_ORIGIN, source, gc, 3, points);
    xcb_poly_line(c, XCB_COORD_MODE_ORIGIN, source, gc, 3, points);
    xcb_segment_t segment[] = {{0, 15, 15, 0}};
    xcb_poly_segment(c, source, gc, 1, segment);
    xcb_rectangle_t rectangle[] = {{3, 3, 8, 7}};
    xcb_poly_rectangle(c, source, gc, 1, rectangle);
    xcb_arc_t arc[] = {{2, 2, 10, 10, 0, 180 * 64}};
    xcb_poly_arc(c, source, gc, 1, arc);
    xcb_copy_plane(c, source, target, copied, 0, 0, 0, 0, 8, 8, 1);
    uint32_t red = 0x00ff0000;
    xcb_change_gc(c, copied, XCB_GC_FOREGROUND, &red);
    xcb_copy_gc(c, copied, gc, XCB_GC_FOREGROUND);
    xcb_point_t redPoint = {15, 15};
    xcb_poly_point(c, XCB_COORD_MODE_ORIGIN, target, gc, 1, &redPoint);
    xcb_image_text_8(c, 1, target, gc, 0, 10, "A");
    xcb_char2b_t letter = {0, 'B'};
    xcb_image_text_16(c, 1, target, gc, 6, 10, &letter);
    xcb_window_t window = xcb_generate_id(c);
    CHECK(
        !xcb_request_check(c, xcb_create_window_checked(
                                  c, screen->root_depth, window, screen->root,
                                  0, 0, 8, 8, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                                  screen->root_visual, 0, NULL)),
        "create window");
    CHECK(
        !xcb_request_check(c, xcb_clear_area_checked(c, 0, window, 0, 0, 0, 0)),
        "clear area");
    error =
        xcb_request_check(c, xcb_free_pixmap_checked(c, UINT32_C(0xdeadbeef)));
    CHECK(error && error->error_code == XCB_PIXMAP, "BadPixmap");
    free(error);
    xcb_pixmap_t invalidPixmap = xcb_generate_id(c);
    error = xcb_request_check(
        c, xcb_create_pixmap_checked(c, screen->root_depth, invalidPixmap,
                                     screen->root, 0, 1));
    CHECK(error && error->error_code == XCB_VALUE && error->resource_id == 0 &&
              error->major_code == XCB_CREATE_PIXMAP,
          "pixmap BadValue");
    free(error);
    CHECK(!xcb_request_check(
              c, xcb_create_pixmap_checked(c, screen->root_depth, invalidPixmap,
                                           screen->root, 1, 1)),
          "reuse pixmap ID after error");
    error = xcb_request_check(
        c, xcb_create_pixmap_checked(c, screen->root_depth, invalidPixmap,
                                     screen->root, 1, 1));
    CHECK(error && error->error_code == XCB_ID_CHOICE &&
              error->resource_id == invalidPixmap &&
              error->major_code == XCB_CREATE_PIXMAP,
          "pixmap BadIDChoice metadata");
    free(error);
    xcb_free_pixmap(c, invalidPixmap);
    xcb_gcontext_t invalidGc = xcb_generate_id(c);
    error = xcb_request_check(
        c, xcb_create_gc_checked(c, invalidGc, UINT32_C(0xdeadbeef), 0, NULL));
    CHECK(error && error->error_code == XCB_DRAWABLE &&
              error->resource_id == UINT32_C(0xdeadbeef) &&
              error->major_code == XCB_CREATE_GC,
          "GC BadDrawable");
    free(error);
    CHECK(!xcb_request_check(
              c, xcb_create_gc_checked(c, invalidGc, target, 0, NULL)),
          "reuse GC ID after error");
    error = xcb_request_check(
        c, xcb_create_gc_checked(c, invalidGc, target, 0, NULL));
    CHECK(error && error->error_code == XCB_ID_CHOICE &&
              error->resource_id == invalidGc &&
              error->major_code == XCB_CREATE_GC,
          "GC BadIDChoice resource");
    free(error);
    xcb_free_gc(c, invalidGc);
    xcb_gcontext_t invalidMaskGc = xcb_generate_id(c);
    uint32_t invalidMaskValue = 0;
    error = xcb_request_check(
        c, xcb_create_gc_checked(c, invalidMaskGc, target, UINT32_C(1) << 31,
                                 &invalidMaskValue));
    CHECK(error && error->error_code == XCB_VALUE &&
              error->major_code == XCB_CREATE_GC,
          "create GC unknown mask");
    free(error);
    CHECK(!xcb_request_check(
              c, xcb_create_gc_checked(c, invalidMaskGc, target, 0, NULL)),
          "reuse GC ID after unknown mask");
    error = xcb_request_check(
        c, xcb_change_gc_checked(c, invalidMaskGc, UINT32_C(1) << 31,
                                 &invalidMaskValue));
    CHECK(error && error->error_code == XCB_VALUE &&
              error->resource_id == (UINT32_C(1) << 31) &&
              error->major_code == XCB_CHANGE_GC,
          "change GC unknown mask");
    free(error);
    uint32_t invalidFunction = 99;
    error = xcb_request_check(
        c, xcb_change_gc_checked(c, invalidMaskGc, XCB_GC_FUNCTION,
                                 &invalidFunction));
    CHECK(error && error->error_code == XCB_VALUE &&
              error->resource_id == invalidFunction &&
              error->major_code == XCB_CHANGE_GC,
          "change GC invalid function");
    free(error);
    uint32_t invalidTile = screen->root;
    error = xcb_request_check(
        c, xcb_change_gc_checked(c, invalidMaskGc, XCB_GC_TILE, &invalidTile));
    CHECK(error && error->error_code == XCB_PIXMAP &&
              error->resource_id == screen->root &&
              error->major_code == XCB_CHANGE_GC,
          "change GC tile BadPixmap");
    free(error);
    uint32_t invalidFont = screen->root;
    error = xcb_request_check(
        c, xcb_change_gc_checked(c, invalidMaskGc, XCB_GC_FONT, &invalidFont));
    CHECK(error && error->error_code == XCB_FONT &&
              error->resource_id == screen->root &&
              error->major_code == XCB_CHANGE_GC,
          "change GC BadFont");
    free(error);
    error = xcb_request_check(
        c, xcb_copy_area_checked(c, source, UINT32_C(0xdeadbeef), gc, 0, 0, 0,
                                 0, 1, 1));
    CHECK(error && error->error_code == XCB_DRAWABLE &&
              error->resource_id == UINT32_C(0xdeadbeef) &&
              error->major_code == XCB_COPY_AREA,
          "copy-area destination BadDrawable metadata");
    free(error);
    error = xcb_request_check(
        c, xcb_copy_gc_checked(c, invalidMaskGc, gc, UINT32_C(1) << 31));
    CHECK(error && error->error_code == XCB_VALUE &&
              error->major_code == XCB_COPY_GC,
          "copy GC unknown mask");
    free(error);
    xcb_free_gc(c, invalidMaskGc);
    error = xcb_request_check(
        c, xcb_poly_point_checked(c, XCB_COORD_MODE_ORIGIN, target,
                                  UINT32_C(0xdeadbeef), 1, &redPoint));
    CHECK(error && error->error_code == XCB_G_CONTEXT, "BadGC");
    free(error);
    error = xcb_request_check(
        c, xcb_poly_point_checked(c, XCB_COORD_MODE_ORIGIN, target, gc,
                                  UINT32_MAX, &redPoint));
    CHECK(error && error->error_code == XCB_LENGTH &&
              error->major_code == XCB_POLY_POINT,
          "drawing oversized item count");
    free(error);
    error = xcb_request_check(
        c, xcb_copy_plane_checked(c, source, target, gc, 0, 0, 0, 0, 1, 1, 3));
    CHECK(error && error->error_code == XCB_VALUE, "copy-plane BadValue");
    free(error);

    /* A depth-16 source makes the out-of-range plane deterministic whatever the
     * screen depth is.
     */
    xcb_pixmap_t shallowSource = xcb_generate_id(c);
    CHECK(!xcb_request_check(c, xcb_create_pixmap_checked(c, 16, shallowSource,
                                                          screen->root, 4, 4)),
          "create shallow copy-plane source");
    error = xcb_request_check(
        c, xcb_copy_plane_checked(c, shallowSource, target, gc, 0, 0, 0, 0, 1,
                                  1, UINT32_C(1) << 20));
    CHECK(error && error->error_code == XCB_VALUE &&
              error->major_code == XCB_COPY_PLANE,
          "copy-plane above source depth");
    free(error);
    CHECK(!xcb_request_check(
              c, xcb_copy_plane_checked(c, shallowSource, target, gc, 0, 0, 0,
                                        0, 1, 1, UINT32_C(1) << 15)),
          "copy-plane within source depth");
    xcb_free_pixmap(c, shallowSource);
    xcb_pixmap_t oddDepthPixmap = xcb_generate_id(c);
    error = xcb_request_check(
        c, xcb_create_pixmap_checked(c, 7, oddDepthPixmap, screen->root, 1, 1));
    CHECK(error && error->error_code == XCB_VALUE &&
              error->major_code == XCB_CREATE_PIXMAP,
          "create pixmap unsupported depth");
    free(error);
    xcb_get_image_cookie_t outsideImage = xcb_get_image(
        c, XCB_IMAGE_FORMAT_Z_PIXMAP, source, 8, 8, 16, 16, UINT32_MAX);
    error = NULL;
    CHECK(!xcb_get_image_reply(c, outsideImage, &error) && error &&
              error->error_code == XCB_MATCH,
          "image outside drawable");
    free(error);
    xcb_free_gc(c, copied);
    xcb_free_gc(c, gc);
    xcb_free_pixmap(c, target);
    xcb_free_pixmap(c, source);
    xcb_destroy_window(c, window);

    xcb_gcontext_t orphanGc = xcb_generate_id(c);
    CHECK(!xcb_request_check(
              c, xcb_create_gc_checked(c, orphanGc, screen->root, 0, NULL)),
          "create disconnect-owned GC");
    xcb_connection_t *survivor = xcb_connect(NULL, NULL);
    CHECK(survivor && !xcb_connection_has_error(survivor),
          "connect GC survivor");
    xcb_disconnect(c);
    xcb_point_t survivorPoint = {0, 0};
    error = xcb_request_check(
        survivor,
        xcb_poly_point_checked(
            survivor, XCB_COORD_MODE_ORIGIN,
            xcb_setup_roots_iterator(xcb_get_setup(survivor)).data->root,
            orphanGc, 1, &survivorPoint));
    CHECK(error && error->error_code == XCB_G_CONTEXT,
          "disconnect removes owned GC entries");
    free(error);
    xcb_disconnect(survivor);
    return 0;
}
