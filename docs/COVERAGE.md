# libX11 API Coverage

The library exports 615 public Xlib symbols, listed in
[`tests/api-symbols.txt`](../tests/api-symbols.txt). The exported manifest is
enforced by `make symbol-coverage`; per-subsystem behavior is checked by
`make check`.

The status column uses three buckets:

- *Functional* — the documented Xlib behavior is implemented for the cases
  the in-tree examples and tests exercise.
- *Partial* — the entry points exist and do useful work, but parts of the
  Xlib contract are not implemented. The Notes column says which.
- *Stub* — the symbol exists for link compatibility but returns a
  conformant unsupported result.

| Area                     | Status     | Notes |
|--------------------------|------------|-------|
| Display lifecycle        | Functional | `XOpenDisplay`, default screen and visual, `ConnectionNumber` for `select()` integration. |
| Windows                  | Functional | Creation, mapping, hierarchy, attribute queries, destruction, debug introspection. |
| Drawables and GCs        | Functional | Lines, points, rectangles, polygons, single and batched arcs, text, line attributes, foreground/background, clip rectangles, and GC raster functions for solid point/line/rectangle/polygon draws. |
| Pixmaps                  | Functional | `XCreatePixmap`, `XCopyArea`, double-buffering patterns. |
| Images                   | Functional | `XCreateImage` / `XPutImage` / `XGetImage` for ZPixmap and bitmap formats. |
| Events                   | Functional | Expose, key, button, motion, configure, enter/leave, focus, client message, mapping notify. |
| Input                    | Functional | Keyboard and pointer state, keysym mapping, modifier translation. |
| Atoms                    | Functional | Stable per-display identity for `XInternAtom`, predefined atoms, `XGetAtomName`. |
| Properties               | Partial    | `XChangeProperty`, `XGetWindowProperty`, ICCCM and `_NET_WM_*` hints used by GTK probes. `XSetWMProtocols` works; `XGetWMProtocols` is a stub. |
| Colors                   | Functional | `XAllocColor`, `XAllocNamedColor`, standard color database, colormap defaults. |
| Fonts and text           | Functional | `XLoadQueryFont`, text width queries, `XDrawString` / `XDrawString16`, SDL2_ttf backing. |
| Cursors                  | Functional | Standard cursor shapes, pixmap cursors. |
| Regions                  | Functional | Rectangle-list regions, set operations, GC clip regions. |
| Selections               | Partial    | ICCCM per-display ownership tracking. `CLIPBOARD` text targets are bridged for *reads* via `SDL_GetClipboardText`; X11 ownership is not pushed back to the SDL clipboard. |
| X Resource Manager       | Partial    | `XrmGetStringDatabase`, `XrmGetResource`, `XrmParseCommand` handle simple `name.resource: value` records with tight (`.`) and loose (`*`) bindings. |
| Geometry and bitmap I/O  | Functional | Xorg `XParseGeometry`; `XReadBitmapFile` / `XWriteBitmapFile` round-trip XBM payloads. |
| MIT-SHM                  | Partial    | `XShmQueryExtension` returns `True`; `XShmAttach`/`XShmDetach` succeed without a real SHM segment; `XShmPutImage`/`XShmGetImage` route to the regular image APIs; `shared_pixmaps` is reported `False`. |
| XSync, XShape            | Stub       | Conformant unsupported returns (`False` / `BadImplementation` / `None`). |
| GLX, Xcms, input methods | Stub       | Quiet capability probes; deeper calls log via `WARN_UNIMPLEMENTED` in debug builds. |

## Surface notes worth knowing before porting

- `_NET_WM_WINDOW_TYPE` of `menu`, `dropdown`, `popup`, `tooltip`, `splash`,
  `dock`, `notification`, or `combo` drops the SDL window border, so
  override-redirect-style behavior approximates a real window manager.
- `_MOTIF_WM_HINTS` synthesizes a permissive default reply when the property
  is unset; clients that set their own hints get the stored value back
  verbatim.
- `_XSETTINGS_SETTINGS` on the root window publishes a minimal payload
  (`Gtk/FontName`, `Xft/DPI`, `Net/IconThemeName`) so GTK probes do not fall
  back to placeholder values.
- Mouse wheel input surfaces as `Button4` / `Button5` (vertical) and
  `Button6` / `Button7` (horizontal) `ButtonPress` events with the current
  modifier state, matching Xorg server convention.
- GC raster functions (`GXclear` through `GXset`) are supported for solid
  `XDrawPoint` / `XDrawPoints`, `XDrawLine` / `XDrawLines` / `XDrawSegments`,
  `XFillRectangle` / `XFillRectangles`, and `XFillPolygon` through a software
  read-back, mutate, and blit path when SDL has no direct renderer equivalent.
- Large solid `XDrawArc` / `XFillArc` calls route through the in-tree
  arc-to-cubic path accelerator and scanline raster path. Small `LineSolid`
  one-pixel arcs stay on the legacy point renderer to avoid regressing tiny
  primitive overhead, but `ArcChord` fills, dashed line styles, and any
  `lineWidth > 1` are always routed through the path accelerator so the
  legacy pie-only / dotty fallbacks never run. Wide solid strokes and
  `LineOnOffDash` / `LineDoubleDash` use the path stroke expansion for line,
  segment, rectangle, polyline, and large arc drawing. The stroke outline
  builder honors `JoinMiter` (with the X11 default miter-limit of 10),
  `JoinBevel`, `JoinRound`, and `CapButt` / `CapRound` / `CapProjecting`.
  Polygon region creation shares the same path edge/span builder and uses
  pixman region union for final storage. Tile, stipple, non-solid fill,
  and non-copy raster ops still use the pre-accelerator drawing paths.

## Compatibility limits

Because the library runs in-process, several X server facilities have no
direct equivalent and behave conservatively rather than failing:

- There is no X socket protocol, no separate window manager process, and no
  cross-process resource ownership. APIs that would otherwise query those
  facilities return local SDL-backed defaults or explicit unsupported
  results.
- Server-side access control, network-transparent selection ownership,
  grabs, and save sets are not implemented.
- Color management conversion (Xcms) is stubbed; clients that depend on ICC
  profiles or device-color spaces must convert outside the library.
- Full clipping-region semantics are approximated by rectangle-list regions
  and SDL clip rects. Complex clip paths used by some renderers may render
  with reduced fidelity.
- Performance is bounded by SDL2's renderer and software-surface paths.
  Pixmap-heavy clients run best when they reuse pixmaps and minimize
  `XGetImage` round-trips.

Capability probes are intentionally quiet so that downstream code does not
need to be modified to silence warnings. Deeper unsupported calls log
through `WARN_UNIMPLEMENTED` in debug builds, which makes missing behavior
actionable without log spam in release builds.
