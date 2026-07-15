#!/usr/bin/env python3
"""Driver for deterministic UI replay scenarios against libx11-compat.

This script reads a `.replay` source file (a one-command-per-line text
format documented in docs/UI-REPLAY.md), launches the target application
with a libx11-compat-aware environment, and feeds synthetic input either
through the in-process LIBX11_COMPAT_REPLAY engine (default) or through
external xdotool. After each step it records BMP/PNG screenshots, JSON
state snapshots, and a JSONL event timeline, then applies declarative
assertion rules against those artifacts.

# Invocation

    scripts/run-ui-replay.py \
        --name        <human-readable-name>           \
        --app         <path-to-target-binary>         \
        [--app-arg    <extra-arg>]                    \
        --replay      <path-to-.replay-file>          \
        --out-root    <artifact-output-directory>     \
        [--env        KEY=VALUE]                      \
        [--input-backend internal|xdotool]            \
        [--in-process-snapshots]                      \
        [--timeline | --timeline-path <path>]         \
        [--render-stats <path-to-tsv>]                \
        [--trace-path <path-to-jsonl>]                \
        [--profile-json <path-to-json>]               \
        [--ci-summary <path-to-json>]                 \
        [--screenshot-command auto|import|gnome-screenshot|screencapture] \
        [--screenshot-region X,Y,W,H]                 \
        [--offscreen]                                 \
        [--xvfb --display N --geometry WxHxD]         \
        [--leave-running]

# Backends

`--input-backend internal` (the default) writes a translated replay script
to <out-root>/internal.replay and points the target process at it via the
LIBX11_COMPAT_REPLAY env var. The replay then runs inside the same
process as the X client, side-stepping the macOS NSApp focus dance that
otherwise drops mid-screencapture input.

`--input-backend xdotool` injects through an external X server; the
target must already have a DISPLAY (e.g. with --xvfb).

`--offscreen` forces SDL_VIDEODRIVER=dummy and requires the internal
backend plus --in-process-snapshots. It is meant for CI jobs that need
replay, state, timeline, and renderer artifacts without a visible desktop.

# Replay grammar (excerpt)

    # comments and blank lines are ignored
    delay <ms>
    motion <x> <y>                  # screen-relative
    target-motion <x> <y>           # window-relative
    target-at <x> <y>               # root coordinate
    button <n> press|release|click
    wheel up|down <count>
    key <scancode>
    resize <W> <H>
    screenshot <name> [x y w h]
    state-snapshot <name>           # requires --in-process-snapshots
    wait-converge [bucket_ms quiet diverged threshold timeout_ms]
    wait-exit <timeout_ms>
    wait-window <regex> <timeout_ms>
    assert-image <screenshot> <rule.json>
    assert-state <state-name> <rule.json>
    timeline-summary <rule.json>
    assert-exit running|any|<status>

See docs/UI-REPLAY.md for the full verb reference and the JSON rule
schemas accepted by assert-image, assert-state, and timeline-summary.

# Output

The runner produces, under <out-root>:

    results.tsv         per-step status (captured|ok|failed) + detail
    metrics.tsv         per-step duration and any observation values
    profile.json        aggregate per-command timing and artifact sizes
    summary.json        CI-oriented pass/fail and artifact index
    trace.jsonl         structured runner events for resource/debug tracing
    junit.xml           JUnit summary suitable for CI gating
    screens/<name>.png  PNG-converted screenshots
    snapshots/<name>.bmp
    states/<name>.json  state-snapshot output (--in-process-snapshots only)
    sync/wait-converge-<line>.json  internal synchronization markers
    timeline.jsonl      JSONL timeline (when --timeline is set)
    logs/<name>.log     target process stdout+stderr
"""

import argparse
import csv
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path

try:
    from PIL import Image, ImageChops
except ImportError:
    Image = None
    ImageChops = None


ROOT = Path(__file__).resolve().parents[1]


class ReplayError(Exception):
    pass


def replay_keysym(code):
    """Map a replay key scancode to an xdotool keysym name.

    Replays carry SDL keycodes (printable ASCII for letters, 225 for the
    low byte of SDLK_LSHIFT). xdotool wants keysym names, so translate the
    common cases the differential needs and reject anything unmapped rather
    than silently passing a bogus argument to xdotool.
    """
    if code == 225:
        return "shift"
    if 0x20 <= code <= 0x7E:
        return chr(code)
    raise ReplayError(f"no xdotool keysym for replay code {code}")


@dataclass
class ArtifactPaths:
    out_root: Path
    name: str
    trace_path: object = None
    profile_path: object = None
    summary_path: object = None
    render_stats_path: object = None

    screenshot_dir: Path = field(init=False)
    log_dir: Path = field(init=False)
    states_dir: Path = field(init=False)
    sync_dir: Path = field(init=False)
    snapshot_dir: Path = field(init=False)
    results_path: Path = field(init=False)
    metrics_path: Path = field(init=False)
    junit_path: Path = field(init=False)
    app_log_path: Path = field(init=False)

    def __post_init__(self):
        self.screenshot_dir = self.out_root / "screens"
        self.log_dir = self.out_root / "logs"
        self.states_dir = self.out_root / "states"
        self.sync_dir = self.out_root / "sync"
        self.snapshot_dir = self.out_root / "snapshots"
        self.results_path = self.out_root / "results.tsv"
        self.metrics_path = self.out_root / "metrics.tsv"
        self.junit_path = self.out_root / "junit.xml"
        self.app_log_path = self.log_dir / f"{self.name}.log"
        if self.trace_path is None:
            self.trace_path = self.out_root / "trace.jsonl"
        if self.profile_path is None:
            self.profile_path = self.out_root / "profile.json"
        if self.summary_path is None:
            self.summary_path = self.out_root / "summary.json"

    def prepare_base(self):
        self.out_root.mkdir(parents=True, exist_ok=True)
        for path in (self.screenshot_dir, self.log_dir):
            if path.exists():
                shutil.rmtree(path)
            path.mkdir(parents=True)

    def reset_dir(self, path):
        if path.exists():
            shutil.rmtree(path)
        path.mkdir(parents=True)


class ReplayTrace:
    def __init__(self, path):
        self.path = path
        self.fp = None
        if path is not None:
            path.parent.mkdir(parents=True, exist_ok=True)
            self.fp = path.open("w")

    def emit(self, event, **fields):
        if self.fp is None:
            return
        record = {
            "t_ms": int(time.perf_counter() * 1000),
            "event": event,
        }
        record.update(fields)
        self.fp.write(json.dumps(record, sort_keys=True) + "\n")
        self.fp.flush()

    def close(self):
        if self.fp is not None:
            self.fp.close()
            self.fp = None


@dataclass
class ReplayProfile:
    started_at_ms: int
    command_counts: dict = field(default_factory=dict)
    command_durations_ms: dict = field(default_factory=dict)
    artifacts: dict = field(default_factory=dict)
    process: dict = field(default_factory=dict)

    def add_step(self, command, duration_ms):
        self.command_counts[command] = self.command_counts.get(command, 0) + 1
        total = self.command_durations_ms.get(command, 0.0)
        self.command_durations_ms[command] = total + duration_ms

    def add_artifact(self, key, path):
        if path is None:
            return
        entry = {"path": str(path)}
        if path.exists() and path.is_file():
            entry["bytes"] = path.stat().st_size
        self.artifacts[key] = entry


def resolve_path(path, *bases):
    candidate = Path(path)
    if candidate.is_absolute():
        return candidate
    for base in bases:
        resolved = base / candidate
        if resolved.exists():
            return resolved
    return bases[0] / candidate


def split_env(values):
    env = {}
    for value in values:
        if "=" not in value:
            raise ReplayError(f"--env value must be NAME=VALUE: {value}")
        key, val = value.split("=", 1)
        env[key] = val
    return env


def ensure_pil():
    if Image is None:
        raise ReplayError("Pillow is required for image assertions")


def run_command(cmd, *, cwd=None, env=None, stdout=None, stderr=None):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run(cmd, cwd=cwd, env=env, stdout=stdout, stderr=stderr)


def start_xvfb(display, geometry, log_path):
    if not display:
        return None
    if os.environ.get("DISPLAY") == f":{display}":
        return None
    if not shutil.which("Xvfb"):
        return None
    lock = Path(f"/tmp/.X{display}-lock")
    if lock.exists():
        try:
            lock.unlink()
        except OSError:
            pass
    log = log_path.open("w")
    proc = subprocess.Popen(
        ["Xvfb", f":{display}", "-screen", "0", geometry],
        stdout=log,
        stderr=subprocess.STDOUT,
    )
    time.sleep(1.0)
    if proc.poll() is not None:
        raise ReplayError(f"Xvfb exited early with status {proc.returncode}")
    return proc


def terminate_process(proc):
    if proc.poll() is not None:
        return proc.returncode
    try:
        proc.terminate()
    except ProcessLookupError:
        return proc.poll()
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if proc.poll() is not None:
            return proc.returncode
        time.sleep(0.1)
    try:
        proc.kill()
    except ProcessLookupError:
        pass
    return proc.wait()


def command_exists(name):
    return shutil.which(name) is not None


def xdotool(env, *args, check=True, capture=False):
    if not command_exists("xdotool"):
        raise ReplayError("xdotool is required for replay input commands")
    kwargs = {
        "env": env,
        "text": True,
        "check": check,
    }
    if capture:
        kwargs["stdout"] = subprocess.PIPE
        kwargs["stderr"] = subprocess.PIPE
    print("+ xdotool", " ".join(args), flush=True)
    try:
        return subprocess.run(["xdotool", *args], **kwargs)
    except subprocess.CalledProcessError as exc:
        # Re-raise as ReplayError so the replay loop's error-handling
        # path records a deterministic failure instead of letting
        # CalledProcessError escape and crash the runner.
        detail = exc.stderr.strip() if isinstance(exc.stderr, str) else ""
        suffix = f": {detail}" if detail else ""
        raise ReplayError(
            f"xdotool {' '.join(args)} failed with status {exc.returncode}{suffix}"
        ) from exc


def wait_window(env, pattern, timeout_ms):
    deadline = time.time() + timeout_ms / 1000.0
    last_error = ""
    while time.time() < deadline:
        if command_exists("xdotool") and env.get("DISPLAY"):
            for field in ("--name", "--class"):
                result = xdotool(
                    env,
                    "search",
                    field,
                    pattern,
                    check=False,
                    capture=True,
                )
                if result.returncode == 0 and result.stdout.strip():
                    return result.stdout.strip().splitlines()[0]
                last_error = result.stderr.strip()
        time.sleep(0.2)
    raise ReplayError(f"timed out waiting for window {pattern!r}: {last_error}")


def wait_process_alive(proc, timeout_ms):
    """Poll the app for the requested duration, failing fast on exit.

    The internal replay backend has no X server to query, so wait-window
    degrades into a fixed-duration alive check. Returning after the first
    sleep tick collapsed every wait-window into a 100 ms delay regardless of
    timeout_ms.
    """
    deadline = time.time() + timeout_ms / 1000.0
    while time.time() < deadline:
        if proc.poll() is not None:
            raise ReplayError(f"process exited with status {proc.returncode}")
        time.sleep(0.1)


def wait_process_exit(proc, timeout_ms):
    deadline = time.time() + timeout_ms / 1000.0
    while time.time() < deadline:
        status = proc.poll()
        if status is not None:
            return status
        time.sleep(0.1)
    raise ReplayError(f"process still running after {timeout_ms} ms")


WAIT_CONVERGE_DEFAULTS = [50, 2, 16, 32, 5000]


def wait_converge_fields(parts):
    # Defaults track the C-side handler in src/replay.c. Malformed input
    # raises ReplayError so the dispatcher converts it into a deterministic
    # step failure (otherwise int("abc") would surface as an uncaught
    # ValueError and crash the runner mid-replay).
    if len(parts) > 6:
        raise ReplayError("wait-converge expects at most 5 arguments")
    fields = WAIT_CONVERGE_DEFAULTS.copy()
    for idx, raw in enumerate(parts[1:]):
        try:
            value = int(raw)
        except ValueError as error:
            raise ReplayError(
                f"wait-converge arg {idx + 1} must be an integer, got {raw!r}"
            ) from error
        if value < 0:
            raise ReplayError(
                f"wait-converge arg {idx + 1} must be non-negative, got {value}"
            )
        fields[idx] = value
    return fields


def wait_converge_timeout_ms(parts):
    try:
        return wait_converge_fields(parts)[4]
    except ReplayError as error:
        raise ReplayError(str(error).replace("arg 5", "timeout_ms")) from error


def wait_for_nonempty_file(path, timeout_s, poll_s=0.05, proc=None):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            stat = path.stat()
        except FileNotFoundError:
            stat = None
        if stat is not None and stat.st_size > 0:
            return True
        if proc is not None and proc.poll() is not None:
            raise ReplayError(f"process exited with status {proc.returncode}")
        time.sleep(poll_s)
    # Deadline blew. Report failure rather than re-probing: a file that only
    # appears after the timeout (or is still being written) must not count as
    # ready, or callers gate on truncated/partial data.
    try:
        stat = path.stat()
    except FileNotFoundError:
        return False
    return stat.st_size > 0 and stat.st_mtime <= deadline


def write_internal_replay(source_path, dest_path, snapshot_dir=None, sync_dir=None):
    """Translate runner replay commands to the in-process replay engine.

    The library-side engine handles input/timing commands and, when
    snapshot_dir is provided, also writes per-screenshot BMP files via
    libx11-compat's snapshot helper. The runner reads those BMPs back
    instead of running screencapture, which on macOS deactivates the
    target NSApp briefly and stalls SDL's event pump (verified
    empirically -- wheel events queued mid-screencapture never reach
    the X client).
    """
    lines = [
        "# generated by scripts/run-ui-replay.py",
        "# consumed by libx11-compat's LIBX11_COMPAT_REPLAY engine",
    ]
    for lineno, parts in parse_replay(source_path):
        command = parts[0]
        if command == "delay":
            if len(parts) != 2:
                raise ReplayError(f"{source_path}:{lineno}: delay expects milliseconds")
            lines.append(f"delay {int(parts[1])}")
        elif command == "motion":
            if len(parts) != 3:
                raise ReplayError(f"{source_path}:{lineno}: motion expects x y")
            lines.append(f"motion {int(parts[1])} {int(parts[2])}")
        elif command == "target-motion":
            if len(parts) != 3:
                raise ReplayError(f"{source_path}:{lineno}: target-motion expects x y")
            lines.append(f"target-motion {int(parts[1])} {int(parts[2])}")
        elif command == "button":
            if len(parts) != 3:
                raise ReplayError(
                    f"{source_path}:{lineno}: button expects n press|release|click"
                )
            button = int(parts[1])
            if parts[2] == "click":
                lines.append(f"button {button} press")
                lines.append("delay 10")
                lines.append(f"button {button} release")
            elif parts[2] in ("press", "release"):
                lines.append(f"button {button} {parts[2]}")
            else:
                raise ReplayError(
                    f"{source_path}:{lineno}: button action must be press, release, or click"
                )
        elif command == "wheel":
            if len(parts) != 3:
                raise ReplayError(
                    f"{source_path}:{lineno}: wheel expects up|down count"
                )
            if parts[1] not in ("up", "down"):
                raise ReplayError(
                    f"{source_path}:{lineno}: wheel direction must be up or down"
                )
            button = 4 if parts[1] == "up" else 5
            for _ in range(int(parts[2])):
                lines.append(f"button {button} press")
                lines.append(f"button {button} release")
                lines.append("delay 50")
        elif command == "key":
            if len(parts) == 2:
                lines.append(f"key {int(parts[1])} press")
                lines.append("delay 10")
                lines.append(f"key {int(parts[1])} release")
            elif len(parts) == 3 and parts[2] in ("down", "up"):
                action = "press" if parts[2] == "down" else "release"
                lines.append(f"key {int(parts[1])} {action}")
                lines.append("delay 10")
            else:
                raise ReplayError(
                    f"{source_path}:{lineno}: key expects scancode [down|up]"
                )
        elif command == "screenshot":
            if snapshot_dir is None:
                # Host screenshots are invisible to the in-process replay
                # thread. Preserve ordering so input after a screenshot does
                # not race ahead and contaminate the captured baseline.
                lines.append("delay 500")
                continue
            if len(parts) < 2:
                raise ReplayError(f"{source_path}:{lineno}: screenshot needs a name")
            name = parts[1]
            # BMP, written by SDL_SaveBMP; the runner converts to PNG
            # after the replay completes.
            lines.append(f"snapshot {snapshot_dir}/{name}.bmp")
        elif command == "resize":
            if len(parts) != 3:
                raise ReplayError(f"{source_path}:{lineno}: resize expects W H")
            lines.append(f"resize {int(parts[1])} {int(parts[2])}")
        elif command == "close":
            if len(parts) > 2:
                raise ReplayError(f"{source_path}:{lineno}: close expects [latest]")
            lines.append("close" if len(parts) == 1 else f"close {parts[1]}")
        elif command == "target":
            if len(parts) != 2 or parts[1] not in ("first", "latest"):
                raise ReplayError(
                    f"{source_path}:{lineno}: target expects first|latest"
                )
            lines.append(f"target {parts[1]}")
        elif command == "target-at":
            if len(parts) != 3:
                raise ReplayError(f"{source_path}:{lineno}: target-at expects x y")
            lines.append(f"target-at {int(parts[1])} {int(parts[2])}")
        elif command == "wait-window":
            if len(parts) != 3:
                raise ReplayError(
                    f"{source_path}:{lineno}: wait-window expects pattern timeout_ms"
                )
            wait_ms = int(parts[2])
            if wait_ms > 0:
                lines.append(f"delay {wait_ms}")
        elif command == "focus-at":
            if len(parts) != 3:
                raise ReplayError(
                    f"{source_path}:{lineno}: focus-at expects target-local x y"
                )
            try:
                fx = int(parts[1])
                fy = int(parts[2])
            except ValueError as error:
                raise ReplayError(
                    f"{source_path}:{lineno}: focus-at coords must be "
                    f"integers, got {parts[1]!r} {parts[2]!r}"
                ) from error
            if fx < 0 or fy < 0:
                raise ReplayError(
                    f"{source_path}:{lineno}: focus-at coords must be "
                    f"non-negative, got {fx} {fy}"
                )
            lines.append(f"focus-at {fx} {fy}")
        elif command == "wait-converge":
            # Default args mirror the C-side defaults; allow override:
            # wait-converge [bucket_ms quiet diverged threshold timeout_ms]
            fields = wait_converge_fields(parts)
            if sync_dir is not None:
                failure_path = sync_dir / f"wait-converge-{lineno}.fail"
                expanded = " ".join(str(value) for value in fields)
                lines.append(f"wait-converge {expanded} failure-marker={failure_path}")
                lines.append(f"state-snapshot {sync_dir}/wait-converge-{lineno}.json")
            else:
                extra = " ".join(parts[1:]) if len(parts) > 1 else ""
                lines.append(f"wait-converge {extra}".rstrip())
        elif command == "state-snapshot":
            if len(parts) < 2:
                raise ReplayError(
                    f"{source_path}:{lineno}: state-snapshot needs a name"
                )
            name = parts[1]
            if snapshot_dir is None:
                # The in-process state writer needs a directory the runner
                # owns; without --in-process-snapshots we have no place to
                # put it. Skip with a delay so a later wait-window doesn't
                # race the missing file.
                lines.append("delay 50")
                continue
            states_dir = snapshot_dir.parent / "states"
            lines.append(f"state-snapshot {states_dir}/{name}.json")
        elif command in (
            "assert-image",
            "assert-exit",
            "assert-state",
            "timeline-summary",
            "wait-exit",
        ):
            continue
        else:
            raise ReplayError(
                f"{source_path}:{lineno}: unknown replay command {command}"
            )

    dest_path.parent.mkdir(parents=True, exist_ok=True)
    dest_path.write_text("\n".join(lines) + "\n")
    return dest_path


def bring_process_to_front(pid):
    if sys.platform != "darwin":
        return
    script = (
        'tell application "System Events" to set frontmost of '
        f"(first process whose unix id is {pid}) to true"
    )
    # Best-effort: a headless runner without osascript, a denied
    # Accessibility prompt, or a vanished pid must not block or fail the
    # replay. Bound the wait so a hung osascript cannot stall capture.
    try:
        subprocess.run(
            ["osascript", "-e", script],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
            timeout=2,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return
    time.sleep(0.1)


def capture_screen(path, env, command, region, frontmost_pid=None):
    """Capture either the full screen or a fixed pixel region.

    region is an (x, y, w, h) tuple in display coordinates, or None for
    full-screen. macOS screencapture, ImageMagick import, and the
    in-process pixman snapshot helper all support the region form; only
    gnome-screenshot has no region equivalent (it captures the whole
    screen and we crop afterward).
    """
    path.parent.mkdir(parents=True, exist_ok=True)
    if command == "auto":
        if env.get("DISPLAY") and command_exists("import"):
            command = "import"
        elif command_exists("gnome-screenshot"):
            command = "gnome-screenshot"
        elif command_exists("screencapture"):
            command = "screencapture"
        else:
            raise ReplayError(
                "no screenshot command found; install ImageMagick import, "
                "gnome-screenshot, or macOS screencapture"
            )
    if command == "screencapture" and frontmost_pid is not None:
        bring_process_to_front(frontmost_pid)

    if command == "import":
        cmd = ["import", "-window", "root"]
        if region is not None:
            x, y, w, h = region
            cmd += ["-crop", f"{w}x{h}+{x}+{y}", "+repage"]
        cmd.append(str(path))
        result = run_command(cmd, env=env)
    elif command == "gnome-screenshot":
        result = run_command(["gnome-screenshot", "-f", str(path)], env=env)
        if result.returncode == 0 and region is not None:
            ensure_pil()
            full = Image.open(path)
            x, y, w, h = region
            full.crop((x, y, x + w, y + h)).save(path)
    elif command == "screencapture":
        cmd = ["screencapture", "-x"]
        if region is not None:
            x, y, w, h = region
            cmd += ["-R", f"{x},{y},{w},{h}"]
        cmd.append(str(path))
        result = run_command(cmd, env=env)
    else:
        raise ReplayError(f"unknown screenshot command: {command}")
    if result.returncode != 0:
        raise ReplayError(f"screenshot command failed for {path}")
    if not path.exists() or path.stat().st_size == 0:
        raise ReplayError(f"screenshot is empty: {path}")


def parse_replay(path):
    steps = []
    with path.open() as f:
        for lineno, line in enumerate(f, 1):
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            try:
                parts = shlex.split(stripped)
            except ValueError as error:
                raise ReplayError(f"{path}:{lineno}: {error}") from error
            steps.append((lineno, parts))
    return steps


def crop(img, rect):
    if rect is None:
        return img
    x, y, w, h = rect
    return img.crop((x, y, x + w, y + h))


def assertion_rect(rule, img):
    if "display_rect" not in rule:
        return rule.get("rect")
    base = rule.get("display_size")
    if not base or len(base) != 2:
        raise ReplayError("display_rect assertions require display_size [w, h]")
    base_w, base_h = base
    if base_w <= 0 or base_h <= 0:
        raise ReplayError("display_size dimensions must be positive")
    sx = img.width / base_w
    sy = img.height / base_h
    x, y, w, h = rule["display_rect"]
    return [
        int(round(x * sx)),
        int(round(y * sy)),
        int(round(w * sx)),
        int(round(h * sy)),
    ]


def dark_mask_stats(img, dark_threshold):
    """Count pixels at or below dark_threshold on all three channels.

    Iterating raw bytes is ~50x faster than Image.getdata() over the
    Python iterator protocol on large frames, and Pillow 14 deprecates
    getdata() outright.
    """
    rgb = img.convert("RGB")
    raw = rgb.tobytes()
    total = max(1, len(raw) // 3)
    dark = 0
    for offset in range(0, len(raw), 3):
        if (
            raw[offset] <= dark_threshold
            and raw[offset + 1] <= dark_threshold
            and raw[offset + 2] <= dark_threshold
        ):
            dark += 1
    return dark, total


def dark_row_runs(img, dark_threshold, min_row_dark_ratio):
    rgb = img.convert("RGB")
    raw = rgb.tobytes()
    width = max(1, rgb.width)
    row_stride = width * 3
    runs = []
    start = None
    for y in range(rgb.height):
        row_dark = 0
        row_start = y * row_stride
        row_end = row_start + row_stride
        for offset in range(row_start, row_end, 3):
            if (
                raw[offset] <= dark_threshold
                and raw[offset + 1] <= dark_threshold
                and raw[offset + 2] <= dark_threshold
            ):
                row_dark += 1
        active = row_dark / width >= min_row_dark_ratio
        if active and start is None:
            start = y
        elif not active and start is not None:
            runs.append(y - start)
            start = None
    if start is not None:
        runs.append(rgb.height - start)
    # Return every run and let the rule's min_run_height / max_run_height bounds
    # decide what counts. Dropping 1px runs here made a default rule
    # (min_run_height=1, min_runs=1) impossible to satisfy on a 1px active run.
    return runs


def dense_dark_rows(img, dark_threshold, max_row_ratio):
    rgb = img.convert("RGB")
    raw = rgb.tobytes()
    width = max(1, rgb.width)
    row_stride = width * 3
    dense_rows = 0
    for y in range(rgb.height):
        row_dark = 0
        row_start = y * row_stride
        row_end = row_start + row_stride
        for offset in range(row_start, row_end, 3):
            if (
                raw[offset] <= dark_threshold
                and raw[offset + 1] <= dark_threshold
                and raw[offset + 2] <= dark_threshold
            ):
                row_dark += 1
        if row_dark / width > max_row_ratio:
            dense_rows += 1
    return dense_rows


def image_changed_ratio(a, b):
    width = min(a.width, b.width)
    height = min(a.height, b.height)
    if width <= 0 or height <= 0:
        return 0.0
    a = a.crop((0, 0, width, height)).convert("RGB")
    b = b.crop((0, 0, width, height)).convert("RGB")
    diff = ImageChops.difference(a, b).tobytes()
    changed = 0
    total = width * height
    for offset in range(0, len(diff), 3):
        if diff[offset] + diff[offset + 1] + diff[offset + 2] > 24:
            changed += 1
    return changed / total


def assert_state(rule_path, state_path):
    """Apply a state-matcher rule file against a UiSnapshot JSON file.

    The rule schema is intentionally narrow so a failing assertion can be
    explained without a debugger. Each matcher is one of:

      Scalar-field matchers naming a single snapshot field (focused_window,
      pointer_grab, keyboard_grab, mapped_window_count, popup_window_count,
      window_count, event_queue_depth):
        {"type": "equal", "field": ..., "expected": ...}
        {"type": "leq",   "field": ..., "max": ...}
        {"type": "geq",   "field": ..., "min": ...}

      Per-window predicates over snapshot["windows"]:
        {"type": "wm_class_present", "wm_class": ...}
        {"type": "window_with_size", "w": ..., "h": ...}
        {"type": "window_with_geometry", ...}  exact match on any of wm_class,
            wm_name, x, y, w, h, map_state, and the shape_* fields.
        {"type": "window_with_geometry_range", ...}  exact match on wm_class,
            wm_name, map_state, plus inclusive bounds via <field>_min /
            <field>_max for each of x, y, w, h (any subset).

      Grab and property matchers:
        {"type": "grab_released"}  pointer_grab and keyboard_grab must be 0.
        {"type": "property_unchanged", "between": [baseline_json, ...], ...}
        {"type": "window_geometry_changed", ...}
        {"type": "property_changed_to", ...}
    """
    if not rule_path.exists():
        raise ReplayError(f"state-rule file not found: {rule_path}")
    if not state_path.exists():
        raise ReplayError(f"state file not found: {state_path}")
    with rule_path.open() as f:
        rule_doc = json.load(f)
    with state_path.open() as f:
        snap = json.load(f)
    failures = []
    for rule in rule_doc.get("assertions", []):
        rule_type = rule.get("type")
        if rule_type == "equal":
            field = rule["field"]
            expected = rule["expected"]
            actual = snap.get(field)
            if actual != expected:
                failures.append(f"field {field}: expected {expected!r}, got {actual!r}")
        elif rule_type == "leq":
            field = rule["field"]
            limit = rule["max"]
            actual = snap.get(field, 0)
            if actual > limit:
                failures.append(f"field {field}: {actual} exceeds max {limit}")
        elif rule_type == "geq":
            field = rule["field"]
            floor = rule["min"]
            actual = snap.get(field, 0)
            if actual < floor:
                failures.append(f"field {field}: {actual} below min {floor}")
        elif rule_type == "wm_class_present":
            wanted = rule["wm_class"]
            found = any(
                entry.get("wm_class") == wanted for entry in snap.get("windows", [])
            )
            if not found:
                failures.append(f"no window with wm_class={wanted!r}")
        elif rule_type == "window_with_size":
            w = rule.get("w")
            h = rule.get("h")
            found = any(
                (w is None or entry.get("w") == w)
                and (h is None or entry.get("h") == h)
                for entry in snap.get("windows", [])
            )
            if not found:
                failures.append(f"no window with size w={w} h={h}")
        elif rule_type == "window_with_geometry":
            fields = (
                "wm_class",
                "wm_name",
                "x",
                "y",
                "w",
                "h",
                "map_state",
                "shape_bounding",
                "shape_bounding_origin",
                "shape_bounding_bottom_left",
                "shape_bounding_bottom_left_hit",
                "shape_clip",
            )
            expected = {field: rule[field] for field in fields if field in rule}
            found = any(
                all(entry.get(field) == value for field, value in expected.items())
                for entry in snap.get("windows", [])
            )
            if not found:
                detail = " ".join(
                    f"{field}={value!r}" for field, value in expected.items()
                )
                failures.append(f"no window with geometry {detail}")
        elif rule_type == "window_with_geometry_range":
            exact_fields = ("wm_class", "wm_name", "map_state")
            range_fields = ("x", "y", "w", "h")
            exact = {field: rule[field] for field in exact_fields if field in rule}

            def in_range(entry):
                for field, value in exact.items():
                    if entry.get(field) != value:
                        return False
                for field in range_fields:
                    actual = entry.get(field)
                    if actual is None:
                        return False
                    min_key = f"{field}_min"
                    max_key = f"{field}_max"
                    if min_key in rule and actual < rule[min_key]:
                        return False
                    if max_key in rule and actual > rule[max_key]:
                        return False
                return True

            if not any(in_range(entry) for entry in snap.get("windows", [])):
                detail = " ".join(
                    f"{key}={value!r}" for key, value in rule.items() if key != "type"
                )
                failures.append(f"no window with geometry range {detail}")
        elif rule_type == "grab_released":
            for field in ("pointer_grab", "keyboard_grab"):
                if snap.get(field, 0) != 0:
                    failures.append(f"{field} still held by window {snap.get(field)}")
        elif rule_type == "property_unchanged":
            # The baseline must live alongside the current state file under
            # states_dir. Reject absolute paths and any segments that walk
            # outside it so a malicious rule cannot point the runner at
            # arbitrary on-disk JSON (e.g. ../../etc/passwd-shaped paths
            # that happen to parse as JSON).
            raw_baseline = rule["between"][0]
            baseline_candidate = Path(raw_baseline)
            if baseline_candidate.is_absolute():
                failures.append(f"property baseline must be relative: {raw_baseline!r}")
                continue
            states_root = state_path.parent.resolve()
            baseline_path = (state_path.parent / baseline_candidate).resolve()
            try:
                baseline_path.relative_to(states_root)
            except ValueError:
                failures.append(
                    f"property baseline {raw_baseline!r} escapes states " f"directory"
                )
                continue
            if not baseline_path.exists():
                failures.append(f"property baseline {baseline_path} missing")
                continue
            with baseline_path.open() as bf:
                baseline = json.load(bf)
            target_window = rule["window"]
            target_atom = rule["atom"]
            new_value = _find_property(snap, target_window, target_atom)
            old_value = _find_property(baseline, target_window, target_atom)
            if new_value != old_value:
                failures.append(
                    f"property atom={target_atom} on window={target_window} "
                    f"changed: {old_value} -> {new_value}"
                )
        elif rule_type == "window_geometry_changed":
            raw_baseline = rule["between"][0]
            baseline_candidate = Path(raw_baseline)
            if baseline_candidate.is_absolute():
                failures.append(f"geometry baseline must be relative: {raw_baseline!r}")
                continue
            states_root = state_path.parent.resolve()
            baseline_path = (state_path.parent / baseline_candidate).resolve()
            try:
                baseline_path.relative_to(states_root)
            except ValueError:
                failures.append(
                    f"geometry baseline {raw_baseline!r} escapes states directory"
                )
                continue
            if not baseline_path.exists():
                failures.append(f"geometry baseline {baseline_path} missing")
                continue
            with baseline_path.open() as bf:
                baseline = json.load(bf)
            wanted = rule["wm_class"]

            def first_window(doc):
                return next(
                    (
                        entry
                        for entry in doc.get("windows", [])
                        if entry.get("wm_class") == wanted
                    ),
                    None,
                )

            old = first_window(baseline)
            new = first_window(snap)
            if old is None or new is None:
                failures.append(f"missing window for geometry compare {wanted!r}")
                continue
            old_geom = tuple(old.get(field) for field in ("x", "y", "w", "h"))
            new_geom = tuple(new.get(field) for field in ("x", "y", "w", "h"))
            if old_geom == new_geom:
                failures.append(f"window geometry unchanged for {wanted!r}: {new_geom}")
        elif rule_type == "property_changed_to":
            target_window = rule["window"]
            target_atom = rule["atom"]
            wanted_hex = rule["hex"]
            value = _find_property(snap, target_window, target_atom)
            if value is None or value.get("hex") != wanted_hex:
                failures.append(
                    f"property atom={target_atom} on window={target_window} "
                    f"not at expected value: got {value}"
                )
        else:
            failures.append(f"unknown state assertion {rule_type!r}")
    if failures:
        raise ReplayError(
            f"{state_path.name} failed {rule_path.name}: {'; '.join(failures)}"
        )
    return []


def _find_property(snap, window_id, atom):
    for entry in snap.get("windows", []):
        if entry.get("window") != window_id:
            continue
        for prop in entry.get("properties", []):
            if prop.get("atom") == atom:
                return {
                    "source": prop.get("source"),
                    "hex": prop.get("hex"),
                    "format": prop.get("format"),
                    "length": prop.get("length"),
                }
    return None


def assert_timeline_summary(rule_path, timeline_path):
    """Read a JSONL timeline and assert per-kind event counts.

    Rule schema:
      {
        "assertions": [
          {"type": "kind_at_least", "kind": "ButtonPress", "min": 1},
          {"type": "kind_at_most",  "kind": "diverged",   "max": 0},
          {"type": "kind_equal",    "kind": "MapNotify",  "expected": 3}
        ]
      }
    """
    if not rule_path.exists():
        raise ReplayError(f"timeline-rule file not found: {rule_path}")
    if not timeline_path.exists():
        raise ReplayError(f"timeline file not found: {timeline_path}")
    counts = {}
    with timeline_path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                continue
            kind = record.get("kind")
            if kind:
                counts[kind] = counts.get(kind, 0) + 1
    with rule_path.open() as f:
        rule_doc = json.load(f)
    failures = []
    for rule in rule_doc.get("assertions", []):
        rule_type = rule.get("type")
        kind = rule.get("kind")
        observed = counts.get(kind, 0)
        if rule_type == "kind_at_least":
            limit = rule["min"]
            if observed < limit:
                failures.append(f"kind {kind}: {observed} below min {limit}")
        elif rule_type == "kind_at_most":
            limit = rule["max"]
            if observed > limit:
                failures.append(f"kind {kind}: {observed} above max {limit}")
        elif rule_type == "kind_equal":
            expected = rule["expected"]
            if observed != expected:
                failures.append(f"kind {kind}: {observed} != expected {expected}")
        else:
            failures.append(f"unknown timeline assertion {rule_type!r}")
    if failures:
        raise ReplayError(
            f"{timeline_path.name} failed {rule_path.name}: " f"{'; '.join(failures)}"
        )
    return counts


def assert_image(rule_path, image_path, screenshots, assertion_base):
    ensure_pil()
    if not rule_path.exists():
        raise ReplayError(f"assertion file not found: {rule_path}")
    with rule_path.open() as f:
        rule_doc = json.load(f)
    img = Image.open(image_path).convert("RGBA")
    failures = []
    observations = []

    for rule in rule_doc.get("assertions", []):
        rule_type = rule.get("type")
        if rule_type == "non_empty":
            if image_path.stat().st_size <= 0 or img.width <= 0 or img.height <= 0:
                failures.append("image is empty")
        elif rule_type == "min_size":
            min_w = int(rule.get("width", 1))
            min_h = int(rule.get("height", 1))
            if img.width < min_w or img.height < min_h:
                failures.append(
                    f"image size {img.width}x{img.height} below {min_w}x{min_h}"
                )
        elif rule_type == "max_size":
            max_w = int(rule.get("width", 999999))
            max_h = int(rule.get("height", 999999))
            if img.width > max_w or img.height > max_h:
                failures.append(
                    f"image size {img.width}x{img.height} above {max_w}x{max_h}"
                )
        elif rule_type == "not_all_black":
            extrema = img.convert("RGB").getextrema()
            if max(channel[1] for channel in extrema) <= 8:
                failures.append("image is all black")
        elif rule_type == "region_non_background":
            region = crop(img, assertion_rect(rule, img))
            dark, total = dark_mask_stats(region, int(rule.get("dark_threshold", 96)))
            ratio = dark / total
            if ratio < float(rule.get("min_dark_ratio", 0.001)):
                failures.append(
                    f"region dark ratio {ratio:.5f} below "
                    f"{float(rule.get('min_dark_ratio', 0.001)):.5f}"
                )
        elif rule_type == "region_max_dark_ratio":
            region = crop(img, assertion_rect(rule, img))
            dark, total = dark_mask_stats(region, int(rule.get("dark_threshold", 32)))
            ratio = dark / total
            if ratio > float(rule.get("max_dark_ratio", 0.35)):
                failures.append(
                    f"region dark ratio {ratio:.5f} above "
                    f"{float(rule.get('max_dark_ratio', 0.35)):.5f}"
                )
        elif rule_type == "dark_row_run_height":
            region = crop(img, assertion_rect(rule, img)).convert("RGB")
            runs = dark_row_runs(
                region,
                int(rule.get("dark_threshold", 96)),
                float(rule.get("min_row_dark_ratio", 0.01)),
            )
            min_runs = int(rule.get("min_runs", 1))
            min_h = int(rule.get("min_run_height", 1))
            max_h = int(rule.get("max_run_height", 9999))
            good = [height for height in runs if min_h <= height <= max_h]
            if len(good) < min_runs:
                failures.append(
                    f"dark row runs {runs} did not include {min_runs} "
                    f"runs in [{min_h}, {max_h}]"
                )
        elif rule_type == "region_min_unique_colors":
            region = crop(img, assertion_rect(rule, img)).convert("RGB")
            raw = region.tobytes()
            colors = {raw[offset : offset + 3] for offset in range(0, len(raw), 3)}
            expected = int(rule.get("min_unique_colors", 2))
            if len(colors) < expected:
                failures.append(f"region unique colors {len(colors)} below {expected}")
        elif rule_type == "region_min_color_coverage":
            # Fraction of pixels matching any of a set of target colors within a
            # per-channel tolerance. Unlike unique-color counts, this detects
            # that specific expected content (e.g. named layer fills) is present
            # in quantity, so a differently-rendered-but-still-colorful frame
            # cannot pass. Tolerance absorbs SDL2/SDL3 color-conversion drift.
            region = crop(img, assertion_rect(rule, img)).convert("RGB")
            raw = region.tobytes()
            targets = [tuple(int(c) for c in color) for color in rule.get("colors", [])]
            tol = int(rule.get("tolerance", 24))
            total = max(1, len(raw) // 3)
            hit = 0
            for offset in range(0, len(raw), 3):
                r, g, b = raw[offset], raw[offset + 1], raw[offset + 2]
                if any(
                    abs(r - tr) <= tol and abs(g - tg) <= tol and abs(b - tb) <= tol
                    for tr, tg, tb in targets
                ):
                    hit += 1
            ratio = hit / total
            expected_ratio = float(rule.get("min_ratio", 0.05))
            if ratio < expected_ratio:
                failures.append(
                    f"target-color coverage {ratio:.4f} below {expected_ratio:.4f}"
                )
        elif rule_type == "changed_region":
            baseline_name = rule.get("baseline")
            if baseline_name not in screenshots:
                failures.append(f"missing baseline screenshot {baseline_name}")
                continue
            baseline = Image.open(screenshots[baseline_name]).convert("RGBA")
            rect = assertion_rect(rule, img)
            ratio = image_changed_ratio(crop(img, rect), crop(baseline, rect))
            if ratio < float(rule.get("min_changed_ratio", 0.01)):
                failures.append(
                    f"changed ratio {ratio:.5f} below "
                    f"{float(rule.get('min_changed_ratio', 0.01)):.5f}"
                )
        elif rule_type == "reference_diff":
            reference = resolve_path(
                rule.get("reference", ""),
                assertion_base,
                assertion_base.parent,
                ROOT,
            )
            if not reference.exists():
                failures.append(f"reference image not found: {reference}")
                continue
            ref_img = Image.open(reference).convert("RGBA")
            rect = assertion_rect(rule, img)
            ratio = image_changed_ratio(crop(img, rect), crop(ref_img, rect))
            if ratio > float(rule.get("max_changed_ratio", 0.35)):
                failures.append(
                    f"reference changed ratio {ratio:.5f} above "
                    f"{float(rule.get('max_changed_ratio', 0.35)):.5f}"
                )
        elif rule_type == "stale_text":
            region = crop(img, rule.get("rect")).convert("RGB")
            threshold = int(rule.get("dark_threshold", 80))
            max_row_ratio = float(rule.get("max_row_dark_ratio", 0.42))
            max_dense_rows = int(rule.get("max_dense_rows", 18))
            dense_rows = dense_dark_rows(region, threshold, max_row_ratio)
            observations.append(
                {
                    "metric": "stale_text_dense_rows",
                    "value": dense_rows,
                    "detail": str(rule_path),
                }
            )
            if dense_rows > max_dense_rows:
                failures.append(
                    f"stale-text dense rows {dense_rows} above {max_dense_rows}"
                )
        else:
            failures.append(f"unknown assertion type {rule_type}")

    if failures:
        rel_rule = rule_path
        try:
            rel_rule = rule_path.relative_to(assertion_base)
        except ValueError:
            pass
        raise ReplayError(f"{image_path.name} failed {rel_rule}: {'; '.join(failures)}")
    return observations


def write_results(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        fields = ["status", "relative_path", "screenshot", "detail"]
        writer = csv.DictWriter(f, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def add_metric(
    rows, lineno, command, target, metric, value="", duration_ms="", detail=""
):
    rows.append(
        {
            "lineno": lineno,
            "command": command,
            "target": target,
            "metric": metric,
            "value": value,
            "duration_ms": duration_ms,
            "detail": detail,
        }
    )


def write_metrics(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        fields = [
            "lineno",
            "command",
            "target",
            "metric",
            "value",
            "duration_ms",
            "detail",
        ]
        writer = csv.DictWriter(f, fieldnames=fields, delimiter="\t")
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def write_json(path, doc):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write("\n")


def tail_text(path, line_count=20):
    if not path.exists():
        return ""
    with path.open() as f:
        return "".join(f.readlines()[-line_count:])


def summarize_metrics(rows):
    summary = {}
    for row in rows:
        command = row.get("command", "")
        metric = row.get("metric", "")
        duration = row.get("duration_ms")
        if not command or duration in (None, ""):
            continue
        try:
            value = float(duration)
        except ValueError:
            continue
        key = f"{command}:{metric}"
        entry = summary.setdefault(
            key,
            {"count": 0, "total_ms": 0.0, "max_ms": 0.0},
        )
        entry["count"] += 1
        entry["total_ms"] += value
        entry["max_ms"] = max(entry["max_ms"], value)
    for entry in summary.values():
        entry["avg_ms"] = entry["total_ms"] / max(1, entry["count"])
    return summary


def summarize_timeline_file(path):
    if path is None or not path.exists():
        return {}
    counts = {}
    malformed = 0
    with path.open() as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                record = json.loads(line)
            except json.JSONDecodeError:
                malformed += 1
                continue
            kind = record.get("kind", "Unknown")
            counts[kind] = counts.get(kind, 0) + 1
    return {"counts": counts, "malformed": malformed}


def summarize_tsv_file(path):
    if path is None or not path.exists():
        return {}
    with path.open(newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        rows = list(reader)
    return {
        "rows": len(rows),
        "columns": reader.fieldnames or [],
        "bytes": path.stat().st_size,
    }


def write_junit(path, name, rows):
    failures = [row for row in rows if row.get("status") not in ("captured", "ok")]
    suite = ET.Element(
        "testsuite",
        {
            "name": name,
            "tests": str(len(rows)),
            "failures": str(len(failures)),
            "errors": "0",
        },
    )
    for row in rows:
        case = ET.SubElement(
            suite,
            "testcase",
            {
                "classname": name,
                "name": row.get("relative_path") or row.get("screenshot") or "step",
            },
        )
        if row in failures:
            failure = ET.SubElement(
                case,
                "failure",
                {
                    "type": row.get("status", "failed"),
                    "message": row.get("detail", ""),
                },
            )
            failure.text = row.get("detail", "")
    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def run_replay(args):
    replay_path = resolve_path(args.replay, ROOT / "tests/ui/replays", ROOT)
    assertion_base = ROOT / "tests/ui/assertions"
    out_root = args.out_root
    artifacts = ArtifactPaths(
        out_root,
        args.name,
        trace_path=args.trace_path,
        profile_path=args.profile_json,
        summary_path=args.ci_summary,
        render_stats_path=args.render_stats,
    )
    artifacts.prepare_base()
    trace = ReplayTrace(artifacts.trace_path)
    profile = ReplayProfile(int(time.time() * 1000))
    trace.emit(
        "runner.start",
        name=args.name,
        replay=str(replay_path),
        backend=args.input_backend,
        offscreen=args.offscreen,
    )

    env = os.environ.copy()
    env.update(split_env(args.env))
    if args.render_stats is not None:
        args.render_stats.parent.mkdir(parents=True, exist_ok=True)
        env["LIBX11_COMPAT_RENDER_STATS"] = str(args.render_stats)
        profile.add_artifact("render_stats", args.render_stats)
    if args.offscreen:
        if args.input_backend != "internal" or not args.in_process_snapshots:
            raise ReplayError(
                "--offscreen requires --input-backend internal "
                "and --in-process-snapshots"
            )
        env["SDL_VIDEODRIVER"] = "dummy"
        trace.emit("resource.offscreen", sdl_videodriver="dummy")
    timeline_path = None
    if args.timeline or args.timeline_path is not None:
        timeline_path = (
            args.timeline_path
            if args.timeline_path is not None
            else out_root / "timeline.jsonl"
        )
        timeline_path.parent.mkdir(parents=True, exist_ok=True)
        if timeline_path.exists():
            timeline_path.unlink()
        env["LIBX11_COMPAT_TIMELINE"] = "1"
        env["LIBX11_COMPAT_TIMELINE_PATH"] = str(timeline_path)
        profile.add_artifact("timeline", timeline_path)
        trace.emit("resource.timeline", path=str(timeline_path))
    states_dir = None
    if args.input_backend == "internal" and args.in_process_snapshots:
        states_dir = artifacts.states_dir
        artifacts.reset_dir(states_dir)
        env.setdefault("LIBX11_COMPAT_STATE_SNAPSHOT_TIMEOUT_SEC", "120")
        trace.emit("resource.states", path=str(states_dir))
    sync_dir = None
    if args.input_backend == "internal":
        sync_dir = artifacts.sync_dir
        artifacts.reset_dir(sync_dir)
        trace.emit("resource.sync", path=str(sync_dir))
    display = args.display
    xvfb_proc = None
    if args.xvfb and args.offscreen:
        trace.emit("resource.xvfb.skip", reason="offscreen")
    elif args.xvfb:
        xvfb_proc = start_xvfb(display, args.geometry, artifacts.log_dir / "xvfb.log")
        env["DISPLAY"] = f":{display}"
        trace.emit("resource.xvfb.start", display=env["DISPLAY"])
    snapshot_dir = None
    if args.input_backend == "internal" and args.in_process_snapshots:
        snapshot_dir = artifacts.snapshot_dir
        artifacts.reset_dir(snapshot_dir)
        trace.emit("resource.snapshots", path=str(snapshot_dir))
    if args.input_backend == "internal":
        internal_replay = write_internal_replay(
            replay_path,
            out_root / "internal.replay",
            snapshot_dir=snapshot_dir,
            sync_dir=sync_dir,
        )
        env["LIBX11_COMPAT_REPLAY"] = str(internal_replay)
        profile.add_artifact("internal_replay", internal_replay)
        trace.emit("resource.internal_replay", path=str(internal_replay))

    app_log = artifacts.app_log_path.open("w")
    app_cmd = [str(args.app), *args.app_arg]
    print("+", " ".join(shlex.quote(str(c)) for c in app_cmd), flush=True)
    proc = subprocess.Popen(
        app_cmd,
        cwd=args.workdir,
        env=env,
        stdout=app_log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    trace.emit("process.start", pid=proc.pid, argv=[str(c) for c in app_cmd])

    rows = []
    metrics = []
    screenshots = {}
    failed = False
    target_window_id = None
    try:
        for lineno, parts in parse_replay(replay_path):
            command = parts[0]
            target = " ".join(parts[1:])
            step_start = time.perf_counter()
            trace.emit(
                "step.start",
                lineno=lineno,
                command=command,
                target=target,
            )
            try:
                if command == "delay":
                    if len(parts) != 2:
                        raise ReplayError("delay expects milliseconds")
                    time.sleep(int(parts[1]) / 1000.0)
                elif command == "wait-window":
                    if len(parts) != 3:
                        raise ReplayError("wait-window expects regex timeout-ms")
                    if args.input_backend == "xdotool":
                        target_window_id = wait_window(env, parts[1], int(parts[2]))
                    else:
                        wait_process_alive(proc, int(parts[2]))
                elif command == "motion":
                    if len(parts) != 3:
                        raise ReplayError("motion expects x y")
                    if args.input_backend == "xdotool":
                        if target_window_id is not None:
                            xdotool(
                                env,
                                "mousemove",
                                "--sync",
                                "--window",
                                target_window_id,
                                parts[1],
                                parts[2],
                            )
                        else:
                            xdotool(env, "mousemove", "--sync", parts[1], parts[2])
                elif command == "target-motion":
                    if len(parts) != 3:
                        raise ReplayError("target-motion expects x y")
                    if args.input_backend == "xdotool":
                        if target_window_id is None:
                            raise ReplayError(
                                "target-motion requires a prior wait-window"
                            )
                        xdotool(
                            env,
                            "mousemove",
                            "--sync",
                            "--window",
                            target_window_id,
                            parts[1],
                            parts[2],
                        )
                elif command == "target-at":
                    if len(parts) != 3:
                        raise ReplayError("target-at expects x y")
                    if args.input_backend == "xdotool":
                        raise ReplayError(
                            "target-at is not supported with the xdotool backend"
                        )
                elif command == "button":
                    if len(parts) != 3:
                        raise ReplayError("button expects n press|release|click")
                    if args.input_backend == "xdotool":
                        if parts[2] == "click":
                            xdotool(env, "click", parts[1])
                        elif parts[2] == "press":
                            xdotool(env, "mousedown", parts[1])
                        elif parts[2] == "release":
                            xdotool(env, "mouseup", parts[1])
                        else:
                            raise ReplayError(
                                "button action must be press, release, or click"
                            )
                elif command == "wheel":
                    if len(parts) != 3:
                        raise ReplayError("wheel expects up|down count")
                    button = "4" if parts[1] == "up" else "5"
                    if parts[1] not in ("up", "down"):
                        raise ReplayError("wheel direction must be up or down")
                    if args.input_backend == "xdotool":
                        for _ in range(int(parts[2])):
                            xdotool(env, "click", button)
                            time.sleep(0.05)
                elif command == "key":
                    if len(parts) not in (2, 3):
                        raise ReplayError("key expects scancode [down|up]")
                    if len(parts) == 3 and parts[2] not in ("down", "up"):
                        raise ReplayError("key direction must be down|up")
                    if args.input_backend == "xdotool":
                        keysym = replay_keysym(int(parts[1]))
                        if len(parts) == 2:
                            xdotool(env, "key", keysym)
                        elif parts[2] == "down":
                            xdotool(env, "keydown", keysym)
                        else:
                            xdotool(env, "keyup", keysym)
                elif command == "focus-at":
                    if len(parts) != 3:
                        raise ReplayError("focus-at expects target-local x y")
                    try:
                        int(parts[1])
                        int(parts[2])
                    except ValueError as error:
                        raise ReplayError(
                            f"focus-at coords must be integers, got "
                            f"{parts[1]!r} {parts[2]!r}"
                        ) from error
                    # The internal backend handles this through the expanded
                    # replay script. For xdotool, fall back to a window
                    # activate so the focus shift still happens on native.
                    # Mirror target-motion: require a prior wait-window so a
                    # missing target surfaces as a deterministic failure
                    # rather than a silent no-op (cubic-flagged).
                    if args.input_backend == "xdotool":
                        if target_window_id is None:
                            raise ReplayError("focus-at requires a prior wait-window")
                        xdotool(env, "windowfocus", target_window_id)
                elif command == "resize":
                    if len(parts) != 3:
                        raise ReplayError("resize expects W H")
                    width = int(parts[1])
                    height = int(parts[2])
                    if args.input_backend == "xdotool":
                        if target_window_id is None:
                            raise ReplayError("resize requires a prior wait-window")
                        xdotool(
                            env,
                            "windowsize",
                            "--sync",
                            target_window_id,
                            str(width),
                            str(height),
                        )
                elif command == "close":
                    if len(parts) > 2 or (len(parts) == 2 and parts[1] != "latest"):
                        raise ReplayError("close expects [latest]")
                    if args.input_backend == "xdotool":
                        if len(parts) == 2:
                            raise ReplayError(
                                "close latest is not supported with the xdotool backend"
                            )
                        if target_window_id is None:
                            raise ReplayError("close requires a prior wait-window")
                        xdotool(env, "windowclose", target_window_id)
                elif command == "target":
                    if len(parts) != 2 or parts[1] not in ("first", "latest"):
                        raise ReplayError("target expects first|latest")
                    if args.input_backend == "xdotool":
                        # The in-process target heuristic has no xdotool analog;
                        # xdotool acts on explicit wait-window ids. Fail loudly
                        # rather than let a script silently diverge per backend.
                        raise ReplayError(
                            "target is not supported with the xdotool backend"
                        )
                elif command == "screenshot":
                    if len(parts) not in (2, 6):
                        raise ReplayError("screenshot expects: name [x y w h]")
                    name = parts[1]
                    shot = artifacts.screenshot_dir / f"{name}.png"
                    bmp_path = (snapshot_dir / f"{name}.bmp") if snapshot_dir else None
                    region = args.screenshot_region
                    if len(parts) == 6:
                        region = tuple(int(v) for v in parts[2:6])
                    if bmp_path is not None:
                        # The internal replay engine writes the BMP
                        # synchronously inside the X-client main thread,
                        # but the Python script and the libx11-compat
                        # replay thread share no clock -- the file may
                        # not exist yet when this step is reached, even
                        # though the in-process timeline schedules its
                        # snapshot at the same point. Poll up to the
                        # libx11-compat snapshot timeout (120s); the
                        # save itself is sub-millisecond but the main
                        # thread may be mid-reflow (e.g. right after a
                        # resize) or flushing SDL's dummy-driver software
                        # queue and not drain the SDL_USEREVENT for several
                        # seconds.
                        wait_start = time.perf_counter()
                        wait_for_nonempty_file(bmp_path, 121.0)
                        add_metric(
                            metrics,
                            lineno,
                            command,
                            name,
                            "snapshot_wait_ms",
                            duration_ms=f"{(time.perf_counter() - wait_start) * 1000.0:.3f}",
                            detail=str(bmp_path),
                        )
                    if (
                        bmp_path is not None
                        and bmp_path.exists()
                        and bmp_path.stat().st_size > 0
                    ):
                        ensure_pil()
                        convert_start = time.perf_counter()
                        Image.open(bmp_path).save(shot)
                        add_metric(
                            metrics,
                            lineno,
                            command,
                            name,
                            "snapshot_convert_ms",
                            duration_ms=f"{(time.perf_counter() - convert_start) * 1000.0:.3f}",
                            detail=str(shot),
                        )
                    elif bmp_path is not None:
                        # --in-process-snapshots was explicitly requested
                        # but no BMP showed up. Silently falling back to
                        # screencapture would re-enter the exact
                        # NSApp-stall path this flag was added to avoid,
                        # so a passing test would no longer prove the
                        # in-process path works (codex-flagged). Fail
                        # loudly with the app-log tail so the
                        # underlying snapshot failure is debuggable.
                        log_tail = ""
                        log_tail = tail_text(artifacts.app_log_path)
                        raise ReplayError(
                            f"in-process snapshot to {bmp_path} did "
                            f"not produce a file within 121s; refusing "
                            f"to silently fall back to screencapture. "
                            f"App log tail:\n{log_tail}"
                        )
                    else:
                        capture_start = time.perf_counter()
                        capture_screen(
                            shot,
                            env,
                            args.screenshot_command,
                            region,
                            frontmost_pid=proc.pid,
                        )
                        add_metric(
                            metrics,
                            lineno,
                            command,
                            name,
                            "screenshot_capture_ms",
                            duration_ms=f"{(time.perf_counter() - capture_start) * 1000.0:.3f}",
                            detail=str(shot),
                        )
                    screenshots[name] = shot
                    profile.add_artifact(f"screen:{name}", shot)
                    if bmp_path is not None:
                        profile.add_artifact(f"snapshot:{name}", bmp_path)
                    rows.append(
                        {
                            "status": "captured",
                            "relative_path": parts[1],
                            "screenshot": str(shot),
                            "detail": str(replay_path),
                        }
                    )
                elif command == "assert-image":
                    if len(parts) != 3:
                        raise ReplayError("assert-image expects screenshot rule")
                    if parts[1] not in screenshots:
                        raise ReplayError(f"unknown screenshot {parts[1]}")
                    rule_path = resolve_path(
                        parts[2], assertion_base, replay_path.parent, ROOT
                    )
                    assert_start = time.perf_counter()
                    observations = assert_image(
                        rule_path,
                        screenshots[parts[1]],
                        screenshots,
                        assertion_base,
                    )
                    add_metric(
                        metrics,
                        lineno,
                        command,
                        f"{parts[1]}:{parts[2]}",
                        "assert_image_ms",
                        duration_ms=f"{(time.perf_counter() - assert_start) * 1000.0:.3f}",
                        detail=str(rule_path),
                    )
                    for observation in observations:
                        add_metric(
                            metrics,
                            lineno,
                            command,
                            parts[1],
                            observation["metric"],
                            value=observation["value"],
                            detail=observation.get("detail", ""),
                        )
                    rows.append(
                        {
                            "status": "ok",
                            "relative_path": f"{parts[1]}:{parts[2]}",
                            "detail": "",
                        }
                    )
                elif command == "wait-converge":
                    if args.input_backend == "internal":
                        sync_target = sync_dir / f"wait-converge-{lineno}.json"
                        failure_target = sync_dir / f"wait-converge-{lineno}.fail"
                        wait_start = time.perf_counter()
                        timeout_s = wait_converge_timeout_ms(parts) / 1000.0 + 121.0
                        if not wait_for_nonempty_file(
                            sync_target, timeout_s, proc=proc
                        ):
                            raise ReplayError(
                                f"wait-converge marker {sync_target} did not "
                                f"appear within "
                                f"{time.perf_counter() - wait_start:.2f}s"
                            )
                        if failure_target.exists():
                            detail = failure_target.read_text().strip() or "failed"
                            raise ReplayError(f"wait-converge {detail}")
                    else:
                        # External backends have no detector; mirror the
                        # legacy delay-shaped behavior so the script stays
                        # portable.
                        time.sleep(0.5)
                elif command == "wait-exit":
                    if len(parts) != 2:
                        raise ReplayError("wait-exit expects timeout-ms")
                    wait_process_exit(proc, int(parts[1]))
                    rows.append(
                        {
                            "status": "ok",
                            "relative_path": f"wait-exit:{parts[1]}",
                            "detail": "",
                        }
                    )
                elif command == "state-snapshot":
                    if len(parts) < 2:
                        raise ReplayError("state-snapshot expects a name")
                    if states_dir is None or args.input_backend != "internal":
                        # No way to capture in-process state without the
                        # internal backend + --in-process-snapshots. Skip
                        # rather than fail so existing replays still run.
                        rows.append(
                            {
                                "status": "ok",
                                "relative_path": f"state-snapshot:{parts[1]}",
                                "detail": "skipped (no in-process snapshots)",
                            }
                        )
                        continue
                    state_target = states_dir / f"{parts[1]}.json"
                    wait_start = time.perf_counter()
                    if not wait_for_nonempty_file(state_target, 121.0):
                        raise ReplayError(
                            f"state-snapshot {state_target} did not appear "
                            f"within {time.perf_counter() - wait_start:.2f}s"
                        )
                    profile.add_artifact(f"state:{parts[1]}", state_target)
                    rows.append(
                        {
                            "status": "captured",
                            "relative_path": f"state-snapshot:{parts[1]}",
                            "screenshot": str(state_target),
                            "detail": "",
                        }
                    )
                elif command == "assert-state":
                    if len(parts) != 3:
                        raise ReplayError("assert-state expects name rule")
                    if states_dir is None:
                        rows.append(
                            {
                                "status": "ok",
                                "relative_path": (
                                    f"assert-state:{parts[1]}:{parts[2]}"
                                ),
                                "detail": "skipped (no in-process snapshots)",
                            }
                        )
                        continue
                    state_target = states_dir / f"{parts[1]}.json"
                    rule_path = resolve_path(
                        parts[2], assertion_base, replay_path.parent, ROOT
                    )
                    assert_state(rule_path, state_target)
                    rows.append(
                        {
                            "status": "ok",
                            "relative_path": (f"assert-state:{parts[1]}:{parts[2]}"),
                            "detail": str(rule_path),
                        }
                    )
                elif command == "timeline-summary":
                    if len(parts) != 2:
                        raise ReplayError("timeline-summary expects a rule path")
                    if timeline_path is None:
                        rows.append(
                            {
                                "status": "ok",
                                "relative_path": f"timeline-summary:{parts[1]}",
                                "detail": "skipped (no --timeline)",
                            }
                        )
                        continue
                    rule_path = resolve_path(
                        parts[1], assertion_base, replay_path.parent, ROOT
                    )
                    assert_timeline_summary(rule_path, timeline_path)
                    rows.append(
                        {
                            "status": "ok",
                            "relative_path": f"timeline-summary:{parts[1]}",
                            "detail": str(rule_path),
                        }
                    )
                elif command == "assert-exit":
                    if len(parts) != 2:
                        raise ReplayError("assert-exit expects status|any|running")
                    status = proc.poll()
                    expected = parts[1]
                    if expected == "running":
                        if status is not None:
                            raise ReplayError(f"process exited with status {status}")
                    elif expected == "any":
                        if status is None:
                            raise ReplayError("process is still running")
                    else:
                        if status is None:
                            raise ReplayError(
                                f"process still running, expected {expected}"
                            )
                        if status != int(expected):
                            raise ReplayError(
                                f"process status {status}, expected {expected}"
                            )
                    rows.append(
                        {
                            "status": "ok",
                            "relative_path": f"assert-exit:{expected}",
                            "detail": "",
                        }
                    )
                else:
                    raise ReplayError(f"unknown replay command {command}")
            except ReplayError as error:
                failed = True
                detail = f"{replay_path}:{lineno}: {error}"
                trace.emit(
                    "step.error",
                    lineno=lineno,
                    command=command,
                    detail=detail,
                )
                rows.append(
                    {
                        "status": "failed",
                        "relative_path": " ".join(parts),
                        "detail": detail,
                    }
                )
                print(detail, file=sys.stderr)
            finally:
                duration_ms = (time.perf_counter() - step_start) * 1000.0
                profile.add_step(command, duration_ms)
                trace.emit(
                    "step.finish",
                    lineno=lineno,
                    command=command,
                    failed=failed,
                    duration_ms=f"{duration_ms:.3f}",
                )
                add_metric(
                    metrics,
                    lineno,
                    command,
                    target,
                    "step_duration_ms",
                    duration_ms=f"{duration_ms:.3f}",
                )
            if failed:
                break
    finally:
        if not args.leave_running:
            if proc.poll() is None:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except OSError:
                    proc.terminate()
                terminate_process(proc)
        profile.process["app_returncode"] = proc.poll()
        trace.emit(
            "process.finish",
            pid=proc.pid,
            returncode=profile.process["app_returncode"],
        )
        app_log.close()
        profile.add_artifact("app_log", artifacts.app_log_path)
        if xvfb_proc is not None:
            terminate_process(xvfb_proc)
            trace.emit(
                "resource.xvfb.finish",
                returncode=xvfb_proc.poll(),
            )

    if not rows:
        rows.append(
            {
                "status": "failed",
                "relative_path": replay_path.name,
                "detail": "replay produced no results",
            }
        )
        failed = True
    write_results(artifacts.results_path, rows)
    write_metrics(artifacts.metrics_path, metrics)
    write_junit(artifacts.junit_path, args.name, rows)
    failures = [row for row in rows if row.get("status") not in ("captured", "ok")]
    trace.emit(
        "runner.finish",
        status="failed" if failed else "ok",
        steps=len(rows),
        failures=len(failures),
    )
    trace.close()
    profile.add_artifact("results", artifacts.results_path)
    profile.add_artifact("metrics", artifacts.metrics_path)
    profile.add_artifact("junit", artifacts.junit_path)
    profile.add_artifact("trace", artifacts.trace_path)
    profile.add_artifact("profile", artifacts.profile_path)
    profile.add_artifact("summary", artifacts.summary_path)
    finished_at_ms = int(time.time() * 1000)
    profile_doc = {
        "name": args.name,
        "started_at_ms": profile.started_at_ms,
        "finished_at_ms": finished_at_ms,
        "duration_ms": finished_at_ms - profile.started_at_ms,
        "command_counts": profile.command_counts,
        "command_durations_ms": profile.command_durations_ms,
        "metric_summary": summarize_metrics(metrics),
        "timeline": summarize_timeline_file(timeline_path),
        "render_stats": summarize_tsv_file(args.render_stats),
        "process": profile.process,
        "artifacts": profile.artifacts,
    }
    summary_doc = {
        "name": args.name,
        "status": "failed" if failed else "ok",
        "backend": args.input_backend,
        "offscreen": args.offscreen,
        "steps": len(rows),
        "failures": failures,
        "artifacts": profile.artifacts,
    }
    write_json(artifacts.profile_path, profile_doc)
    write_json(artifacts.summary_path, summary_doc)
    print(f"Wrote {artifacts.results_path}")
    print(f"Wrote {artifacts.metrics_path}")
    print(f"Wrote {artifacts.profile_path}")
    print(f"Wrote {artifacts.summary_path}")
    print(f"Wrote {artifacts.trace_path}")
    if args.render_stats is not None:
        print(f"Wrote {args.render_stats}")
    print(f"Wrote {artifacts.junit_path}")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(
        description="Run deterministic UI replay scenarios and screenshot assertions."
    )
    parser.add_argument("--name", required=True)
    parser.add_argument(
        "--retries",
        type=int,
        default=1,
        help=(
            "re-launch the app and re-run the whole replay up to this many "
            "times on failure. Used for smokes whose target app has a rare, "
            "benign process crash in its own shutdown/teardown path that is "
            "otherwise unobservable (any debugger/tracer perturbs the race "
            "away). A genuine failure still fails after all attempts."
        ),
    )
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--app-arg", action="append", default=[])
    parser.add_argument("--workdir", type=Path, default=ROOT)
    parser.add_argument("--replay", required=True)
    parser.add_argument("--out-root", type=Path, required=True)
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--display", default="121")
    parser.add_argument("--geometry", default="1280x1024x24")
    parser.add_argument("--xvfb", action="store_true")
    parser.add_argument(
        "--input-backend",
        choices=("internal", "xdotool"),
        default="internal",
        help=(
            "internal uses LIBX11_COMPAT_REPLAY inside the target process; "
            "xdotool sends external X11 input"
        ),
    )
    parser.add_argument(
        "--screenshot-command",
        choices=("auto", "import", "gnome-screenshot", "screencapture"),
        default="auto",
    )
    parser.add_argument(
        "--screenshot-region",
        default=None,
        help=(
            "default region for screenshot command, as X,Y,W,H. Replay "
            "scripts may override per-shot with `screenshot NAME X Y W H`. "
            "Pair with the app's geometry argument so the captured pixels "
            "always come from the target window rather than whatever the "
            "host desktop happens to show on top."
        ),
    )
    parser.add_argument(
        "--render-stats",
        type=Path,
        default=None,
        help=(
            "write libx11-compat renderer timing/readback stats to this TSV "
            "by setting LIBX11_COMPAT_RENDER_STATS for the target process"
        ),
    )
    parser.add_argument(
        "--trace-path",
        type=Path,
        default=None,
        help=(
            "write structured runner trace JSONL here. Defaults to "
            "<out-root>/trace.jsonl."
        ),
    )
    parser.add_argument(
        "--profile-json",
        type=Path,
        default=None,
        help=(
            "write aggregate command timing, artifact sizes, timeline counts, "
            "and render-stats metadata here. Defaults to <out-root>/profile.json."
        ),
    )
    parser.add_argument(
        "--ci-summary",
        type=Path,
        default=None,
        help=(
            "write compact CI pass/fail and artifact index JSON here. "
            "Defaults to <out-root>/summary.json."
        ),
    )
    parser.add_argument(
        "--offscreen",
        action="store_true",
        help=(
            "force SDL_VIDEODRIVER=dummy for headless CI. Requires "
            "--input-backend internal and --in-process-snapshots."
        ),
    )
    parser.add_argument("--leave-running", action="store_true")
    parser.add_argument(
        "--timeline",
        action="store_true",
        help=(
            "set LIBX11_COMPAT_TIMELINE=1 so the target process writes "
            "a JSONL trace of every X event, configure, destroy, present, "
            "and focus / grab transition. Defaults to "
            "<out-root>/timeline.jsonl when --timeline-path is unset."
        ),
    )
    parser.add_argument(
        "--timeline-path",
        type=Path,
        default=None,
        help=("explicit path for the timeline JSONL. Implies --timeline."),
    )
    parser.add_argument(
        "--in-process-snapshots",
        action="store_true",
        help=(
            "have libx11-compat's snapshot helper write the target "
            "window surface to a BMP for each screenshot step, and "
            "use those BMPs in place of screencapture. On macOS the "
            "system screencapture deactivates the target NSApp "
            "briefly while it grabs pixels, which stalls SDL's "
            "event pump just long enough that synthetic input "
            "queued by the in-process replay engine never gets "
            "delivered. The in-process path side-steps that entirely."
        ),
    )
    args = parser.parse_args()
    if args.screenshot_region is not None:
        try:
            parts = [int(v) for v in args.screenshot_region.split(",")]
        except ValueError as error:
            print(
                f"--screenshot-region expects X,Y,W,H ints: {error}",
                file=sys.stderr,
            )
            return 2
        if len(parts) != 4:
            print(
                "--screenshot-region expects four comma-separated ints: X,Y,W,H",
                file=sys.stderr,
            )
            return 2
        args.screenshot_region = tuple(parts)
    if args.offscreen and (
        args.input_backend != "internal" or not args.in_process_snapshots
    ):
        print(
            "--offscreen requires --input-backend internal "
            "and --in-process-snapshots",
            file=sys.stderr,
        )
        return 2

    attempts = max(1, args.retries)
    for attempt in range(1, attempts + 1):
        # run_replay catches per-step ReplayErrors internally and reports a
        # failed run by returning nonzero; it only raises for setup-phase
        # errors (bad flags). Retry on either signal, otherwise a --retries
        # smoke would run exactly once regardless of failure.
        try:
            rc = run_replay(args)
            message = "replay reported failures"
        except ReplayError as error:
            rc = 1
            message = str(error)
        if rc == 0:
            return 0
        if attempt < attempts:
            print(
                f"run-ui-replay: attempt {attempt}/{attempts} failed "
                f"({message}); re-running",
                file=sys.stderr,
            )
            continue
        print(f"run-ui-replay: {message}", file=sys.stderr)
        return rc


if __name__ == "__main__":
    sys.exit(main())
