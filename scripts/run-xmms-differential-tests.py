#!/usr/bin/env python3
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

from differential import (
    WAIT_FOR_DISPLAY_SH,
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
DEFAULT_OUT_ROOT = ROOT / "build" / "xmms-differential"


def remote_script(args, remote_repo):
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(args.remote_root + '/system-gtk1')} "
            f"{q(args.remote_root + '/system-xmms')} "
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
        autoconf automake build-essential ca-certificates cmake git imagemagick \\
        libice-dev libsm-dev libtool libx11-dev libxext-dev libxi-dev make \\
        patch pkg-config python3 python3-pil rsync xauth xvfb xdotool
fi
"""

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
need cmake
need gcc
need git
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
system_gtk="$remote_root/system-gtk1"
system_xmms="$remote_root/system-xmms"
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
        tail -80 "$log" >&2 || true
        exit "$status"
    fi
}}

stage_xmms_home() {{
    home=$1
    rm -rf "$home"
    mkdir -p "$home/.xmms"
    cp "$repo/tests/ui/fixtures/xmms/config" "$home/.xmms/config"
    sed 's/^allow_multiple_instances=.*/allow_multiple_instances=TRUE/' \\
        "$home/.xmms/config" > "$home/.xmms/config.tmp"
    mv "$home/.xmms/config.tmp" "$home/.xmms/config"
    printf '%s\\n' "skin=$repo/tests/ui/fixtures/xmms/skin" >> "$home/.xmms/config"
}}

capture_xmms() {{
    name=$1
    app=$2
    workdir=$3
    libpath=$4
    log_dir=$5
    screen_dir=$6
    input_backend=$7
    extra_flags=$8
    use_display=$9

    replay_out="$remote_root/replay-$name"
    home="$remote_root/home-$name"
    rm -rf "$replay_out"
    mkdir -p "$log_dir" "$screen_dir"
    stage_xmms_home "$home"

    lib_env="$libpath"
    if [ -n "${{LD_LIBRARY_PATH:-}}" ]; then
        if [ -n "$lib_env" ]; then
            lib_env="$lib_env:$LD_LIBRARY_PATH"
        else
            lib_env="$LD_LIBRARY_PATH"
        fi
    fi
    display_flags=""
    if [ "$use_display" = 1 ]; then
        display_flags="--env DISPLAY=$DISPLAY"
    fi

    display_num=${{DISPLAY#:}}
    display_num=${{display_num%%.*}}
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "xmms-$name-startup" \\
        --app "$app" \\
        --workdir "$workdir" \\
        --replay "$remote_root/xmms-differential.replay" \\
        --out-root "$replay_out" \\
        --display "$display_num" \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        --screenshot-region {q(args.screenshot_region)} \\
        $extra_flags \\
        $display_flags \\
        --env HOME="$home" \\
        --env LD_LIBRARY_PATH="$lib_env" \\
        --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
    cp "$replay_out"/screens/*.png "$screen_dir"/
    cp "$replay_out"/results.tsv "$log_dir/results.tsv"
    cp "$replay_out"/junit.xml "$log_dir/junit.xml"
    cp "$replay_out"/logs/* "$log_dir"/ 2>/dev/null || true
}}

build_system_gtk1() {{
    rm -rf "$system_gtk/source" "$system_gtk/cmake"
    mkdir -p "$system_gtk/source" "$system_gtk/cmake"
    tar --exclude .git --exclude '*.o' --exclude '*.a' \\
        -cf - -C "$repo/build/upstream/gtk1" . | tar -xf - -C "$system_gtk/source"
    for patch_file in "$repo"/compat/gtk1-patches/*.patch; do
        [ -e "$patch_file" ] || continue
        patch -d "$system_gtk/source" -p1 < "$patch_file"
    done

    : >"$remote_root/logs/system-gtk1-configure.log"
    run_logged "$remote_root/logs/system-gtk1-configure.log" \\
        cmake -S "$system_gtk/source" -B "$system_gtk/cmake" \\
            -DCMAKE_C_COMPILER="$cc_wrapped" \\
            -DCMAKE_CXX_COMPILER="${{CXX:-g++}}" \\
            -DCMAKE_C_FLAGS="-include stdio.h -include glib/gstrfuncs.h -I$system_gtk/source -DNO_SYS_ERRLIST -DNO_SYS_SIGLIST -DHAVE_ATEXIT -DHAVE_SHAPE_EXT -Wno-error=implicit-function-declaration -Wno-incompatible-pointer-types" \\
            -DCMAKE_CXX_FLAGS="" \\
            -DBUILD_EXAMPLES=OFF -DTABLET_TEST=OFF
    : >"$remote_root/logs/system-gtk1-build.log"
    run_logged "$remote_root/logs/system-gtk1-build.log" \\
        cmake --build "$system_gtk/cmake" --parallel {q(args.jobs)}
}}

build_system_xmms() {{
    rm -rf "$system_xmms/source" "$system_xmms/install" "$system_xmms/config-bin"
    mkdir -p "$system_xmms/source" "$system_xmms/config-bin"
    git -C "$repo/build/upstream/xmms" archive "$xmms_system_rev" | tar -xf - -C "$system_xmms/source"
    for patch_file in "$repo"/compat/xmms-patches/*.patch; do
        [ -e "$patch_file" ] || continue
        patch -d "$system_xmms/source" -p1 < "$patch_file"
    done

    gtk_cflags="-I$system_gtk/source -I$system_gtk/source/gtk -I$system_gtk/source/glib -I$system_gtk/source/gdk -I$system_gtk/source/gdk/x11"
    gtk_libs="-L$system_gtk/cmake -lgtk1 -lX11 -lXext -lm -ldl"
    cat >"$system_xmms/config-bin/gtk-config" <<EOF
#!/bin/sh
for arg in "\\$@"; do
  case "\\$arg" in
    --version) echo 1.2.10; exit 0 ;;
    --cflags) echo $gtk_cflags; exit 0 ;;
    --libs) echo $gtk_libs; exit 0 ;;
  esac
done
exit 0
EOF
    chmod +x "$system_xmms/config-bin/gtk-config"
    cp "$system_xmms/config-bin/gtk-config" "$system_xmms/config-bin/glib-config"

    xmms_cflags="-include stdio.h -include glib/gprintf.h -include $repo/compat/xmms-compat-preinclude.h -include glib/gstrfuncs.h $gtk_cflags -Wno-error=implicit-function-declaration -Wno-incompatible-pointer-types -Wno-error=int-conversion -fcommon"
    xmms_ldflags="-L$system_gtk/cmake -Wl,-rpath,$system_gtk/cmake -Wl,-rpath,$system_xmms/install/lib"
    export PATH="$system_xmms/config-bin:$PATH"
    export LD_LIBRARY_PATH="$system_gtk/cmake${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"

    : >"$remote_root/logs/system-xmms-configure.log"
    (
        cd "$system_xmms/source"
        run_logged "$remote_root/logs/system-xmms-configure.log" \\
            env CC="$cc_wrapped" CFLAGS="$xmms_cflags" LDFLAGS="$xmms_ldflags" \\
            ./configure \\
                --prefix="$system_xmms/install" \\
                --disable-glibtest --disable-gtktest --disable-nls \\
                --disable-opengl --disable-esd --disable-mikmod \\
                --disable-vorbis --disable-oss --disable-alsatest \\
                --disable-ipv6 --disable-user-plugin-dir --enable-one-plugin-dir
    )

    : >"$remote_root/logs/system-xmms-build.log"
    run_logged "$remote_root/logs/system-xmms-build.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C "$system_xmms/source/libxmms" -j{q(args.jobs)}
    run_logged "$remote_root/logs/system-xmms-build.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C "$system_xmms/source/xmms" xmms_LDFLAGS="-export-dynamic" -j{q(args.jobs)}
    run_logged "$remote_root/logs/system-xmms-build.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C "$system_xmms/source/Output/disk_writer" -j{q(args.jobs)}
    run_logged "$remote_root/logs/system-xmms-build.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C "$system_xmms/source/Input/tonegen" -j{q(args.jobs)}

    : >"$remote_root/logs/system-xmms-install.log"
    run_logged "$remote_root/logs/system-xmms-install.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C "$system_xmms/source/libxmms" install
    run_logged "$remote_root/logs/system-xmms-install.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C "$system_xmms/source/xmms" xmms_LDFLAGS="-export-dynamic" install
    run_logged "$remote_root/logs/system-xmms-install.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C "$system_xmms/source/Output/disk_writer" install
    run_logged "$remote_root/logs/system-xmms-install.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C "$system_xmms/source/Input/tonegen" install
}}

{clean_remote}
rm -rf "$remote_root/screens" "$remote_root/logs" "$remote_root/diff" \\
    "$remote_root/report.tsv" "$remote_root/junit.xml" "$remote_root"/replay-* \\
    "$remote_root"/home-*
mkdir -p "$system_logs" "$compat_logs" "$system_screens" "$compat_screens" \\
    "$remote_root/logs"
cat >"$remote_root/xmms-differential.replay" <<'EOF'
delay 5000
screenshot initial
assert-image initial common-visible.json
EOF

if command -v ccache >/dev/null 2>&1; then
    if [ -d /usr/lib/ccache ]; then
        export PATH="/usr/lib/ccache:$PATH"
    fi
    export CCACHE_DIR="${{CCACHE_DIR:-$HOME/.cache/ccache}}"
fi
cc_wrapped="gcc"

(cd "$repo" && make build/upstream/.gtk1-source-stamp build/upstream/.xmms-source-stamp)

# Pin the XMMS revision now, while the shared upstream checkout is quiescent.
# The compat `make xmms` below runs in parallel and re-resets/cleans that same
# checkout, so the system-side extraction must archive an immutable object
# (git archive reads the object database, unaffected by the concurrent working
# tree reset) rather than racing a moving HEAD.
xmms_system_rev="$(git -C "$repo/build/upstream/xmms" rev-parse HEAD)"

compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" CXX="${{CXX:-g++}}" xmms
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    build_system_gtk1
    build_system_xmms
) &
system_pid=$!

compat_status=0
wait "$compat_pid" || compat_status=$?
system_status=0
wait "$system_pid" || system_status=$?

if [ "$compat_status" -ne 0 ]; then
    echo "compat-side build failed (exit $compat_status); see $compat_make_log" >&2
    tail -80 "$compat_make_log" >&2 || true
fi
if [ "$system_status" -ne 0 ]; then
    echo "system-side build failed (exit $system_status); see system-gtk1/system-xmms logs" >&2
    tail -80 "$remote_root/logs/system-gtk1-build.log" >&2 || true
    tail -80 "$remote_root/logs/system-xmms-build.log" >&2 || true
fi
[ "$compat_status" -eq 0 ] || exit "$compat_status"
[ "$system_status" -eq 0 ] || exit "$system_status"

test -x "$system_xmms/install/bin/xmms" || {{
    echo "missing system xmms binary" >&2
    exit 1
}}
test -x "$repo/build/xmms/install/bin/xmms" || {{
    echo "missing compat xmms binary" >&2
    exit 1
}}

rm -f "/tmp/.X{q(args.display)}-lock" "/tmp/.X{compat_display_num}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} -dpi 96 >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} -dpi 96 >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
trap 'kill "$xvfb_pid" "$compat_xvfb_pid" >/dev/null 2>&1 || true' EXIT

{WAIT_FOR_DISPLAY_SH}
wait_for_display "$display" "$xvfb_pid"
wait_for_display "$compat_display" "$compat_xvfb_pid"

system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"
echo "xmms differential: diffing skinned startup replay" >&2

(
    set -e
    export DISPLAY="$display"
    capture_xmms system \\
        "$system_xmms/install/bin/xmms" \\
        "$system_xmms/install" \\
        "$system_xmms/install/lib:$system_gtk/cmake" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool \\
        "" \\
        1
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    capture_xmms compat \\
        "$repo/build/xmms/install/bin/xmms" \\
        "$repo/build/xmms/install" \\
        "$repo/build/xmms/install/lib:$repo/build/gtk1/cmake:$repo/build" \\
        "$compat_logs" \\
        "$compat_screens" \\
        internal \\
        "--in-process-snapshots --offscreen" \\
        1
) >"$compat_cap_log" 2>&1 &
compat_cap_pid=$!

system_cap_status=0
wait "$system_cap_pid" || system_cap_status=$?
compat_cap_status=0
wait "$compat_cap_pid" || compat_cap_status=$?

for replay_dir in "$remote_root"/replay-*; do
    [ -d "$replay_dir" ] || continue
    cp -r "$replay_dir" "$remote_root/logs/$(basename "$replay_dir")" 2>/dev/null || true
done
cp "$remote_root"/xvfb-*.log "$remote_root/logs/" 2>/dev/null || true

if [ "$system_cap_status" -ne 0 ]; then
    echo "system screenshot capture failed (exit $system_cap_status); see $system_cap_log" >&2
    tail -80 "$system_cap_log" >&2 || true
fi
if [ "$compat_cap_status" -ne 0 ]; then
    echo "compat screenshot capture failed (exit $compat_cap_status); see $compat_cap_log" >&2
    tail -80 "$compat_cap_log" >&2 || true
fi
[ "$system_cap_status" -eq 0 ] || exit "$system_cap_status"
[ "$compat_cap_status" -eq 0 ] || exit "$compat_cap_status"
"""


def remote_compare_script(args):
    repo = str(ROOT) if args.local else f"{args.remote_root}/repo"
    return f"""
set -eu
remote_root={q(args.remote_root)}
repo={q(repo)}
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
            "Build XMMS 1.2.x on a Linux SSH host against system libX11 and "
            "libx11-compat, capture the skinned window replay, and compare output."
        )
    )
    parser.add_argument(
        "--remote", default=parse_env_default("XMMS_DIFF_REMOTE", "node11")
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > XMMS_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-xmms-differential)."
        ),
    )
    parser.add_argument(
        "--display", default=parse_env_default("XMMS_DIFF_DISPLAY", "129")
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("XMMS_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("XMMS_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--screenshot-region",
        default=parse_env_default("XMMS_DIFF_SCREENSHOT_REGION", "0,0,640,420"),
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
        default=parse_env_bool("XMMS_DIFF_LOCAL"),
        help="run locally instead of SSHing to --remote",
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("XMMS_DIFF_MAE_THRESHOLD", "0.16")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("XMMS_DIFF_CHANGED_THRESHOLD", "0.42")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("XMMS_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("XMMS_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
        help="local artifact directory for synced screenshots, diffs, TSV, and JUnit",
    )
    parser.add_argument("--compare-location", choices=("remote", "local"), default=None)
    args = parser.parse_args()

    if not re.fullmatch(r"\d+", args.display):
        parser.error("--display must be a numeric X display index")
    if not re.fullmatch(r"\d+", str(args.jobs)):
        parser.error("--jobs must be a positive integer")
    if int(args.jobs) <= 0:
        parser.error("--jobs must be a positive integer")
    if not re.fullmatch(r"\d+,\d+,\d+,\d+", args.screenshot_region):
        parser.error("--screenshot-region must use x,y,width,height")

    if args.remote_root is None:
        env_remote_root = os.environ.get("XMMS_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-xmms-differential"

    if args.compare_location is None:
        env_compare_location = os.environ.get("XMMS_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error("XMMS_DIFF_COMPARE_LOCATION must be 'remote' or 'local'")
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
        print(f"warning: result fetch failed (exit {fetch_status})", file=sys.stderr)

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
