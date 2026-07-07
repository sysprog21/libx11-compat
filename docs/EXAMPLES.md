# Bundled Examples

`examples/` ships real Xlib clients built against the local `libX11-compat.so` so behavior can be exercised end-to-end without a system Xlib.
They double as concrete porting references for the steps in [`PORTING.md`](PORTING.md).
The deterministic UI replay harness that drives them in CI is documented in [`UI-REPLAY.md`](UI-REPLAY.md).

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
| `catclock` | [`examples/catclock.c`](../examples/catclock.c) | KitCat wall clock: swinging tail, tracking eyes, analog hands (MIT/X11, reworked from the xclock cat mode via [barkythedog/catclock](https://github.com/barkythedog/catclock); Motif, Xt, and alarm audio stripped). Composites the cat face on the CPU with `XCreateImage` + `XPutPixel` (the stipple/tile and pixmap clip-mask paths are not honored yet), blits precomputed tail and eye frames with `XCopyPlane` off 1bpp bitmaps (the eye patch from `XCreateBitmapFromData`, the tail line drawn onto a cleared depth-1 `XCreatePixmap`), draws hands with `XFillPolygon` + `XDrawLines`, pins the window size via `XSetWMNormalHints`, and paces the pendulum on a 64-bit `gettimeofday` millisecond clock, stepping from the prior deadline so `select()` wakeups and render time neither speed it up nor stretch the cadence. |
| `mandel` | [`examples/mandel.c`](../examples/mandel.c) | Interactive Mandelbrot viewer (MIT, in-tree). `XCreateImage` + `XPutImage` with a 32-bit ZPixmap raster (buffer ownership transferred to the `XImage`), `ButtonPress` dispatch. |
| `processing` | [`examples/processing.c`](../examples/processing.c) | Standalone Processing-like showcase (MIT, in-tree). Exercises Xlib polygons, arcs, lines, strings, pointer input, key input, and timed redraws. |
| `clipboard` | [`examples/clipboard.c`](../examples/clipboard.c) | Interactive selection conversion probe. Lets the user seed SDL clipboard text, request `TARGETS`, read `UTF8_STRING`, and quit from a small Xlib window. Pass `--once` for a headless `TARGETS` printout. |
| `x11perf` | [`examples/x11perf/`](../examples/x11perf/README.md) | Upstream X.Org `x11perf` benchmark. Imported from `0c3597b6` with only local build glue (a `config.h` shim and one cosmetic output tweak). Self-contained performance harness for the SDL2-backed paths. |
| `glxgears` | [`examples/glxgears.c`](../examples/glxgears.c) | GLX-over-EGL/ANGLE showcase: an in-tree port of Mesa's three-gear `glxgears` scene to GLES2, driven through the in-process GLX layer (`glXCreateContext` / `glXMakeCurrent` / `glXSwapBuffers`). Built and run separately from `make examples`: `make glxgears` for an on-screen window, `make glxgears-headless` for a bounded offscreen readback. Requires the ANGLE provider (`make build-angle` on macOS); see the GLX and OpenGL section of the top-level README. |

The short regression loop used during performance work:

```sh
SDL_VIDEODRIVER=dummy build/examples/x11perf -all -repeat 1 -reps 1
```

A full `x11perf -all` pass auto-calibrates against the default timing window and takes several minutes;
it is not a quick check.
