# UI Replay and State-Aware Smoke Infrastructure

The `make check-smoke-*` targets drive real workloads (Motif, ViolaWWW, Mosaic,
Osiris, GTK, raw Xlib examples) through `scripts/run-ui-replay.py`. The driver
reads a deterministic `.replay` script, launches the target binary against
`libX11-compat`, injects synthetic input via the in-process XTest engine,
captures screenshots, state snapshots and a JSONL event timeline, and applies
declarative assertion rules.

This document is the reference for the replay grammar, the runner CLI,
the assertion rule schemas, and the artifact layout. The script's own
top-of-file docstring is the quick start; this file is the long-form spec.

See [`EXAMPLES.md`](EXAMPLES.md) for the bundled clients the replay engine
exercises and [`COVERAGE.md`](COVERAGE.md) for the underlying API surface.

## Module overview

The pipeline is split across four C modules and the Python driver:

| Module | Role |
|---|---|
| [`src/replay.c`](../src/replay.c) | Reads `LIBX11_COMPAT_REPLAY`, runs the replay script on a detached pthread, dispatches input through XTest fake events. |
| [`src/replay-target.c`](../src/replay-target.c) | Caches the active top-level SDL window so `target-motion`, screenshots, and state snapshots agree on which window matters. |
| [`src/snapshot.c`](../src/snapshot.c) | Round-trips a BMP screenshot or `SDL_SetWindowSize` request through `SDL_USEREVENT` so they run on the main thread (an SDL requirement on macOS). |
| [`src/state-snapshot.c`](../src/state-snapshot.c) | Marshals `UiSnapshot` (focus, grabs, mapped/popup counts, per-window geometry, named properties with `source: stored|synthesized`) on the main thread via the same round-trip. |
| [`src/timeline.c`](../src/timeline.c) | JSONL writer gated on `LIBX11_COMPAT_TIMELINE=1`. Per-kind atomic counters are always live so the convergence detector works even without the writer. Taps live in `events.c:convertEvent`, `window-internal.c:configureWindow`/`destroyWindow`, `drawing.c:drawWindowDataToScreen`, `input.c:XSetInputFocus`/`XGrabKeyboard`/`XUngrabKeyboard`, `pointer.c:XGrabPointer`/`XUngrabPointer`. |
| [`scripts/run-ui-replay.py`](../scripts/run-ui-replay.py) | Translates the high-level `.replay` source into the in-process script, manages artifacts, and applies assertion rules. |
| [`scripts/mine-timeline-latency.py`](../scripts/mine-timeline-latency.py) | Extracts menu/scroll/dialog/keyboard latency spans from the JSONL timeline; supports a hybrid regression gate against per-driver baselines. |

## Backends

`--input-backend internal` (the default) writes a translated replay script
to `<out-root>/internal.replay` and points the target process at it via
`LIBX11_COMPAT_REPLAY`. Synthetic input flows through `src/replay.c` inside
the X-client process and through the normal `convertEvent` path. No host X
server is involved. Works on macOS, Linux, and headless CI without xvfb.

`--input-backend xdotool` injects through an external X server; the target
must already have a `DISPLAY` (typically through `--xvfb`). Used by the
differential runners
([`scripts/run-motif-differential-tests.py`](../scripts/run-motif-differential-tests.py),
`run-violawww-differential-tests.py`, `run-mosaic-differential-tests.py`,
`run-osiris-differential-tests.py`) for direct comparison against native
libX11.

`--offscreen` forces `SDL_VIDEODRIVER=dummy` and requires the internal
backend plus `--in-process-snapshots`. It is the CI configuration: full
replay, state-snapshot, timeline, and renderer artifacts, no visible
desktop, no xvfb dependency.

The internal backend is the canonical path for everything under
`tests/ui/replays/`. The `state-snapshot`, `wait-converge`, and
`timeline-summary` verbs documented below require the internal backend
plus `--in-process-snapshots`.

## Replay file grammar

`.replay` files are line-oriented. Blank lines and lines beginning with
`#` are ignored. Each non-blank line is one command followed by
shell-quoted arguments (`shlex.split` semantics).

### Timing / synchronization

| Command | Arguments | Notes |
|---|---|---|
| `delay` | `<ms>` | Sleep the worker thread for `<ms>` milliseconds. Prefer `wait-converge` for any sleep meant to mask post-input churn. |
| `wait-window` | `<pattern> <timeout_ms>` | On `xdotool`: poll `xdotool search --name` / `--class`. On `internal`: poll that the process is still alive for `timeout_ms`. |
| `wait-converge` | `[bucket_ms quiet_buckets diverged_buckets threshold_per_bucket timeout_ms]` | Block until the per-kind atomic counters for `Expose`/`MapNotify`/`UnmapNotify`/`ConfigureNotify`/`DestroyNotify`/`Present` show `quiet_buckets` consecutive empty buckets (converged), `diverged_buckets` consecutive over-threshold buckets (divergent, emits a `diverged` timeline record), or until the timeout fires. Defaults: 50 ms buckets, 2 quiet, 16 diverged, threshold 32 events per bucket, 5 s timeout. |

### Input

| Command | Arguments | Notes |
|---|---|---|
| `motion` | `<x> <y>` | Screen-relative pointer motion via `XTestFakeMotionEvent`. |
| `target-motion` | `<x> <y>` | Motion relative to the cached replay-target window. Use this when the test cares about a click landing on a specific widget regardless of where the host placed the window. |
| `button` | `<n> press|release|click` | `click` is `press` + 10 ms delay + `release`. |
| `wheel` | `up|down <count>` | Translates to `<count>` repeats of button 4 (`up`) or 5 (`down`) press/release pairs, 50 ms apart. |
| `key` | `<scancode>` | Source-level form (`scripts/run-ui-replay.py`). Fans out to internal-level `key <scancode> press` + `delay 10` + `key <scancode> release`. |
| `resize` | `<W> <H>` | On `internal`: `SDL_SetWindowSize` on the cached target. On `xdotool`: `xdotool windowsize --sync`. |

### Capture

| Command | Arguments | Notes |
|---|---|---|
| `screenshot` | `<name> [x y w h]` | Capture pixels for the named step. On `internal --in-process-snapshots`, writes `snapshots/<name>.bmp` via `SDL_SaveBMP` and converts to `screens/<name>.png`. Otherwise calls the host screenshot tool (`screencapture` on macOS, ImageMagick `import`, or `gnome-screenshot`). The optional `x y w h` overrides `--screenshot-region`. |
| `state-snapshot` | `<name>` | Round-trips a `UiSnapshot` request through `src/state-snapshot.c`. The main thread fills the struct (focused window, grabs, mapped/popup counts, per-window geometry, wm_class, wm_name, tracked properties) and writes `states/<name>.json`. Requires `--in-process-snapshots`. |

### Assertions

| Command | Arguments | Notes |
|---|---|---|
| `assert-image` | `<screenshot> <rule.json>` | Apply an image rule (see "Image rule schema" below) against the named screenshot. |
| `assert-state` | `<state-name> <rule.json>` | Apply a state rule against a prior `state-snapshot`. |
| `timeline-summary` | `<rule.json>` | Apply a timeline rule against the JSONL written when `--timeline` is set. |
| `assert-exit` | `running|any|<status>` | Verify the target process exit state at this point. `running` requires the process is still alive; `any` requires it exited; `<status>` requires that exact integer. |

## Runner CLI

Typical invocation as used by `mk/motif.mk:check-smoke-motif`:

```sh
scripts/run-ui-replay.py \
    --name           motif-fileview-done                          \
    --app            $(OUT)/motif-fileview                        \
    --replay         tests/ui/replays/motif-fileview-done.replay  \
    --out-root       $(OUT)/ui-smoke/motif-fileview-done          \
    --input-backend  internal                                     \
    --in-process-snapshots                                        \
    --timeline                                                    \
    --offscreen
```

| Flag | Effect |
|---|---|
| `--name` | Logical name; used as the JUnit suite name and to compose log paths. |
| `--app` | Path to the target binary. |
| `--app-arg` | Repeatable; appended to the target argv. |
| `--workdir` | Working directory for the target (default: repository root). |
| `--replay` | Path to the `.replay` source. Relative paths are resolved against `tests/ui/replays/` and the repository root. |
| `--out-root` | Directory where artifacts land. The runner always wipes `screens/`, `logs/`, `states/`, `sync/`, `snapshots/` before each run. |
| `--env KEY=VALUE` | Repeatable; merged into the target environment. |
| `--input-backend internal|xdotool` | See "Backends" above. |
| `--in-process-snapshots` | Enables `state-snapshot` and BMP screenshot writes through `src/snapshot.c` / `src/state-snapshot.c`. Required for the in-process pipeline on macOS. |
| `--offscreen` | Force `SDL_VIDEODRIVER=dummy`. Requires `--input-backend internal --in-process-snapshots`. |
| `--timeline` | Set `LIBX11_COMPAT_TIMELINE=1` and write JSONL to `<out-root>/timeline.jsonl`. |
| `--timeline-path` | Explicit JSONL path; implies `--timeline`. |
| `--render-stats` | Set `LIBX11_COMPAT_RENDER_STATS` to this TSV path; captures per-present timing from `src/drawing.c`. |
| `--trace-path` | JSONL trace of runner-level events (`runner.start`, `step.finish`, `resource.*`, `process.start/finish`). Defaults to `<out-root>/trace.jsonl`. |
| `--profile-json` | Aggregate profile (command counts, durations, metric summary, timeline counts, render-stats metadata, artifact index). Defaults to `<out-root>/profile.json`. |
| `--ci-summary` | Compact pass/fail and artifact index suitable for CI dashboards. Defaults to `<out-root>/summary.json`. |
| `--screenshot-command auto|import|gnome-screenshot|screencapture` | `auto` picks the first available. |
| `--screenshot-region X,Y,W,H` | Default region for host screenshots. Per-shot `screenshot NAME X Y W H` overrides it. |
| `--xvfb` | Spawn Xvfb at `:<display>` with `<geometry>`. Implies the runner sets `DISPLAY` for the target. Skipped when `--offscreen` is set. |
| `--display` | Xvfb display number (default `121`). |
| `--geometry` | Xvfb geometry (default `1280x1024x24`). |
| `--leave-running` | Skip the SIGTERM at the end of the replay; useful for manual inspection. |

## Assertion rule schemas

Each rule file is a JSON document with a top-level `"assertions"` array.
Every entry has a `"type"` plus type-specific fields. Unknown types fail
the assertion with an explicit error.

### Image rules (`assert-image`)

| Type | Fields | Effect |
|---|---|---|
| `non_empty` | _none_ | Fail when the file is zero bytes or the image has no pixels. |
| `not_all_black` | _none_ | Fail when every channel maxes at <= 8. |
| `min_size` | `width` (default 1), `height` (default 1) | Fail when the image is narrower than `width` or shorter than `height` pixels. |
| `max_size` | `width` (default 999999), `height` (default 999999) | Fail when the image is wider than `width` or taller than `height` pixels. |
| `region_non_background` | `rect` or `display_rect`+`display_size`, `dark_threshold` (default 96), `min_dark_ratio` (default 0.001) | Fail when fewer than `min_dark_ratio` of pixels in the region are at or below the dark threshold. |
| `region_max_dark_ratio` | `rect` (or display_rect/display_size), `dark_threshold` (default 32), `max_dark_ratio` (default 0.35) | Inverse of the above. |
| `dark_row_run_height` | `rect` (or display_rect/display_size), `dark_threshold` (default 96), `min_row_dark_ratio` (default 0.01), `min_run_height` (default 1), `max_run_height` (default 9999), `min_runs` (default 1) | Fail when text-like dark row runs do not include enough rows in the expected pixel-height range. Each optional field may be omitted; the defaults accept runs of any height (`min_run_height`/`max_run_height`) and require at least one qualifying run (`min_runs`). |
| `region_min_unique_colors` | `rect` (or display_rect/display_size), `min_unique_colors` (default 2) | Fail when the region contains fewer than `min_unique_colors` distinct RGB colors. |
| `region_min_color_coverage` | `rect` (or display_rect/display_size), `colors` (list of `[r, g, b]` targets), `tolerance` (default 24), `min_ratio` (default 0.05) | Fail when fewer than `min_ratio` of region pixels match any target color within `tolerance` per channel. Detects that specific expected content (for example named layer fills) is present in quantity; the tolerance absorbs SDL2/SDL3 color-conversion drift. |
| `changed_region` | `baseline` (screenshot name), `rect` (or display_rect/display_size), `min_changed_ratio` (default 0.01) | Fail when fewer than `min_changed_ratio` of pixels differ vs the baseline. |
| `reference_diff` | `reference` (path), `rect`, `max_changed_ratio` (default 0.35) | Fail when more than `max_changed_ratio` of pixels differ vs the on-disk reference. |
| `stale_text` | `rect`, `dark_threshold` (default 80), `max_row_dark_ratio` (default 0.42), `max_dense_rows` (default 18) | Heuristic for stale ghost text; counts rows whose dark pixel ratio exceeds `max_row_dark_ratio`. |

`display_rect` + `display_size` lets a rule reference logical coordinates
that scale with the actual screenshot dimensions, useful when retina
scaling kicks in for a subset of runs.

### State rules (`assert-state`)

A `UiSnapshot` is a JSON object with `focused_window`, `revert_to`,
`pointer_grab`, `keyboard_grab`, `mapped_window_count`,
`popup_window_count`, `event_queue_depth`, `truncation_flagged`,
`window_count`, and a `windows` array. Each window entry carries
`window`, `parent`, `x`, `y`, `w`, `h`, `sdl_window_id`, `sdl_flags`,
`override_redirect`, `map_state`, `event_mask`, `wm_class`, `wm_name`,
and a `properties` array. Each property record has `atom`, `format`,
`length`, `source` (`none|stored|synthesized`), `truncated`, and a
`hex` byte dump bounded to the first 128 bytes.

| Type | Fields | Effect |
|---|---|---|
| `equal` | `field`, `expected` | Top-level snapshot field equals `expected`. |
| `leq` | `field`, `max` | Top-level field <= `max`. |
| `geq` | `field`, `min` | Top-level field >= `min`. |
| `wm_class_present` | `wm_class` | Some window has the given `wm_class`. |
| `window_with_size` | `w`, `h` | Some window matches both dimensions (either field can be omitted to skip that check). |
| `grab_released` | _none_ | Both `pointer_grab` and `keyboard_grab` are 0. End-of-replay leak check. |
| `property_unchanged` | `window`, `atom`, `between` (`[baseline_state.json]`) | Named property on the window matches the value recorded in the baseline state file (compared by `{source, hex, format, length}`). |
| `property_changed_to` | `window`, `atom`, `hex` | Named property is at the supplied raw-bytes-hex value. |

The `source: synthesized` flag distinguishes default values fabricated by
`XGetWindowProperty` (e.g. Motif's `_MOTIF_WM_HINTS` default) from values
the client actually stored. The snapshot reads `WindowStruct.properties`
directly so this distinction survives.

### Timeline rules (`timeline-summary`)

Reads the JSONL written by `src/timeline.c` and counts records per
`kind`. Schema:

```json
{
  "assertions": [
    {"type": "kind_at_least", "kind": "ButtonPress", "min": 1},
    {"type": "kind_at_most",  "kind": "diverged",   "max": 0},
    {"type": "kind_equal",    "kind": "MapNotify",  "expected": 3}
  ]
}
```

`kind` strings match `KIND_NAMES` in `src/timeline.c`. The full set:
`Expose`, `MapNotify`, `UnmapNotify`, `ConfigureNotify`,
`DestroyNotify`, `ButtonPress`, `ButtonRelease`, `KeyPress`,
`KeyRelease`, `MotionNotify`, `EnterNotify`, `LeaveNotify`, `FocusIn`,
`FocusOut`, `PropertyNotify`, `ClientMessage`, `SelectionNotify`,
`VisibilityNotify`, `ReparentNotify`, `Present`, `Configure`, `Destroy`,
`SetInputFocus`, `GrabPointer`, `UngrabPointer`, `GrabKeyboard`,
`UngrabKeyboard`, `diverged`.

## Output artifacts

Every successful run produces, under `<out-root>`:

| Path | Contents |
|---|---|
| `results.tsv` | Per-step status (`captured`, `ok`, `failed`) and detail string. |
| `metrics.tsv` | Per-step `step_duration_ms`, plus observation rows for rules like `stale_text` that emit metrics. |
| `profile.json` | Aggregate runner profile: command counts, per-command total durations, metric summary, timeline kind counts, render-stats metadata, artifact index. |
| `summary.json` | Compact CI dashboard view: name, backend, offscreen flag, overall pass/fail, failure rows, artifact index. |
| `trace.jsonl` | Structured runner trace; one JSON record per high-level event (`runner.start`, `runner.finish`, `resource.*`, `process.start`, `process.finish`, `step.start`, `step.finish`, `step.error`). |
| `junit.xml` | JUnit XML for CI gating. |
| `internal.replay` | The translated script the in-process engine ran (internal backend only). |
| `logs/<name>.log` | Target stdout+stderr (captures `LIBX11_COMPAT` `LOG` output when built with `DEBUG_LIBX11_COMPAT`). |
| `screens/<name>.png` | PNG-converted screenshots. |
| `snapshots/<name>.bmp` | BMP captured by `SDL_SaveBMP` (internal + `--in-process-snapshots` only). |
| `states/<name>.json` | `UiSnapshot` JSON (one per `state-snapshot` step). |
| `sync/wait-converge-<line>.json` | Internal synchronization markers so the runner can block on the in-process converge without re-implementing the detector. |
| `timeline.jsonl` | JSONL event timeline (when `--timeline` is set). |

## Latency mining

`scripts/mine-timeline-latency.py <timeline.jsonl> --out latency.json`
extracts:

- `menu_open_ms`: `ButtonPress` -> first following `MapNotify`
- `scroll_settle_ms`: `ButtonPress` -> first following `Present`
- `dialog_open_ms`: `ConfigureNotify` -> first following `Present`
- `keyboard_settle_ms`: `KeyPress` -> first following `Present`

Each span emits `count`, `median_ms`, `p95_ms`, `min_ms`, `max_ms`, plus
the raw samples. Pass
`--baseline tests/ui/baselines/<target>-<driver>-latency.json` to fail
the gate when the observed median exceeds the baseline by both
`>--rel` (default 25%) and `>--abs ms` (default 50 ms). The hybrid gate
avoids CI scheduling jitter triggering spurious failures at sub-10-ms
latencies.

Baseline storage is per-`(replay, SDL_VIDEODRIVER)` because renderer
latency varies enough between `dummy`, `x11`, `wayland`, and `cocoa`
that a single cross-driver baseline would either pass everything on the
slowest driver or fail everything on the fastest.

## Threading and synchronization model

The driver script runs on the host Python process. The target binary
runs its X client on its own main thread; `src/replay.c` spawns a
detached pthread (the "worker") that reads `LIBX11_COMPAT_REPLAY` and
issues XTest fake events. The worker pushes `SDL_USEREVENT` round-trips
for `snapshot`, `resize`, and `state-snapshot`; the main thread handles
them on the same condvar (`src/snapshot.c` and `src/state-snapshot.c`
share semantics).

`wait-converge` does not round-trip the main thread; it polls per-kind
atomic counters maintained by `src/timeline.c` from the worker thread.
The runner blocks on the in-process synchronization by polling on-disk
marker files (`states/<name>.json`,
`sync/wait-converge-<line>.json`) the C side writes. This keeps the
runner agnostic of any C-side synchronization primitives and survives
the worker reporting back via the file system even when the host clock
and the target clock differ.

## Adding a new replay

1. Pick a target binary already produced by the build (e.g.
   `$(OUT)/motif-foo`).
2. Drop a `<name>.replay` file under
   [`tests/ui/replays/`](../tests/ui/replays/).
3. Add the matching assertion `.json` files under
   [`tests/ui/assertions/`](../tests/ui/assertions/) (`assert-image`,
   `assert-state`, or `timeline-summary`).
4. Add a `make` target under [`mk/`](../mk/) so CI runs it (model after
   `check-smoke-motif` in [`mk/motif.mk`](../mk/motif.mk)).
5. Run it 10 consecutive times locally to confirm determinism before
   committing.

The TODO has an open follow-up to convert specific fixed-`delay`
sequences in `tests/ui/replays/` to `wait-converge`; new replays should
use `wait-converge` from the start. End-of-replay `assert-state` rules
with `grab_released` catch grab leaks that pixel diffs miss entirely.
