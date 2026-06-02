# libx11-compat

`libx11-compat` is an in-process implementation of the
[X Window System](https://en.wikipedia.org/wiki/X_Window_System) client
library (Xlib) layered on top of [SDL2](https://www.libsdl.org/), SDL2_ttf,
and pixman. It is intended as a transition tool: existing Xlib clients can
keep their source unchanged while running on platforms where no X server is
available — Android, headless CI, macOS without XQuartz, Wayland-only
sessions, and similar environments.

The library is not a re-implementation of the X11 wire protocol and does
not replace a real X server. Its goal is to keep legacy Xlib code building
and running while it is being migrated to a different toolkit or display
stack.

## Building

The build is Makefile-based, organized as small `mk/` fragments. The
required dependencies are SDL2, SDL2_ttf, and pixman.

```sh
make
make check
```

Verbose diagnostics for unimplemented or fall-back paths are gated behind a
build flag:

```sh
make CFLAGS_EXTRA=-DDEBUG_LIBX11_COMPAT
```

The output artifact is `build/libX11-compat.so`. Clients link against it
the same way they would link against the system `libX11.so`.

## Examples

`examples/` bundles real Xlib clients built against the local
`libX11-compat.so`:

```sh
make examples
SDL_VIDEODRIVER=dummy build/examples/2048   # omit env var for a visible window
```

The bundle covers a 2048 game, a paint demo, Conway's Game of Life, an
analog clock, an interactive Mandelbrot viewer, a single-runner Processing-style
showcase, an SDL-backed clipboard `TARGETS` probe, and the upstream X.Org
`x11perf` benchmark. See
[`docs/EXAMPLES.md`](docs/EXAMPLES.md) for the API
each example exercises.

## Coverage and Compatibility

The library exports 615 public Xlib symbols listed in
[`tests/api-symbols.txt`](tests/api-symbols.txt), covering window, drawable,
GC, pixmap, image, event, input, atom, property, color, font, cursor, and
region subsystems for the cases that real Xlib clients exercise. Selection,
property, and resource-manager support is partial; MIT-SHM is a thin
wrapper over the regular image path; GLX, Xcms, and input methods are
intentionally stubbed.

See [`docs/COVERAGE.md`](docs/COVERAGE.md) for the per-subsystem status
table, surface notes (window manager hints, raster ops, mouse wheel
mapping), and compatibility limits.

## Porting an Existing Xlib Client

Most Xlib sources need no edits — porting is a build-system and
runtime-environment exercise. The general shape:

1. Link the application against `build/libX11-compat.so` plus SDL2,
   SDL2_ttf, and pixman, replacing the system `-lX11`.
2. Keep the existing Xlib source unchanged.
3. Drive the event loop with `XPending` / `XNextEvent` and
   `ConnectionNumber` + `select()`; the library pumps SDL internally.
4. Use `WM_DELETE_WINDOW` via `XSetWMProtocols` so the SDL close button
   produces a clean shutdown.
5. Run with `SDL_VIDEODRIVER=dummy` for headless CI.

See [`docs/PORTING.md`](docs/PORTING.md) for the full guide, including
code snippets keyed to the bundled examples and a checklist of cases where
the library is a stepping stone rather than a destination (GLX, real XIM,
ICC color management, cross-process selection).

## Contributing

Reports of incorrect Xlib semantics, missing entry points that block real
applications, and patches that tighten coverage against the manifest in
[`tests/api-symbols.txt`](tests/api-symbols.txt) are particularly useful.

## License

`libX11-compat` is available under a permissive
[MIT](https://opensource.org/license/mit)-style license. Use of this source
code is governed by an MIT license that can be found in the
[LICENSE](LICENSE) file.
