"""Shared plumbing for the run-*-differential-tests.py drivers.

Every downstream workload (Motif, Mosaic, Osiris, Xfig, GIMP, XNEdit, XEphem,
ViolaWWW, XCircuit) drives the same double-build-and-compare pipeline: rsync
the tree to a remote (or stage it locally), run a build/capture shell payload
over SSH, then pull the screenshots back and diff them. Only the per-workload
build steps, remote scripts, and assertions differ; the transport and staging
helpers below are identical. Keeping them in one place means a fix to the SSH
or rsync path lands for every driver at once instead of nine times.

The two functions that used to diverge between drivers take an optional
argument so each caller keeps its exact prior behavior:
  - sync_repo(..., extra_excludes=[...]) for drivers that skip /.git//externals
  - check_local_paths(..., reference_dir=...) for drivers with a reference set
  - fetch_results(args) exports args.reference_dir when a driver defines one
"""

import os
import shlex
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


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
    """Run a build/capture/compare shell payload locally or via SSH.

    Forward the caller's DIFF_CC into the payload so the compiler override
    reaches the generated script in both local and SSH runs; a bare
    `ssh remote sh -s` would otherwise resolve ${DIFF_CC} against the remote
    environment, which does not carry it.
    """
    diff_cc = os.environ.get("DIFF_CC")
    if diff_cc:
        script = f"export DIFF_CC={shlex.quote(diff_cc)}\n" + script
    if args.local:
        run(["sh", "-s"], input_text=script)
    else:
        ssh(args.remote, script)


def remote_uri(args, path):
    """Format a path for rsync; local mode strips the remote: prefix."""
    return str(path) if args.local else f"{args.remote}:{path}"


def q(value):
    return shlex.quote(str(value))


# Xvfb readiness probe shared by the drivers that stand up two servers and
# capture from both. A fixed sleep races the first client launch and the
# xdotool query against a display that is not yet listening, surfacing as a
# spurious capture timeout; probe with xdotool (already a hard dependency in
# the capture package set, unlike xdpyinfo) and fail fast if an Xvfb died on a
# stale display lock. Injected into a driver f-string remote script via
# {WAIT_FOR_DISPLAY_SH}, so it carries single braces rather than the doubled
# f-string form.
WAIT_FOR_DISPLAY_SH = """wait_for_display() {
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
}"""

# Modern clang (16+) and gcc (14+) promote the C99-removal diagnostics to
# errors by default. Pre-C99 application sources (mosaic's libwww2, xcircuit's
# Xw widgets, xwpe) only compile once these are silenced. Use the plain -Wno-
# (disable) spelling, not -Wno-error=: gcc hard-errors on an unknown
# -Wno-error=<name> but silently ignores an unknown -Wno-<name>, and the
# leading -Wno-unknown-warning-option keeps clang quiet about names it does not
# know. That lets one flag string cover gcc-13, gcc-14 and clang without
# branching on the compiler (e.g. return-mismatch is gcc-14/clang-only,
# deprecated-non-prototype is clang-only).
LEGACY_CC_FLAGS = (
    "-Wno-unknown-warning-option "
    "-Wno-implicit-int -Wno-implicit-function-declaration "
    "-Wno-return-type -Wno-return-mismatch -Wno-int-conversion "
    "-Wno-incompatible-pointer-types -Wno-deprecated-non-prototype"
)


def compiler_setup_sh(*, legacy=False, source_date_epoch=False):
    """Shell that resolves the app-build C compiler into $cc_wrapped.

    Emitted verbatim into a runner's build script (single braces, like
    WAIT_FOR_DISPLAY_SH). DIFF_CC overrides the default gcc so node11 docker
    runs build with clang; ccache is fronted via PATH when present.

    legacy=True wraps the compiler in $remote_root/bin/cc-legacy so a modern
    clang's C99-removal errors drop to warnings and pre-C99 sources still build
    (codegen unchanged). This keeps ancient apps buildable under a bare
    `make check-differential-<app> DIFF_CC=clang`, not just via the docker
    image's own wrapper. source_date_epoch=True supplies a valid
    SOURCE_DATE_EPOCH for builds (e.g. xwpe) that derive one from `git log` in a
    git-less source copy, which clang rejects when empty. Requires $remote_root.
    """
    lines = [
        "# Resolve the app-build compiler ($cc_wrapped); DIFF_CC overrides gcc.",
        "# ccache fronts it via PATH to reuse the CI object cache.",
        "if command -v ccache >/dev/null 2>&1; then",
        "    if [ -d /usr/lib/ccache ]; then",
        '        export PATH="/usr/lib/ccache:$PATH"',
        "    fi",
        '    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache}"',
        "fi",
    ]
    if source_date_epoch:
        lines.append('export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-1704067200}"')
    if legacy:
        # Quoted heredoc: the wrapper body is written verbatim and resolves
        # DIFF_CC at run time (it is exported through the whole make chain), so
        # a DIFF_CC value with shell-special characters cannot be expanded or
        # executed while the wrapper is being generated.
        lines += [
            'mkdir -p "$remote_root/bin"',
            "cat > \"$remote_root/bin/cc-legacy\" <<'LEGACYEOF'",
            "#!/bin/sh",
            f'exec "${{DIFF_CC:-gcc}}" {LEGACY_CC_FLAGS} "$@"',
            "LEGACYEOF",
            'chmod +x "$remote_root/bin/cc-legacy"',
            'cc_wrapped="$remote_root/bin/cc-legacy"',
        ]
    else:
        lines.append('cc_wrapped="${DIFF_CC:-gcc}"')
    return "\n".join(lines)


# Shell helper: abort with a clear message when a required command is missing.
# Emitted verbatim into a runner's build script (single braces).
NEED_SH = """need() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "missing required command: $1" >&2
        exit 127
    }
}"""


def run_logged_sh(tail_lines=60):
    """Shell helper `run_logged LOG CMD...` that appends a command's output to
    LOG and, on failure, prints the last tail_lines of LOG and exits with the
    command's status. Emitted verbatim into a runner's build script."""
    return f"""run_logged() {{
    log=$1
    shift
    if "$@" >>"$log" 2>&1; then
        return 0
    else
        # Capture $? inside the else branch: after `fi` the if-statement's own
        # status is 0 when no branch ran (POSIX), which would mask the failure
        # if read on the line below.
        status=$?
        echo "FAIL $*; see $log" >&2
        tail -{tail_lines} "$log" >&2 || true
        exit "$status"
    fi
}}"""


def compare(args, system_dir, compat_dir, out_root, *, extra_args=None):
    """Run compare-motif-reference.py over captured system vs compat screenshots.

    Shared by every runner; extra_args injects per-app flags (motif passes the
    --allow-diff ceilings) ahead of --top.
    """
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
        *(extra_args or []),
        "--top",
        str(args.top),
    ]
    run(cmd)


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

    fetch_results() rmtrees out_root/{system,compat,logs,diff} (and the
    optional reference_dir) before rsyncing from remote_root/{screens/system,
    screens/compat,logs,diff}. If remote_root equals out_root, equals
    reference_dir, or lives inside one of those subdirectories, the rmtree
    wipes the staging tree before rsync can read from it.
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


def sync_repo(args, *, extra_excludes=None):
    if args.local:
        Path(args.remote_root).mkdir(parents=True, exist_ok=True)
        return str(ROOT)
    remote_repo = f"{args.remote_root}/repo"
    run(["ssh", args.remote, "mkdir", "-p", args.remote_root])
    excludes = ["--exclude", "/build/"]
    for pattern in extra_excludes or ():
        excludes.extend(["--exclude", pattern])
    rsync("./", f"{args.remote}:{remote_repo}/", extra_args=excludes)
    upstream_cache = ROOT / "build" / "upstream" / ".cache"
    if upstream_cache.exists():
        run(["ssh", args.remote, "mkdir", "-p", f"{remote_repo}/build/upstream/.cache"])
        rsync(
            f"{upstream_cache}/", f"{args.remote}:{remote_repo}/build/upstream/.cache/"
        )
    return remote_repo


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
    reference_dir = getattr(args, "reference_dir", None)
    if reference_dir is not None:
        reference_dir = Path(reference_dir)
        if reference_dir.exists():
            shutil.rmtree(reference_dir)
        reference_dir.mkdir(parents=True)
        rsync(remote_uri(args, f"{args.remote_root}/screens/system/"), reference_dir)
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
