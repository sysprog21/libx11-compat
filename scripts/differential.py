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
