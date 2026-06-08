#!/usr/bin/env python3
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
from pathlib import Path

try:
    from PIL import Image, ImageChops
except ImportError:
    Image = None
    ImageChops = None


ROOT = Path(__file__).resolve().parents[1]


class ReplayError(Exception):
    pass


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
    sleep tick (the prior behavior) collapsed every wait-window into a
    100 ms delay regardless of timeout_ms.
    """
    deadline = time.time() + timeout_ms / 1000.0
    while time.time() < deadline:
        if proc.poll() is not None:
            raise ReplayError(f"process exited with status {proc.returncode}")
        time.sleep(0.1)


def write_internal_replay(source_path, dest_path, snapshot_dir=None):
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
            if len(parts) != 2:
                raise ReplayError(f"{source_path}:{lineno}: key expects scancode")
            lines.append(f"key {int(parts[1])} press")
            lines.append("delay 10")
            lines.append(f"key {int(parts[1])} release")
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
        elif command == "wait-window":
            if len(parts) != 3:
                raise ReplayError(
                    f"{source_path}:{lineno}: wait-window expects pattern timeout_ms"
                )
            lines.append(f"delay {int(parts[2])}")
        elif command in ("assert-image", "assert-exit"):
            continue
        else:
            raise ReplayError(
                f"{source_path}:{lineno}: unknown replay command {command}"
            )

    dest_path.parent.mkdir(parents=True, exist_ok=True)
    dest_path.write_text("\n".join(lines) + "\n")
    return dest_path


def capture_screen(path, env, command, region):
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
    screenshot_dir = out_root / "screens"
    log_dir = out_root / "logs"
    out_root.mkdir(parents=True, exist_ok=True)
    if screenshot_dir.exists():
        shutil.rmtree(screenshot_dir)
    if log_dir.exists():
        shutil.rmtree(log_dir)
    screenshot_dir.mkdir(parents=True)
    log_dir.mkdir(parents=True)

    env = os.environ.copy()
    env.update(split_env(args.env))
    if args.render_stats is not None:
        args.render_stats.parent.mkdir(parents=True, exist_ok=True)
        env["LIBX11_COMPAT_RENDER_STATS"] = str(args.render_stats)
    display = args.display
    xvfb_proc = None
    if args.xvfb:
        xvfb_proc = start_xvfb(display, args.geometry, log_dir / "xvfb.log")
        env["DISPLAY"] = f":{display}"
    snapshot_dir = None
    if args.input_backend == "internal" and args.in_process_snapshots:
        snapshot_dir = out_root / "snapshots"
        # Wipe stale BMPs from a previous run; otherwise an earlier
        # screenshot step's output could satisfy a later assertion
        # against a snapshot the current run never produced.
        if snapshot_dir.exists():
            shutil.rmtree(snapshot_dir)
        snapshot_dir.mkdir(parents=True)
    if args.input_backend == "internal":
        internal_replay = write_internal_replay(
            replay_path,
            out_root / "internal.replay",
            snapshot_dir=snapshot_dir,
        )
        env["LIBX11_COMPAT_REPLAY"] = str(internal_replay)

    app_log = (log_dir / f"{args.name}.log").open("w")
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
                    if len(parts) != 2:
                        raise ReplayError("key expects keysym")
                    if args.input_backend == "xdotool":
                        xdotool(env, "key", parts[1])
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
                elif command == "screenshot":
                    if len(parts) not in (2, 6):
                        raise ReplayError("screenshot expects: name [x y w h]")
                    name = parts[1]
                    shot = screenshot_dir / f"{name}.png"
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
                        # libx11-compat snapshot timeout (15s); the
                        # save itself is sub-millisecond but the main
                        # thread may be mid-reflow (e.g. right after a
                        # resize) and not drain the SDL_USEREVENT for
                        # several seconds.
                        wait_start = time.perf_counter()
                        deadline = time.time() + 16.0
                        while time.time() < deadline:
                            if bmp_path.exists() and bmp_path.stat().st_size > 0:
                                break
                            time.sleep(0.05)
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
                        app_log_path = log_dir / f"{args.name}.log"
                        if app_log_path.exists():
                            with app_log_path.open() as f:
                                tail_lines = f.readlines()[-20:]
                                log_tail = "".join(tail_lines)
                        raise ReplayError(
                            f"in-process snapshot to {bmp_path} did "
                            f"not produce a file within 5s; refusing "
                            f"to silently fall back to screencapture. "
                            f"App log tail:\n{log_tail}"
                        )
                    else:
                        capture_start = time.perf_counter()
                        capture_screen(shot, env, args.screenshot_command, region)
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
                rows.append(
                    {
                        "status": "failed",
                        "relative_path": " ".join(parts),
                        "detail": detail,
                    }
                )
                print(detail, file=sys.stderr)
            finally:
                add_metric(
                    metrics,
                    lineno,
                    command,
                    target,
                    "step_duration_ms",
                    duration_ms=f"{(time.perf_counter() - step_start) * 1000.0:.3f}",
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
        app_log.close()
        if xvfb_proc is not None:
            terminate_process(xvfb_proc)

    if not rows:
        rows.append(
            {
                "status": "failed",
                "relative_path": replay_path.name,
                "detail": "replay produced no results",
            }
        )
        failed = True
    write_results(out_root / "results.tsv", rows)
    write_metrics(out_root / "metrics.tsv", metrics)
    write_junit(out_root / "junit.xml", args.name, rows)
    print(f"Wrote {out_root / 'results.tsv'}")
    print(f"Wrote {out_root / 'metrics.tsv'}")
    if args.render_stats is not None:
        print(f"Wrote {args.render_stats}")
    print(f"Wrote {out_root / 'junit.xml'}")
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(
        description="Run deterministic UI replay scenarios and screenshot assertions."
    )
    parser.add_argument("--name", required=True)
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
    parser.add_argument("--leave-running", action="store_true")
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

    try:
        return run_replay(args)
    except ReplayError as error:
        print(f"run-ui-replay: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
