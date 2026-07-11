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
    check_local_paths,
    execute,
    fetch_results,
    parse_env_bool,
    parse_env_default,
    q,
    sync_repo,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT_ROOT = ROOT / "build" / "gimp-differential"


def remote_script(args, remote_repo):
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(args.remote_root + '/system-gimp')} "
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
        autoconf automake build-essential ca-certificates git imagemagick \\
        libjpeg-dev libmotif-dev libpixman-1-dev libpng-dev libsdl2-dev \\
        libsdl2-ttf-dev libx11-dev libxext-dev libxmu-dev libxpm-dev \\
        libxt-dev make patch pkg-config python3 python3-pil rsync xauth \\
        xvfb xdotool zlib1g-dev
fi
"""

    # Offset compat-side Xvfb by 1 so the parallel screenshot block
    # below can run system-side and compat-side captures concurrently.
    compat_display_num = int(args.display) + 1

    return f"""
set -eu

{install_deps}

{NEED_SH}

need autoconf
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
system_build="$remote_root/system-gimp"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
display=:{q(args.display)}
compat_display=:{compat_display_num}

{run_logged_sh()}

capture_gimp() {{
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
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "$replay" \\
        --app "$app" \\
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
mkdir -p "$system_build/source" "$system_logs" "$compat_logs" \\
    "$system_screens" "$compat_screens" "$remote_root/logs"

# Pick the C compiler. GIMP's balooii configure.in CFLAGS carry
# -Wno-error=return-mismatch, which only exists on GCC 14+ and clang; an
# older system gcc (e.g. Ubuntu 20.04 gcc 9) errors out on the unknown
# warning name. Prefer clang to match the toolchain CI and macOS already
# use for gimp-motif, falling back to gcc only when no clang is present.
cc_real="${{DIFF_CC:-}}"
if [ -z "$cc_real" ]; then
    for c in clang clang-18 clang-17 clang-16 clang-15 gcc; do
        if command -v "$c" >/dev/null 2>&1; then
            cc_real="$c"
            break
        fi
    done
fi
[ -n "$cc_real" ] || {{ echo "no C compiler found" >&2; exit 127; }}
need "$cc_real"
# Front the compiler with ccache so the system-side and compat-side objects
# hit the ccache populated by the GitHub Actions cache action; a bare CC
# would recompile cold every run.
if command -v ccache >/dev/null 2>&1; then
    cc_wrapped="ccache $cc_real"
    export CCACHE_DIR="${{CCACHE_DIR:-$HOME/.cache/ccache}}"
else
    cc_wrapped="$cc_real"
fi
echo "using CC=$cc_wrapped"

# Pre-extract the upstream GIMP tarball so the parallel compat-side and
# system-side builds below do not race on the source-stamp step. The
# make rule is a no-op when the actions/cache step already restored the
# gimp-src cache.
(cd "$repo" && make build/upstream/gimp-0.54.1/.source-stamp)

# Run compat-side and system-side builds concurrently. They write into
# disjoint trees ($repo/build/gimp-motif vs $system_build/source) and
# share only the read-only upstream tarball extraction. ccache is
# process-safe via its own locking. The compat side builds libx11-compat
# plus the bundled thentenaar/motif as gimp-motif prerequisites; the
# system side links the real -lXm / -lXt / -lX11 from OpenMotif.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" gimp-motif
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    rm -rf "$system_build/source"
    mkdir -p "$system_build/source"
    tar --exclude .git --exclude '*.o' --exclude '*.a' --exclude '*.dSYM' \\
        -cf - -C "$repo/build/upstream/gimp-0.54.1" . | \\
        tar -xf - -C "$system_build/source"
    for patch_file in "$repo"/compat/gimp-patches/*.patch; do
        [ -e "$patch_file" ] || continue
        patch -d "$system_build/source" -p1 < "$patch_file"
    done

    cd "$system_build/source"
    : >"$remote_root/logs/system-autoconf.log"
    run_logged "$remote_root/logs/system-autoconf.log" \\
        autoconf -f -o configure configure.in
    # No --x-includes / --x-libraries overrides: the system build resolves
    # libX11 and OpenMotif (libXm) through the default compiler search path
    # via the GIMP configure AC_PATH_XTRA + AC_CHECK_LIB(Xm) probes.
    : >"$remote_root/logs/system-configure.log"
    run_logged "$remote_root/logs/system-configure.log" \\
        env CC="$cc_wrapped" ./configure --prefix="$system_build/install"
    # Pass CC to both sub-makes. app/Makefile honors configure's CC, but
    # plug-ins/Makefile hardcodes CC = gcc and the balooii CFLAGS carry a
    # GCC-14+/clang-only warning flag, so force the chosen compiler here.
    : >"$remote_root/logs/system-build.log"
    run_logged "$remote_root/logs/system-build.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C app CC="$cc_wrapped"
    : >"$remote_root/logs/system-plugins.log"
    run_logged "$remote_root/logs/system-plugins.log" \\
        env -u MAKEFLAGS -u MFLAGS make -C plug-ins CC="$cc_wrapped"
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
    echo "system-side build failed (exit $system_status); see system-*.log" >&2
    tail -40 "$remote_root/logs/system-configure.log" >&2 || true
    tail -40 "$remote_root/logs/system-build.log" >&2 || true
fi
[ "$compat_status" -eq 0 ] || exit "$compat_status"
[ "$system_status" -eq 0 ] || exit "$system_status"

test -x "$system_build/source/app/gimp" || {{
    echo "missing system GIMP binary" >&2
    exit 1
}}
test -x "$repo/build/gimp-motif/source/app/gimp" || {{
    echo "missing compat GIMP binary" >&2
    exit 1
}}

rm -f "/tmp/.X{q(args.display)}-lock" "/tmp/.X{compat_display_num}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
trap 'kill "$xvfb_pid" "$compat_xvfb_pid" >/dev/null 2>&1 || true' EXIT
sleep 1

# Run the toolbox-startup replay for both sides concurrently on separate
# Xvfb instances so the capture phase scales with one side rather than
# both. The system side drives input with xdotool against real OpenMotif;
# the compat side uses libx11-compat's internal replay backend.
system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"

(
    set -e
    export DISPLAY="$display"
    capture_gimp system-startup \\
        "$system_build/source/app/gimp" \\
        "$system_build/source/app" \\
        gimp-motif-startup \\
        "" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    capture_gimp compat-startup \\
        "$repo/build/gimp-motif/source/app/gimp" \\
        "$repo/build/gimp-motif/source/app" \\
        gimp-motif-startup \\
        "$repo/build:$repo/build/gimp-motif/lib-aliases" \\
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


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Build GIMP 0.54.1 on a Linux SSH host against system libX11 + "
            "OpenMotif and libx11-compat + bundled Motif, capture the Motif "
            "toolbox startup screen, and compare output."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("GIMP_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > GIMP_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-gimp-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("GIMP_DIFF_DISPLAY", "110"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("GIMP_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("GIMP_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--screenshot-region",
        default=parse_env_default("GIMP_DIFF_SCREENSHOT_REGION", "0,0,1024,768"),
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
        default=parse_env_bool("GIMP_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote. Used by the GitHub Actions "
            "differential workflow."
        ),
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("GIMP_DIFF_MAE_THRESHOLD", "0.16")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("GIMP_DIFF_CHANGED_THRESHOLD", "0.42")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("GIMP_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("GIMP_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
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
    # the GIMP_DIFF_REMOTE_ROOT env var, then the local-mode default
    # (out_root/_work) or the SSH default.
    if args.remote_root is None:
        env_remote_root = os.environ.get("GIMP_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-gimp-differential"

    # Resolve --compare-location precedence: explicit CLI flag wins,
    # then the GIMP_DIFF_COMPARE_LOCATION env var, then the local-mode
    # default (local) or the SSH default (remote).
    if args.compare_location is None:
        env_compare_location = os.environ.get("GIMP_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error("GIMP_DIFF_COMPARE_LOCATION must be 'remote' or 'local'")
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
