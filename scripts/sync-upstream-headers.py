#!/usr/bin/env python3
"""Sync Xorg headers and selected libX11 sources from pinned upstream tarballs.

The library aims to keep ``include/X11/`` small and to compile libX11
internals directly from upstream rather than carrying local forks. Both
header and source files are fetched at build time and staged under a build
directory so client compiles still resolve ``<X11/...>`` includes and the
Makefile can compile the upstream translation units the library reuses.

Subcommands:

``fetch DEST [--force]``
    Download (cached) tarballs and extract headers into ``DEST/X11/`` and
    the whitelisted libX11 ``src/`` files into ``DEST/../src/``. Idempotent
    through ``DEST/.upstream-stamp``.

``diff``
    Compare local ``include/X11/`` to upstream and classify each path as
    match, differ, only-local, or only-upstream.

``prune [--dry-run]``
    Delete tracked local headers that match upstream byte for byte; leave
    the modified subset in place.

``info``
    Print pinned versions and source URLs.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
LOCAL_INCLUDE = ROOT / "include" / "X11"
CACHE_DIR = ROOT / "build" / "upstream" / ".cache"
DEFAULT_DEST = ROOT / "build" / "upstream" / "include"
STAMP_NAME = ".upstream-stamp"

# Pinned upstream releases. Bumping a version requires updating the matching
# sha256; the script refuses to use a tarball that fails verification.
# libX11 is listed first so it wins if a path ever overlaps with xorgproto.
SOURCES = [
    {
        "name": "libX11",
        "version": "libX11-1.8.9",
        "url": "https://www.x.org/releases/individual/lib/libX11-1.8.9.tar.xz",
        "sha256": "779d8f111d144ef93e2daa5f23a762ce9555affc99592844e71c4243d3bd3262",
    },
    {
        "name": "xorgproto",
        "version": "xorgproto-2025.1",
        "url": "https://www.x.org/releases/individual/proto/xorgproto-2025.1.tar.xz",
        "sha256": "56898c716c0578df8a2d828c9c3e5c528277705c0484381a81960fe1a67668e8",
    },
]

# Build-system noise that we never want to extract.
SKIP_BASENAMES = {"meson.build", "Makefile.am", "Makefile.in", "Makefile"}
SKIP_SUFFIXES = (".in",)

# libX11 source-tree files compiled directly from the pinned tarball. Keyed
# by tarball name so we only pull from libX11 and not from xorgproto. Paths
# are tarball-relative beneath ``<topdir>/src/`` and are written to
# ``<stage_root>/src/<basename>``.
SRC_WHITELIST = {
    "libX11": frozenset(
        {
            "Context.c",
            "ParseGeom.c",
            "Quarks.c",
            "SetWMProto.c",
            "Xprivate.h",
            "Xresinternal.h",
            "locking.c",
            "locking.h",
            "reallocarray.c",
            "reallocarray.h",
        }
    ),
}

# Minimal text patches applied to staged src/ files. Each entry maps a
# basename to (original, replacement) tuples; the script refuses to write
# the file if `original` no longer matches, which surfaces upstream changes
# the next time the pinned tarball moves.
SRC_PATCHES: dict[str, tuple[tuple[str, str], ...]] = {
    "locking.c": (
        (
            '#include "Xlibint.h"\n',
            '#include "Xlibint.h"\n#include "events.h"\n',
        ),
        # Anchor on a line that only appears inside XInitThreads so the
        # injection cannot drift to another function with a similar tail.
        # captureMainEventThreadIfUnset() is the SDL event-loop hook the
        # local fork used to call from this site.
        (
            "    _Xthread_self_fn = _Xthread_self;\n",
            "    _Xthread_self_fn = _Xthread_self;\n"
            "    captureMainEventThreadIfUnset();\n",
        ),
        # XFreeThreads upstream only releases mutex memory; it leaves
        # _Xglobal_lock and the function-pointer hooks pointing at freed
        # storage, so a subsequent XInitThreads early-returns and any
        # later LockDisplay path dispatches through stale pointers. Clear
        # the globals defined here (the *_DisplayLock_fn pair lives in
        # src/display.c and is reset there if it ever needs to be).
        (
            "    if (conv_lock.lock != NULL) {\n"
            "\txmutex_free(conv_lock.lock);\n"
            "\tconv_lock.lock = NULL;\n"
            "    }\n"
            "\n"
            "    return 1;\n"
            "}\n",
            "    if (conv_lock.lock != NULL) {\n"
            "\txmutex_free(conv_lock.lock);\n"
            "\tconv_lock.lock = NULL;\n"
            "    }\n"
            "\n"
            "    _Xglobal_lock = NULL;\n"
            "    _Xi18n_lock = NULL;\n"
            "    _conv_lock = NULL;\n"
            "    _XCreateMutex_fn = NULL;\n"
            "    _XFreeMutex_fn = NULL;\n"
            "    _XLockMutex_fn = NULL;\n"
            "    _XUnlockMutex_fn = NULL;\n"
            "    _Xthread_self_fn = NULL;\n"
            "\n"
            "    return 1;\n"
            "}\n",
        ),
    ),
    # Anything that includes Xlibint.h before locking.h already has
    # xmalloc/xfree from Xthreads.h. The redefinition is intentional, so
    # quietly clear the prior definitions instead of relying on a global
    # -Wno-macro-redefined.
    "locking.h": (
        (
            "#define xmalloc(s) Xmalloc(s)\n#define xfree(s) Xfree(s)\n",
            "#undef xmalloc\n#undef xfree\n"
            "#define xmalloc(s) Xmalloc(s)\n#define xfree(s) Xfree(s)\n",
        ),
    ),
}

# Guard against typos when bumping versions: every URL must contain its tag.
for _src in SOURCES:
    if _src["version"] not in _src["url"]:
        raise SystemExit(
            f"SOURCES[{_src['name']!r}] version/url drift: "
            f"version={_src['version']!r} not in url={_src['url']!r}"
        )


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, expected_sha: str) -> Path:
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    dest = CACHE_DIR / url.rsplit("/", 1)[-1]
    if dest.exists():
        try:
            cached_sha = sha256_of(dest)
        except FileNotFoundError:
            cached_sha = None
        if cached_sha == expected_sha:
            return dest
        if cached_sha is not None:
            print(
                f"  WARN    cached {dest.name} has unexpected sha256; redownloading",
                file=sys.stderr,
            )
            dest.unlink(missing_ok=True)
    print(f"  FETCH   {url}", file=sys.stderr)
    tmp_fd, tmp_name = tempfile.mkstemp(dir=CACHE_DIR, prefix=".dl-")
    os.close(tmp_fd)
    tmp_path = Path(tmp_name)
    try:
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                with tmp_path.open("wb") as handle:
                    shutil.copyfileobj(response, handle)
        except urllib.error.URLError as exc:
            raise SystemExit(
                f"failed to download {url}: {exc}\n"
                f"  pre-seed the cache at {CACHE_DIR}/{dest.name} "
                f"to build offline"
            ) from exc
        actual = sha256_of(tmp_path)
        if actual != expected_sha:
            raise SystemExit(
                f"sha256 mismatch for {url}\n"
                f"  expected {expected_sha}\n"
                f"  got      {actual}"
            )
        os.replace(tmp_path, dest)
    finally:
        if tmp_path.exists():
            tmp_path.unlink()
    return dest


def _is_safe(parts: tuple[str, ...]) -> bool:
    return bool(parts) and not any(part in ("", "..") for part in parts)


def relevant_member(member: tarfile.TarInfo) -> str | None:
    """Return the X11/-relative path if ``member`` is a header we want.

    Rejects absolute paths and ``..`` segments so a malicious tar cannot
    escape the staging directory through the ``include/X11/`` prefix check.
    """
    if not member.isreg():
        return None
    posix = PurePosixPath(member.name)
    if posix.is_absolute():
        return None
    parts = posix.parts
    if not _is_safe(parts):
        return None
    # Expected layout: <topdir>/include/X11/...
    if len(parts) < 4 or parts[1] != "include" or parts[2] != "X11":
        return None
    rel = PurePosixPath(*parts[2:]).as_posix()
    base = parts[-1]
    if base in SKIP_BASENAMES:
        return None
    if base.endswith(SKIP_SUFFIXES):
        return None
    return rel


def relevant_src_member(
    member: tarfile.TarInfo, whitelist: frozenset[str]
) -> str | None:
    """Return the basename if ``member`` is a whitelisted libX11 src/ file."""
    if not member.isreg() or not whitelist:
        return None
    posix = PurePosixPath(member.name)
    if posix.is_absolute():
        return None
    parts = posix.parts
    if not _is_safe(parts):
        return None
    # Expected layout: <topdir>/src/<basename> (we only take direct children
    # of src/ to avoid pulling in nested helper directories like src/xcms/).
    if len(parts) != 3 or parts[1] != "src":
        return None
    base = parts[-1]
    if base not in whitelist:
        return None
    return base


def upstream_index() -> dict[str, tuple[str, bytes]]:
    """Return ``{rel_path: (source_name, content)}`` merged across sources."""
    index: dict[str, tuple[str, bytes]] = {}
    for source in SOURCES:
        tarball = download(source["url"], source["sha256"])
        with tarfile.open(tarball, "r:xz") as tar:
            for member in tar:
                rel = relevant_member(member)
                if rel is None:
                    continue
                if rel in index:
                    continue
                fh = tar.extractfile(member)
                if fh is None:
                    continue
                index[rel] = (source["name"], fh.read())
    return index


def upstream_src_index() -> dict[str, tuple[str, bytes]]:
    """Return ``{basename: (source_name, content)}`` for whitelisted src files.

    Iterates per-source so a file's whitelist entry only matches inside the
    tarball it was registered under (currently libX11 only).
    """
    index: dict[str, tuple[str, bytes]] = {}
    for source in SOURCES:
        whitelist = SRC_WHITELIST.get(source["name"], frozenset())
        if not whitelist:
            continue
        tarball = download(source["url"], source["sha256"])
        with tarfile.open(tarball, "r:xz") as tar:
            for member in tar:
                rel = relevant_src_member(member, whitelist)
                if rel is None or rel in index:
                    continue
                fh = tar.extractfile(member)
                if fh is None:
                    continue
                index[rel] = (source["name"], fh.read())
        missing = whitelist - set(index)
        if missing:
            raise SystemExit(
                f"{source['name']}: whitelisted src/ files missing from tarball: "
                + ", ".join(sorted(missing))
            )
    return index


def _apply_patch(rel: str, content: bytes) -> bytes:
    edits = SRC_PATCHES.get(rel)
    if not edits:
        return content
    text = content.decode("utf-8")
    for original, replacement in edits:
        if original not in text:
            raise SystemExit(
                f"src patch for {rel!r}: anchor not found, upstream may have "
                f"diverged. Adjust SRC_PATCHES."
            )
        text = text.replace(original, replacement, 1)
    return text.encode("utf-8")


def stamp_token() -> str:
    lines = [
        "stamp-format=4",
        f"sync-script-sha256={sha256_of(Path(__file__).resolve())}",
    ]
    lines.extend(f"{src['name']}={src['version']}#{src['sha256']}" for src in SOURCES)
    for src_name, basenames in sorted(SRC_WHITELIST.items()):
        lines.append(f"{src_name}-src={','.join(sorted(basenames))}")
    return "\n".join(lines) + "\n"


def _validate_dest(dest: Path) -> Path:
    """Refuse to fetch into the source tree outside ``build/``.

    Prevents ``fetch include`` from wiping the tracked public header tree
    and other in-tree footguns. Out-of-tree destinations (e.g. ``/tmp/x``)
    are allowed for ad-hoc testing.
    """
    resolved = dest.resolve()
    try:
        rel = resolved.relative_to(ROOT)
    except ValueError:
        return resolved
    if not rel.parts or rel.parts[0] != "build":
        raise SystemExit(
            f"refusing to fetch into {resolved}: in-tree destinations must "
            f"live under {ROOT / 'build'}"
        )
    return resolved


def cmd_fetch(args: argparse.Namespace) -> int:
    dest = _validate_dest(Path(args.dest))
    src_dest = _validate_dest(dest.parent / "src")
    stamp = dest / STAMP_NAME
    token = stamp_token()
    if not args.force and stamp.exists() and stamp.read_text() == token:
        # Touch so the stamp's mtime catches up to a freshly edited script;
        # otherwise Make would re-run this recipe on every invocation.
        os.utime(stamp, None)
        return 0
    x11_root = dest / "X11"
    if x11_root.exists():
        shutil.rmtree(x11_root)
    if src_dest.exists():
        shutil.rmtree(src_dest)
    if stamp.exists():
        stamp.unlink()
    dest.mkdir(parents=True, exist_ok=True)
    index = upstream_index()
    for rel, (_source, content) in index.items():
        out_path = dest / rel
        # Defense in depth: even if relevant_member missed a traversal
        # vector, the resolved write path must stay inside dest.
        try:
            out_path.resolve().relative_to(dest)
        except ValueError:
            raise SystemExit(f"refusing unsafe tar path: {rel}")
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(content)
    src_index = upstream_src_index()
    if src_index:
        src_dest.mkdir(parents=True, exist_ok=True)
        for rel, (_source, content) in src_index.items():
            out_path = src_dest / rel
            try:
                out_path.resolve().relative_to(src_dest)
            except ValueError:
                raise SystemExit(f"refusing unsafe tar path: src/{rel}")
            out_path.write_bytes(_apply_patch(rel, content))
    stamp.write_text(token)
    print(
        f"  STAGE   {len(index)} upstream header(s) -> {dest}",
        file=sys.stderr,
    )
    if src_index:
        print(
            f"  STAGE   {len(src_index)} upstream source(s) -> {src_dest}",
            file=sys.stderr,
        )
    return 0


def _local_relative_paths() -> set[str]:
    if not LOCAL_INCLUDE.exists():
        return set()
    return {
        path.relative_to(LOCAL_INCLUDE.parent).as_posix()
        for path in LOCAL_INCLUDE.rglob("*")
        if path.is_file()
    }


def cmd_diff(args: argparse.Namespace) -> int:
    index = upstream_index()
    upstream_paths = set(index)
    local_paths = _local_relative_paths()

    match: list[str] = []
    differ: list[str] = []
    only_local = sorted(local_paths - upstream_paths)
    only_upstream = sorted(upstream_paths - local_paths)
    for rel in sorted(upstream_paths & local_paths):
        local_bytes = (LOCAL_INCLUDE.parent / rel).read_bytes()
        if local_bytes == index[rel][1]:
            match.append(rel)
        else:
            differ.append(rel)

    sections = [
        ("match (could prune)", match),
        ("differ (kept locally)", differ),
        ("only-local (no upstream copy)", only_local),
        ("only-upstream (added by fetch)", only_upstream),
    ]
    for header, paths in sections:
        print(f"== {header} [{len(paths)}] ==")
        for rel in paths:
            print(f"  {rel}")
        print()
    return 0


def cmd_prune(args: argparse.Namespace) -> int:
    index = upstream_index()
    deleted: list[str] = []
    for rel, (_source, upstream_content) in index.items():
        local_path = LOCAL_INCLUDE.parent / rel
        if not local_path.is_file():
            continue
        if local_path.read_bytes() != upstream_content:
            continue
        deleted.append(rel)
        if not args.dry_run:
            local_path.unlink()
    verb = "would delete" if args.dry_run else "deleted"
    print(f"{verb} {len(deleted)} local header(s) identical to upstream:")
    for rel in sorted(deleted):
        print(f"  {rel}")
    if not args.dry_run:
        for path in sorted(LOCAL_INCLUDE.rglob("*"), reverse=True):
            if path.is_dir() and not any(path.iterdir()):
                path.rmdir()
    return 0


def cmd_info(args: argparse.Namespace) -> int:
    for source in SOURCES:
        print(f"{source['name']:<10} {source['version']}")
        print(f"  url     {source['url']}")
        print(f"  sha256  {source['sha256']}")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_fetch = sub.add_parser("fetch", help="download and extract upstream headers")
    p_fetch.add_argument("dest", nargs="?", default=str(DEFAULT_DEST))
    p_fetch.add_argument(
        "--force", action="store_true", help="ignore the stamp and re-extract"
    )
    p_fetch.set_defaults(func=cmd_fetch)

    p_diff = sub.add_parser("diff", help="compare local include/X11/ to upstream")
    p_diff.set_defaults(func=cmd_diff)

    p_prune = sub.add_parser("prune", help="delete local headers identical to upstream")
    p_prune.add_argument(
        "--dry-run", action="store_true", help="list deletions without applying them"
    )
    p_prune.set_defaults(func=cmd_prune)

    p_info = sub.add_parser("info", help="print pinned versions")
    p_info.set_defaults(func=cmd_info)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
