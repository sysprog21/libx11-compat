# Bundled Examples

`examples/` ships real Xlib clients built against the local
`libX11-compat.so` so behavior can be exercised end-to-end without a system
Xlib. They double as concrete porting references for the steps in
[`PORTING.md`](PORTING.md).

```sh
make examples
SDL_VIDEODRIVER=dummy build/examples/2048   # omit env var for a visible window
```

| Example | Source | What it exercises |
|---------|--------|-------------------|
| `2048`   | [`examples/2048.c`](../examples/2048.c)     | 2048 sliding-tile game (MIT, in-tree). Window creation, GC, event loop, `XDrawString` / `XDrawRectangle`, keysym lookup. |
| `paint`  | [`examples/paint.c`](../examples/paint.c)   | Minimalist paint program (MIT, in-tree). Pointer motion plus drag, multiple GCs, `XAllocColor`, palette switching via `XSetForeground` / `XFillRectangle`, `XDrawLine`, `WM_DELETE_WINDOW` through `XSetWMProtocols`. |
| `life`   | [`examples/life.c`](../examples/life.c)     | Conway's Game of Life (MIT, in-tree). Pixmap double buffering (`XCreatePixmap` + `XCopyArea`), bulk `XFillRectangle`, `select()`-driven timed redraws via `ConnectionNumber`, `WM_DELETE_WINDOW`. |
| `clock`  | [`examples/clock.c`](../examples/clock.c)   | Analog clock (MIT, in-tree). `XDrawArc` / `XFillArc`, multiple GCs with `XSetLineAttributes`, one-second redraw cadence driven by `gettimeofday`. |
| `mandel` | [`examples/mandel.c`](../examples/mandel.c) | Interactive Mandelbrot viewer (MIT, in-tree). `XCreateImage` + `XPutImage` with a 32-bit ZPixmap raster (buffer ownership transferred to the `XImage`), `ButtonPress` dispatch. |
| `processing` | [`examples/processing.c`](../examples/processing.c) | Standalone Processing-like showcase (MIT, in-tree). Exercises Xlib polygons, arcs, lines, strings, pointer input, key input, and timed redraws. |
| `x11perf` | [`examples/x11perf/`](../examples/x11perf/README.md) | Upstream X.Org `x11perf` benchmark. Imported from `0c3597b6` with only local build glue (a `config.h` shim and one cosmetic output tweak). Self-contained performance harness for the SDL2-backed paths. |

The short regression loop used during performance work:

```sh
SDL_VIDEODRIVER=dummy build/examples/x11perf -all -repeat 1 -reps 1
```

A full `x11perf -all` pass auto-calibrates against the default timing
window and takes several minutes; it is not a quick check.
