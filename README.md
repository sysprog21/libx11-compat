# libx11-compat

`libx11-compat` is an in-process implementation of the [X Window System](https://en.wikipedia.org/wiki/X_Window_System) client library (Xlib) layered on top of [SDL2](https://www.libsdl.org/), SDL2_ttf, and pixman.
It lets existing Xlib clients keep their source unchanged while running on platforms where a conventional X server is unavailable or inconvenient:
macOS without XQuartz, Wayland-only sessions, headless CI, Android apps with their own SDL2 integration, and similar environments.

The library is not a re-implementation of the X11 wire protocol and does not replace a real X server.
Its goal is to keep legacy Xlib code building and running while it is being migrated to a different toolkit or display stack.

## Building

The build is Makefile-based and organized as small `mk/` fragments.
The required dependencies are SDL2, SDL2_ttf, and pixman.
Optional validation workloads may need their upstream build tools; Osiris uses
Meson and Ninja at build time and links against libjpeg, libpng, and freetype.

```sh
make
make check
```

`make check` runs the in-tree C tests, exported-symbol coverage, Motif link and demo checks, replay-driven UI smoke tests, and system-X11 differential checks.
For faster local loops, use `make check-unit`, `make check-smoke`, or `make check-differential` depending on the subsystem being changed.

Verbose diagnostics for unimplemented or fall-back paths are gated behind a build flag:

```sh
make CFLAGS_EXTRA=-DDEBUG_LIBX11_COMPAT
```

The output artifact is `build/libX11-compat.so`.
Clients link against it the same way they would link against the system `libX11.so`.

## Examples

`examples/` bundles real Xlib clients built against the local `libX11-compat.so`:

```sh
make examples
build/examples/2048
```

The bundle covers a 2048 game, a paint demo, Conway's Game of Life, an analog clock, an interactive Mandelbrot viewer, a single-runner Processing-style showcase, an SDL-backed clipboard `TARGETS` probe, and the upstream X.Org `x11perf` benchmark.
See [`docs/EXAMPLES.md`](docs/EXAMPLES.md) for the API each example exercises.
The screenshot above is from the larger ViolaWWW port described in [Larger Workloads Under Investigation](#larger-workloads-under-investigation).

## Larger Workloads Under Investigation

Five ports beyond the bundled demos now provide high-value integration coverage for `libx11-compat`.
They are still compatibility workloads rather than daily-use application ports,
but each exercises behavior that small examples do not reach.

- [Motif](https://en.wikipedia.org/wiki/Motif_(software)): upstream Motif and its demo suite build against the compatibility libraries.
  Menu posting, pointer grabs, focus changes, text rendering, resource lookups, and selected demo workflows are covered by local replay smoke tests.
  Screenshot-based differential checks compare selected paths against native X11/Motif and continue to flag visible layout or repaint regressions.
  Some widget paths still expose layout artifacts or map/expose propagation gaps that require further work.
- [ViolaWWW](https://en.wikipedia.org/wiki/ViolaWWW): the 1992-era Motif web browser builds and runs out of the consolidated `build/` tree,
  loads HTTP pages over the network,
  renders inline XPM images through `libXpm-compat`,
  and is covered by replay checks for scrolling, resize redraw, and the Help menu.
  Recent fixes corrected stale glyphs after wheel scrolling, scrollbar dragging, and resize reflow.
  HTTPS, complex modern HTML, and several interactive flows are known limitations of the application itself rather than the compatibility layer.

  <a href="assets/violawww.png"><img src="assets/violawww.png" alt="ViolaWWW running through libx11-compat on macOS" width="420"></a>

  ```sh
  make violawww                          # build ViolaWWW (depends on motif)
  build/violawww/source/src/vw/vw        # launch the browser
  make check-smoke-violawww              # replay-driven smoke checks (scroll + Help menu)
  make check-differential-violawww       # screenshot diff vs system libX11 (needs remote host)
  ```

- [NCSA Mosaic](https://en.wikipedia.org/wiki/Mosaic_(web_browser)): the maintained [thentenaar/mosaic-ng](https://github.com/thentenaar/mosaic-ng) fork of the seminal 1993 graphical browser builds against the compat stack plus the bundled Motif.
  It loads HTTP pages, lays out inline images and tables, and is covered by replay smoke checks for initial paint, scrolling, hyperlink navigation, and URL-field editing.
  HTTPS and modern HTML/CSS remain application-level limitations.

  <a href="assets/mosaic.png"><img src="assets/mosaic.png" alt="NCSA Mosaic running through libx11-compat" width="420"></a>

  ```sh
  make mosaic                            # build Mosaic (depends on motif)
  build/mosaic/source/src/Mosaic         # launch the browser
  make check-smoke-mosaic                # replay-driven smoke checks (paint + scroll + navigation)
  make check-differential-mosaic         # screenshot diff vs system libX11 (needs remote host)
  ```

- [Osiris](https://centre.libranext.com/libranext/osiris): a maintained Qt 2.3.2 fork builds against `libX11-compat`, `libXext-compat`, `libXmu-compat`, and the link-only `libICE-compat` / `libSM-compat` shims.
  This is the Qt2 validation target and exercises widget stacking, popup/menu placement, font metrics, timer delivery, expose handling, event-loop responsiveness, and Qt Designer behavior.
  The in-tree build disables Osiris Xft, OpenGL, SM, MNG, and GIF feature probes so the default dependency boundary stays SDL2, SDL2_ttf, and pixman.
  Building this workload requires Meson, Ninja, libjpeg, libpng, and freetype; override the Meson and Ninja paths with `OSIRIS_MESON=/path/to/meson` and `OSIRIS_NINJA=/path/to/ninja` if needed.

  <a href="assets/osiris.png"><img src="assets/osiris.png" alt="Osiris Qt Designer running through libx11-compat on macOS" width="420"></a>

  ```sh
  make osiris                            # fetch, patch, configure, and build Osiris
  build/osiris/build/designer            # launch Qt Designer
  make check-demos-osiris                # launch built tutorials/examples long enough to catch crashes
  make check-smoke-osiris                # replay-driven t1 and Designer menu smoke checks
  make check-differential-osiris         # screenshot diff vs system libX11 (needs remote host)
  ```

- [Xfig](https://mcj.sourceforge.net/): the classic Athena-based vector drawing program builds against the compat stack and the bundled libXaw.
  It exercises the full Xt + Athena widget stack (menus, scrollbars, text widgets), large-canvas polyline and ellipse redraw, `XDrawString` font metrics, GXxor rubber-band feedback during mouse drags, and ICCCM text-property round-tripping in the File Open dialog.
  Sample drawings live in [`tests/data/`](tests/data/) and cover a small flowchart, a voltage-divider circuit, a mind map, and an 8x6 rendering stress grid.

  <a href="assets/xfig.png"><img src="assets/xfig.png" alt="Xfig running through libx11-compat on macOS" width="420"></a>

  ```sh
  make xfig                              # build Xfig (depends on libXaw)
  build/xfig/source/src/xfig tests/data/mindmap.fig
  make check-smoke-xfig                  # replay-driven startup smoke check
  ```

The `check-smoke-*` targets use deterministic replay files and in-process snapshots, with artifacts written under `build/ui-smoke/`.
`make profile-ui` runs the Motif, ViolaWWW, and Mosaic replay smokes with timing capture and prints the generated `metrics.tsv` and `render-stats.tsv` paths; the Osiris and Xfig smokes are still invoked individually via `make check-smoke-osiris` / `make check-smoke-xfig`.
They do not require `node11`, `xdotool`, or a native X11 reference run.
Set `UI_REPLAY_XVFB=--xvfb` only when a local Xvfb display is useful for the host environment.

See [`docs/UI-REPLAY.md`](docs/UI-REPLAY.md) for the replay grammar, the runner CLI, the state / image / timeline assertion schemas, and the artifact layout.

The value of these targets is not that they replace a real X11, Motif, or Qt2 install today,
but that they keep legacy Xlib clients building and running on platforms where no X server is available while migration is in progress.

These ports rely on community input.
Concrete reproducers, screenshot diffs, and small fixes posted to [GitHub Issues](https://github.com/sysprog21/libx11-compat/issues) are the most effective way to push these workloads forward.
Specific gaps worth opening issues for include widget paths that misrender, Motif demos that crash or differ visibly from the native baseline, Osiris/Qt2 menu or Designer interactions that differ from native X11, ViolaWWW interactions (navigation, dialogs, image formats) that do not behave as the historical browser did, and Xfig drawings whose objects rasterize differently than under a real X server.

## Coverage and Compatibility

The exported public Xlib surface is listed in [`tests/api-symbols.txt`](tests/api-symbols.txt) and enforced by `make symbol-coverage`.
It covers window, drawable, GC, pixmap, image, event, input, atom, property, color, font, cursor, region, X Resource Manager, and selected Xt/Motif-adjacent compatibility paths for the cases that real Xlib clients exercise.
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
