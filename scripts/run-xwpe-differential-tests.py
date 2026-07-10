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
    WAIT_FOR_DISPLAY_SH,
)

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUT_ROOT = ROOT / "build" / "xwpe-differential"


def remote_script(args, remote_repo):
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(args.remote_root + '/system-xwpe')} "
            f"{q(args.remote_root + '/screens')} "
            f"{q(args.remote_root + '/logs')} "
            f"{q(args.remote_root + '/diff')}\n"
        )

    install_deps = ""
    if args.install_deps:
        install_deps = """
if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    # A transient failure on an unrelated third-party repo must not abort
    # the differential: the metadata refresh is best-effort, and the
    # install below still fails loudly if a package we need is missing.
    sudo apt-get update || true
    sudo apt-get install -y --no-install-recommends \\
        autoconf automake build-essential ca-certificates git imagemagick \\
        libfontconfig1-dev libice-dev libjson-c-dev libncurses-dev libsm-dev \\
        libx11-dev libxext-dev libxft-dev make patch pkg-config python3 \\
        python3-pil rsync xauth xvfb xdotool
fi
"""

    # Offset the compat-side Xvfb by 1 so the two capture subshells below
    # can run system-side and compat-side captures concurrently.
    compat_display_num = int(args.display) + 1

    return f"""
set -eu

{install_deps}

{NEED_SH}

need "${{DIFF_CC:-gcc}}"
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
system_build="$remote_root/system-xwpe"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
display=:{q(args.display)}
compat_display=:{compat_display_num}

{run_logged_sh()}

capture_xwpe() {{
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
    # Recover the numeric display index the parallel capture subshells set
    # in their own env (strip the leading colon and any .screen suffix).
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
        --env XWPE_LIB="$(dirname "$app")" \\
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

# xwpe's File-Manager lists the working directory, so its dialog height and
# vertical centering depend on that directory's contents. Point both sides
# at one fixture directory with identical contents (and an identical
# absolute path, since the tree pane shows it) so the two captures line up
# and the diff reflects rendering, not a different file listing.
fm_fixture="$remote_root/fm-fixture"
rm -rf "$fm_fixture"
mkdir -p "$fm_fixture/subdir"
printf 'alpha\\n' > "$fm_fixture/alpha.txt"
printf 'bravo bravo\\n' > "$fm_fixture/bravo.c"
printf 'charlie\\n' > "$fm_fixture/charlie.h"
printf 'delta delta delta\\n' > "$fm_fixture/delta.md"
printf 'echo\\n' > "$fm_fixture/echo.sh"

{compiler_setup_sh(legacy=True, source_date_epoch=True)}

# Clone upstream xwpe once so the parallel builds below do not race on the
# git checkout. A no-op when the source-stamp is already present.
(cd "$repo" && make build/upstream/xwpe/.source-stamp)

# Build the compat side and the system side concurrently. They write into
# disjoint trees ($repo/build/xwpe vs $system_build/source) and share only
# the read-only upstream clone.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" xwpe
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    rm -rf "$system_build/source"
    mkdir -p "$system_build/source"
    tar --exclude .git --exclude '*.o' --exclude '*.a' \\
        -cf - -C "$repo/build/upstream/xwpe" . | \\
        tar -xf - -C "$system_build/source"
    for patch_file in "$repo"/compat/xwpe-patches/*.patch; do
        [ -e "$patch_file" ] || continue
        patch -d "$system_build/source" -p1 < "$patch_file"
    done

    cd "$system_build/source"

    # Hide cairo / vterm from pkg-config so the system build takes the same
    # Xlib chrome render path the compat build uses, rather than xwpe's
    # cairo frontend. Without this the two sides render through unrelated
    # stacks and the pixel diff is meaningless.
    pc_wrap="$system_build/source/pc-wrap.sh"
    cat > "$pc_wrap" <<'PCW'
#!/bin/sh
for arg do
  case "$arg" in *cairo*|*pangocairo*|vterm|vterm03) exit 1;; esac
done
exec pkg-config "$@"
PCW
    chmod +x "$pc_wrap"

    : >"$remote_root/logs/system-configure.log"
    run_logged "$remote_root/logs/system-configure.log" \\
        env PKG_CONFIG="$pc_wrap" autoreconf -fi
    run_logged "$remote_root/logs/system-configure.log" \\
        env CC="$cc_wrapped" PKG_CONFIG="$pc_wrap" \\
            ./configure --prefix="$system_build/install" --without-gpm
    : >"$remote_root/logs/system-build.log"
    run_logged "$remote_root/logs/system-build.log" \\
        env PKG_CONFIG="$pc_wrap" make -j{q(args.jobs)}
    # The X11 frontend is selected from argv[0]; the build emits the single
    # `we` binary and the install step is what normally adds the aliases.
    ln -sf we "$system_build/source/xwpe"
    ln -sf we "$system_build/source/xwe"
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
    echo "system-side build failed (exit $system_status); see system-configure.log / system-build.log" >&2
    tail -60 "$remote_root/logs/system-configure.log" >&2 || true
    tail -60 "$remote_root/logs/system-build.log" >&2 || true
fi
[ "$compat_status" -eq 0 ] || exit "$compat_status"
[ "$system_status" -eq 0 ] || exit "$system_status"

test -x "$system_build/source/we" || {{
    echo "missing system xwpe binary" >&2
    exit 1
}}
test -x "$repo/build/xwpe/source/we" || {{
    echo "missing compat xwpe binary" >&2
    exit 1
}}

rm -f "/tmp/.X{q(args.display)}-lock" "/tmp/.X{compat_display_num}-lock"
# Pin the X server to 96 DPI to match the compat side's 96-DPI parity so the
# two fonts are requested at the same point size. The native Xft path still
# picks a slightly taller line box than the SDL_ttf path, so the two
# File-Managers do not line up pixel for pixel; the diff thresholds are set
# wide enough (see mk/xwpe.mk) to absorb that structural line-height gap
# while still catching gross rendering regressions.
Xvfb "$display" -screen 0 {q(args.geometry)} -dpi 96 >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} -dpi 96 >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
trap 'kill "$xvfb_pid" "$compat_xvfb_pid" >/dev/null 2>&1 || true' EXIT

# Wait for each Xvfb to accept connections rather than sleeping a fixed
# second; a loaded runner can take longer and race the first launch.
{WAIT_FOR_DISPLAY_SH}
wait_for_display "$display" "$xvfb_pid"
wait_for_display "$compat_display" "$compat_xvfb_pid"

system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"
echo "xwpe differential: diffing File-Manager startup" >&2

(
    set -e
    export DISPLAY="$display"
    capture_xwpe system-startup \\
        "$system_build/source/xwpe" \\
        "$fm_fixture" \\
        xwpe-startup-differential \\
        "" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    capture_xwpe compat-startup \\
        "$repo/build/xwpe/source/xwpe" \\
        "$fm_fixture" \\
        xwpe-startup-differential \\
        "$repo/build:$repo/build/xwpe/lib-aliases" \\
        "$compat_logs" \\
        "$compat_screens" \\
        internal
) >"$compat_cap_log" 2>&1 &
compat_cap_pid=$!

system_cap_status=0
wait "$system_cap_pid" || system_cap_status=$?
compat_cap_status=0
wait "$compat_cap_pid" || compat_cap_status=$?

# Stage Xvfb + replay logs so the artifact upload picks them up regardless
# of capture success.
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


def main():
    parser = argparse.ArgumentParser(
        description=(
            "Build xwpe on a Linux SSH host against system libX11 and "
            "libx11-compat, capture the File-Manager startup, and compare output."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("XWPE_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > XWPE_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-xwpe-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("XWPE_DIFF_DISPLAY", "114"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("XWPE_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("XWPE_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--screenshot-region",
        default=parse_env_default("XWPE_DIFF_SCREENSHOT_REGION", "0,0,660,780"),
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
        default=parse_env_bool("XWPE_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote. Used by the GitHub Actions "
            "differential workflow."
        ),
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("XWPE_DIFF_MAE_THRESHOLD", "0.16")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("XWPE_DIFF_CHANGED_THRESHOLD", "0.42")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("XWPE_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("XWPE_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
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
        env_remote_root = os.environ.get("XWPE_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-xwpe-differential"

    if args.compare_location is None:
        env_compare_location = os.environ.get("XWPE_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error("XWPE_DIFF_COMPARE_LOCATION must be 'remote' or 'local'")
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
