#include "xcb-shared.h"

enum { MAX_ITERATIONS = 96 };

static uint32_t color(unsigned int iteration)
{
    if (iteration == MAX_ITERATIONS)
        return 0;
    unsigned int shade = iteration * 255 / MAX_ITERATIONS;
    return ((shade * 3 & 0xff) << 16) | ((shade * 7 & 0xff) << 8) |
           (255 - shade);
}

static int paint(xcb_connection_t *connection,
                 xcb_window_t window,
                 xcb_gcontext_t gc,
                 uint8_t depth)
{
    uint32_t *pixels =
        malloc((size_t) SHOWCASE_WIDTH * SHOWCASE_HEIGHT * sizeof(*pixels));
    if (!pixels)
        return 0;
    for (unsigned int py = 0; py < SHOWCASE_HEIGHT; py++) {
        double ci = (double) py / SHOWCASE_HEIGHT * 2.4 - 1.2;
        for (unsigned int px = 0; px < SHOWCASE_WIDTH; px++) {
            double cr = (double) px / SHOWCASE_WIDTH * 3.2 - 2.2;
            double zr = 0.0, zi = 0.0;
            unsigned int iteration = 0;
            while (zr * zr + zi * zi <= 4.0 && iteration < MAX_ITERATIONS) {
                double next = zr * zr - zi * zi + cr;
                zi = 2.0 * zr * zi + ci;
                zr = next;
                iteration++;
            }
            pixels[(size_t) py * SHOWCASE_WIDTH + px] = color(iteration);
        }
    }
    showcasePutImage(connection, window, gc, depth, pixels);
    free(pixels);
    xcb_flush(connection);
    return 1;
}

int main(int argc, char **argv)
{
    return showcaseRun(argc, argv, "xcb-mandelbrot", paint);
}
