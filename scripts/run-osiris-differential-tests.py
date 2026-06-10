#!/usr/bin/env python3
import argparse
import os
import re
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT_ROOT = ROOT / "build" / "osiris-differential"
DEFAULT_REFERENCE_DIR = ROOT / "build" / "osiris-reference-screens"


def run(cmd, *, cwd=ROOT, input_text=None):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, input=input_text, text=True, check=True)


def rsync(src, dest, *, extra_args=None):
    cmd = ["rsync", "-a", "--delete"]
    if extra_args:
        cmd.extend(extra_args)
    cmd.extend([str(src), str(dest)])
    run(cmd)


def ssh(remote, script):
    run(["ssh", remote, "sh", "-s"], input_text=script)


def q(value):
    return shlex.quote(str(value))


def parse_env_default(name, default):
    value = os.environ.get(name)
    if value is None or value == "":
        return default
    return value


def sync_repo(args):
    remote_repo = f"{args.remote_root}/repo"
    run(["ssh", args.remote, "mkdir", "-p", args.remote_root])
    rsync(
        "./",
        f"{args.remote}:{remote_repo}/",
        extra_args=[
            "--exclude",
            "/build/",
        ],
    )
    upstream_cache = ROOT / "build" / "upstream" / ".cache"
    if upstream_cache.exists():
        run(["ssh", args.remote, "mkdir", "-p", f"{remote_repo}/build/upstream/.cache"])
        rsync(
            f"{upstream_cache}/", f"{args.remote}:{remote_repo}/build/upstream/.cache/"
        )
    return remote_repo


def remote_script(args, remote_repo):
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(args.remote_root + '/system-build')} "
            f"{q(args.remote_root + '/system-out')} "
            f"{q(args.remote_root + '/screens')} "
            f"{q(args.remote_root + '/logs')} "
            f"{q(args.remote_root + '/diff')}\n"
        )

    install_deps = ""
    if args.install_deps:
        install_deps = """
if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \\
        build-essential ca-certificates git imagemagick meson ninja-build \\
        pkg-config python3 python3-pil rsync xauth xvfb xdotool \\
        libfreetype-dev libice-dev libjpeg-dev libpng-dev libsm-dev \\
        libx11-dev libxext-dev libxmu-dev zlib1g-dev
fi
"""

    return f"""
set -eu

{install_deps}

need() {{
    command -v "$1" >/dev/null 2>&1 || {{
        echo "missing required command: $1" >&2
        exit 127
    }}
}}

need gcc
need git
need import
need make
need meson
need ninja
need pkg-config
need python3
need rsync
need Xvfb
need xdotool

remote_root={q(args.remote_root)}
repo={q(remote_repo)}
system_build="$remote_root/system-build"
system_out="$remote_root/system-out"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
display=:{q(args.display)}

run_logged() {{
    log=$1
    shift
    if "$@" >>"$log" 2>&1; then
        return 0
    else
        status=$?
        echo "FAIL $*; see $log" >&2
        tail -60 "$log" >&2 || true
        exit "$status"
    fi
}}

capture_osiris() {{
    name=$1
    app=$2
    workdir=$3
    replay=$4
    geometry_arg=$5
    libpath=$6
    log_dir=$7
    screen_dir=$8
    input_backend=$9
    replay_out="$remote_root/replay-$name"
    rm -rf "$replay_out" "$remote_root/home-$name"
    mkdir -p "$log_dir" "$screen_dir" "$remote_root/home-$name"
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "osiris-$name" \\
        --app "$app" \\
        --app-arg=-geometry --app-arg="$geometry_arg" \\
        --workdir "$workdir" \\
        --replay "$repo/tests/ui/replays/$replay" \\
        --out-root "$replay_out" \\
        --display {q(args.display)} \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        --screenshot-region {q(args.screenshot_region)} \\
        --env DISPLAY="$display" \\
        --env HOME="$remote_root/home-$name" \\
        --env LD_LIBRARY_PATH="$libpath${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}" \\
        --env LIBX11_COMPAT_FONT_DIR="$repo/fonts"
    cp "$replay_out"/screens/*.png "$screen_dir"/
    if [ -f "$log_dir/results.tsv" ]; then
        tail -n +2 "$replay_out/results.tsv" >>"$log_dir/results.tsv"
    else
        cp "$replay_out/results.tsv" "$log_dir/results.tsv"
    fi
    cp "$replay_out"/junit.xml "$log_dir/junit.xml"
    cp "$replay_out"/logs/* "$log_dir"/ 2>/dev/null || true
}}

{clean_remote}
rm -rf "$remote_root/screens" "$remote_root/logs" "$remote_root/diff" \\
    "$remote_root/report.tsv" "$remote_root/junit.xml" "$remote_root"/replay-*
mkdir -p "$system_build" "$system_out" "$system_logs" "$compat_logs" \\
    "$system_screens" "$compat_screens" "$remote_root/logs"

cd "$repo"
make -j{q(args.jobs)} CC=gcc osiris

osiris_src="$repo/build/upstream/osiris"
if [ ! -f "$system_build/.configure-stamp" ]; then
    rm -rf "$system_build"
    mkdir -p "$system_build"
    run_logged "$remote_root/logs/system-configure.log" \\
        meson setup "$system_build" "$osiris_src" \\
            --prefix="$system_out/install" \\
            -Dxft=disabled \\
            -Dopengl=disabled \\
            -Dsm=disabled \\
            -Dmng=disabled \\
            -Dgif=disabled \\
            -Dexamples=enabled \\
            -Dtutorial=enabled
    touch "$system_build/.configure-stamp"
fi
run_logged "$remote_root/logs/system-build.log" ninja -C "$system_build"

check_target() {{
    test -x "$1" || {{
        echo "missing Osiris representative target: $1" >&2
        exit 1
    }}
}}

system_application="$system_build/examples/application/application"
system_listviews="$system_build/examples/listviews/listviews"
system_fileiconview="$system_build/examples/fileiconview/fileiconview"
system_designer="$system_build/designer"
compat_application="$repo/build/osiris/build/examples/application/application"
compat_listviews="$repo/build/osiris/build/examples/listviews/listviews"
compat_fileiconview="$repo/build/osiris/build/examples/fileiconview/fileiconview"
compat_designer="$repo/build/osiris/build/designer"
for target in \\
    "$system_application" "$system_listviews" "$system_fileiconview" \\
    "$system_designer" "$compat_application" \\
    "$compat_listviews" "$compat_fileiconview" \\
    "$compat_designer"; do
    check_target "$target"
done

rm -f "/tmp/.X{q(args.display)}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb.log" 2>&1 &
xvfb_pid=$!
trap 'kill "$xvfb_pid" >/dev/null 2>&1 || true' EXIT
sleep 1

capture_osiris system-application \\
    "$system_application" \\
    "$osiris_src/examples/application" \\
    osiris-application.replay \\
    520x680+0+0 \\
    "$system_build" \\
    "$system_logs" \\
    "$system_screens" \\
    xdotool

capture_osiris system-listviews \\
    "$system_listviews" \\
    "$osiris_src/examples/listviews" \\
    osiris-listviews.replay \\
    720x540+0+0 \\
    "$system_build" \\
    "$system_logs" \\
    "$system_screens" \\
    xdotool

capture_osiris system-fileiconview \\
    "$system_fileiconview" \\
    "$osiris_src/examples/fileiconview" \\
    osiris-fileiconview.replay \\
    760x560+0+0 \\
    "$system_build" \\
    "$system_logs" \\
    "$system_screens" \\
    xdotool

capture_osiris system-designer \\
    "$system_designer" \\
    "$osiris_src/tools/designer/designer" \\
    osiris-designer.replay \\
    940x740+0+0 \\
    "$system_build" \\
    "$system_logs" \\
    "$system_screens" \\
    xdotool

capture_osiris compat-application \\
    "$compat_application" \\
    "$osiris_src/examples/application" \\
    osiris-application.replay \\
    520x680+0+0 \\
    "$repo/build/osiris/build:$repo/build" \\
    "$compat_logs" \\
    "$compat_screens" \\
    internal

capture_osiris compat-listviews \\
    "$compat_listviews" \\
    "$osiris_src/examples/listviews" \\
    osiris-listviews.replay \\
    720x540+0+0 \\
    "$repo/build/osiris/build:$repo/build" \\
    "$compat_logs" \\
    "$compat_screens" \\
    internal

capture_osiris compat-fileiconview \\
    "$compat_fileiconview" \\
    "$osiris_src/examples/fileiconview" \\
    osiris-fileiconview.replay \\
    760x560+0+0 \\
    "$repo/build/osiris/build:$repo/build" \\
    "$compat_logs" \\
    "$compat_screens" \\
    internal

capture_osiris compat-designer \\
    "$compat_designer" \\
    "$osiris_src/tools/designer/designer" \\
    osiris-designer.replay \\
    940x740+0+0 \\
    "$repo/build/osiris/build:$repo/build" \\
    "$compat_logs" \\
    "$compat_screens" \\
    internal
"""


def remote_compare_script(args):
    return f"""
set -eu
remote_root={q(args.remote_root)}
repo="$remote_root/repo"
python3 "$repo/scripts/compare-motif-reference.py" \\
    --skip-local \\
    --skip-remote \\
    --local-dir "$remote_root/screens/compat" \\
    --ref-dir "$remote_root/screens/system" \\
    --diff-dir "$remote_root/diff" \\
    --report "$remote_root/report.tsv" \\
    --junit "$remote_root/junit.xml" \\
    --local-results "$remote_root/logs/compat/results.tsv" \\
    --ref-results "$remote_root/logs/system/results.tsv" \\
    --mae-threshold {q(args.mae_threshold)} \\
    --changed-threshold {q(args.changed_threshold)} \\
    --top {q(args.top)}
"""


def fetch_results(args, *, fetch_remote_compare=False):
    out_root = args.out_root
    system_dir = out_root / "system"
    compat_dir = out_root / "compat"
    log_dir = out_root / "logs"
    diff_dir = out_root / "diff"
    out_root.mkdir(parents=True, exist_ok=True)
    for path in (system_dir, compat_dir, log_dir, diff_dir):
        if path.exists():
            shutil.rmtree(path)
        path.mkdir(parents=True)

    rsync(f"{args.remote}:{args.remote_root}/screens/system/", system_dir)
    rsync(f"{args.remote}:{args.remote_root}/screens/compat/", compat_dir)
    rsync(f"{args.remote}:{args.remote_root}/logs/", log_dir)

    if args.reference_dir:
        reference_dir = args.reference_dir
        if reference_dir.exists():
            shutil.rmtree(reference_dir)
        reference_dir.mkdir(parents=True)
        rsync(f"{args.remote}:{args.remote_root}/screens/system/", reference_dir)

    if fetch_remote_compare:
        rsync(f"{args.remote}:{args.remote_root}/diff/", diff_dir)
        rsync(f"{args.remote}:{args.remote_root}/report.tsv", out_root / "report.tsv")
        rsync(f"{args.remote}:{args.remote_root}/junit.xml", out_root / "junit.xml")
    return system_dir, compat_dir, out_root


def compare(args, system_dir, compat_dir, out_root):
    cmd = [
        sys.executable,
        "scripts/compare-motif-reference.py",
        "--skip-local",
        "--skip-remote",
        "--local-dir",
        str(compat_dir),
        "--ref-dir",
        str(system_dir),
        "--diff-dir",
        str(out_root / "diff"),
        "--report",
        str(out_root / "report.tsv"),
        "--junit",
        str(out_root / "junit.xml"),
        "--local-results",
        str(out_root / "logs" / "compat" / "results.tsv"),
        "--ref-results",
        str(out_root / "logs" / "system" / "results.tsv"),
        "--mae-threshold",
        str(args.mae_threshold),
        "--changed-threshold",
        str(args.changed_threshold),
        "--top",
        str(args.top),
    ]
    run(cmd)


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Build Osiris on a Linux SSH host against system libX11 and "
            "libx11-compat, capture representative Qt2 screenshots, and "
            "compare output."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("OSIRIS_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=parse_env_default(
            "OSIRIS_DIFF_REMOTE_ROOT",
            "/tmp/libx11-compat-osiris-differential",
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("OSIRIS_DIFF_DISPLAY", "124"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("OSIRIS_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("OSIRIS_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--screenshot-region",
        default=parse_env_default("OSIRIS_DIFF_SCREENSHOT_REGION", "0,0,360,180"),
    )
    parser.add_argument("--clean", action="store_true")
    parser.add_argument(
        "--install-deps",
        action="store_true",
        help="install minimal Ubuntu packages on the remote via sudo apt-get",
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("OSIRIS_DIFF_MAE_THRESHOLD", "0.16")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("OSIRIS_DIFF_CHANGED_THRESHOLD", "0.42")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("OSIRIS_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("OSIRIS_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
        help="local artifact directory for synced screenshots, diffs, TSV, and JUnit",
    )
    parser.add_argument(
        "--reference-dir",
        type=Path,
        default=Path(
            parse_env_default("OSIRIS_REFERENCE_SCREEN_DIR", DEFAULT_REFERENCE_DIR)
        ),
        help="local directory for native Osiris reference screenshots",
    )
    parser.add_argument(
        "--compare-location",
        choices=("remote", "local"),
        default=parse_env_default("OSIRIS_DIFF_COMPARE_LOCATION", "remote"),
    )
    args = parser.parse_args()

    if not re.fullmatch(r"\d+", args.display):
        parser.error("--display must be a numeric X display index")
    if not re.fullmatch(r"\d+", str(args.jobs)):
        parser.error("--jobs must be a positive integer")
    if int(args.jobs) <= 0:
        parser.error("--jobs must be a positive integer")
    if not re.fullmatch(r"\d+,\d+,\d+,\d+", args.screenshot_region):
        parser.error("--screenshot-region must use x,y,width,height")

    remote_repo = sync_repo(args)
    remote_status = 0
    compare_status = 0
    fetch_status = 0
    try:
        ssh(args.remote, remote_script(args, remote_repo))
    except subprocess.CalledProcessError as error:
        remote_status = error.returncode

    if args.compare_location == "remote" and not remote_status:
        try:
            ssh(args.remote, remote_compare_script(args))
        except subprocess.CalledProcessError as error:
            compare_status = error.returncode

    try:
        system_dir, compat_dir, out_root = fetch_results(
            args,
            fetch_remote_compare=args.compare_location == "remote"
            and not remote_status,
        )
    except subprocess.CalledProcessError as error:
        fetch_status = error.returncode
        system_dir = compat_dir = out_root = None
        print(
            f"warning: result fetch failed (exit {fetch_status})",
            file=sys.stderr,
        )

    if args.compare_location == "local" and system_dir is not None:
        try:
            compare(args, system_dir, compat_dir, out_root)
        except subprocess.CalledProcessError as error:
            compare_status = error.returncode

    if remote_status:
        sys.exit(remote_status)
    if compare_status:
        sys.exit(compare_status)
    if fetch_status:
        sys.exit(fetch_status)


if __name__ == "__main__":
    main()
