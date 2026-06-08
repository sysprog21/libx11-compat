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
DEFAULT_OUT_ROOT = ROOT / "build" / "violawww-differential"


def run(cmd, *, cwd=ROOT, env=None, input_text=None):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, env=env, input=input_text, text=True, check=True)


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
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \\
        autoconf automake bison build-essential ca-certificates git \\
        imagemagick libtool make pkg-config python3-pil rsync xauth xvfb \\
        xdotool libice-dev libsm-dev libx11-dev libxext-dev libxmu-dev libxpm-dev \\
        libxrender-dev libxt-dev
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
system_build="$repo/build/violawww-system-motif"
system_out="$repo/build/violawww-system-out"
system_viola="$repo/build/violawww-system"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
fixture="$remote_root/violawww-fixture.html"
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
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "violawww-$name-scroll" \\
        --app "$bin" \\
        --app-arg "file:$fixture" \\
        --workdir "$workdir" \\
        --replay "$repo/tests/ui/replays/$replay" \\
        --out-root "$replay_out" \\
        --display {q(args.display)} \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        --env DISPLAY="$display" \\
        --env HOME="$remote_root/home-$name" \\
        --env WWW_HOME="file:$fixture" \\
        --env LD_LIBRARY_PATH="$libpath${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}"
    cp "$replay_out"/screens/*.png "$screen_dir"/
    cp "$replay_out"/results.tsv "$log_dir/results.tsv"
    cp "$replay_out"/junit.xml "$log_dir/junit.xml"
    cp "$replay_out"/logs/* "$log_dir"/ 2>/dev/null || true
}}

{clean_remote}
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

cd "$repo"
make -j{q(args.jobs)} CC=gcc violawww

motif_src="$repo/build/upstream/motif-src"
cd "$system_build"
if [ ! -f .configure-stamp ]; then
    : >"$remote_root/logs/system-motif-configure.log"
    run_logged "$remote_root/logs/system-motif-configure.log" env \\
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
run_logged "$remote_root/logs/system-motif-build.log" make -C config
run_logged "$remote_root/logs/system-motif-build.log" make -C lib/Xm CFLAGS="-g -O0 -include stdlib.h"
run_logged "$remote_root/logs/system-motif-build.log" make -C lib/Mrm CFLAGS="-g -O0 -include stdlib.h"

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
    make -C "$system_viola/source" \\
        CC=gcc \\
        OPENMOTIF_PREFIX="$system_out/motif-install" \\
        CFLAGS="-O0 -g -std=gnu17 -funsigned-char -DVIOLA_LINUX -DNO_ALLOCA -Wno-everything" \\
        CFLAGS_LIBS="-O0 -g -funsigned-char -DVIOLA_LINUX -DNO_ALLOCA -Wno-everything" \\
        X11_CFLAGS="$x11_cflags" \\
        X11_LIBS="$x11_libs" \\
        LDFLAGS="$motif_libs -Wl,-rpath,$system_build/lib/Xm/.libs -Wl,-rpath,$system_build/lib/Mrm/.libs" \\
        LIBS="-lXm $x11_libs -lm" \\
        all

rm -f "/tmp/.X{q(args.display)}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb.log" 2>&1 &
xvfb_pid=$!
trap 'kill "$xvfb_pid" >/dev/null 2>&1 || true' EXIT
sleep 1

capture_vw system \\
    "$system_viola/source/src/vw/vw" \\
    "$system_viola/source" \\
    "$system_build/lib/Xm/.libs:$system_build/lib/Mrm/.libs" \\
    "$system_logs" \\
    "$system_screens" \\
    violawww-scroll-system.replay \\
    xdotool

capture_vw compat \\
    "$repo/build/violawww/source/src/vw/vw" \\
    "$repo/build/violawww/source" \\
    "$repo/build:$repo/build/motif-install/lib" \\
    "$compat_logs" \\
    "$compat_screens" \\
    violawww-scroll-system.replay \\
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
        default=parse_env_default(
            "VIOLAWWW_DIFF_REMOTE_ROOT",
            "/tmp/libx11-compat-violawww-differential",
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("VIOLAWWW_DIFF_DISPLAY", "120"),
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
        default=parse_env_default("VIOLAWWW_DIFF_COMPARE_LOCATION", "remote"),
    )
    args = parser.parse_args()

    if not re.fullmatch(r"\d+", args.display):
        parser.error("--display must be a numeric X display index")
    if not re.fullmatch(r"\d+", str(args.jobs)):
        parser.error("--jobs must be a positive integer")
    if int(args.jobs) <= 0:
        parser.error("--jobs must be a positive integer")

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
