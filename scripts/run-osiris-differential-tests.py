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
DEFAULT_OUT_ROOT = ROOT / "build" / "osiris-differential"
DEFAULT_REFERENCE_DIR = ROOT / "build" / "osiris-reference-screens"


def run(cmd, *, cwd=ROOT, input_text=None):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, cwd=cwd, input=input_text, text=True, check=True)


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


def check_local_paths(out_root, remote_root, reference_dir=None):
    """Reject --remote-root values that fetch_results would delete.

    fetch_results() rmtrees out_root/{system,compat,logs,diff} (and
    the optional reference_dir) before rsyncing from
    remote_root/{screens/system,screens/compat,logs,diff}. If
    remote_root equals out_root, equals reference_dir, or lives
    inside one of those subdirectories, the rmtree wipes the staging
    tree before rsync can read from it.
    """
    out_root = Path(out_root).resolve()
    remote_root = Path(remote_root).resolve()

    if remote_root == out_root:
        raise ValueError(
            "--remote-root cannot equal --out-root in local mode; "
            "fetch_results would delete out_root/logs and out_root/diff "
            "before rsync."
        )

    forbidden = [out_root / name for name in ("system", "compat", "logs", "diff")]
    if reference_dir is not None:
        ref = Path(reference_dir).resolve()
        if ref == remote_root:
            raise ValueError(
                "--remote-root cannot equal --reference-dir in local mode; "
                "fetch_results would delete reference_dir before rsync."
            )
        forbidden.append(ref)

    for dest in forbidden:
        try:
            remote_root.relative_to(dest)
        except ValueError:
            continue
        raise ValueError(
            f"--remote-root {remote_root} lives inside fetch destination "
            f"{dest}; fetch_results would delete the staging tree before "
            f"rsync. Pick a remote_root outside out_root/{{system,compat,"
            f"logs,diff}} and outside --reference-dir."
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
    clean_remote = ""
    if args.clean:
        clean_remote = (
            f"rm -rf {q(args.remote_root + '/system-build')} "
            f"{q(args.remote_root + '/system-out')} "
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
        build-essential ca-certificates git imagemagick meson ninja-build \\
        pkg-config python3 python3-pil rsync xauth xvfb xdotool \\
        libfreetype-dev libice-dev libjpeg-dev libpng-dev libsm-dev \\
        libx11-dev libxext-dev libxmu-dev zlib1g-dev
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
need git
need import
need make
need meson
need ninja
need pkg-config
need python3
need rsync
need Xvfb
need xdotool

remote_root={q(args.remote_root)}
repo={q(remote_repo)}
system_build="$remote_root/system-build"
system_out="$remote_root/system-out"
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

capture_osiris() {{
    name=$1
    app=$2
    workdir=$3
    replay=$4
    geometry_arg=$5
    libpath=$6
    log_dir=$7
    screen_dir=$8
    input_backend=$9
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
        --name "osiris-$name" \\
        --app "$app" \\
        --app-arg=-geometry --app-arg="$geometry_arg" \\
        --workdir "$workdir" \\
        --replay "$repo/tests/ui/replays/$replay" \\
        --out-root "$replay_out" \\
        --display "$display_num" \\
        --geometry {q(args.geometry)} \\
        --input-backend "$input_backend" \\
        --screenshot-command import \\
        --screenshot-region {q(args.screenshot_region)} \\
        --env DISPLAY="$DISPLAY" \\
        --env HOME="$remote_root/home-$name" \\
        --env LD_LIBRARY_PATH="$libpath${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}" \\
        --env LIBX11_COMPAT_FONT_DIR="$repo/fonts"
    cp "$replay_out"/screens/*.png "$screen_dir"/
    if [ -f "$log_dir/results.tsv" ]; then
        tail -n +2 "$replay_out/results.tsv" >>"$log_dir/results.tsv"
    else
        cp "$replay_out/results.tsv" "$log_dir/results.tsv"
    fi
    cp "$replay_out"/junit.xml "$log_dir/junit.xml"
    cp "$replay_out"/logs/* "$log_dir"/ 2>/dev/null || true
}}

{clean_remote}
rm -rf "$remote_root/screens" "$remote_root/logs" "$remote_root/diff" \\
    "$remote_root/report.tsv" "$remote_root/junit.xml" "$remote_root"/replay-*
mkdir -p "$system_build" "$system_out" "$system_logs" "$compat_logs" \\
    "$system_screens" "$compat_screens" "$remote_root/logs"

# Wrap gcc with ccache so the system-side Osiris meson build and the
# compat-side osiris build both hit the ccache populated by the
# GitHub Actions cache action. Meson bakes $CC into its build.ninja
# at configure time, so set it before `meson setup`.
if command -v ccache >/dev/null 2>&1; then
    if [ -d /usr/lib/ccache ]; then
        export PATH="/usr/lib/ccache:$PATH"
    fi
    export CCACHE_DIR="${{CCACHE_DIR:-$HOME/.cache/ccache}}"
fi
cc_wrapped="gcc"
cxx_wrapped="g++"

osiris_src="$repo/build/upstream/osiris"

# Pre-extract upstream Osiris so the parallel compat-side and
# system-side builds below don't race on the source-stamp step. The
# make rule is a no-op when the actions/cache step already restored
# the osiris-src cache.
(cd "$repo" && make build/upstream/osiris/.source-stamp)

# Run compat-side and system-side builds concurrently. They write into
# disjoint trees ($repo/build/osiris vs $system_build) and share only
# the read-only upstream source. ccache is process-safe via its own
# locking. Pin ninja's parallelism on both sides; its default
# auto-detect would otherwise launch $vCPU+2 per side and oversubscribe
# the runner when the two builds run concurrently.
compat_make_log="$remote_root/logs/compat-make.log"
: >"$compat_make_log"
(
    set -e
    cd "$repo"
    make -j{q(args.jobs)} CC="$cc_wrapped" CXX="$cxx_wrapped" \\
        OSIRIS_NINJA="ninja -j{q(args.jobs)}" osiris
) >"$compat_make_log" 2>&1 &
compat_pid=$!

(
    set -e
    if [ ! -f "$system_build/.configure-stamp" ]; then
        rm -rf "$system_build"
        mkdir -p "$system_build"
        run_logged "$remote_root/logs/system-configure.log" \\
            env CC="$cc_wrapped" CXX="$cxx_wrapped" \\
            meson setup "$system_build" "$osiris_src" \\
                --prefix="$system_out/install" \\
                -Dxft=disabled \\
                -Dopengl=disabled \\
                -Dsm=disabled \\
                -Dmng=disabled \\
                -Dgif=disabled \\
                -Dexamples=enabled \\
                -Dtutorial=enabled
        touch "$system_build/.configure-stamp"
    fi
    run_logged "$remote_root/logs/system-build.log" ninja -j{q(args.jobs)} -C "$system_build"
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

check_target() {{
    test -x "$1" || {{
        echo "missing Osiris representative target: $1" >&2
        exit 1
    }}
}}

system_application="$system_build/examples/application/application"
system_listviews="$system_build/examples/listviews/listviews"
system_fileiconview="$system_build/examples/fileiconview/fileiconview"
system_designer="$system_build/designer"
compat_application="$repo/build/osiris/build/examples/application/application"
compat_listviews="$repo/build/osiris/build/examples/listviews/listviews"
compat_fileiconview="$repo/build/osiris/build/examples/fileiconview/fileiconview"
compat_designer="$repo/build/osiris/build/designer"
for target in \\
    "$system_application" "$system_listviews" "$system_fileiconview" \\
    "$system_designer" "$compat_application" \\
    "$compat_listviews" "$compat_fileiconview" \\
    "$compat_designer"; do
    check_target "$target"
done

rm -f "/tmp/.X{q(args.display)}-lock" "/tmp/.X{compat_display_num}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-system.log" 2>&1 &
xvfb_pid=$!
Xvfb "$compat_display" -screen 0 {q(args.geometry)} >"$remote_root/xvfb-compat.log" 2>&1 &
compat_xvfb_pid=$!
trap 'kill "$xvfb_pid" "$compat_xvfb_pid" >/dev/null 2>&1 || true' EXIT
sleep 1

# Osiris has 4 demos per side and each capture runs a replay so the
# capture phase dominates the job. Run system-side and compat-side
# captures concurrently on separate Xvfb instances.
system_cap_log="$remote_root/logs/system-capture.log"
compat_cap_log="$remote_root/logs/compat-capture.log"
: >"$system_cap_log"
: >"$compat_cap_log"

(
    set -e
    export DISPLAY="$display"
    capture_osiris system-application \\
        "$system_application" \\
        "$osiris_src/examples/application" \\
        osiris-application.replay \\
        520x680+0+0 \\
        "$system_build" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
    capture_osiris system-listviews \\
        "$system_listviews" \\
        "$osiris_src/examples/listviews" \\
        osiris-listviews.replay \\
        720x540+0+0 \\
        "$system_build" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
    capture_osiris system-fileiconview \\
        "$system_fileiconview" \\
        "$osiris_src/examples/fileiconview" \\
        osiris-fileiconview.replay \\
        760x560+0+0 \\
        "$system_build" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
    capture_osiris system-designer \\
        "$system_designer" \\
        "$osiris_src/tools/designer/designer" \\
        osiris-designer.replay \\
        940x740+0+0 \\
        "$system_build" \\
        "$system_logs" \\
        "$system_screens" \\
        xdotool
) >"$system_cap_log" 2>&1 &
system_cap_pid=$!

(
    set -e
    export DISPLAY="$compat_display"
    capture_osiris compat-application \\
        "$compat_application" \\
        "$osiris_src/examples/application" \\
        osiris-application.replay \\
        520x680+0+0 \\
        "$repo/build/osiris/build:$repo/build" \\
        "$compat_logs" \\
        "$compat_screens" \\
        internal
    capture_osiris compat-listviews \\
        "$compat_listviews" \\
        "$osiris_src/examples/listviews" \\
        osiris-listviews.replay \\
        720x540+0+0 \\
        "$repo/build/osiris/build:$repo/build" \\
        "$compat_logs" \\
        "$compat_screens" \\
        internal
    capture_osiris compat-fileiconview \\
        "$compat_fileiconview" \\
        "$osiris_src/examples/fileiconview" \\
        osiris-fileiconview.replay \\
        760x560+0+0 \\
        "$repo/build/osiris/build:$repo/build" \\
        "$compat_logs" \\
        "$compat_screens" \\
        internal
    capture_osiris compat-designer \\
        "$compat_designer" \\
        "$osiris_src/tools/designer/designer" \\
        osiris-designer.replay \\
        940x740+0+0 \\
        "$repo/build/osiris/build:$repo/build" \\
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

    if args.reference_dir:
        reference_dir = args.reference_dir
        if reference_dir.exists():
            shutil.rmtree(reference_dir)
        reference_dir.mkdir(parents=True)
        rsync(
            remote_uri(args, f"{args.remote_root}/screens/system/"),
            reference_dir,
        )

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
            "Build Osiris on a Linux SSH host against system libX11 and "
            "libx11-compat, capture representative Qt2 screenshots, and "
            "compare output."
        )
    )
    parser.add_argument(
        "--remote",
        default=parse_env_default("OSIRIS_DIFF_REMOTE", "node11"),
    )
    parser.add_argument(
        "--remote-root",
        default=None,
        help=(
            "staging directory. Precedence: CLI flag > OSIRIS_DIFF_REMOTE_ROOT "
            "env > local-mode default (out_root/_work) > SSH default "
            "(/tmp/libx11-compat-osiris-differential)."
        ),
    )
    parser.add_argument(
        "--display",
        default=parse_env_default("OSIRIS_DIFF_DISPLAY", "124"),
    )
    parser.add_argument(
        "--geometry",
        default=parse_env_default("OSIRIS_DIFF_GEOMETRY", "1280x1024x24"),
    )
    parser.add_argument(
        "--jobs",
        default=parse_env_default("OSIRIS_DIFF_JOBS", os.environ.get("JOBS", "1")),
    )
    parser.add_argument(
        "--screenshot-region",
        default=parse_env_default("OSIRIS_DIFF_SCREENSHOT_REGION", "0,0,360,180"),
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
        default=parse_env_bool("OSIRIS_DIFF_LOCAL"),
        help=(
            "run the build / capture / compare pipeline on the local host "
            "instead of SSHing to --remote. Used by the GitHub Actions "
            "differential workflow."
        ),
    )
    parser.add_argument(
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("OSIRIS_DIFF_MAE_THRESHOLD", "0.16")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("OSIRIS_DIFF_CHANGED_THRESHOLD", "0.42")),
    )
    parser.add_argument(
        "--top",
        type=int,
        default=int(parse_env_default("OSIRIS_DIFF_TOP", "12")),
    )
    parser.add_argument(
        "--out-root",
        type=Path,
        default=Path(parse_env_default("OSIRIS_DIFF_OUT_ROOT", DEFAULT_OUT_ROOT)),
        help="local artifact directory for synced screenshots, diffs, TSV, and JUnit",
    )
    parser.add_argument(
        "--reference-dir",
        type=Path,
        default=Path(
            parse_env_default("OSIRIS_REFERENCE_SCREEN_DIR", DEFAULT_REFERENCE_DIR)
        ),
        help="local directory for native Osiris reference screenshots",
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
    # the OSIRIS_DIFF_REMOTE_ROOT env var, then the local-mode default
    # (out_root/_work) or the SSH default.
    if args.remote_root is None:
        env_remote_root = os.environ.get("OSIRIS_DIFF_REMOTE_ROOT")
        if env_remote_root:
            args.remote_root = env_remote_root
        elif args.local:
            args.remote_root = str(args.out_root / "_work")
        else:
            args.remote_root = "/tmp/libx11-compat-osiris-differential"

    # Resolve --compare-location precedence: explicit CLI flag wins,
    # then the OSIRIS_DIFF_COMPARE_LOCATION env var, then the local-mode
    # default (local) or the SSH default (remote).
    if args.compare_location is None:
        env_compare_location = os.environ.get("OSIRIS_DIFF_COMPARE_LOCATION")
        if env_compare_location:
            if env_compare_location not in ("remote", "local"):
                parser.error(
                    "OSIRIS_DIFF_COMPARE_LOCATION must be 'remote' or 'local'"
                )
            args.compare_location = env_compare_location
        elif args.local:
            args.compare_location = "local"
        else:
            args.compare_location = "remote"

    if args.local:
        try:
            check_local_paths(
                args.out_root,
                Path(args.remote_root),
                reference_dir=args.reference_dir,
            )
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
