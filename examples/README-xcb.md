# XCB Showcases

These native examples exercise the XCB compatibility library without using
Xlib client calls:

- `xcb-kaleidoscope` combines image upload with XCB line and rectangle
  primitives.
- `xcb-mandelbrot` renders a CPU-generated fractal through `xcb_put_image`.

The XCB compatibility layer is off by default, so these two build with
`make XCB=1 examples` alongside the Xlib clients. Run
`build/examples/xcb-kaleidoscope` or `build/examples/xcb-mandelbrot` and press
any key to exit. Use `make XCB=1 check-xcb-showcases` for a headless,
non-interactive smoke test with SDL's dummy video driver.
