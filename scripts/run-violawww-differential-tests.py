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
DEFAULT_OUT_ROOT = ROOT / "build" / "violawww-differential"


def remote_script(args, remote_repo):
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(remote_repo + '/build/violawww-system-motif')} "
            f"{q(remote_repo + '/build/violawww-system-out')} "
            f"{q(remote_repo + '/build/violawww-system')} "
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
        autoconf automake bison build-essential ca-certificates git \\
        imagemagick libtool make pkg-config python3-pil rsync xauth xvfb \\
        xdotool libice-dev libsm-dev libx11-dev libxext-dev libxmu-dev libxpm-dev \\
        libfontconfig1-dev libxft-dev libxrender-dev libxt-dev
fi
"""

    # Offset compat-side Xvfb by 1 so the parallel screenshot block
    # below can run system-side and compat-side captures concurrently.
    compat_display_num = int(args.display) + 1

    return f"""
set -eu

{install_deps}

{NEED_SH}

need autoreconf
need "${{DIFF_CC:-gcc}}"
# The system-side Motif build preprocesses with CPP="gcc -E", so gcc is
# required even when DIFF_CC selects another compiler.
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
system_build="$repo/build/violawww-system-motif"
system_out="$repo/build/violawww-system-out"
system_viola="$repo/build/violawww-system"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
fixture="$remote_root/violawww-fixture.html"
display=:{q(args.display)}
compat_display=:{compat_display_num}

{run_logged_sh()}

capture_vw() {{
    name=$1
    bin=$2
    workdir=$3
    libpath=$4
    log_dir=$5
    screen_dir=$6
    replay=$7
    input_backend=$8
    replay_out="$remote_root/replay-$name"
    rm -rf "$replay_out"
    mkdir -p "$log_dir" "$screen_dir"
    # Read display from the current env so the parallel capture
    # subshells can each target their own Xvfb. Strip the leading
    # colon and any trailing .screen suffix to recover the numeric
    # display index that run-ui-replay's --display flag wants.
    display_num=${{DISPLAY#:}}
    display_num=${{display_num%%.*}}
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "violawww-$name-scroll" \\
        --app "$bin" \\
        --app-arg "file:$fixture" \\
        --workdir "$workdir" \\
        --replay "$repo/tests/ui/replays/$replay" \\
        --out-root "$replay_out" \\
        --display "$display_num" \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        $([ "$input_backend" = internal ] && printf %s --in-process-snapshots) \\
        --env DISPLAY="$DISPLAY" \\
        --env HOME="$remote_root/home-$name" \\
        --env VIOLA_PATH="$workdir/res/objs" \\
        --env VIOLA_SGML="$workdir/res" \\
        --env WWW_HOME="file:$fixture" \\
        --env LD_LIBRARY_PATH="$libpath${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
    cp "$replay_out"/screens/*.png "$screen_dir"/
    cp "$replay_out"/results.tsv "$log_dir/results.tsv"
    cp "$replay_out"/junit.xml "$log_dir/junit.xml"
    cp "$replay_out"/logs/* "$log_dir"/ 2>/dev/null || true
}}

{clean_remote}
rm -rf "$remote_root/screens" "$remote_root/logs" "$remote_root/diff" \\
    "$remote_root/report.tsv" "$remote_root/junit.xml" "$remote_root"/replay-*
mkdir -p "$system_build" "$system_out" "$system_viola" "$system_logs" \\
    "$compat_logs" "$system_screens" "$compat_screens" "$remote_root/home-system" \\
    "$remote_root/home-compat" "$remote_root/logs"

cat >"$fixture" <<'EOF'
<!doctype html>
<html>
<head>
<title>ViolaWWW libX11 differential fixture</title>
</head>
<body>
<h1>ViolaWWW Differential</h1>
<p>This local document exercises Motif chrome, Xlib drawing, text, links, lists,
forms, and table layout without depending on network access.</p>
<ul>
<li><a href="https://info.cern.ch/">CERN home</a></li>
<li><a href="https://web.archive.org/">Wayback</a></li>
</ul>
<table border="1">
<tr><th>API</th><th>Expected visible result</th></tr>
<tr><td>XDrawString</td><td>Readable proportional text</td></tr>
<tr><td>XFillRectangle</td><td>Cell backgrounds and borders</td></tr>
<tr><td>XDrawLine</td><td>Table grid lines remain aligned while scrolling</td></tr>
<tr><td>XCopyArea</td><td>Viewport updates do not leave stale glyphs</td></tr>
<tr><td>XClearArea</td><td>Rows repaint cleanly after expose and resize</td></tr>
</table>
<p>Scroll marker 01: this paragraph makes the page taller than the viewport.</p>
<p>Scroll marker 02: text should move after wheel input on system X11.</p>
<p>Scroll marker 03: text should move after wheel input on libx11-compat.</p>
<p>Scroll marker 04: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 05: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 06: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 07: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 08: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 09: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 10: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 11: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 12: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 13: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 14: repeated rows exercise clipping and repaint.</p>
<p>Scroll marker 15: repeated rows exercise clipping and repaint.</p>
<form>
<input type="text" value="typed text">
<input type="submit" value="Submit">
</form>
</body>
</html>
EOF

{compiler_setup_sh()}

motif_src="$repo/build/upstream/motif"

# Pre-extract upstream Motif + ViolaWWW so the parallel compat-side
# and system-side builds below don't race on the source / autoreconf
# steps. Each make is a no-op when the actions/cache step already
# restored the matching cache.
(cd "$repo" && make build/upstream/motif/.autogen-stamp build/upstream/violawww-src/.source-stamp)

# Run compat-side and system-side builds concurrently. They write into
# disjoint trees ($repo/build/violawww vs $system_build / $system_viola)
# and share only the read-only upstream sources. ccache is process-safe
# via its own locking.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" violawww
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
            --with-xft \\
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

    rm -rf "$system_viola/source"
    mkdir -p "$system_viola/source"
    tar --exclude .git --exclude '*.o' --exclude '*.d' --exclude '*.a' \\
        --exclude src/vw/vw --exclude src/viola/viola --exclude vplot_dir/vplot \\
        -cf - -C "$repo/build/upstream/violawww-src" . | tar -xf - -C "$system_viola/source"
    for patch_file in "$repo"/compat/violawww-patches/*.patch; do
        [ -e "$patch_file" ] || continue
        patch -d "$system_viola/source" -p1 < "$patch_file"
    done

    x11_cflags=$(pkg-config --cflags x11 xext xmu xt sm ice xrender xpm 2>/dev/null || true)
    x11_libs=$(pkg-config --libs x11 xext xmu xt sm ice xrender xpm 2>/dev/null || \\
        printf '%s\\n' "-lXext -lXmu -lXt -lSM -lICE -lX11 -lXrender -lXpm")
    motif_cflags="-I$motif_src/lib -I$system_build/lib"
    motif_libs="-L$system_build/lib/Xm/.libs -L$system_build/lib/Mrm/.libs"
    : "${{x11_cflags:=$motif_cflags}}"
    x11_cflags="$motif_cflags $x11_cflags"
    : >"$remote_root/logs/system-violawww-build.log"
    run_logged "$remote_root/logs/system-violawww-build.log" \\
        make -j{q(args.jobs)} -C "$system_viola/source" \\
            CC="$cc_wrapped" \\
            OPENMOTIF_PREFIX="$system_out/motif-install" \\
            CFLAGS="-O0 -g -std=gnu17 -funsigned-char -DVIOLA_LINUX -DNO_ALLOCA -Wno-everything" \\
            CFLAGS_LIBS="-O0 -g -funsigned-char -DVIOLA_LINUX -DNO_ALLOCA -Wno-everything" \\
            X11_CFLAGS="$x11_cflags" \\
            X11_LIBS="$x11_libs" \\
            LDFLAGS="$motif_libs -Wl,-rpath,$system_build/lib/Xm/.libs -Wl,-rpath,$system_build/lib/Mrm/.libs" \\
            LIBS="-lXm $x11_libs -lm" \\
            all
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
    echo "system-side build failed (exit $system_status); see system-motif-build.log / system-violawww-build.log" >&2
    tail -60 "$remote_root/logs/system-motif-build.log" >&2 || true
    tail -60 "$remote_root/logs/system-violawww-build.log" >&2 || true
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

# Capture system-side and compat-side ViolaWWW replays on separate
# Xvfb instances. Each capture drives the same scroll replay, so running
# them concurrently halves the dominant runtime cost of this job.
system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"

(
    set -e
    export DISPLAY="$display"
    capture_vw system \\
        "$system_viola/source/src/vw/vw" \\
        "$system_viola/source" \\
        "$system_build/lib/Xm/.libs:$system_build/lib/Mrm/.libs" \\
        "$system_logs" \\
        "$system_screens" \\
        violawww-scroll-system.replay \\
        xdotool
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    capture_vw compat \\
        "$repo/build/violawww/source/src/vw/vw" \\
        "$repo/build/violawww/source" \\
        "$repo/build:$repo/build/motif-install/lib" \\
        "$compat_logs" \\
        "$compat_screens" \\
        violawww-scroll-system.replay \\
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
            "Build ViolaWWW on a Linux SSH host against system libX11 and "
            "libx11-compat, capture screenshots under Xvfb, and compare output."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("VIOLAWWW_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > "
            "VIOLAWWW_DIFF_REMOTE_ROOT env > local-mode default "
            "(out_root/_work) > SSH default "
            "(/tmp/libx11-compat-violawww-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("VIOLAWWW_DIFF_DISPLAY", "102"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("VIOLAWWW_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("VIOLAWWW_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--seconds",
        default=parse_env_default("VIOLAWWW_DIFF_SECONDS", "5"),
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
        default=parse_env_bool("VIOLAWWW_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote. Used by the GitHub Actions "
            "differential workflow."
        ),
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("VIOLAWWW_DIFF_MAE_THRESHOLD", "0.12")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("VIOLAWWW_DIFF_CHANGED_THRESHOLD", "0.35")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("VIOLAWWW_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("VIOLAWWW_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
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

    # Resolve --remote-root precedence: explicit CLI flag wins, then
    # the VIOLAWWW_DIFF_REMOTE_ROOT env var, then the local-mode default
    # (out_root/_work) or the SSH default.
    if args.remote_root is None:
        env_remote_root = os.environ.get("VIOLAWWW_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-violawww-differential"

    # Resolve --compare-location precedence: explicit CLI flag wins,
    # then the VIOLAWWW_DIFF_COMPARE_LOCATION env var, then the local-mode
    # default (local) or the SSH default (remote).
    if args.compare_location is None:
        env_compare_location = os.environ.get("VIOLAWWW_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error(
                    "VIOLAWWW_DIFF_COMPARE_LOCATION must be 'remote' or 'local'"
                )
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

    # Catch rsync failures here so an unrelated transfer hiccup can't
    # mask the original remote/compare failure on the exit path below.
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
