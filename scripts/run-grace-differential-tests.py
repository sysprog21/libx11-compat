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
DEFAULT_OUT_ROOT = ROOT / "build" / "grace-differential"
# Kept in sync with GRACE_VERSION in mk/grace.mk; the tarball extracts to
# build/upstream/grace-<version> on both the compat pre-stage and the system
# untar below.
GRACE_VERSION = "5.1.25"


def remote_script(args, remote_repo):
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(args.remote_root + '/system-grace')} "
            f"{q(args.remote_root + '/screens')} "
            f"{q(args.remote_root + '/logs')} "
            f"{q(args.remote_root + '/diff')}\n"
        )

    install_deps = ""
    if args.install_deps:
        install_deps = """
if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    # Best-effort metadata refresh: a broken third-party repo must not abort
    # the differential. The install still fails loudly on a package we need.
    sudo apt-get update || true
    sudo apt-get install -y --no-install-recommends \\
        autoconf automake bison build-essential ca-certificates flex git \\
        imagemagick libmotif-dev libx11-dev libxext-dev libxmu-dev libxpm-dev \\
        libxt-dev make patch pkg-config python3 python3-pil rsync xauth xvfb \\
        xdotool
fi
"""

    # Offset compat-side Xvfb by 1 so the parallel screenshot block below can
    # run system-side and compat-side captures concurrently.
    compat_display_num = int(args.display) + 1
    grace_dir = f"build/upstream/grace-{GRACE_VERSION}"

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
system_build="$remote_root/system-grace"
system_home="$system_build/runtime"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
display=:{q(args.display)}
compat_display=:{compat_display_num}
fixture="$repo/tests/ui/fixtures/grace/simple.agr"

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

# Capture one side. Grace needs a repo-local GRACE_HOME (fonts, templates,
# examples) and the .agr fixture as an argument, unlike the XEphem model, so
# both are threaded through here. The system side runs against the real X
# server via xdotool; the compat side runs through the internal SDL backend.
capture_grace() {{
    name=$1
    app=$2
    workdir=$3
    replay=$4
    libpath=$5
    log_dir=$6
    screen_dir=$7
    input_backend=$8
    grace_home=$9
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
    display_num=${{DISPLAY#:}}
    display_num=${{display_num%%.*}}
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "$replay" \\
        --app "$app" \\
        --app-arg="$fixture" \\
        --workdir "$workdir" \\
        --replay "$repo/tests/ui/replays/$replay.replay" \\
        --out-root "$replay_out" \\
        --display "$display_num" \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        --screenshot-region {q(args.screenshot_region)} \\
        --env DISPLAY="$DISPLAY" \\
        --env GRACE_HOME="$grace_home" \\
        --env HOME="$remote_root/home-$name" \\
        --env LD_LIBRARY_PATH="$lib_env"
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
mkdir -p "$system_build" "$system_logs" "$compat_logs" \\
    "$system_screens" "$compat_screens" "$remote_root/logs"

if command -v ccache >/dev/null 2>&1; then
    if [ -d /usr/lib/ccache ]; then
        export PATH="/usr/lib/ccache:$PATH"
    fi
    export CCACHE_DIR="${{CCACHE_DIR:-$HOME/.cache/ccache}}"
fi
cc_wrapped="gcc"

# Extract the pinned Grace tarball once before the parallel builds. Both the
# compat-side make grace and the system-side untar below read from this
# single checkout. This also fetches the tarball if the synced cache lacks it.
# Prime the upstream header/source sync in the same step: on a cold tree a
# parallel `make -j grace` can start compiling build/upstream/src/*.o before the
# sync recipe writes those .c/.h files (cf. the xephem script).
(cd "$repo" && make {grace_dir}/.source-stamp \
    build/upstream/include/.upstream-stamp)

# Grace 5.1.25 predates C99; gcc keeps implicit int/decl as warnings (unlike
# clang), but pass the same flags the in-tree build uses so a strict host does
# not turn them into errors on the system side. -D_DEFAULT_SOURCE matches
# mk/grace.mk: it exposes glibc's signgam/caddr_t under -std=c99 via __USE_MISC.
grace_legacy_cflags="-Wno-implicit-function-declaration -Wno-implicit-int -D_DEFAULT_SOURCE"

# Run compat-side and system-side builds concurrently into disjoint trees. The
# compat make grace builds the in-tree Motif + libx11-compat stack and stages
# the runtime; the system side configures Grace against native OpenMotif.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" grace
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    # Keep the native baseline hermetic: strip any inherited toolchain search
    # paths so configure/link/run cannot pick up the in-tree compat headers or
    # libs. Dropping --with-extra-* is not enough on a contaminated shell.
    unset LD_LIBRARY_PATH LIBRARY_PATH CPATH C_INCLUDE_PATH \\
        CPLUS_INCLUDE_PATH PKG_CONFIG_PATH
    rm -rf "$system_build/source" "$system_home"
    mkdir -p "$system_build/source" "$system_home"
    tar --exclude .git --exclude '*.o' --exclude '*.a' \\
        -cf - -C "$repo/{grace_dir}" . | tar -xf - -C "$system_build/source"
    : >"$remote_root/logs/system-build.log"
    for p in "$repo"/compat/grace-patches/*.patch; do
        run_logged "$remote_root/logs/system-build.log" \\
            patch -d "$system_build/source" -p1 -i "$p"
    done
    # Configure Grace against the SYSTEM OpenMotif + system X11: no
    # --with-extra-incpath / --with-extra-ldpath and no lib-aliases farm, so
    # -lXm and -lX11 resolve to /usr/lib (libmotif-dev), which is the whole
    # point of the native baseline. Keep the same feature-disable flags the
    # in-tree build uses so the two binaries render the same fixture.
    (
        cd "$system_build/source"
        run_logged "$remote_root/logs/system-build.log" \\
            env CC="$cc_wrapped" CFLAGS="$grace_legacy_cflags" \\
                ./configure --prefix="$system_build/install" \\
                    --enable-grace-home="$system_home" \\
                    --with-motif-library=-lXm \\
                    --with-bundled-t1lib \\
                    --enable-netcdf=no \\
                    --enable-jpegdrv=no \\
                    --enable-pngdrv=no \\
                    --enable-pdfdrv=no \\
                    --enable-f77-wrapper=no \\
                    --enable-editres=no \\
                    --enable-xmhtml=no \\
                    --with-fftw=no
        run_logged "$remote_root/logs/system-build.log" \\
            env -u MAKEFLAGS -u MFLAGS make
        # Stage the same runtime tree the compat side stages (mk/grace.mk
        # GRACE_RUNTIME_SUBDIRS) into the system GRACE_HOME.
        for d in fonts templates examples; do
            run_logged "$remote_root/logs/system-build.log" \\
                env -u MAKEFLAGS -u MFLAGS \\
                    make -C "$d" install GRACE_HOME="$system_home" DESTDIR=
        done
        cp gracerc gracerc.user "$system_home/"
    )
) &
system_pid=$!

compat_status=0
wait "$compat_pid" || compat_status=$?
system_status=0
wait "$system_pid" || system_status=$?

if [ "$compat_status" -ne 0 ]; then
    echo "compat-side build failed (exit $compat_status); see $compat_make_log" >&2
    tail -60 "$compat_make_log" >&2 || true
fi
if [ "$system_status" -ne 0 ]; then
    echo "system-side build failed (exit $system_status); see system-build.log" >&2
    tail -60 "$remote_root/logs/system-build.log" >&2 || true
fi
[ "$compat_status" -eq 0 ] || exit "$compat_status"
[ "$system_status" -eq 0 ] || exit "$system_status"

sys_bin="$system_build/source/src/xmgrace"
compat_bin="$repo/build/grace/source/src/xmgrace"
compat_home="$repo/build/grace/runtime"
test -x "$sys_bin" || {{ echo "missing system xmgrace binary" >&2; exit 1; }}
test -x "$compat_bin" || {{ echo "missing compat xmgrace binary" >&2; exit 1; }}

rm -f "/tmp/.X{q(args.display)}-lock" "/tmp/.X{compat_display_num}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
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
        sleep 0.1
        waited=$((waited + 1))
    done
    echo "Xvfb for $target did not become ready within 10s" >&2
    return 1
}}
wait_for_display "$display" "$xvfb_pid"
wait_for_display "$compat_display" "$compat_xvfb_pid"

system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"
echo "grace differential: diffing startup" >&2

(
    set -e
    # Native capture must load only /usr/lib X11/Motif, so clear any inherited
    # LD_LIBRARY_PATH before capture_grace folds it into the run env.
    unset LD_LIBRARY_PATH
    export DISPLAY="$display"
    capture_grace system-startup \\
        "$sys_bin" \\
        "$system_build/source/src" \\
        grace-startup-differential \\
        "" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool \\
        "$system_home"
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    capture_grace compat-startup \\
        "$compat_bin" \\
        "$repo/build/grace/source/src" \\
        grace-startup-differential \\
        "$repo/build" \\
        "$compat_logs" \\
        "$compat_screens" \\
        internal \\
        "$compat_home"
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
            "Build Grace (xmgrace) on a Linux SSH host against system OpenMotif "
            "and libx11-compat, capture the startup screenshot, and compare."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("GRACE_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > GRACE_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-grace-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("GRACE_DIFF_DISPLAY", "131"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("GRACE_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("GRACE_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--screenshot-region",
        default=parse_env_default("GRACE_DIFF_SCREENSHOT_REGION", "0,0,680,700"),
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
        default=parse_env_bool("GRACE_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote."
        ),
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("GRACE_DIFF_MAE_THRESHOLD", "0.10")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("GRACE_DIFF_CHANGED_THRESHOLD", "0.25")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("GRACE_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("GRACE_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
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

    if args.remote_root is None:
        env_remote_root = os.environ.get("GRACE_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-grace-differential"

    if args.compare_location is None:
        env_compare_location = os.environ.get("GRACE_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error("GRACE_DIFF_COMPARE_LOCATION must be 'remote' or 'local'")
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

    remote_repo = sync_repo(args, extra_excludes=["/.git/", "/externals/"])
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
