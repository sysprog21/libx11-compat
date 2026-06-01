# Porting an Xlib Client to libx11-compat

`libx11-compat` is designed so that existing Xlib sources need no edits.
Porting an application is primarily a build-system and runtime-environment
exercise. The bundled examples in [`examples/`](../examples) are deliberately
chosen to illustrate the common cases.

See [`COVERAGE.md`](COVERAGE.md) for the supported API surface and
[`EXAMPLES.md`](EXAMPLES.md) for what each example exercises.

## 1. Link the application against `libX11-compat.so`

Replace the system `-lX11` link with the local artifact. The example build
in `mk/examples.mk` shows the pattern:

```make
$(OUT)/examples/%: examples/%.c $(TARGET)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(TARGET) $(LDLIBS) -o $@
```

Here `$(TARGET)` is `build/libX11-compat.so` and `$(LDLIBS)` brings in
SDL2, SDL2_ttf, and pixman. For an autotools or CMake project, point
`X11_LIBRARIES` (or the equivalent variable) at the local shared object and
add the SDL2 dependencies to the link line. No source-level changes are
required.

## 2. Keep the existing Xlib code unchanged

`examples/2048.c` is the canonical demonstration: it is largely imported
from its upstream X11 project with only minor local edits, and builds
without modification. The upstream `x11perf` sources under
`examples/x11perf/` are also imported with only build glue (a local
`config.h` shim replacing the autotools configure step and a one-line drop
of a trailing blank in `ProcessTest()`). If an application currently builds
against system Xlib, it should build against `libx11-compat` with the same
source.

## 3. Drive the event loop the way Xlib already drives it

`XNextEvent`, `XPending`, `XCheckIfEvent`, and the `ConnectionNumber`
hand-off to `select()` all work. `examples/life.c` shows the classic
drain-then-sleep loop with a periodic timer:

```c
int xfd = ConnectionNumber(display);
for (;;) {
    /* Drain and dispatch any pending events without blocking. */
    while (XPending(display)) {
        XEvent event;
        XNextEvent(display, &event);
        handle_event(&event);
    }

    /* Sleep for the remainder of the frame. */
    XFlush(display);
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(xfd, &fds);
    struct timeval tv = { 0, frame_us };
    select(xfd + 1, &fds, NULL, NULL, &tv);
}
```

No SDL event pumping is required from client code — the library pumps SDL
internally and translates events into the Xlib queue.

## 4. Use pixmaps and `XCopyArea` for double buffering

Software-surface rendering benefits from double buffering. `examples/life.c`
allocates a backing pixmap, draws the grid into it, and blits with
`XCopyArea` on each frame. The same pattern works for any client that
currently uses double buffering against a real X server; no change is
needed.

## 5. Use `XCreateImage` / `XPutImage` for client-side rasters

Rasterizers and software renderers that produce a full-frame buffer should
hand it to the library through `XPutImage` rather than per-pixel
`XDrawPoint`. `examples/mandel.c` allocates a 32-bit ZPixmap buffer and
hands ownership to the `XImage` — `XDestroyImage` will free it later:

```c
size_t bytes = (size_t) width * (size_t) height * 4u;  /* check for overflow */
char *pixels = malloc(bytes);
XImage *img = XCreateImage(display, visual, 24, ZPixmap, 0,
                           pixels, width, height, 32, 0);
XPutImage(display, window, gc, img, 0, 0, 0, 0, width, height);
/* Later: XDestroyImage(img) frees the pixels buffer too. */
```

This is the recommended fast path for image clients.

## 6. Handle `WM_DELETE_WINDOW` so the SDL close button works

Because the SDL window owns the host-level close affordance, ICCCM
`WM_DELETE_WINDOW` is the recommended way to detect a close request. The
sequence appears in both `examples/paint.c` and `examples/life.c`:

```c
Atom wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
XSetWMProtocols(display, window, &wm_delete, 1);
```

`ClientMessage` events with `data.l[0] == wm_delete` then signal a clean
exit. This is standard ICCCM behavior; no platform-specific code is needed.
Note that `XGetWMProtocols` is currently a stub, so clients that need to
read back the protocols they set must track them locally.

## 7. Choose between visible and headless modes at runtime

The library honors the standard `SDL_VIDEODRIVER` environment variable.
Setting `SDL_VIDEODRIVER=dummy` runs the application without a visible
window, which is useful for CI, automated screenshot capture, and
performance measurement. All bundled examples support this mode.

```sh
SDL_VIDEODRIVER=dummy build/examples/x11perf -all -repeat 1 -reps 1
```

## 8. Validate against the example suite and `make check`

After wiring up the build, run `make check` to confirm the library and its
in-tree regression coverage are healthy in the target environment. For
end-to-end validation, build the examples and run them — they cover window,
GC, event, pixmap, image, and ICCCM paths in combinations that mirror
common application structure.

## When porting is not enough

Some applications depend on facilities that this library does not provide
because they have no in-process analogue:

- Cross-process selection, drag-and-drop, or write-back to a host clipboard
  beyond the read-only `CLIPBOARD` bridge.
- GLX or any OpenGL-via-X11 surface creation.
- A real, conformant XIM input method server.
- ICC color-managed output through Xcms conversions.
- Wide polygon and multi-arc rendering (`XFillPolygon`, `XDrawArcs`,
  `XFillArcs`) until those entry points are implemented.

For those cases the library is best used as a stepping stone: it keeps the
application running on the new platform while the affected subsystem is
rewritten against a native API (SDL2 directly, the platform's clipboard
service, OpenGL/EGL/Metal, the host IME, and so on).
