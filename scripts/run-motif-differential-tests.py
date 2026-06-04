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
        libice-dev libsm-dev libx11-dev libxext-dev libxmu-dev libxt-dev
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
system_build="$remote_root/system-build"
system_out="$remote_root/system-out"
system_screens="$remote_root/screens/system"
compat_screens="$remote_root/screens/compat"
system_logs="$remote_root/logs/system"
compat_logs="$remote_root/logs/compat"
system_build_log="$remote_root/logs/system-build.log"
system_config_log="$remote_root/logs/system-configure.log"
display=:{q(args.display)}

run_logged() {{
    log=$1
    shift
    # Capture $? inside the else-branch: after `fi` the exit status of
    # the if-statement is 0 when no branch ran (POSIX), which would mask
    # the original failure if we read $? on the line below `fi`.
    if "$@" >>"$log" 2>&1; then
        return 0
    else
        status=$?
        echo "FAIL $*; see $log" >&2
        tail -40 "$log" >&2 || true
        exit "$status"
    fi
}}

{clean_remote}
mkdir -p "$system_build" "$system_out" "$system_screens" "$compat_screens" \\
    "$system_logs" "$compat_logs"
: >"$system_build_log"

cd "$repo"
make -j{q(args.jobs)} CC=gcc motif-demos

motif_src="$repo/build/upstream/motif-src"
cd "$system_build"
if [ ! -f .configure-stamp ]; then
    : >"$system_config_log"
    run_logged "$system_config_log" env \\
        CPP="gcc -E" \\
        CFLAGS="-g -O0" \\
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
run_logged "$system_build_log" make -C config
run_logged "$system_build_log" make -C lib/Xm CFLAGS="-g -O0"
run_logged "$system_build_log" make -C lib/Mrm CFLAGS="-g -O0"
run_logged "$system_build_log" make -C tools/wml CPP="gcc -E" CFLAGS="-g -O0"
run_logged "$system_build_log" make -C clients/uil CPP="gcc -E" CFLAGS="-g -O0"
run_logged "$system_build_log" make -C demos CPP="gcc -E" CFLAGS="-g -O0"

rm -f "/tmp/.X{q(args.display)}-lock"
Xvfb "$display" -screen 0 {q(args.geometry)} \\
    >"$remote_root/xvfb.log" 2>&1 &
xvfb_pid=$!
trap 'kill "$xvfb_pid" >/dev/null 2>&1 || true' EXIT
sleep 1

export DISPLAY="$display"
export MOTIF_SCREENSHOT_COMMAND=import
export MOTIF_DEMO_SCREENSHOT_SECONDS={q(args.seconds)}
export MOTIF_DEMO_SOURCE_DIR="$motif_src"
{filter_export}

export MOTIF_DEMO_SCREENSHOT_DIR="$system_screens"
export MOTIF_DEMO_SCREENSHOT_LOG_DIR="$system_logs"
sh "$repo/scripts/capture-motif-demo-screenshots.sh" "$system_build" "$system_out"

export MOTIF_DEMO_SCREENSHOT_DIR="$compat_screens"
export MOTIF_DEMO_SCREENSHOT_LOG_DIR="$compat_logs"
sh "$repo/scripts/capture-motif-demo-screenshots.sh" \\
    "$repo/build/motif-demos" "$repo/build"
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
        default=parse_env_default(
            "MOTIF_DIFF_REMOTE_ROOT",
            "/tmp/libx11-compat-motif-differential",
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
        "--mae-threshold",
        type=float,
        default=float(parse_env_default("MOTIF_DIFF_MAE_THRESHOLD", "0.08")),
    )
    parser.add_argument(
        "--changed-threshold",
        type=float,
        default=float(parse_env_default("MOTIF_DIFF_CHANGED_THRESHOLD", "0.35")),
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
        default=parse_env_default("MOTIF_DIFF_COMPARE_LOCATION", "remote"),
        help=(
            "where to run screenshot image comparison; Motif execution always "
            "runs on the remote"
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
