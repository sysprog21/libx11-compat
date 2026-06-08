# libx11-compat

`libx11-compat` is an in-process implementation of the [X Window System](https://en.wikipedia.org/wiki/X_Window_System) client library (Xlib) layered on top of [SDL2](https://www.libsdl.org/), SDL2_ttf, and pixman.
It is intended as a transition tool:
existing Xlib clients can keep their source unchanged while running on platforms where no X server is available:
Android, headless CI, macOS without XQuartz, Wayland-only sessions, and similar environments.

The library is not a re-implementation of the X11 wire protocol and does not replace a real X server.
Its goal is to keep legacy Xlib code building and running while it is being migrated to a different toolkit or display stack.

## Building

The build is Makefile-based, organized as small `mk/` fragments.
The required dependencies are SDL2, SDL2_ttf, and pixman.

```sh
make
make check
```

Verbose diagnostics for unimplemented or fall-back paths are gated behind a build flag:

```sh
make CFLAGS_EXTRA=-DDEBUG_LIBX11_COMPAT
```

The output artifact is `build/libX11-compat.so`.
Clients link against it the same way they would link against the system `libX11.so`.

## Examples

![ViolaWWW running through libx11-compat on macOS](assets/violawww.png)

`examples/` bundles real Xlib clients built against the local `libX11-compat.so`:

```sh
make examples
build/examples/2048
```

The bundle covers a 2048 game, a paint demo, Conway's Game of Life, an analog clock, an interactive Mandelbrot viewer, a single-runner Processing-style showcase, an SDL-backed clipboard `TARGETS` probe, and the upstream X.Org `x11perf` benchmark.
See [`docs/EXAMPLES.md`](docs/EXAMPLES.md) for the API each example exercises.
The screenshot above is from the larger ViolaWWW port described in [Larger Workloads Under Investigation](#larger-workloads-under-investigation).

## Larger Workloads Under Investigation

Two ports beyond the bundled demos are actively exercising `libx11-compat`.
Both are work-in-progress rather than ready for daily use,
but each surfaces gaps in coverage and edge cases that small examples never reach.

- [Motif](https://en.wikipedia.org/wiki/Motif_(software)): upstream Motif and its demo suite build against the compatibility libraries.
  Menu posting, pointer grabs, focus changes, and text rendering all work in the cases tested so far,
  and the screenshot-based differential harness flags regressions against native X11/Motif.
  Some widget paths still trigger fallback or layout artifacts that require further work.
- [ViolaWWW](https://en.wikipedia.org/wiki/ViolaWWW): the 1992-era Motif web browser builds and runs out of the consolidated `build/` tree,
  loads HTTP pages over the network,
  and renders inline XPM images through `libXpm-compat`.
  HTTPS, complex modern HTML, and several interactive flows are known limitations of the application itself rather than the compatibility layer.

  ```sh
  make violawww                          # build ViolaWWW (depends on motif)
  build/violawww/source/src/vw/vw        # launch the browser
  make check-smoke-violawww              # replay-driven smoke checks (scroll + Help menu)
  make check-differential-violawww       # screenshot diff vs system libX11 (needs remote host)
  ```

  The `check-*-smoke` targets need an X display, or `UI_REPLAY_XVFB=--xvfb` to drive an in-process Xvfb;
  on macOS the browser launches against the host SDL backend without extra setup.

The value of these targets is not that they replace a real X11/Motif install today,
but that they keep legacy Xlib/Motif code building and running on platforms where no X server is available while migration is in progress.

Both ports rely on community input.
Concrete reproducers, screenshot diffs, and small fixes posted to [GitHub Issues](https://github.com/sysprog21/libx11-compat/issues) are the most effective way to push these workloads forward.
Specific gaps worth opening issues for include widget paths that misrender, Motif demos that crash or differ visibly from the native baseline, and ViolaWWW interactions (navigation, dialogs, image formats) that do not behave as the historical browser did.

## Coverage and Compatibility

The library exports 631 public Xlib symbols listed in [`tests/api-symbols.txt`](tests/api-symbols.txt),
covering window, drawable, GC, pixmap, image, event, input, atom, property, color, font, cursor, and region subsystems for the cases that real Xlib clients exercise.
Selection, property, and resource-manager support is partial;
MIT-SHM is a thin wrapper over the regular image path;
GLX, Xcms, and input methods are intentionally stubbed.

See [`docs/COVERAGE.md`](docs/COVERAGE.md) for the per-subsystem status table, surface notes (window manager hints, raster ops, mouse wheel mapping), and compatibility limits.

## Porting an Existing Xlib Client

Most Xlib sources need no edits (porting is a build-system and runtime-environment exercise).
The general shape:

1. Link the application against `build/libX11-compat.so` plus SDL2, SDL2_ttf, and pixman, replacing the system `-lX11`.
2. Keep the existing Xlib source unchanged.
3. Drive the event loop with `XPending` / `XNextEvent` and `ConnectionNumber` + `select()`;
   the library pumps SDL internally.
4. Use `WM_DELETE_WINDOW` via `XSetWMProtocols` so the SDL close button produces a clean shutdown.
5. Run with `SDL_VIDEODRIVER=dummy` for headless CI.

See [`docs/PORTING.md`](docs/PORTING.md) for the full guide,
including code snippets keyed to the bundled examples and a checklist of cases where the library is a stepping stone rather than a destination (GLX, real XIM, ICC color management, cross-process selection).

## Contributing

Reports of incorrect Xlib semantics, missing entry points that block real applications, and patches that tighten coverage against the manifest in [`tests/api-symbols.txt`](tests/api-symbols.txt) are particularly useful.

## License

`libX11-compat` is available under a permissive [MIT](https://opensource.org/license/mit)-style license.
Use of this source code is governed by an MIT license that can be found in the [LICENSE](LICENSE) file.
