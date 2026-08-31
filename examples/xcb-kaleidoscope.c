#include "xcb-shared.h"

static int paint(xcb_connection_t *connection,
                 xcb_window_t window,
                 xcb_gcontext_t gc,
                 uint8_t depth)
{
    uint32_t *pixels =
        malloc((size_t) SHOWCASE_WIDTH * SHOWCASE_HEIGHT * sizeof(*pixels));
    if (!pixels)
        return 0;
    for (unsigned int y = 0; y < SHOWCASE_HEIGHT; y++) {
        for (unsigned int x = 0; x < SHOWCASE_WIDTH; x++) {
            unsigned int dx = x > SHOWCASE_WIDTH / 2 ? x - SHOWCASE_WIDTH / 2
                                                     : SHOWCASE_WIDTH / 2 - x;
            unsigned int dy = y > SHOWCASE_HEIGHT / 2 ? y - SHOWCASE_HEIGHT / 2
                                                      : SHOWCASE_HEIGHT / 2 - y;
            unsigned int r = (dx * 3 + dy) & 0xff;
            unsigned int g = (dx + dy * 3) & 0xff;
            unsigned int b = ((dx ^ dy) * 2) & 0xff;
            pixels[(size_t) y * SHOWCASE_WIDTH + x] = (r << 16) | (g << 8) | b;
        }
    }
    showcasePutImage(connection, window, gc, depth, pixels);
    free(pixels);

    const xcb_segment_t rays[] = {
        {SHOWCASE_WIDTH / 2, SHOWCASE_HEIGHT / 2, 0, 0},
        {SHOWCASE_WIDTH / 2, SHOWCASE_HEIGHT / 2, SHOWCASE_WIDTH - 1, 0},
        {SHOWCASE_WIDTH / 2, SHOWCASE_HEIGHT / 2, SHOWCASE_WIDTH - 1,
         SHOWCASE_HEIGHT - 1},
        {SHOWCASE_WIDTH / 2, SHOWCASE_HEIGHT / 2, 0, SHOWCASE_HEIGHT - 1},
    };
    const uint32_t white = 0x00ffffff;
    xcb_change_gc(connection, gc, XCB_GC_FOREGROUND, &white);
    xcb_poly_segment(connection, window, gc, sizeof(rays) / sizeof(rays[0]),
                     rays);
    for (unsigned int inset = 24; inset < 220; inset += 24) {
        xcb_rectangle_t rectangle = {inset, inset, SHOWCASE_WIDTH - 2 * inset,
                                     SHOWCASE_HEIGHT - 2 * inset};
        xcb_poly_rectangle(connection, window, gc, 1, &rectangle);
    }
    xcb_flush(connection);
    return 1;
}

int main(int argc, char **argv)
{
    return showcaseRun(argc, argv, "xcb-kaleidoscope", paint);
}
