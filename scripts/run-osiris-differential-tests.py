#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from differential import (
    NEED_SH,
    run_logged_sh,
    compare,
    compiler_setup_sh,
    check_local_paths,
    execute,
    fetch_results,
    parse_env_bool,
    parse_env_default,
    q,
    sync_repo,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT_ROOT = ROOT / "build" / "osiris-differential"
DEFAULT_REFERENCE_DIR = ROOT / "build" / "osiris-reference-screens"


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
    sudo apt-get update || true
    sudo apt-get install -y --no-install-recommends \\
        build-essential ca-certificates git imagemagick meson ninja-build \\
        pkg-config python3 python3-pil rsync xauth xvfb xdotool \\
        libfreetype-dev libice-dev libjpeg-dev libpng-dev libsm-dev \\
        libx11-dev libxext-dev libxmu-dev zlib1g-dev
fi
"""

    # Offset compat-side Xvfb by 1 so the parallel screenshot block
    # below can run system-side and compat-side captures concurrently.
    compat_display_num = int(args.display) + 1

    return f"""
set -eu

{install_deps}

{NEED_SH}

need "${{DIFF_CC:-gcc}}"
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
compat_display=:{compat_display_num}

{run_logged_sh()}

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
    mkdir -p "$replay_out" "$log_dir" "$screen_dir" "$remote_root/home-$name"
    replay_path="$repo/tests/ui/replays/$replay"
    if [ "$input_backend" = xdotool ]; then
        replay_path="$replay_out/$replay"
        awk '/^wait-window / {{ sub(/[0-9]+[ \t]*$/, "15000") }} {{ print }}' \
            "$repo/tests/ui/replays/$replay" > "$replay_path"
    fi
    # Read display from the current env so the parallel capture
    # subshells can each target their own Xvfb. Strip the leading
    # colon and any trailing .screen suffix to recover the numeric
    # display index that run-ui-replay's --display flag wants.
    display_num=${{DISPLAY#:}}
    display_num=${{display_num%%.*}}
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "osiris-$name" \\
        --app "$app" \\
        --app-arg=-geometry --app-arg="$geometry_arg" \\
        --workdir "$workdir" \\
        --replay "$replay_path" \\
        --out-root "$replay_out" \\
        --display "$display_num" \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        $([ "$input_backend" = internal ] && printf %s --in-process-snapshots) \\
        --screenshot-region {q(args.screenshot_region)} \\
        --env DISPLAY="$DISPLAY" \\
        --env HOME="$remote_root/home-$name" \\
        --env LD_LIBRARY_PATH="$libpath${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
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

# Meson bakes $CC into its build.ninja at configure time, so the compiler
# must be resolved before `meson setup` below.
{compiler_setup_sh()}
cxx_wrapped="g++"

osiris_src="$repo/build/upstream/osiris"

# Pre-extract upstream Osiris so the parallel compat-side and
# system-side builds below don't race on the source-stamp step. The
# make rule is a no-op when the actions/cache step already restored
# the osiris-src cache.
(cd "$repo" && make build/upstream/osiris/.source-stamp)

# Run compat-side and system-side builds concurrently. They write into
# disjoint trees ($repo/build/osiris vs $system_build) and share only
# the read-only upstream source. ccache is process-safe via its own
# locking. Pin ninja's parallelism on both sides; its default
# auto-detect would otherwise launch $vCPU+2 per side and oversubscribe
# the runner when the two builds run concurrently.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" CXX="$cxx_wrapped" \\
        OSIRIS_NINJA="ninja -j{q(args.jobs)}" osiris
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    if [ ! -f "$system_build/.configure-stamp" ]; then
        rm -rf "$system_build"
        mkdir -p "$system_build"
        run_logged "$remote_root/logs/system-configure.log" \\
            env CC="$cc_wrapped" CXX="$cxx_wrapped" \\
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
    run_logged "$remote_root/logs/system-build.log" ninja -j{q(args.jobs)} -C "$system_build"
) &
system_pid=$!

compat_status=0
wait "$compat_pid" || compat_status=$?
system_status=0
wait "$system_pid" || system_status=$?

# Surface diagnostics for any failed side before exiting; show both
# tails when both fail so the first-listed exit code does not mask a
# concurrent failure on the other side.
if [ "$compat_status" -ne 0 ]; then
    echo "compat-side build failed (exit $compat_status); see $compat_make_log" >&2
    tail -60 "$compat_make_log" >&2 || true
fi
if [ "$system_status" -ne 0 ]; then
    echo "system-side build failed (exit $system_status); see system-configure.log / system-build.log" >&2
    tail -60 "$remote_root/logs/system-configure.log" >&2 || true
    tail -60 "$remote_root/logs/system-build.log" >&2 || true
fi
[ "$compat_status" -eq 0 ] || exit "$compat_status"
[ "$system_status" -eq 0 ] || exit "$system_status"

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

rm -f "/tmp/.X{q(args.display)}-lock" "/tmp/.X{compat_display_num}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
trap 'exit' INT TERM HUP
trap 'kill "$xvfb_pid" "$compat_xvfb_pid" >/dev/null 2>&1 || true' EXIT

wait_for_display() {{
    target=$1
    server_pid=$2
    waited=0
    while [ "$waited" -lt 100 ]; do
        if ! kill -0 "$server_pid" 2>/dev/null; then
            echo "Xvfb for $target exited before accepting connections" >&2
            return 1
        fi
        if DISPLAY="$target" xdotool getdisplaygeometry >/dev/null 2>&1; then
            return 0
        fi
        # Sub-second poll assumes GNU coreutils sleep (the CI
        # runner); switch to integer sleep if a POSIX-only sleep is ever
        # targeted.
        sleep 0.1
        waited=$((waited + 1))
    done
    echo "Xvfb for $target did not become ready within 10s" >&2
    return 1
}}
wait_for_display "$display" "$xvfb_pid"
wait_for_display "$compat_display" "$compat_xvfb_pid"

# Osiris has 5 demos per side and each capture runs a replay so the
# capture phase dominates the job. Run system-side and compat-side
# captures concurrently on separate Xvfb instances.
system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"

(
    set -e
    export DISPLAY="$display"
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
    capture_osiris system-designer-menu \\
        "$system_designer" \\
        "$osiris_src/tools/designer/designer" \\
        osiris-designer-menu.replay \\
        940x740+0+0 \\
        "$system_build" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
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
    capture_osiris compat-designer-menu \\
        "$compat_designer" \\
        "$osiris_src/tools/designer/designer" \\
        osiris-designer-menu.replay \\
        940x740+0+0 \\
        "$repo/build/osiris/build:$repo/build" \\
        "$compat_logs" \\
        "$compat_screens" \\
        internal
) >"$compat_cap_log" 2>&1 &
compat_cap_pid=$!

system_cap_status=0
wait "$system_cap_pid" || system_cap_status=$?
compat_cap_status=0
wait "$compat_cap_pid" || compat_cap_status=$?

# Stage Xvfb logs and any partial replay traces into $remote_root/logs
# so the artifact upload picks them up regardless of capture success.
for replay_dir in "$remote_root"/replay-*; do
    [ -d "$replay_dir" ] || continue
    cp -r "$replay_dir" "$remote_root/logs/$(basename "$replay_dir")" 2>/dev/null || true
done
cp "$remote_root"/xvfb-*.log "$remote_root/logs/" 2>/dev/null || true

if [ "$system_cap_status" -ne 0 ]; then
    echo "system screenshot capture failed (exit $system_cap_status); see $system_cap_log" >&2
    tail -60 "$system_cap_log" >&2 || true
fi
if [ "$compat_cap_status" -ne 0 ]; then
    echo "compat screenshot capture failed (exit $compat_cap_status); see $compat_cap_log" >&2
    tail -60 "$compat_cap_log" >&2 || true
fi
[ "$system_cap_status" -eq 0 ] || exit "$system_cap_status"
[ "$compat_cap_status" -eq 0 ] || exit "$compat_cap_status"
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
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > OSIRIS_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-osiris-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("OSIRIS_DIFF_DISPLAY", "106"),
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
        "--local",
        action="store_true",
        default=parse_env_bool("OSIRIS_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote. Used by the GitHub Actions "
            "differential workflow."
        ),
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("OSIRIS_DIFF_MAE_THRESHOLD", "0.16")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("OSIRIS_DIFF_CHANGED_THRESHOLD", "0.55")),
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
        default=None,
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

    # Resolve --remote-root precedence: explicit CLI flag wins, then
    # the OSIRIS_DIFF_REMOTE_ROOT env var, then the local-mode default
    # (out_root/_work) or the SSH default.
    if args.remote_root is None:
        env_remote_root = os.environ.get("OSIRIS_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-osiris-differential"

    # Resolve --compare-location precedence: explicit CLI flag wins,
    # then the OSIRIS_DIFF_COMPARE_LOCATION env var, then the local-mode
    # default (local) or the SSH default (remote).
    if args.compare_location is None:
        env_compare_location = os.environ.get("OSIRIS_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error("OSIRIS_DIFF_COMPARE_LOCATION must be 'remote' or 'local'")
            args.compare_location = env_compare_location
        elif args.local:
            args.compare_location = "local"
        else:
            args.compare_location = "remote"

    if args.local:
        try:
            check_local_paths(
                args.out_root,
                Path(args.remote_root),
                reference_dir=args.reference_dir,
            )
        except ValueError as error:
            parser.error(str(error))

    remote_repo = sync_repo(args)
    remote_status = 0
    compare_status = 0
    fetch_status = 0
    try:
        execute(args, remote_script(args, remote_repo))
    except subprocess.CalledProcessError as error:
        remote_status = error.returncode

    if args.compare_location == "remote" and not remote_status:
        try:
            execute(args, remote_compare_script(args))
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
