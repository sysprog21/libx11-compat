#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path
from differential import (
    check_local_paths,
    execute,
    fetch_results,
    parse_env_bool,
    parse_env_default,
    q,
    run,
    sync_repo,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT_ROOT = ROOT / "build" / "xcircuit-differential"


def remote_script(args, remote_repo):
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(args.remote_root + '/system-xcircuit')} "
            f"{q(args.remote_root + '/screens')} "
            f"{q(args.remote_root + '/logs')} "
            f"{q(args.remote_root + '/diff')}\n"
        )

    install_deps = ""
    if args.install_deps:
        install_deps = """
if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    # Do not let an unrelated third-party repo with a transient GPG or
    # signature error abort the whole differential: the metadata refresh
    # is best-effort, and the install below still fails loudly if a
    # package we actually need cannot be resolved from the cached lists.
    sudo apt-get update || true
    sudo apt-get install -y --no-install-recommends \\
        autoconf automake build-essential ca-certificates git imagemagick \\
        libice-dev libsm-dev libx11-dev libxext-dev libxmu-dev libxpm-dev \\
        libxt-dev make patch \\
        pkg-config python3 python3-pil rsync xauth xvfb xdotool
fi
"""

    # Offset compat-side Xvfb by 1 so the parallel screenshot block
    # below can run system-side and compat-side captures concurrently.
    compat_display_num = int(args.display) + 1

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
need import
need make
need patch
need pkg-config
need python3
need rsync
need Xvfb
need xdotool

remote_root={q(args.remote_root)}
repo={q(remote_repo)}
system_build="$remote_root/system-xcircuit"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
display=:{q(args.display)}
compat_display=:{compat_display_num}

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

capture_xcircuit() {{
    name=$1
    app=$2
    workdir=$3
    replay=$4
    libpath=$5
    log_dir=$6
    screen_dir=$7
    input_backend=$8
    replay_out="$remote_root/replay-$name"
    rm -rf "$replay_out" "$remote_root/home-$name"
    mkdir -p "$log_dir" "$screen_dir" "$remote_root/home-$name"
    lib_env="$libpath"
    if [ -n "${{LD_LIBRARY_PATH:-}}" ]; then
        if [ -n "$lib_env" ]; then
            lib_env="$lib_env:$LD_LIBRARY_PATH"
        else
            lib_env="$LD_LIBRARY_PATH"
        fi
    fi
    # Read display from the current env so the parallel capture
    # subshells can each target their own Xvfb. Strip the leading
    # colon and any trailing .screen suffix to recover the numeric
    # display index that run-ui-replay's --display flag wants.
    display_num=${{DISPLAY#:}}
    display_num=${{display_num%%.*}}
    # No -geometry: xcircuit only loads a command-line file when its own
    # main() sees argc == 2 (program + file). Passing -geometry leaves two
    # extra argv entries, so xcircuit takes the crash-recovery branch and
    # the drawing stays empty. The Xvfb screen size already fixes the
    # capture region, so the geometry flag is unnecessary here.
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "$replay" \\
        --app "$app" \\
        --app-arg=examples/FlareLED.ps \\
        --workdir "$workdir" \\
        --replay "$repo/tests/ui/replays/$replay.replay" \\
        --out-root "$replay_out" \\
        --display "$display_num" \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        --screenshot-region {q(args.screenshot_region)} \\
        --env DISPLAY="$DISPLAY" \\
        --env HOME="$remote_root/home-$name" \\
        --env LD_LIBRARY_PATH="$lib_env" \\
        --env LIBX11_COMPAT_FONT_DIR="$repo/fonts"
    python3 - "$replay_out/results.tsv" "$log_dir/results.tsv" "$replay" \\
        "$screen_dir" <<'PY'
import csv
import shutil
import sys
from pathlib import Path

src_results = Path(sys.argv[1])
dest_results = Path(sys.argv[2])
prefix = sys.argv[3]
screen_dir = Path(sys.argv[4])

with src_results.open(newline="") as f:
    rows = list(csv.DictReader(f, delimiter="\\t"))

for row in rows:
    screenshot = row.get("screenshot") or ""
    if not screenshot:
        continue
    src = Path(screenshot)
    dest = screen_dir / f"{{prefix}}-{{src.name}}"
    shutil.copy2(src, dest)
    row["screenshot"] = str(dest)

write_header = not dest_results.exists()
with dest_results.open("a", newline="") as f:
    fields = ["status", "relative_path", "screenshot", "detail"]
    writer = csv.DictWriter(f, fieldnames=fields, delimiter="\\t")
    if write_header:
        writer.writeheader()
    for row in rows:
        writer.writerow({{field: row.get(field, "") for field in fields}})
PY
    cp "$replay_out"/junit.xml "$log_dir/junit.xml"
    cp "$replay_out"/logs/* "$log_dir"/ 2>/dev/null || true
}}

{clean_remote}
rm -rf "$remote_root/screens" "$remote_root/logs" "$remote_root/diff" \\
    "$remote_root/report.tsv" "$remote_root/junit.xml" "$remote_root"/replay-*
mkdir -p "$system_build/source" "$system_logs" "$compat_logs" \\
    "$system_screens" "$compat_screens" "$remote_root/logs"

# Wrap gcc with ccache so the system-side and compat-side gcc objects
# hit the ccache populated by the GitHub Actions cache action. Bare
# `CC=gcc` would skip the cache and recompile cold every CI run.
if command -v ccache >/dev/null 2>&1; then
    if [ -d /usr/lib/ccache ]; then
        export PATH="/usr/lib/ccache:$PATH"
    fi
    export CCACHE_DIR="${{CCACHE_DIR:-$HOME/.cache/ccache}}"
fi
cc_wrapped="gcc"

# Pre-extract upstream XCircuit so the parallel compat-side and
# system-side builds below don't race on the source-stamp step. The
# make rule is a no-op when the actions/cache step already restored
# the xcircuit-src cache.
(cd "$repo" && make build/upstream/xcircuit-src/.source-stamp)

# Run compat-side and system-side builds concurrently. They write into
# disjoint trees ($repo/build/xcircuit vs $system_build/source) and share
# only the read-only upstream tarball extraction. ccache is
# process-safe via its own locking.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" xcircuit
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    rm -rf "$system_build/source"
    mkdir -p "$system_build/source"
    tar --exclude .git --exclude '*.o' --exclude '*.a' --exclude '*.dSYM' \\
        -cf - -C "$repo/build/upstream/xcircuit-src" . | \\
        tar -xf - -C "$system_build/source"
    for patch_file in "$repo"/compat/xcircuit-patches/*.patch; do
        [ -e "$patch_file" ] || continue
        patch -d "$system_build/source" -p1 < "$patch_file"
    done

    # Configure generates Makefile in the current working directory,
    # not at the configure script's location. cd into the source tree
    # before invoking it so the Makefiles land where the subsequent
    # make -C expects them.
    cd "$system_build/source"

    : >"$remote_root/logs/system-configure.log"
    run_logged "$remote_root/logs/system-configure.log" \\
        env CC="$cc_wrapped" \\
            CPPFLAGS="$(pkg-config --cflags x11 xext xpm xt ice sm 2>/dev/null || true)" \\
            LDFLAGS="$(pkg-config --libs-only-L x11 xext xpm xt ice sm 2>/dev/null || true)" \\
            ./configure \\
                --prefix="$system_build/install" \\
                --without-tcl \\
                --without-cairo \\
                --without-python
    : >"$remote_root/logs/system-build.log"
    run_logged "$remote_root/logs/system-build.log" \\
        make -j{q(args.jobs)} \\
            librarydir="$system_build/source/lib" \\
            scriptsdir="$system_build/source/lib"
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

test -x "$system_build/source/xcircuit" || {{
    echo "missing system XCircuit binary" >&2
    exit 1
}}
test -x "$repo/build/xcircuit/source/xcircuit" || {{
    echo "missing compat XCircuit binary" >&2
    exit 1
}}

# The default displays are high to dodge collisions, but on a shared SSH
# host another user's server may already own one. Only clear a lock when no
# server actually answers on that display; a live server's lock must not be
# clobbered out from under it.
claim_display() {{
    disp=$1
    num=${{disp#:}}
    if DISPLAY="$disp" xdotool getdisplaygeometry >/dev/null 2>&1; then
        echo "display $disp already has a live X server; refusing to clobber its lock" >&2
        exit 1
    fi
    # xdotool failing does not prove the display is free: a live server we
    # cannot authenticate against still owns its socket. Treat a bound
    # /tmp/.X11-unix socket as proof the display is taken and leave the lock
    # alone; only a socket-less display has a genuinely stale lock to clear.
    if [ -e "/tmp/.X11-unix/X$num" ]; then
        echo "display $disp has a bound X11 socket; refusing to clobber its lock" >&2
        exit 1
    fi
    rm -f "/tmp/.X$num-lock"
}}
claim_display "$display"
claim_display "$compat_display"
Xvfb "$display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
trap 'kill "$xvfb_pid" "$compat_xvfb_pid" >/dev/null 2>&1 || true' EXIT

# Wait for each Xvfb to accept connections instead of sleeping a fixed
# second. On a loaded headless runner the server can take longer than a
# second to come up, and the old `sleep 1` then raced the first xcircuit
# launch and xdotool query against a display that was not listening yet,
# surfacing as a spurious capture timeout. Probe with xdotool (already a
# hard dependency above; xdpyinfo is not in the differential package set)
# and fail fast if an Xvfb died, e.g. on a stale display lock.
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
        sleep 0.1
        waited=$((waited + 1))
    done
    echo "Xvfb for $target did not become ready within 10s" >&2
    return 1
}}
wait_for_display "$display" "$xvfb_pid"
wait_for_display "$compat_display" "$compat_xvfb_pid"

# Run the startup replay for both sides concurrently on separate Xvfb
# instances so the capture phase scales with one side rather than both.
system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"
echo "xcircuit differential: diffing FlareLED startup" >&2
(
    set -e
    export DISPLAY="$display"
    capture_xcircuit system-startup \\
        "$system_build/source/xcircuit" \\
        "$system_build/source" \\
        xcircuit-flareled-differential \\
        "" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    capture_xcircuit compat-startup \\
        "$repo/build/xcircuit/source/xcircuit" \\
        "$repo/build/xcircuit/source" \\
        xcircuit-flareled-differential \\
        "$repo/build" \\
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


def remote_compare_script(args, remote_repo):
    return f"""
set -eu
remote_root={q(args.remote_root)}
repo={q(remote_repo)}
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
            "Build XCircuit on a Linux SSH host against system libX11 and "
            "libx11-compat, capture representative screenshots, and compare output."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("XCIRCUIT_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > XCIRCUIT_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-xcircuit-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("XCIRCUIT_DIFF_DISPLAY", "131"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("XCIRCUIT_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("XCIRCUIT_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--screenshot-region",
        default=parse_env_default("XCIRCUIT_DIFF_SCREENSHOT_REGION", "0,0,1024,768"),
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
        default=parse_env_bool("XCIRCUIT_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote. Used by the GitHub Actions "
            "differential workflow."
        ),
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("XCIRCUIT_DIFF_MAE_THRESHOLD", "0.20")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("XCIRCUIT_DIFF_CHANGED_THRESHOLD", "0.50")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("XCIRCUIT_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("XCIRCUIT_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
        help="local artifact directory for synced screenshots, diffs, TSV, and JUnit",
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
    # the XCIRCUIT_DIFF_REMOTE_ROOT env var, then the local-mode default
    # (out_root/_work) or the SSH default.
    if args.remote_root is None:
        env_remote_root = os.environ.get("XCIRCUIT_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-xcircuit-differential"

    # Resolve --compare-location precedence: explicit CLI flag wins,
    # then the XCIRCUIT_DIFF_COMPARE_LOCATION env var, then the local-mode
    # default (local) or the SSH default (remote).
    if args.compare_location is None:
        env_compare_location = os.environ.get("XCIRCUIT_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error(
                    "XCIRCUIT_DIFF_COMPARE_LOCATION must be 'remote' or 'local'"
                )
            args.compare_location = env_compare_location
        elif args.local:
            args.compare_location = "local"
        else:
            args.compare_location = "remote"

    if args.local:
        # In local mode the shell payload writes under remote_root and
        # fetch_results then rmtrees the matching out_root subdirs before
        # syncing back. Reject overlap so a misconfigured remote_root
        # cannot delete its own source tree.
        try:
            check_local_paths(args.out_root, Path(args.remote_root))
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
            execute(args, remote_compare_script(args, remote_repo))
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
