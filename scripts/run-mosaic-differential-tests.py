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
DEFAULT_OUT_ROOT = ROOT / "build" / "mosaic-differential"


def remote_script(args, remote_repo):
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(remote_repo + '/build/mosaic-system-motif')} "
            f"{q(remote_repo + '/build/mosaic-system-out')} "
            f"{q(remote_repo + '/build/mosaic-system')} "
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
        autoconf automake bison build-essential ca-certificates git \\
        imagemagick libtool make pkg-config python3-pil rsync xauth xvfb \\
        xdotool libice-dev libsm-dev libx11-dev libxext-dev libxmu-dev libxpm-dev \\
        libxt-dev
fi
"""

    # Offset compat-side Xvfb by 1 so the parallel screenshot block can
    # run system-side and compat-side captures concurrently without lock
    # collisions on the X server socket.
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

need autoreconf
need gcc
need git
need import
need make
need pkg-config
need python3
need rsync
need Xvfb
need xdotool
if command -v yacc >/dev/null 2>&1; then
    yacc_bin=yacc
elif command -v bison >/dev/null 2>&1; then
    yacc_bin="bison -y"
else
    echo "missing required command: yacc or bison" >&2
    exit 127
fi

remote_root={q(args.remote_root)}
repo={q(remote_repo)}
system_build="$repo/build/mosaic-system-motif"
system_out="$repo/build/mosaic-system-out"
system_mosaic="$repo/build/mosaic-system"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
fixture="$repo/tests/ui/fixtures/mosaic/link-index.html"
display=:{q(args.display)}
compat_display=:{compat_display_num}
replay={q(args.replay)}

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

capture_mosaic() {{
    name=$1
    bin=$2
    workdir=$3
    libpath=$4
    log_dir=$5
    screen_dir=$6
    input_backend=$7
    replay_out="$remote_root/replay-$name"
    rm -rf "$replay_out" "$remote_root/home-$name"
    mkdir -p "$log_dir" "$screen_dir" "$remote_root/home-$name"
    # Read display from the current env so the parallel capture
    # subshells can each target their own Xvfb. Strip the leading
    # colon and any trailing .screen suffix to recover the numeric
    # display index that run-ui-replay's --display flag wants.
    display_num=${{DISPLAY#:}}
    display_num=${{display_num%%.*}}
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "mosaic-$name-link" \\
        --app "$bin" \\
        --app-arg=-geometry --app-arg=900x720+0+0 \\
        --app-arg "file://localhost$fixture" \\
        --workdir "$workdir" \\
        --replay "$repo/tests/ui/replays/$replay" \\
        --out-root "$replay_out" \\
        --display "$display_num" \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        --screenshot-region 0,0,900,720 \\
        --env DISPLAY="$DISPLAY" \\
        --env HOME="$remote_root/home-$name" \\
        --env LD_LIBRARY_PATH="$libpath${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}" \\
        --env XAPPLRESDIR="$workdir" \\
        --env XFILESEARCHPATH="$workdir/%N:$workdir/%N.ad" \\
        --env LIBX11_COMPAT_FONT_DIR="$repo/build/../fonts"
    cp "$replay_out"/screens/*.png "$screen_dir"/
    cp "$replay_out"/results.tsv "$log_dir/results.tsv"
    cp "$replay_out"/junit.xml "$log_dir/junit.xml"
    cp "$replay_out"/logs/* "$log_dir"/ 2>/dev/null || true
}}

{clean_remote}
rm -rf "$remote_root/screens" "$remote_root/logs" "$remote_root/diff" \\
    "$remote_root/report.tsv" "$remote_root/junit.xml" "$remote_root"/replay-*
mkdir -p "$system_build" "$system_out" "$system_mosaic" "$system_logs" \\
    "$compat_logs" "$system_screens" "$compat_screens" "$remote_root/logs"

# Wrap gcc with ccache so the system-side Motif build, the system-side
# Mosaic build, and the compat-side mosaic build all hit the ccache
# populated by the GitHub Actions cache action. Use PATH-based wrapping
# rather than CC="ccache gcc" so recursive Makefiles that pass $(CC)
# unquoted to sub-make (Mosaic's makefiles/Makefile.linux is one) do
# not tokenize the value into "CC=ccache gcc-target".
if command -v ccache >/dev/null 2>&1; then
    if [ -d /usr/lib/ccache ]; then
        export PATH="/usr/lib/ccache:$PATH"
    fi
    export CCACHE_DIR="${{CCACHE_DIR:-$HOME/.cache/ccache}}"
fi
cc_wrapped="gcc"

motif_src="$repo/build/upstream/motif"

# Pre-extract upstream Motif + Mosaic so the parallel compat-side and
# system-side builds below don't race on the source / autoreconf steps.
# Each make is a no-op when the actions/cache step already restored
# the matching cache.
(cd "$repo" && make build/upstream/motif/.autogen-stamp build/upstream/mosaic/.source-stamp)

# Run compat-side and system-side builds concurrently. They write into
# disjoint trees ($repo/build/mosaic vs $system_build / $system_mosaic)
# and share only the read-only upstream sources. ccache is process-safe
# via its own locking.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" mosaic
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    cd "$system_build"
    if [ ! -f .configure-stamp ]; then
        : >"$remote_root/logs/system-motif-configure.log"
        run_logged "$remote_root/logs/system-motif-configure.log" env \\
            CC="$cc_wrapped" \\
            CPP="gcc -E" \\
            CFLAGS="-g -O0 -include stdlib.h" \\
            YACC="$yacc_bin" \\
            "$motif_src/configure" \\
            --prefix="$system_out/motif-install" \\
            --disable-glw \\
            --disable-tests \\
            --without-xft \\
            --with-jpeg=no \\
            --with-png=no \\
            --with-xrandr=no \\
            --with-xrender=no \\
            --with-xcursor=no \\
            --with-xinerama=no
        touch .configure-stamp
    fi
    : >"$remote_root/logs/system-motif-build.log"
    run_logged "$remote_root/logs/system-motif-build.log" make -j{q(args.jobs)} -C config
    run_logged "$remote_root/logs/system-motif-build.log" make -j{q(args.jobs)} -C lib/Xm CC="$cc_wrapped" CFLAGS="-g -O0 -include stdlib.h"
    run_logged "$remote_root/logs/system-motif-build.log" make -j{q(args.jobs)} -C lib/Mrm CC="$cc_wrapped" CFLAGS="-g -O0 -include stdlib.h"

    rm -rf "$system_mosaic/source"
    mkdir -p "$system_mosaic/source"
    tar --exclude .git --exclude '*.o' --exclude '*.a' \\
        --exclude '*.dSYM' --exclude src/Mosaic \\
        -cf - -C "$repo/build/upstream/mosaic" . | tar -xf - -C "$system_mosaic/source"
    for patch_file in "$repo"/compat/mosaic-patches/*.patch; do
        [ -e "$patch_file" ] || continue
        patch -d "$system_mosaic/source" -p1 < "$patch_file"
    done
    touch "$system_mosaic/source/config.h"

    x11_cflags=$(pkg-config --cflags x11 xext xmu xt xpm 2>/dev/null || true)
    x11_libs=$(pkg-config --libs x11 xext xmu xt xpm 2>/dev/null || \\
        printf '%s\\n' "-lXext -lXmu -lXt -lXpm -lX11")
    motif_cflags="-I$motif_src/lib -I$system_build/lib"
    motif_libs="-L$system_build/lib/Xm/.libs -L$system_build/lib/Mrm/.libs"
    : >"$remote_root/logs/system-mosaic-build.log"
    run_logged "$remote_root/logs/system-mosaic-build.log" \\
        make -j{q(args.jobs)} -C "$system_mosaic/source" -f makefiles/Makefile.linux \\
            MAKEFLAGS= MFLAGS= \\
            CC="$cc_wrapped" \\
            RANLIB=ranlib \\
            CFLAGS="-O0 -g -std=gnu89 -fcommon -w -DMOTIF -DMOTIF1_2 -DLINUX -DXMOSAIC -D_GNU_SOURCE -D_DARWIN_C_SOURCE" \\
            sysconfigflags="-DMOTIF -DMOTIF1_2 -DLINUX" \\
            prereleaseflags="" \\
            customflags='-DHOME_PAGE_DEFAULT=\\\\\\"http://info.cern.ch/\\\\\\"' \\
            xinc="$motif_cflags $x11_cflags" \\
            xlibs="-lXm $x11_libs" \\
            ldflags="$motif_libs -Wl,-rpath,$system_build/lib/Xm/.libs -Wl,-rpath,$system_build/lib/Mrm/.libs" \\
            pngflags="" \\
            pnglibs="" \\
            jpegflags="" \\
            jpeglibs="" \\
            krbflags="" \\
            krblibs="" \\
            syslibs="-lm" \\
            default
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
    echo "system-side build failed (exit $system_status); see system-motif-build.log / system-mosaic-build.log" >&2
    tail -60 "$remote_root/logs/system-motif-build.log" >&2 || true
    tail -60 "$remote_root/logs/system-mosaic-build.log" >&2 || true
fi
[ "$compat_status" -eq 0 ] || exit "$compat_status"
[ "$system_status" -eq 0 ] || exit "$system_status"

rm -f "/tmp/.X{q(args.display)}-lock" "/tmp/.X{compat_display_num}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
trap 'kill "$xvfb_pid" "$compat_xvfb_pid" >/dev/null 2>&1 || true' EXIT
sleep 1

# Mosaic system-side and compat-side captures each run a replay
# scripted against the binary; running them on disjoint X servers in
# parallel cuts the capture phase by roughly half.
system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"

(
    set -e
    export DISPLAY="$display"
    capture_mosaic system \\
        "$system_mosaic/source/src/Mosaic" \\
        "$system_mosaic/source" \\
        "$system_build/lib/Xm/.libs:$system_build/lib/Mrm/.libs" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    capture_mosaic compat \\
        "$repo/build/mosaic/source/src/Mosaic" \\
        "$repo/build/mosaic/source" \\
        "$repo/build:$repo/build/motif-install/lib" \\
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
            "Build Mosaic on a Linux SSH host against system libX11 and "
            "libx11-compat, capture screenshots under Xvfb, and compare output."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("MOSAIC_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > MOSAIC_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-mosaic-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("MOSAIC_DIFF_DISPLAY", "123"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("MOSAIC_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("MOSAIC_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--replay",
        default=parse_env_default("MOSAIC_DIFF_REPLAY", "mosaic-differential.replay"),
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
        default=parse_env_bool("MOSAIC_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote. Used by the GitHub Actions "
            "differential workflow."
        ),
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("MOSAIC_DIFF_MAE_THRESHOLD", "0.16")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("MOSAIC_DIFF_CHANGED_THRESHOLD", "0.42")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("MOSAIC_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("MOSAIC_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
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
    if "/" in args.replay or args.replay.startswith("."):
        parser.error("--replay must name a replay file under tests/ui/replays")

    # Resolve --remote-root precedence: explicit CLI flag wins, then
    # the MOSAIC_DIFF_REMOTE_ROOT env var, then the local-mode default
    # (out_root/_work) or the SSH default.
    if args.remote_root is None:
        env_remote_root = os.environ.get("MOSAIC_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-mosaic-differential"

    # Resolve --compare-location precedence: explicit CLI flag wins,
    # then the MOSAIC_DIFF_COMPARE_LOCATION env var, then the local-mode
    # default (local) or the SSH default (remote).
    if args.compare_location is None:
        env_compare_location = os.environ.get("MOSAIC_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error("MOSAIC_DIFF_COMPARE_LOCATION must be 'remote' or 'local'")
            args.compare_location = env_compare_location
        elif args.local:
            args.compare_location = "local"
        else:
            args.compare_location = "remote"

    if args.local:
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
