/*
 * Scaffolding shared by the native XCB showcases: connection setup, a mapped
 * window, a graphics context, the redraw loop and teardown. Each showcase then
 * consists of its paint function alone.
 *
 * Header-only on purpose. The showcases are single-file programs built without
 * a link step of their own, and the scaffolding is not a library anyone else
 * consumes.
 */
#ifndef XCB_SHARED_H
#define XCB_SHARED_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xcb/xcb.h>

enum { SHOWCASE_WIDTH = 640, SHOWCASE_HEIGHT = 480 };
enum { SHOWCASE_STRIPE_HEIGHT = 32 };

/* Returns nonzero on success. Called once before the loop and again on expose.
 */
typedef int (*ShowcasePaint)(xcb_connection_t *connection,
                             xcb_window_t window,
                             xcb_gcontext_t gc,
                             uint8_t depth);

/* Upload a full frame of 32-bit pixels in horizontal stripes. */
static inline void showcasePutImage(xcb_connection_t *connection,
                                    xcb_window_t window,
                                    xcb_gcontext_t gc,
                                    uint8_t depth,
                                    const uint32_t *pixels)
{
    for (unsigned int y = 0; y < SHOWCASE_HEIGHT; y += SHOWCASE_STRIPE_HEIGHT) {
        unsigned int rows = SHOWCASE_HEIGHT - y < SHOWCASE_STRIPE_HEIGHT
                                ? SHOWCASE_HEIGHT - y
                                : SHOWCASE_STRIPE_HEIGHT;
        xcb_put_image(
            connection, XCB_IMAGE_FORMAT_Z_PIXMAP, window, gc, SHOWCASE_WIDTH,
            rows, 0, y, 0, depth,
            (uint32_t) ((size_t) SHOWCASE_WIDTH * rows * sizeof(*pixels)),
            (const uint8_t *) (pixels + (size_t) y * SHOWCASE_WIDTH));
    }
}

/* Drain the queue after a smoke run. The drawing requests are unchecked, so a
 * run that failed every one of them would otherwise still exit successfully.
 */
static inline int showcaseDrainErrors(xcb_connection_t *connection,
                                      const char *name)
{
    xcb_flush(connection);
    xcb_generic_event_t *queued;
    while ((queued = xcb_poll_for_event(connection))) {
        int failed = (queued->response_type & 0x7f) == 0;
        uint8_t code = ((xcb_generic_error_t *) queued)->error_code;
        free(queued);
        if (failed) {
            fprintf(stderr, "%s: request failed (%u)\n", name, code);
            return 1;
        }
    }
    return 0;
}

static inline int showcaseRun(int argc,
                              char **argv,
                              const char *name,
                              ShowcasePaint paint)
{
    int smoke = argc == 2 && strcmp(argv[1], "--smoke") == 0;
    xcb_connection_t *connection = xcb_connect(NULL, NULL);
    if (!connection || xcb_connection_has_error(connection)) {
        fprintf(stderr, "%s: cannot connect\n", name);
        xcb_disconnect(connection);
        return 1;
    }
    xcb_screen_t *screen =
        xcb_setup_roots_iterator(xcb_get_setup(connection)).data;

    /* The pixel buffers are packed 32 bits per pixel, so say so rather than
     * uploading a mis-strided image on a screen that wants something else.
     */
    if (screen->root_depth != 24 && screen->root_depth != 32) {
        fprintf(stderr, "%s: needs a 24 or 32 bit screen, got %u\n", name,
                screen->root_depth);
        xcb_disconnect(connection);
        return 1;
    }
    xcb_window_t window = xcb_generate_id(connection);
    uint32_t windowValues[] = {
        screen->black_pixel,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_KEY_PRESS};
    xcb_generic_error_t *error = xcb_request_check(
        connection, xcb_create_window_checked(
                        connection, screen->root_depth, window, screen->root, 0,
                        0, SHOWCASE_WIDTH, SHOWCASE_HEIGHT, 0,
                        XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, windowValues));
    if (error) {
        fprintf(stderr, "%s: create failed (%u)\n", name, error->error_code);
        free(error);
        xcb_disconnect(connection);
        return 1;
    }
    xcb_gcontext_t gc = xcb_generate_id(connection);
    xcb_create_gc(connection, gc, window, 0, NULL);
    xcb_map_window(connection, window);
    if (!paint(connection, window, gc, screen->root_depth)) {
        fprintf(stderr, "%s: image allocation failed\n", name);
        xcb_disconnect(connection);
        return 1;
    }

    int status = smoke ? showcaseDrainErrors(connection, name) : 0;
    while (!smoke) {
        xcb_generic_event_t *event = xcb_wait_for_event(connection);
        if (!event) {
            fprintf(stderr, "%s: connection closed while waiting\n", name);
            status = 1;
            break;
        }
        if ((event->response_type & 0x7f) == XCB_KEY_PRESS) {
            free(event);
            break;
        }
        if ((event->response_type & 0x7f) == XCB_EXPOSE &&
            !paint(connection, window, gc, screen->root_depth)) {
            fprintf(stderr, "%s: repaint failed\n", name);
            free(event);
            status = 1;
            break;
        }
        free(event);
    }
    xcb_free_gc(connection, gc);
    xcb_destroy_window(connection, window);
    xcb_disconnect(connection);
    return status;
}

#endif
