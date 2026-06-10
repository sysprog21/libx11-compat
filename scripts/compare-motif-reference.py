#!/usr/bin/env python3
import argparse
import csv
import os
import re
import shlex
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

ROOT = Path(__file__).resolve().parents[1]


def run(cmd, *, cwd=ROOT, env=None):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, env=env, check=True)


def out(cmd, *, cwd=ROOT):
    return subprocess.check_output(cmd, cwd=cwd, text=True).strip()


def ssh(remote, script):
    print("+ ssh", remote, "sh -s", flush=True)
    subprocess.run(["ssh", remote, "sh", "-s"], input=script, text=True, check=True)


def rsync(src, dest):
    run(["rsync", "-a", "--delete", str(src), str(dest)])


def sanitize(rel):
    return rel.replace("/", "_").replace(" ", "_")


def screenshot_name_for_result(row):
    shot = row.get("screenshot") or ""
    if shot:
        return Path(shot).name
    return sanitize(row["relative_path"]) + ".png"


def read_results(path):
    results = {}
    if not path or not path.exists():
        return results
    with path.open(newline="") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            if not row.get("relative_path"):
                continue
            if row.get("status") == "ok" and not row.get("screenshot"):
                continue
            results[screenshot_name_for_result(row)] = row
    return results


def nonblack_bbox(img):
    rgba = img.convert("RGBA")
    alpha = rgba.getchannel("A")
    rgb = rgba.convert("RGB")
    mask = Image.new("L", rgb.size, 0)
    px = rgb.load()
    mx = mask.load()
    for y in range(rgb.height):
        for x in range(rgb.width):
            r, g, b = px[x, y]
            if alpha.getpixel((x, y)) > 0 and r + g + b > 18:
                mx[x, y] = 255
    return mask.getbbox()


def crop_visible(path):
    img = Image.open(path).convert("RGBA")
    bbox = nonblack_bbox(img)
    if bbox:
        img = img.crop(bbox)
    return img


def resample_filter():
    if hasattr(Image, "Resampling"):
        return Image.Resampling.BICUBIC
    return Image.BICUBIC


def normalize_hidpi(local, ref):
    if local.width <= 0 or local.height <= 0 or ref.width <= 0 or ref.height <= 0:
        return local, ref
    width_ratio = local.width / ref.width
    height_ratio = local.height / ref.height
    if 1.75 <= width_ratio <= 2.50 and 1.75 <= height_ratio <= 2.50:
        scale = (width_ratio + height_ratio) / 2.0
        local = local.resize(
            (max(1, round(local.width / scale)), max(1, round(local.height / scale))),
            resample_filter(),
        )
    elif 1.75 <= 1.0 / width_ratio <= 2.50 and 1.75 <= 1.0 / height_ratio <= 2.50:
        scale = (1.0 / width_ratio + 1.0 / height_ratio) / 2.0
        ref = ref.resize(
            (max(1, round(ref.width / scale)), max(1, round(ref.height / scale))),
            resample_filter(),
        )
    return local, ref


def compare(local_path, ref_path, diff_path):
    local = crop_visible(local_path)
    ref = crop_visible(ref_path)
    local, ref = normalize_hidpi(local, ref)
    width = min(local.width, ref.width)
    height = min(local.height, ref.height)
    if width <= 0 or height <= 0:
        raise ValueError("empty image after crop")
    local = local.crop((0, 0, width, height)).convert("RGB")
    ref = ref.crop((0, 0, width, height)).convert("RGB")
    diff = ImageChops.difference(local, ref)
    diff_path.parent.mkdir(parents=True, exist_ok=True)
    diff.save(diff_path)
    stat = ImageStat.Stat(diff)
    mae = sum(stat.mean) / (3 * 255.0)
    extrema = diff.getextrema()
    max_delta = max(channel[1] for channel in extrema) / 255.0
    changed = 0
    total = width * height
    diff_data = (
        diff.get_flattened_data()
        if hasattr(diff, "get_flattened_data")
        else diff.getdata()
    )
    for pixel in diff_data:
        if pixel[0] + pixel[1] + pixel[2] > 24:
            changed += 1
    return {
        "width": width,
        "height": height,
        "mae": mae,
        "max_delta": max_delta,
        "changed_ratio": changed / total,
    }


def capture_local(args):
    env = os.environ.copy()
    env["MAKEFLAGS"] = "-j1"
    if args.filter:
        env["MOTIF_DEMO_SCREENSHOT_FILTER"] = args.filter
    run(["make", "-j1", "motif-demos-screenshots"], env=env)


def capture_remote(args):
    # args.display is interpolated unquoted into a remote shell as
    # display=":{args.display}". Require digits so a malicious value
    # like "0;rm -rf /" can't slip through.
    if not re.fullmatch(r"\d+", str(args.display)):
        raise ValueError(
            f"--display must be a numeric X display index (got {args.display!r})"
        )
    remote_root = args.remote_root
    remote_src = f"{remote_root}/motif-src/"
    remote_build = f"{remote_root}/motif-build"
    remote_out = f"{remote_root}/out"
    remote_script_dir = f"{remote_root}/scripts"

    run(["ssh", args.remote, "mkdir", "-p", remote_root, remote_script_dir])
    rsync(str(ROOT / "build/upstream/motif") + "/", f"{args.remote}:{remote_src}")
    rsync(
        ROOT / "scripts/capture-motif-demo-screenshots.sh",
        f"{args.remote}:{remote_script_dir}/capture-motif-demo-screenshots.sh",
    )

    filter_export = ""
    if args.filter:
        safe_filter = args.filter.replace("'", "'\\''")
        filter_export = f"export MOTIF_DEMO_SCREENSHOT_FILTER='{safe_filter}';"

    remote_cmd = f"""
set -eu
mkdir -p '{remote_build}' '{remote_out}'
cd '{remote_build}'
if [ ! -f .configure-stamp ]; then
  PKG_CONFIG_PATH="${{PKG_CONFIG_PATH:-}}" \\
  CPP="gcc -E" \\
  CPPFLAGS="-include stdlib.h" \\
  CFLAGS="-g -O0" \\
  YACC="yacc" \\
  '{remote_src}/configure' \\
    --prefix='{remote_out}/motif-install' \\
    --disable-glw --disable-tests --without-xft \\
    --with-jpeg=no --with-png=no --with-xrandr=no \\
    --with-xrender=no --with-xcursor=no --with-xinerama=no \\
    --enable-demos
  touch .configure-stamp
fi
make -C config
make -C lib/Xm CFLAGS="-g -O0"
make -C lib/Mrm CFLAGS="-g -O0"
make -C tools/wml CPP="gcc -E" CFLAGS="-g -O0"
make -C clients/uil CPP="gcc -E" CFLAGS="-g -O0"
make -C demos CPP="gcc -E" CFLAGS="-g -O0"
display=":{args.display}"
rm -f /tmp/.X{args.display}-lock
Xvfb "$display" -screen 0 1280x1024x24 >/tmp/libx11-compat-motif-xvfb.log 2>&1 &
xvfb_pid=$!
trap 'kill "$xvfb_pid" >/dev/null 2>&1 || true' EXIT
sleep 1
export DISPLAY="$display"
export MOTIF_SCREENSHOT_COMMAND=import
{filter_export}
sh '{remote_script_dir}/capture-motif-demo-screenshots.sh' '{remote_build}' '{remote_out}'
"""
    ssh(args.remote, remote_cmd)

    ref_dir = ROOT / "build/motif-reference-screens"
    rsync(f"{args.remote}:{remote_out}/motif-demo-screenshots/", ref_dir)


def compare_dirs(args):
    local_dir = args.local_dir or ROOT / "build/motif-demo-screenshots"
    ref_dir = args.ref_dir or ROOT / "build/motif-reference-screens"
    diff_dir = args.diff_dir or ROOT / "build/motif-diff-screens"
    report = args.report or ROOT / "build/motif-screenshot-diff.tsv"
    if diff_dir.exists():
        shutil.rmtree(diff_dir)
    diff_dir.mkdir(parents=True)

    local_results = read_results(args.local_results)
    ref_results = read_results(args.ref_results)
    rows = []
    handled = set()

    for name in sorted(set(local_results) | set(ref_results)):
        local_status = local_results.get(name, {}).get("status")
        ref_status = ref_results.get(name, {}).get("status")
        if local_status == "captured" and ref_status == "captured":
            continue
        if (
            local_status == "exited-before-capture"
            and ref_status == "exited-before-capture"
        ):
            rows.append({"name": name, "status": "no-screenshot"})
            handled.add(name)
            continue
        if local_status and local_status != "captured":
            rows.append(
                {
                    "name": name,
                    "status": "local-" + local_status,
                    "detail": local_results[name].get("detail", ""),
                }
            )
            handled.add(name)
            continue
        if ref_status and ref_status != "captured":
            rows.append(
                {
                    "name": name,
                    "status": "reference-" + ref_status,
                    "detail": ref_results[name].get("detail", ""),
                }
            )
            handled.add(name)

    for local_path in sorted(local_dir.glob("*.png")):
        if local_path.name in handled:
            continue
        ref_path = ref_dir / local_path.name
        if not ref_path.exists():
            rows.append({"name": local_path.name, "status": "missing-reference"})
            continue
        diff_path = diff_dir / local_path.name
        metrics = compare(local_path, ref_path, diff_path)
        status = "ok"
        if (
            metrics["mae"] > args.mae_threshold
            or metrics["changed_ratio"] > args.changed_threshold
        ):
            status = "diff"
        rows.append({"name": local_path.name, "status": status, **metrics})

    for ref_path in sorted(ref_dir.glob("*.png")):
        if ref_path.name in handled:
            continue
        local_path = local_dir / ref_path.name
        if not local_path.exists():
            rows.append({"name": ref_path.name, "status": "missing-local"})

    with report.open("w", newline="") as f:
        fields = [
            "status",
            "name",
            "mae",
            "changed_ratio",
            "max_delta",
            "width",
            "height",
            "detail",
        ]
        writer = csv.DictWriter(
            f, fieldnames=fields, delimiter="\t", extrasaction="ignore"
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    sorted_rows = sorted(rows, key=lambda r: float(r.get("mae") or 0.0), reverse=True)
    if args.junit:
        write_junit(args.junit, sorted_rows)
    print(f"Wrote {report}")
    if args.junit:
        print(f"Wrote {args.junit}")
    print_summary(sorted_rows)
    for row in sorted_rows[: args.top]:
        print(row)
    ok_statuses = {"ok", "no-screenshot"}
    if not sorted_rows:
        print("No screenshots found to compare", file=sys.stderr)
        return 1
    return 1 if any(row["status"] not in ok_statuses for row in sorted_rows) else 0


def row_failure_message(row):
    status = row["status"]
    if status == "diff":
        return (
            f"mae={row.get('mae')} changed_ratio={row.get('changed_ratio')} "
            f"max_delta={row.get('max_delta')}"
        )
    return row.get("detail") or status


def print_summary(rows):
    counts = {}
    for row in rows:
        counts[row["status"]] = counts.get(row["status"], 0) + 1
    parts = [f"{status}={count}" for status, count in sorted(counts.items())]
    print("Motif differential summary:", ", ".join(parts))


def write_junit(path, rows):
    ok_statuses = {"ok", "no-screenshot"}
    failures = sum(1 for row in rows if row["status"] not in ok_statuses)
    suite = ET.Element(
        "testsuite",
        {
            "name": "motif-differential",
            "tests": str(len(rows)),
            "failures": str(failures),
            "errors": "0",
        },
    )
    for row in rows:
        case = ET.SubElement(
            suite,
            "testcase",
            {
                "classname": "motif-differential",
                "name": row["name"],
            },
        )
        if row["status"] not in ok_statuses:
            failure = ET.SubElement(
                case,
                "failure",
                {
                    "type": row["status"],
                    "message": row_failure_message(row),
                },
            )
            failure.text = row_failure_message(row)
        elif row["status"] == "no-screenshot":
            case.set("status", "no-screenshot")

    path.parent.mkdir(parents=True, exist_ok=True)
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--remote", default="node11")
    parser.add_argument("--remote-root", default="/tmp/libx11-compat-motif-ref")
    parser.add_argument("--display", default="119")
    parser.add_argument("--filter", help="regex passed to screenshot capture")
    parser.add_argument("--skip-local", action="store_true")
    parser.add_argument("--skip-remote", action="store_true")
    parser.add_argument("--mae-threshold", type=float, default=0.08)
    parser.add_argument("--changed-threshold", type=float, default=0.35)
    parser.add_argument("--top", type=int, default=12)
    parser.add_argument("--local-dir", type=Path)
    parser.add_argument("--ref-dir", type=Path)
    parser.add_argument("--diff-dir", type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--junit", type=Path)
    parser.add_argument("--local-results", type=Path)
    parser.add_argument("--ref-results", type=Path)
    args = parser.parse_args()

    if not args.skip_local:
        capture_local(args)
    if not args.skip_remote:
        capture_remote(args)
    return compare_dirs(args)


if __name__ == "__main__":
    sys.exit(main())
