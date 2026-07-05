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
    WAIT_FOR_DISPLAY_SH,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT_ROOT = ROOT / "build" / "xephem-differential"


def remote_script(args, remote_repo):
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(args.remote_root + '/system-xephem')} "
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
        libmotif-dev libssl-dev libx11-dev libxext-dev libxmu-dev libxt-dev \\
        make patch pkg-config python3 python3-pil rsync xauth xvfb xdotool
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
system_build="$remote_root/system-xephem"
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

capture_xephem() {{
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
    mkdir -p "$log_dir" "$screen_dir" "$remote_root/home-$name/Library"
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

# Fetch upstream XEphem once before the parallel builds. Both the
# compat-side `make xephem` and the system-side tar below read from
# this single read-only checkout. Stage the upstream libX11 headers and
# sources in the same serial step: the in-tree compat objects include
# staged headers like locking.h via -iquote build/upstream/src, but the
# generic $(OUT)/%.o rule has no order-only edge on the staging stamp,
# so a high -j compat build can compile src/display.c before the stage
# completes. Pre-building the stamp here closes that race.
(cd "$repo" && make build/upstream/xephem-src/.source-stamp \
    build/upstream/include/.upstream-stamp)

# Run compat-side and system-side builds concurrently. They write into
# disjoint trees ($repo/build/xephem vs $system_build/source) and share
# only the read-only upstream checkout. ccache is process-safe via its
# own locking.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" xephem
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    rm -rf "$system_build/source"
    mkdir -p "$system_build/source"
    tar --exclude .git --exclude '*.o' --exclude '*.a' --exclude '*.dSYM' \\
        --exclude GUI/xephem/xephem \\
        -cf - -C "$repo/build/upstream/xephem-src" . | \\
        tar -xf - -C "$system_build/source"
    : >"$remote_root/logs/system-build.log"
    # Apply the same source patches the compat build applies (see
    # mk/xephem.mk): the ShareDir default define and the degenerate
    # polyline guard. patch -p1 mirrors the in-tree recipe.
    for p in "$repo"/compat/xephem-patches/*.patch; do
        run_logged "$remote_root/logs/system-build.log" \\
            patch -d "$system_build/source" -p1 -i "$p"
    done
    # Build XEphem's bundled support libraries first; the GUI link line
    # pulls them in via LIBLIB. Built serially to match the in-tree
    # recipe; they are small.
    for d in libastro libip libjpegd liblilxml libpng libz; do
        run_logged "$remote_root/logs/system-build.log" \\
            env -u MAKEFLAGS -u MFLAGS \\
                make -C "$system_build/source/$d" CC="$cc_wrapped"
    done
    # Build the GUI binary against the SYSTEM OpenMotif + system X11.
    # MOTIFI=/usr/include holds Xm/, MOTIFL the x86_64 multiarch lib
    # dir. sharedir_def is assembled with literal backslash-quotes so
    # the GUI Makefile's $(CC) ... $(PLATI) re-expansion hands gcc a
    # C string literal for -DXEPHEM_DEFAULT_SHAREDIR.
    sys_gui="$system_build/source/GUI/xephem"
    sharedir_def='-DXEPHEM_DEFAULT_SHAREDIR=\\"'"$sys_gui"'\\"'
    run_logged "$remote_root/logs/system-build.log" \\
        env -u MAKEFLAGS -u MFLAGS \\
            make -C "$sys_gui" xephem \\
                CC="$cc_wrapped" \\
                MOTIFI=/usr/include \\
                MOTIFL=/usr/lib/x86_64-linux-gnu \\
                PLATI="$sharedir_def" \\
                PLATL="" \\
                XLIBS="-lXm -lXt -lXmu -lXext -lX11" \\
                LIBS='$(XLIBS) $(LIBLIB) -lm -lssl -lcrypto'
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
    echo "system-side build failed (exit $system_status); see system-build.log" >&2
    tail -60 "$remote_root/logs/system-build.log" >&2 || true
fi
[ "$compat_status" -eq 0 ] || exit "$compat_status"
[ "$system_status" -eq 0 ] || exit "$system_status"

test -x "$system_build/source/GUI/xephem/xephem" || {{
    echo "missing system XEphem binary" >&2
    exit 1
}}
test -x "$repo/build/xephem/source/GUI/xephem/xephem" || {{
    echo "missing compat XEphem binary" >&2
    exit 1
}}

rm -f "/tmp/.X{q(args.display)}-lock" "/tmp/.X{compat_display_num}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
trap 'kill "$xvfb_pid" "$compat_xvfb_pid" >/dev/null 2>&1 || true' EXIT

# Wait for each Xvfb to accept connections instead of sleeping a fixed
# second. On a loaded headless runner the server can take longer than a
# second to come up, and a fixed sleep then races the first xephem
# launch and xdotool query against a display that is not listening yet,
# surfacing as a spurious capture timeout. Probe with xdotool (already a
# hard dependency above; xdpyinfo is not in the differential package set)
# and fail fast if an Xvfb died, e.g. on a stale display lock.
{WAIT_FOR_DISPLAY_SH}
wait_for_display "$display" "$xvfb_pid"
wait_for_display "$compat_display" "$compat_xvfb_pid"

# Run the startup replay for both sides concurrently on separate Xvfb
# instances so the capture phase scales with one side rather than both.
system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"
echo "xephem differential: diffing startup" >&2

(
    set -e
    export DISPLAY="$display"
    capture_xephem system-startup \\
        "$system_build/source/GUI/xephem/xephem" \\
        "$system_build/source/GUI/xephem" \\
        xephem-startup-differential \\
        "" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    capture_xephem compat-startup \\
        "$repo/build/xephem/source/GUI/xephem/xephem" \\
        "$repo/build/xephem/source/GUI/xephem" \\
        xephem-startup-differential \\
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


def remote_compare_script(args):
    # In local mode sync_repo runs the build straight from this checkout and
    # never populates remote_root/repo, so resolve the compare script against
    # ROOT. Only the SSH path stages a copy under remote_root/repo.
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
            "Build XEphem on a Linux SSH host against system OpenMotif and "
            "libx11-compat, capture the startup screenshot, and compare output."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("XEPHEM_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > XEPHEM_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-xephem-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("XEPHEM_DIFF_DISPLAY", "129"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("XEPHEM_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("XEPHEM_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--screenshot-region",
        default=parse_env_default("XEPHEM_DIFF_SCREENSHOT_REGION", "0,0,1000,700"),
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
        default=parse_env_bool("XEPHEM_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote. Used by the GitHub Actions "
            "differential workflow."
        ),
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("XEPHEM_DIFF_MAE_THRESHOLD", "0.22")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("XEPHEM_DIFF_CHANGED_THRESHOLD", "0.55")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("XEPHEM_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("XEPHEM_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
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
    # the XEPHEM_DIFF_REMOTE_ROOT env var, then the local-mode default
    # (out_root/_work) or the SSH default.
    if args.remote_root is None:
        env_remote_root = os.environ.get("XEPHEM_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-xephem-differential"

    # Resolve --compare-location precedence: explicit CLI flag wins,
    # then the XEPHEM_DIFF_COMPARE_LOCATION env var, then the local-mode
    # default (local) or the SSH default (remote).
    if args.compare_location is None:
        env_compare_location = os.environ.get("XEPHEM_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error("XEPHEM_DIFF_COMPARE_LOCATION must be 'remote' or 'local'")
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
