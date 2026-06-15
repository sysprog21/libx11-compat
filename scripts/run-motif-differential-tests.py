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
REPRESENTATIVE_FILTER = (
    r"programs/(ColorSel/colordemo|Ext18List/ext18list|TabStack/tabstack|"
    r"draw/draw|fileview/fileview|i18ninput/i18ninput|workspace/wsm)"
)
DEFAULT_OUT_ROOT = ROOT / "build" / "motif-differential"


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


def execute(args, script):
    """Run a build/capture/compare shell payload locally or via SSH."""
    if args.local:
        run(["sh", "-s"], input_text=script)
    else:
        ssh(args.remote, script)


def remote_uri(args, path):
    """Format a path for rsync; local mode strips the remote: prefix."""
    return str(path) if args.local else f"{args.remote}:{path}"


def q(value):
    return shlex.quote(str(value))


def effective_filter(args):
    if args.filter:
        return args.filter
    if args.mode == "representative":
        return args.representative_filter
    return None


def parse_env_default(name, default):
    value = os.environ.get(name)
    if value is None or value == "":
        return default
    return value


def parse_env_bool(name, default=False):
    value = os.environ.get(name)
    if value is None or value == "":
        return default
    return value.lower() in ("1", "yes", "true", "on")


def check_local_paths(out_root, remote_root):
    """Reject --remote-root values that fetch_results would delete.

    fetch_results() rmtrees out_root/{system,compat,logs,diff} before
    rsyncing from remote_root/{screens/system,screens/compat,logs,diff}.
    If remote_root equals out_root or lives inside one of those four
    subdirectories, the rmtree wipes the staging tree before rsync can
    read from it.
    """
    out_root = Path(out_root).resolve()
    remote_root = Path(remote_root).resolve()

    if remote_root == out_root:
        raise ValueError(
            "--remote-root cannot equal --out-root in local mode; "
            "fetch_results would delete out_root/logs and out_root/diff "
            "before rsync."
        )

    for name in ("system", "compat", "logs", "diff"):
        dest = out_root / name
        try:
            remote_root.relative_to(dest)
        except ValueError:
            continue
        raise ValueError(
            f"--remote-root {remote_root} lives inside fetch destination "
            f"{dest}; fetch_results would delete the staging tree before "
            f"rsync. Pick a remote_root outside out_root/{{system,compat,"
            f"logs,diff}}."
        )


def sync_repo(args):
    if args.local:
        Path(args.remote_root).mkdir(parents=True, exist_ok=True)
        return str(ROOT)
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
    filter_export = ""
    if effective_filter(args):
        filter_export = (
            "export MOTIF_DEMO_SCREENSHOT_FILTER=" f"{q(effective_filter(args))}\n"
        )

    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(args.remote_root + '/system-build')} "
            f"{q(args.remote_root + '/system-out')} "
            f"{q(args.remote_root + '/screens')}\n"
        )

    install_deps = ""
    if args.install_deps:
        install_deps = """
if command -v apt-get >/dev/null 2>&1; then
    export DEBIAN_FRONTEND=noninteractive
    sudo apt-get update
    sudo apt-get install -y --no-install-recommends \\
        autoconf automake bison build-essential ca-certificates git \\
        imagemagick libtool make pkg-config rsync xauth xvfb \\
        xdotool libice-dev libsm-dev libx11-dev libxext-dev libxmu-dev libxt-dev
fi
"""

    replay_deps = "need xdotool\n" if args.replay_smoke else ""

    # The parallel screenshot capture runs system-side and compat-side
    # demos concurrently on separate Xvfb instances. Pick a numeric
    # display offset by one so the two X servers do not collide on lock
    # files or socket paths.
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
{replay_deps}if command -v yacc >/dev/null 2>&1; then
    yacc_bin=yacc
elif command -v bison >/dev/null 2>&1; then
    yacc_bin="bison -y"
else
    echo "missing required command: yacc or bison" >&2
    exit 127
fi

remote_root={q(args.remote_root)}
repo={q(remote_repo)}
system_build="$remote_root/system-build"
system_out="$remote_root/system-out"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_build_log="$remote_root/logs/system-build.log"
system_config_log="$remote_root/logs/system-configure.log"
display=:{q(args.display)}
compat_display=:{compat_display_num}

run_logged() {{
    log=$1
    shift
    # Capture $? inside the else-branch: after fi the exit status of
    # the if-statement is 0 when no branch ran (POSIX), which would mask
    # the original failure if we read $? on the line below fi.
    if "$@" >>"$log" 2>&1; then
        return 0
    else
        status=$?
        echo "FAIL $*; see $log" >&2
        tail -60 "$log" >&2 || true
        exit "$status"
    fi
}}

write_wsm_home() {{
    home_dir=$1
    mkdir -p "$home_dir"
    cat >"$home_dir/.wsmdb" <<'WSMEOF'
wsm_WSM.WSM.0.linked:True
wsm_WSM.WSM.0.allWorkspaces:True
wsm_WSM.WSM.0.linkedRoom.hidden:0
saveAsShell_WSM*allWorkspaces:True
configureShell_WSM*allWorkspaces:True
nameShell_WSM*allWorkspaces: True
backgroundShell_WSM*allWorkspaces:True
deleteShell_WSM*allWorkspaces:True
occupyShell_WSM*allWorkspaces:True
WSMEOF
}}

run_motif_replay() {{
    name=$1
    replay=$2
    app=$3
    workdir=$4
    libpath=$5
    log_dir=$6
    screen_dir=$7
    home_dir=$8
    input_backend=$9
    replay_out="$remote_root/replay-$name"
    rm -rf "$replay_out"
    mkdir -p "$log_dir" "$screen_dir"
    python3 "$repo/scripts/run-ui-replay.py" \\
        --name "$name" \\
        --app "$app" \\
        --workdir "$workdir" \\
        --replay "$repo/tests/ui/replays/$replay" \\
        --out-root "$replay_out" \\
        --display {q(args.display)} \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        --env DISPLAY="$display" \\
        --env HOME="$home_dir" \\
        --env LD_LIBRARY_PATH="$libpath${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}" \\
        --env XAPPLRESDIR="$(dirname "$app")" \\
        --env XFILESEARCHPATH="$(dirname "$app")/%N.ad:$(dirname "$app")/%N:$motif_src/demos/$(basename "$(dirname "$app")")/%N.ad:$motif_src/demos/$(basename "$(dirname "$app")")/%N"
    # Replay assertions are enforced by run-ui-replay itself. Keep their
    # screenshots out of the differential screenshot directories so these
    # smoke interactions do not require symmetric reference/local frames.
    cp "$replay_out"/junit.xml "$log_dir/$name-junit.xml"
    if [ -f "$log_dir/results.tsv" ]; then
        tail -n +2 "$replay_out/results.tsv" >>"$log_dir/results.tsv"
    else
        cp "$replay_out/results.tsv" "$log_dir/results.tsv"
    fi
    cp "$replay_out"/logs/* "$log_dir"/ 2>/dev/null || true
}}

{clean_remote}
rm -rf "$remote_root/screens" "$remote_root/logs" "$remote_root/diff" \\
    "$remote_root/report.tsv" "$remote_root/junit.xml" "$remote_root"/replay-*
mkdir -p "$system_build" "$system_out" "$system_screens" "$compat_screens" \\
    "$system_logs" "$compat_logs"
: >"$system_build_log"

# Wrap gcc with ccache so the system-side Motif build and the compat
# motif-demos build both hit the ccache populated by the GitHub
# Actions cache action. Bare `CC=gcc` would skip the cache and
# recompile cold every CI run.
if command -v ccache >/dev/null 2>&1; then
    if [ -d /usr/lib/ccache ]; then
        export PATH="/usr/lib/ccache:$PATH"
    fi
    export CCACHE_DIR="${{CCACHE_DIR:-$HOME/.cache/ccache}}"
fi
cc_wrapped="gcc"

motif_src="$repo/build/upstream/motif"

# Pre-extract upstream Motif so the parallel compat-side and system-side
# builds below don't race on the source / autoreconf step. The make
# rule is a no-op when the actions/cache step already restored the
# motif-src cache; on a cache miss it clones + autoreconfs once.
(cd "$repo" && make build/upstream/motif/.autogen-stamp)

# Run compat-side and system-side builds concurrently. They write into
# disjoint trees ($repo/build/motif-demos vs $system_build) and share
# only the read-only upstream source. ccache is process-safe via its
# own locking. Per-side stdout/stderr is captured to a dedicated log so
# failure triage stays unambiguous.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" motif-demos
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    cd "$system_build"
    if [ ! -f .configure-stamp ]; then
        : >"$system_config_log"
        run_logged "$system_config_log" env \\
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
            --with-xinerama=no \\
            --enable-demos
        touch .configure-stamp
    fi
    run_logged "$system_build_log" make -j{q(args.jobs)} -C config
    run_logged "$system_build_log" make -j{q(args.jobs)} -C lib/Xm CC="$cc_wrapped" CFLAGS="-g -O0 -include stdlib.h"
    run_logged "$system_build_log" make -j{q(args.jobs)} -C lib/Mrm CC="$cc_wrapped" CFLAGS="-g -O0 -include stdlib.h"
    run_logged "$system_build_log" make -j{q(args.jobs)} -C tools/wml CC="$cc_wrapped" CPP="gcc -E" CFLAGS="-g -O0 -include stdlib.h"
    run_logged "$system_build_log" make -j{q(args.jobs)} -C clients/uil CC="$cc_wrapped" CPP="gcc -E" CFLAGS="-g -O0 -include stdlib.h"
    run_logged "$system_build_log" make -j{q(args.jobs)} -C demos CC="$cc_wrapped" CPP="gcc -E" CFLAGS="-g -O0 -include stdlib.h"
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
    echo "system-side build failed (exit $system_status); see $system_build_log" >&2
    tail -60 "$system_build_log" >&2 || true
fi
[ "$compat_status" -eq 0 ] || exit "$compat_status"
[ "$system_status" -eq 0 ] || exit "$system_status"

rm -f "/tmp/.X{q(args.display)}-lock" "/tmp/.X{compat_display_num}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} \\
    >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} \\
    >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
trap 'kill "$xvfb_pid" "$compat_xvfb_pid" >/dev/null 2>&1 || true' EXIT
sleep 1

export MOTIF_SCREENSHOT_COMMAND=import
export MOTIF_DEMO_SCREENSHOT_SECONDS={q(args.seconds)}
export MOTIF_DEMO_SOURCE_DIR="$motif_src"
{filter_export}

# Capture system-side and compat-side screenshots on separate Xvfb
# instances in parallel. capture-motif-demo-screenshots.sh runs each
# demo serially and waits MOTIF_DEMO_SCREENSHOT_SECONDS per shot, so
# halving its serial run via two Xvfb is the only way to cut its
# wallclock contribution without dropping demos.
system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"

(
    set -e
    export DISPLAY="$display"
    export MOTIF_DEMO_SCREENSHOT_DIR="$system_screens"
    export MOTIF_DEMO_SCREENSHOT_LOG_DIR="$system_logs"
    sh "$repo/scripts/capture-motif-demo-screenshots.sh" "$system_build" "$system_out"
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    export MOTIF_DEMO_SCREENSHOT_DIR="$compat_screens"
    export MOTIF_DEMO_SCREENSHOT_LOG_DIR="$compat_logs"
    sh "$repo/scripts/capture-motif-demo-screenshots.sh" \\
        "$repo/build/motif-demos" "$repo/build"
) >"$compat_cap_log" 2>&1 &
compat_cap_pid=$!

system_cap_status=0
wait "$system_cap_pid" || system_cap_status=$?
compat_cap_status=0
wait "$compat_cap_pid" || compat_cap_status=$?

# Stage Xvfb logs into $remote_root/logs so the artifact upload picks
# them up regardless of capture success, then surface the diagnostics
# for any side that failed.
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

if [ {q("1" if args.replay_smoke else "0")} = 1 ]; then
    export DISPLAY="$display"
    mkdir -p "$remote_root/home-system" "$remote_root/home-compat" \\
        "$remote_root/home-system-wsm" "$remote_root/home-compat-wsm"
    write_wsm_home "$remote_root/home-system-wsm"
    write_wsm_home "$remote_root/home-compat-wsm"
    run_motif_replay \\
        motif-fileview-done \\
        motif-fileview-done.replay \\
        "$system_build/demos/programs/fileview/fileview" \\
        "$system_build/demos/programs/fileview" \\
        "$system_build/lib/Xm/.libs:$system_build/lib/Mrm/.libs:$system_build/clients/uil/.libs" \\
        "$system_logs" \\
        "$system_screens" \\
        "$remote_root/home-system" \\
        xdotool
    run_motif_replay \\
        motif-fileview-done \\
        motif-fileview-done.replay \\
        "$repo/build/motif-demos/demos/programs/fileview/fileview" \\
        "$repo/build/motif-demos/demos/programs/fileview" \\
        "$repo/build/motif-demos/lib/Xm/.libs:$repo/build/motif-demos/lib/Mrm/.libs:$repo/build/motif-demos/clients/uil/.libs:$repo/build" \\
        "$compat_logs" \\
        "$compat_screens" \\
        "$remote_root/home-compat" \\
        internal
    run_motif_replay \\
        motif-wsm-labels \\
        motif-wsm-labels.replay \\
        "$system_build/demos/programs/workspace/wsm" \\
        "$system_build/demos/programs/workspace" \\
        "$system_build/lib/Xm/.libs:$system_build/lib/Mrm/.libs:$system_build/clients/uil/.libs" \\
        "$system_logs" \\
        "$system_screens" \\
        "$remote_root/home-system-wsm" \\
        xdotool
    run_motif_replay \\
        motif-wsm-labels \\
        motif-wsm-labels.replay \\
        "$repo/build/motif-demos/demos/programs/workspace/wsm" \\
        "$repo/build/motif-demos/demos/programs/workspace" \\
        "$repo/build/motif-demos/lib/Xm/.libs:$repo/build/motif-demos/lib/Mrm/.libs:$repo/build/motif-demos/clients/uil/.libs:$repo/build" \\
        "$compat_logs" \\
        "$compat_screens" \\
        "$remote_root/home-compat-wsm" \\
        internal
fi
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

    rsync(remote_uri(args, f"{args.remote_root}/screens/system/"), system_dir)
    rsync(remote_uri(args, f"{args.remote_root}/screens/compat/"), compat_dir)
    rsync(remote_uri(args, f"{args.remote_root}/logs/"), log_dir)
    if fetch_remote_compare:
        rsync(remote_uri(args, f"{args.remote_root}/diff/"), diff_dir)
        rsync(
            remote_uri(args, f"{args.remote_root}/report.tsv"),
            out_root / "report.tsv",
        )
        rsync(
            remote_uri(args, f"{args.remote_root}/junit.xml"),
            out_root / "junit.xml",
        )
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
            "Build thentenaar/motif on a Linux SSH host against system libX11 "
            "and libx11-compat, capture demo screenshots under Xvfb, and "
            "compare the rendered output."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("MOTIF_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > MOTIF_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-motif-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("MOTIF_DIFF_DISPLAY", "119"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("MOTIF_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("MOTIF_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--seconds",
        default=parse_env_default("MOTIF_DIFF_SECONDS", "3"),
    )
    parser.add_argument(
        "--mode",
        choices=("representative", "full"),
        default="representative",
        help="representative checks selected high-signal demos; full compares all demos",
    )
    parser.add_argument(
        "--filter",
        default=parse_env_default("MOTIF_DIFF_FILTER", None),
        help="regex passed to screenshot capture",
    )
    parser.add_argument(
        "--representative-filter",
        default=parse_env_default(
            "MOTIF_DIFF_REPRESENTATIVE_FILTER",
            REPRESENTATIVE_FILTER,
        ),
        help="regex used by --mode representative when --filter is not set",
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
        default=parse_env_bool("MOTIF_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote. Used by the GitHub Actions "
            "differential workflow."
        ),
    )
    # Defaults match mk/motif.mk and scripts/compare-motif-reference.py.
    # The MAE gate tolerates font hinting / AA differences between Xft on
    # system X11 and our SDL_ttf path; the changed-pixel gate stays tighter so
    # missing widgets or wrong-color backgrounds still trip the representative
    # comparison.
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("MOTIF_DIFF_MAE_THRESHOLD", "0.08")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("MOTIF_DIFF_CHANGED_THRESHOLD", "0.20")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("MOTIF_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("MOTIF_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
        help="local artifact directory for synced screenshots, diffs, TSV, and JUnit",
    )
    parser.add_argument(
        "--compare-location",
        choices=("remote", "local"),
        default=None,
        help=(
            "where to run screenshot image comparison; defaults to local "
            "when --local is set, otherwise remote"
        ),
    )
    parser.add_argument(
        "--replay-smoke",
        action="store_true",
        default=parse_env_default("MOTIF_DIFF_REPLAY", "0").lower()
        in ("1", "yes", "true"),
        help=(
            "also run replay-based Motif UI interactions on system-X11 and "
            "libx11-compat; currently includes the fileview Done-button path"
        ),
    )
    args = parser.parse_args()

    if not re.fullmatch(r"\d+", args.display):
        parser.error("--display must be a numeric X display index")
    if not re.fullmatch(r"\d+", str(args.jobs)):
        parser.error("--jobs must be a positive integer")
    if int(args.jobs) <= 0:
        parser.error("--jobs must be a positive integer")
    if args.filter and args.mode == "representative":
        print(
            "--filter overrides --mode representative's default filter",
            file=sys.stderr,
        )

    # Resolve --remote-root precedence: explicit CLI flag wins, then
    # the MOTIF_DIFF_REMOTE_ROOT env var, then the local-mode default
    # (out_root/_work) or the SSH default.
    if args.remote_root is None:
        env_remote_root = os.environ.get("MOTIF_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-motif-differential"

    # Resolve --compare-location precedence: explicit CLI flag wins,
    # then the MOTIF_DIFF_COMPARE_LOCATION env var, then the local-mode
    # default (local) or the SSH default (remote).
    if args.compare_location is None:
        env_compare_location = os.environ.get("MOTIF_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error(
                    "MOTIF_DIFF_COMPARE_LOCATION must be 'remote' or 'local'"
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
            args, fetch_remote_compare=args.compare_location == "remote"
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

    # Precedence: original remote failure first, then compare failure,
    # then fetch failure last so rsync issues don't shadow real bugs.
    if remote_status:
        sys.exit(remote_status)
    if compare_status:
        sys.exit(compare_status)
    if fetch_status:
        sys.exit(fetch_status)


if __name__ == "__main__":
    main()
