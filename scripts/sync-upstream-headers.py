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
    through ``DEST/.upstream-stamp``. For offline builds, set
    ``LIBX11_COMPAT_TARBALL_DIR`` to a directory holding the pinned
    tarballs; entries whose sha256 matches the pin are used instead of
    downloading.

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
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
LOCAL_INCLUDE = ROOT / "include" / "X11"
CACHE_DIR = ROOT / "build" / "upstream" / ".cache"
DEFAULT_DEST = ROOT / "build" / "upstream" / "include"
STAMP_NAME = ".upstream-stamp"
# Records every staged file (relative to build/upstream) so the stamp-current
# fast path can confirm the sources it guards were not cleaned out from under it.
MANIFEST_NAME = ".upstream-manifest"

# Pinned upstream releases. Bumping a version requires updating the matching
# sha256; the script refuses to use a tarball that fails verification.
# libX11 is listed first so it wins if a path ever overlaps with xorgproto.
SOURCES = [
    {
        "name": "libX11",
        "version": "libX11-1.8.13",
        "url": "https://xorg.freedesktop.org/archive/individual/lib/libX11-1.8.13.tar.xz",
        "sha256": "69606f485c2c07c14ef64f75b7bb326d48587af33795d9ab3e607c0b5f94f11c",
    },
    {
        "name": "xorgproto",
        "version": "xorgproto-2025.1",
        "url": "https://xorg.freedesktop.org/archive/individual/proto/xorgproto-2025.1.tar.xz",
        "sha256": "56898c716c0578df8a2d828c9c3e5c528277705c0484381a81960fe1a67668e8",
    },
    {
        "name": "libXt",
        "version": "libXt-1.3.1",
        "url": "https://xorg.freedesktop.org/archive/individual/lib/libXt-1.3.1.tar.xz",
        "sha256": "e0a774b33324f4d4c05b199ea45050f87206586d81655f8bef4dba434d931288",
        # libXt's src/ goes to its own staging dir so it does not collide
        # with the libX11 src/ slice (different headers, different build
        # flags). All .c files in src/ are taken so the Makefile picks them
        # up without a hand-maintained whitelist.
        "src_subdir": "src-libXt",
        "src_take_all": True,
        # makestrs (compiled host-side) and string.list together regenerate
        # StringDefs.c / StringDefs.h at build time. The .ct/.ht templates
        # live alongside string.list and are consumed by makestrs. The
        # nested ``util'' segment matches the path strings hard-coded in
        # string.list (``#ctmpl util/StrDefs.ct'', etc.) so makestrs can
        # find the templates with the topdir-libXt directory as its base.
        "util_files": frozenset(
            {"makestrs.c", "string.list", "StrDefs.ct", "StrDefs.ht", "Shell.ht"}
        ),
        "util_subdir": "topdir-libXt/util",
    },
    {
        "name": "libXpm",
        "version": "libXpm-3.5.19",
        "url": "https://xorg.freedesktop.org/archive/individual/lib/libXpm-3.5.19.tar.xz",
        "sha256": "ad3576d689221a39dc728f0e0dc02ca7bb6a0d724c9a77fd1bfa1e9af83be900",
        "src_subdir": "src-libXpm",
        "src_take_all": True,
    },
    {
        "name": "libXmu",
        "version": "libXmu-1.3.1",
        "url": "https://xorg.freedesktop.org/archive/individual/lib/libXmu-1.3.1.tar.xz",
        "sha256": "81a99e94c4501e81c427cbaa4a11748b584933e94b7a156830c3621256857bc4",
        # Pulled in for clients/mwm and Motif's clients/ tree. The
        # tarball ships its public surface under include/X11/Xmu/ which
        # the existing extractor already routes into the staged X11
        # tree, so the prefix-stub headers we ship in
        # include/X11/Xmu/Editres.h get overwritten by the real ones.
        "src_subdir": "src-libXmu",
        "src_take_all": True,
    },
    {
        "name": "libXaw",
        "version": "libXaw-1.0.16",
        "url": "https://xorg.freedesktop.org/archive/individual/lib/libXaw-1.0.16.tar.xz",
        "sha256": "731d572b54c708f81e197a6afa8016918e2e06dfd3025e066ca642a5b8c39c8f",
        "src_subdir": "src-libXaw",
        "src_take_all": True,
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
    # libXt 1.3.1's SessionSetValues closes its `#ifndef XT_NO_SM` block
    # halfway through the function, then keeps using the cw/nw widget
    # casts declared inside that block. When we build with -DXT_NO_SM the
    # declarations vanish and the tail-end SM_CLIENT_ID property update
    # references undeclared identifiers. Extend the conditional to the
    # function's `return False;` so the whole body becomes a no-op without
    # session management. Anchors carry enough surrounding text (the
    # StopManagingSession call and the strlen line that hands the session
    # id to XChangeProperty) to remain unique within Shell.c.
    "Shell.c": (
        (
            "        StopManagingSession(nw, nw->session.connection);\n"
            "#endif                          /* !XT_NO_SM */\n"
            "\n"
            "    if (cw->wm.client_leader != nw->wm.client_leader ||\n",
            "        StopManagingSession(nw, nw->session.connection);\n"
            "\n"
            "    if (cw->wm.client_leader != nw->wm.client_leader ||\n",
        ),
        (
            "                                (unsigned char *) nw->session.session_id,\n"
            "                                (int) strlen(nw->session.session_id));\n"
            "        }\n"
            "    }\n"
            "    return False;\n"
            "}\n"
            "\n"
            "void\n"
            "_XtShellGetCoordinates(Widget widget, Position *x, Position *y)\n",
            "                                (unsigned char *) nw->session.session_id,\n"
            "                                (int) strlen(nw->session.session_id));\n"
            "        }\n"
            "    }\n"
            "#endif                          /* !XT_NO_SM */\n"
            "    return False;\n"
            "}\n"
            "\n"
            "void\n"
            "_XtShellGetCoordinates(Widget widget, Position *x, Position *y)\n",
        ),
    ),
    # Keep one spare NULL callback record after every internal list. libXt's
    # public XtCallbackList form is NULL-terminated, and Motif resource
    # converters can hand lists produced from these internals back through
    # _XtCompileCallbackList(). Without the sentinel, sanitizer builds read
    # past single-callback lists while creating widgets. Every producer
    # (_XtAddCallback, AddCallbacks, _XtRemoveCallback, XtRemoveCallbacks,
    # _XtCompileCallbackList) must allocate count+1 records and write the
    # NULL terminator, or removal/compile paths will drop the spare slot.
    "Callback.c": (
        (
            "                                   sizeof(XtCallbackRec) * (size_t) (count +\n"
            "                                                                     1)));\n"
            "        (void) memmove((char *) ToList(icl), (char *) ToList(*callbacks),\n",
            "                                   sizeof(XtCallbackRec) * (size_t) (count +\n"
            "                                                                     2)));\n"
            "        (void) memmove((char *) ToList(icl), (char *) ToList(*callbacks),\n",
        ),
        (
            "                                                sizeof(XtCallbackRec) *\n"
            "                                                (size_t) (count + 1)));\n"
            "    }\n"
            "    *callbacks = icl;\n",
            "                                                sizeof(XtCallbackRec) *\n"
            "                                                (size_t) (count + 2)));\n"
            "    }\n"
            "    *callbacks = icl;\n",
        ),
        (
            "    cl = ToList(icl) + count;\n"
            "    cl->callback = callback;\n"
            "    cl->closure = closure;\n"
            "}                               /* _XtAddCallback */\n",
            "    cl = ToList(icl) + count;\n"
            "    cl->callback = callback;\n"
            "    cl->closure = closure;\n"
            "    (++cl)->callback = (XtCallbackProc) NULL;\n"
            "    cl->closure = NULL;\n"
            "}                               /* _XtAddCallback */\n",
        ),
        (
            "                        sizeof(XtCallbackRec) * (size_t) (i + j)));\n"
            "        (void) memmove((char *) ToList(*callbacks), (char *) ToList(icl),\n",
            "                        sizeof(XtCallbackRec) * (size_t) (i + j + 1)));\n"
            "        (void) memmove((char *) ToList(*callbacks), (char *) ToList(icl),\n",
        ),
        (
            "                                                           sizeof(XtCallbackRec)\n"
            "                                                           * (size_t) (i + j)));\n"
            "    }\n"
            "    *callbacks = icl;\n",
            "                                                           sizeof(XtCallbackRec)\n"
            "                                                           * (size_t) (i + j + 1)));\n"
            "    }\n"
            "    *callbacks = icl;\n",
        ),
        (
            "    for (cl = ToList(icl) + i; --j >= 0;)\n"
            "        *cl++ = *newcallbacks++;\n"
            "}                               /* AddCallbacks */\n",
            "    for (cl = ToList(icl) + i; --j >= 0;)\n"
            "        *cl++ = *newcallbacks++;\n"
            "    cl->callback = (XtCallbackProc) NULL;\n"
            "    cl->closure = NULL;\n"
            "}                               /* AddCallbacks */\n",
        ),
        # _XtRemoveCallback (call_state branch): grow allocation by one and
        # write the sentinel after both copy loops finish populating ncl.
        (
            "                    icl = (InternalCallbackList)\n"
            "                        __XtMalloc((Cardinal) (sizeof(InternalCallbackRec) +\n"
            "                                               sizeof(XtCallbackRec) *\n"
            "                                               (size_t) (i + j)));\n"
            "                    icl->count = (unsigned short) (i + j);\n"
            "                    icl->is_padded = 0;\n"
            "                    icl->call_state = 0;\n"
            "                    ncl = ToList(icl);\n"
            "                    while (--j >= 0)\n"
            "                        *ncl++ = *ocl++;\n"
            "                    while (--i >= 0)\n"
            "                        *ncl++ = *++cl;\n"
            "                    *callbacks = icl;\n",
            "                    icl = (InternalCallbackList)\n"
            "                        __XtMalloc((Cardinal) (sizeof(InternalCallbackRec) +\n"
            "                                               sizeof(XtCallbackRec) *\n"
            "                                               (size_t) (i + j + 1)));\n"
            "                    icl->count = (unsigned short) (i + j);\n"
            "                    icl->is_padded = 0;\n"
            "                    icl->call_state = 0;\n"
            "                    ncl = ToList(icl);\n"
            "                    while (--j >= 0)\n"
            "                        *ncl++ = *ocl++;\n"
            "                    while (--i >= 0)\n"
            "                        *ncl++ = *++cl;\n"
            "                    ncl->callback = (XtCallbackProc) NULL;\n"
            "                    ncl->closure = NULL;\n"
            "                    *callbacks = icl;\n",
        ),
        # _XtRemoveCallback (non-call_state branch): grow XtRealloc by one
        # and re-anchor the sentinel at the new tail.
        (
            "                if (--icl->count) {\n"
            "                    ncl = cl + 1;\n"
            "                    while (--i >= 0)\n"
            "                        *cl++ = *ncl++;\n"
            "                    icl = (InternalCallbackList)\n"
            "                        XtRealloc((char *) icl,\n"
            "                                  (Cardinal) (sizeof(InternalCallbackRec)\n"
            "                                              +\n"
            "                                              sizeof(XtCallbackRec) *\n"
            "                                              icl->count));\n"
            "                    icl->is_padded = 0;\n"
            "                    *callbacks = icl;\n",
            "                if (--icl->count) {\n"
            "                    ncl = cl + 1;\n"
            "                    while (--i >= 0)\n"
            "                        *cl++ = *ncl++;\n"
            "                    icl = (InternalCallbackList)\n"
            "                        XtRealloc((char *) icl,\n"
            "                                  (Cardinal) (sizeof(InternalCallbackRec)\n"
            "                                              +\n"
            "                                              sizeof(XtCallbackRec) *\n"
            "                                              (size_t) (icl->count + 1)));\n"
            "                    icl->is_padded = 0;\n"
            "                    cl = ToList(icl) + icl->count;\n"
            "                    cl->callback = (XtCallbackProc) NULL;\n"
            "                    cl->closure = NULL;\n"
            "                    *callbacks = icl;\n",
        ),
        # XtRemoveCallbacks: same idea — grow by one and write sentinel.
        (
            "    if (icl->count) {\n"
            "        icl = (InternalCallbackList)\n"
            "            XtRealloc((char *) icl, (Cardinal) (sizeof(InternalCallbackRec) +\n"
            "                                                sizeof(XtCallbackRec) *\n"
            "                                                icl->count));\n"
            "        icl->is_padded = 0;\n"
            "        *callbacks = icl;\n"
            "    }\n",
            "    if (icl->count) {\n"
            "        icl = (InternalCallbackList)\n"
            "            XtRealloc((char *) icl, (Cardinal) (sizeof(InternalCallbackRec) +\n"
            "                                                sizeof(XtCallbackRec) *\n"
            "                                                (size_t) (icl->count + 1)));\n"
            "        icl->is_padded = 0;\n"
            "        ccl = ToList(icl) + icl->count;\n"
            "        ccl->callback = (XtCallbackProc) NULL;\n"
            "        ccl->closure = NULL;\n"
            "        *callbacks = icl;\n"
            "    }\n",
        ),
        # _XtCompileCallbackList: allocate n+1 records and stamp the
        # terminator after the copy loop.
        (
            "    callbacks =\n"
            "        (InternalCallbackList)\n"
            "        __XtMalloc((Cardinal)\n"
            "                   (sizeof(InternalCallbackRec) +\n"
            "                    sizeof(XtCallbackRec) * (size_t) n));\n"
            "    callbacks->count = (unsigned short) n;\n"
            "    callbacks->is_padded = 0;\n"
            "    callbacks->call_state = 0;\n"
            "    cl = ToList(callbacks);\n"
            "    while (--n >= 0)\n"
            "        *cl++ = *xtcallbacks++;\n"
            "    return (callbacks);\n",
            "    callbacks =\n"
            "        (InternalCallbackList)\n"
            "        __XtMalloc((Cardinal)\n"
            "                   (sizeof(InternalCallbackRec) +\n"
            "                    sizeof(XtCallbackRec) * (size_t) (n + 1)));\n"
            "    callbacks->count = (unsigned short) n;\n"
            "    callbacks->is_padded = 0;\n"
            "    callbacks->call_state = 0;\n"
            "    cl = ToList(callbacks);\n"
            "    while (--n >= 0)\n"
            "        *cl++ = *xtcallbacks++;\n"
            "    cl->callback = (XtCallbackProc) NULL;\n"
            "    cl->closure = NULL;\n"
            "    return (callbacks);\n",
        ),
    ),
    # libx11-compat's ConnectionNumber is a wake pipe, so poll() can return
    # readable after the SDL pump drains a timer byte but produces no X event.
    # Upstream Xt treats that as impossible and restarts the wait without
    # refreshing wt.cur_time, which can starve Xt timers forever under a steady
    # pump-wake cadence. Refresh the clock before the spurious-ready retry.
    "NextEvent.c": (
        (
            "    if (block)\n" "        goto WaitLoop;\n" "    else {\n",
            "    if (block) {\n"
            "        X_GETTIMEOFDAY(&wt.cur_time);\n"
            "        FIXUP_TIMEVAL(wt.cur_time);\n"
            "        goto WaitLoop;\n"
            "    }\n"
            "    else {\n",
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


# The XCB headers are built, not shipped: libxcb's release carries xcb.h and
# xcbext.h by hand but generates xproto.h from xcb-proto's XML through
# c_client.py. Both releases are pinned here and the generator runs at stage
# time, so nothing generated is tracked in the repository.
XCB_SOURCES = {
    "libxcb": {
        "version": "libxcb-1.15",
        "url": (
            "https://xorg.freedesktop.org/archive/individual/lib/" "libxcb-1.15.tar.xz"
        ),
        "sha256": ("cc38744f817cf6814c847e2df37fcb8997357d72fa4bcbc228ae0fe47219a059"),
    },
    "xcb-proto": {
        "version": "xcb-proto-1.15.2",
        "url": (
            "https://xorg.freedesktop.org/archive/individual/proto/"
            "xcb-proto-1.15.2.tar.xz"
        ),
        "sha256": ("7072beb1f680a2fe3f9e535b797c146d22528990c72f63ddb49d2f350a3653ed"),
    },
}

# Handed to c_client.py the way libxcb's own src/Makefile.am does.
XCB_MAN_PAGE = "X Version 11"
XCB_MAN_SUFFIX = "3"


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
    # Offline builds (distro packaging, network-restricted chroots) can drop
    # the pinned tarballs into a directory and point this variable at it; a
    # tarball whose sha256 matches the pin is used instead of the network.
    # The cache under build/ does not survive `make distclean`, so the seed
    # directory should live outside the build tree.
    seed_dir = os.environ.get("LIBX11_COMPAT_TARBALL_DIR")
    if seed_dir:
        candidate = Path(seed_dir) / dest.name
        if candidate.is_file():
            if sha256_of(candidate) == expected_sha:
                print(f"  SEED    {candidate}", file=sys.stderr)
                shutil.copyfile(candidate, dest)
                return dest
            print(
                f"  WARN    seed {candidate} has unexpected sha256; ignoring",
                file=sys.stderr,
            )
    print(f"  FETCH   {url}", file=sys.stderr)
    tmp_fd, tmp_name = tempfile.mkstemp(dir=CACHE_DIR, prefix=".dl-")
    os.close(tmp_fd)
    tmp_path = Path(tmp_name)
    try:
        # xorg.freedesktop.org intermittently times out from CI runners, and a
        # single-attempt fetch turns that transient blip into a hard build
        # failure. Retry a few times with exponential backoff before giving up;
        # a sha256 mismatch is never retried since that is not transient.
        attempts = 4
        last_exc: Exception | None = None
        for attempt in range(1, attempts + 1):
            try:
                with urllib.request.urlopen(url, timeout=120) as response:
                    with tmp_path.open("wb") as handle:
                        shutil.copyfileobj(response, handle)
                last_exc = None
                break
            except (urllib.error.URLError, TimeoutError, ConnectionError) as exc:
                last_exc = exc
                if attempt < attempts:
                    delay = 2**attempt
                    print(
                        f"  RETRY   {url} ({attempt}/{attempts - 1}) after "
                        f"{type(exc).__name__}; sleeping {delay}s",
                        file=sys.stderr,
                    )
                    time.sleep(delay)
        if last_exc is not None:
            raise SystemExit(
                f"failed to download {url} after {attempts} attempts: {last_exc}\n"
                f"  to build offline, place {dest.name} in a directory and\n"
                f"  set LIBX11_COMPAT_TARBALL_DIR to that directory"
            ) from last_exc
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


def _direct_child_basename(member: tarfile.TarInfo, dir_name: str) -> str | None:
    """Return the basename if ``member`` is a regular file at
    ``<topdir>/<dir_name>/<basename>`` (no deeper nesting), else None.

    Rejects absolute paths and ``..`` segments so a malicious tar cannot
    escape the staging directory.
    """
    if not member.isreg():
        return None
    posix = PurePosixPath(member.name)
    if posix.is_absolute():
        return None
    parts = posix.parts
    if not _is_safe(parts):
        return None
    if len(parts) != 3 or parts[1] != dir_name:
        return None
    return parts[-1]


def relevant_src_member(
    member: tarfile.TarInfo,
    whitelist: frozenset[str],
    take_all: bool = False,
) -> str | None:
    """Return the basename if ``member`` is a wanted src/ file.

    Only direct children of src/ are returned, to avoid pulling in nested
    helper directories like src/xcms/. When ``take_all`` is true (libXt),
    every .c child is returned regardless of whitelist contents; otherwise
    the whitelist is consulted in the original libX11 sense.
    """
    base = _direct_child_basename(member, "src")
    if base is None:
        return None
    if take_all:
        return base if base.endswith((".c", ".h")) else None
    if not whitelist or base not in whitelist:
        return None
    return base


def relevant_util_member(
    member: tarfile.TarInfo, whitelist: frozenset[str]
) -> str | None:
    """Return the basename if ``member`` is a wanted util/ file.

    libXt's util/ holds the host-side makestrs generator plus the
    string.list / .ct / .ht templates it consumes. The Makefile compiles
    makestrs and runs it to emit StringDefs.c before the library link.
    """
    if not whitelist:
        return None
    base = _direct_child_basename(member, "util")
    if base is None or base not in whitelist:
        return None
    return base


def _extract_tarball(tarball: Path, dest: Path) -> Path:
    """Unpack tarball under dest and return its single top-level directory."""
    with tarfile.open(tarball, "r:*") as tar:
        members = [m for m in tar.getmembers() if _is_safe(PurePosixPath(m.name).parts)]
        roots = {PurePosixPath(m.name).parts[0] for m in members if m.name != "."}
        if len(roots) != 1:
            raise SystemExit(f"{tarball.name} has no single top-level directory")
        for member in members:
            tar.extract(member, dest, filter="data")
    return dest / roots.pop()


def xcb_index() -> dict[str, tuple[str, bytes]]:
    """Build the public XCB headers and return them keyed by staged path.

    xcb.h and xcbext.h ship in libxcb's source release; xproto.h does not
    exist there and is generated from xcb-proto's XML by libxcb's c_client.py,
    which is what libxcb itself does at build time.
    """
    tarballs = {
        name: download(source["url"], source["sha256"])
        for name, source in XCB_SOURCES.items()
    }
    index: dict[str, tuple[str, bytes]] = {}
    with tempfile.TemporaryDirectory() as work_name:
        work = Path(work_name)
        libxcb = _extract_tarball(tarballs["libxcb"], work / "libxcb")
        proto = _extract_tarball(tarballs["xcb-proto"], work / "xcb-proto")
        for name in ("xcb.h", "xcbext.h"):
            index[f"xcb/{name}"] = (
                XCB_SOURCES["libxcb"]["version"],
                (libxcb / "src" / name).read_bytes(),
            )
        generated = work / "generated"
        generated.mkdir()
        command = [
            sys.executable,
            str(libxcb / "src" / "c_client.py"),
            "-c",
            XCB_SOURCES["libxcb"]["version"].replace("-", " "),
            "-l",
            XCB_MAN_PAGE,
            "-s",
            XCB_MAN_SUFFIX,
            "-p",
            str(proto),
            str(proto / "src" / "xproto.xml"),
        ]
        result = subprocess.run(
            command, cwd=generated, capture_output=True, text=True, check=False
        )
        if result.returncode != 0:
            raise SystemExit(
                "c_client.py failed to generate xproto.h:\n" + result.stderr.strip()
            )
        header = generated / "xproto.h"
        if not header.exists():
            raise SystemExit("c_client.py produced no xproto.h")
        index["xcb/xproto.h"] = (
            XCB_SOURCES["xcb-proto"]["version"],
            header.read_bytes(),
        )
    return index


def upstream_index() -> dict[str, tuple[str, bytes]]:
    """Return ``{rel_path: (source_name, content)}`` merged across sources."""
    index: dict[str, tuple[str, bytes]] = {}
    for source in SOURCES:
        tarball = download(source["url"], source["sha256"])
        with tarfile.open(tarball, "r:*") as tar:
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


def upstream_src_index() -> dict[tuple[str, str], tuple[str, bytes]]:
    """Return ``{(subdir, basename): (source_name, content)}`` for src/ files.

    Iterates per-source so a file's whitelist entry only matches inside the
    tarball it was registered under. libX11 uses the SRC_WHITELIST set;
    libXt sets ``src_take_all`` and pulls every direct .c child of src/.
    Keying by ``(subdir, basename)`` keeps libX11 and libXt staged into
    separate directories under ``$(OUT)/upstream/`` so each library builds
    against its own include-path layering without name collisions.
    """
    index: dict[tuple[str, str], tuple[str, bytes]] = {}
    for source in SOURCES:
        take_all = source.get("src_take_all", False)
        whitelist = SRC_WHITELIST.get(source["name"], frozenset())
        if not take_all and not whitelist:
            continue
        subdir = source.get("src_subdir", "src")
        tarball = download(source["url"], source["sha256"])
        seen: set[str] = set()
        with tarfile.open(tarball, "r:*") as tar:
            for member in tar:
                rel = relevant_src_member(member, whitelist, take_all)
                if rel is None or rel in seen:
                    continue
                fh = tar.extractfile(member)
                if fh is None:
                    continue
                seen.add(rel)
                index[(subdir, rel)] = (source["name"], fh.read())
        if not take_all:
            missing = whitelist - seen
            if missing:
                raise SystemExit(
                    f"{source['name']}: whitelisted src/ files missing "
                    f"from tarball: " + ", ".join(sorted(missing))
                )
    return index


def upstream_util_index() -> dict[tuple[str, str], tuple[str, bytes]]:
    """Return ``{(subdir, basename): (source_name, content)}`` for util/ files.

    Currently only libXt declares util files (makestrs.c and the string
    templates). Other sources skip this step.
    """
    index: dict[tuple[str, str], tuple[str, bytes]] = {}
    for source in SOURCES:
        whitelist = source.get("util_files")
        if not whitelist:
            continue
        subdir = source.get("util_subdir", "util")
        tarball = download(source["url"], source["sha256"])
        seen: set[str] = set()
        with tarfile.open(tarball, "r:*") as tar:
            for member in tar:
                rel = relevant_util_member(member, whitelist)
                if rel is None or rel in seen:
                    continue
                fh = tar.extractfile(member)
                if fh is None:
                    continue
                seen.add(rel)
                index[(subdir, rel)] = (source["name"], fh.read())
        missing = whitelist - seen
        if missing:
            raise SystemExit(
                f"{source['name']}: util/ files missing from tarball: "
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


def stamp_token(with_xcb: bool) -> str:
    lines = [
        "stamp-format=6",
        f"sync-script-sha256={sha256_of(Path(__file__).resolve())}",
    ]
    lines.extend(f"{src['name']}={src['version']}#{src['sha256']}" for src in SOURCES)
    lines.append(f"xcb-staged={int(with_xcb)}")
    if with_xcb:
        lines.extend(
            f"{name}={source['version']}#{source['sha256']}"
            for name, source in sorted(XCB_SOURCES.items())
        )
        lines.append(f"xcb-c-client-args={XCB_MAN_PAGE}#{XCB_MAN_SUFFIX}")
    for src_name, basenames in sorted(SRC_WHITELIST.items()):
        lines.append(f"{src_name}-src={','.join(sorted(basenames))}")
    for src in SOURCES:
        if src.get("src_take_all"):
            lines.append(f"{src['name']}-src=*->{src.get('src_subdir', 'src')}")
        util = src.get("util_files")
        if util:
            lines.append(
                f"{src['name']}-util="
                f"{','.join(sorted(util))}->{src.get('util_subdir', 'util')}"
            )
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


def _collect_staging_subdirs() -> list[str]:
    """All staging subdirs that ``cmd_fetch`` may write under ``dest.parent``."""
    subs = {"src"}
    for source in SOURCES:
        if source.get("src_take_all") or SRC_WHITELIST.get(source["name"]):
            subs.add(source.get("src_subdir", "src"))
        if source.get("util_files"):
            subs.add(source.get("util_subdir", "util"))
    return sorted(subs)


def _stage_indexed(
    staging_root: Path,
    index: dict[tuple[str, str], tuple[str, bytes]],
    patch: bool,
) -> dict[str, int]:
    """Write each ``(subdir, rel) -> content`` entry under ``staging_root``
    and return a per-subdir count. Applies SRC_PATCHES when ``patch`` is
    True; passes content through unchanged otherwise.
    """
    counts: dict[str, int] = {}
    for (subdir, rel), (_source, content) in index.items():
        sub_dir = staging_root / subdir
        sub_dir.mkdir(parents=True, exist_ok=True)
        out_path = sub_dir / rel
        try:
            out_path.resolve().relative_to(sub_dir)
        except ValueError:
            raise SystemExit(f"refusing unsafe tar path: {subdir}/{rel}")
        out_path.write_bytes(_apply_patch(rel, content) if patch else content)
        counts[subdir] = counts.get(subdir, 0) + 1
    return counts


def _restage_subdir(staging_root: Path, subdir: str) -> int:
    """Re-stage exactly one src/ staging subdir, pristine, touching nothing else.

    Used by the per-library patch steps (e.g. libXt) to reset their own source
    tree before re-applying compat patches. The whole-tree ``fetch --force``
    would rmtree the shared build/upstream/include/X11 and build/upstream/src
    trees that first-party objects compile against, which races those compiles
    under parallel make. Scoping the reset to the library's own subdir keeps the
    reset off the shared trees: only that library's objects read this subdir,
    and they are already ordered after the patch stamp.
    """
    known = set(_collect_staging_subdirs())
    if subdir not in known or subdir == "src":
        raise SystemExit(
            f"refusing to restage unknown or shared subdir {subdir!r}; "
            f"known per-library subdirs: {sorted(known - {'src'})}"
        )
    entries = {
        (sub, rel): payload
        for (sub, rel), payload in upstream_src_index().items()
        if sub == subdir
    }
    if not entries:
        raise SystemExit(f"no staged sources found for subdir {subdir!r}")
    sub_dir = staging_root / subdir
    if sub_dir.exists():
        shutil.rmtree(sub_dir)
    counts = _stage_indexed(staging_root, entries, patch=True)
    for staged, count in sorted(counts.items()):
        print(
            f"  STAGE   {count} upstream source(s) -> {staging_root / staged}",
            file=sys.stderr,
        )
    return 0


def _staged_files_present(staging_root: Path, dest: Path) -> bool:
    """True only if every file recorded in the staging manifest still exists.

    The stamp lives under dest/ (build/upstream/include) but guards sources
    staged into sibling dirs under staging_root (build/upstream/src*, util),
    so the stamp can read current while those .c/.h were cleaned or restored
    independently (a partial clean, or a CI cache that saved include/ but not
    src/). Verifying the manifest closes that desync using stat only, with no
    tarball download. A missing manifest (older stamp format) counts as stale.
    """
    manifest = dest / MANIFEST_NAME
    if not manifest.exists():
        return False
    for line in manifest.read_text().splitlines():
        rel = line.strip()
        if rel and not (staging_root / rel).exists():
            return False
    return True


def cmd_fetch(args: argparse.Namespace) -> int:
    dest = _validate_dest(Path(args.dest))
    staging_root = _validate_dest(dest.parent)
    if getattr(args, "subdir", None):
        return _restage_subdir(staging_root, args.subdir)
    stamp = dest / STAMP_NAME
    with_xcb = bool(getattr(args, "with_xcb", False))
    token = stamp_token(with_xcb)
    if (
        not args.force
        and stamp.exists()
        and stamp.read_text() == token
        and _staged_files_present(staging_root, dest)
    ):
        # Touch so the stamp's mtime catches up to a freshly edited script;
        # otherwise Make would re-run this recipe on every invocation.
        os.utime(stamp, None)
        return 0
    for root_name in ("X11", "xcb"):
        root = dest / root_name
        if root.exists():
            shutil.rmtree(root)

    for sub in _collect_staging_subdirs():
        sub_dir = staging_root / sub
        if sub_dir.exists():
            shutil.rmtree(sub_dir)
    if stamp.exists():
        stamp.unlink()
    dest.mkdir(parents=True, exist_ok=True)
    index = upstream_index()
    if with_xcb:
        index.update(xcb_index())
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
    util_index = upstream_util_index()
    src_counts = _stage_indexed(staging_root, src_index, patch=True)
    util_counts = _stage_indexed(staging_root, util_index, patch=False)
    manifest = [(dest / rel).relative_to(staging_root).as_posix() for rel in index]
    manifest += [(Path(sub) / rel).as_posix() for (sub, rel) in src_index]
    manifest += [(Path(sub) / rel).as_posix() for (sub, rel) in util_index]
    (dest / MANIFEST_NAME).write_text("\n".join(sorted(manifest)) + "\n")
    stamp.write_text(token)
    print(
        f"  STAGE   {len(index)} upstream header(s) -> {dest}",
        file=sys.stderr,
    )
    for subdir, count in sorted(src_counts.items()):
        print(
            f"  STAGE   {count} upstream source(s) -> {staging_root / subdir}",
            file=sys.stderr,
        )
    for subdir, count in sorted(util_counts.items()):
        print(
            f"  STAGE   {count} upstream util file(s) -> {staging_root / subdir}",
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
    p_fetch.add_argument(
        "--with-xcb",
        action="store_true",
        help="also build and stage the public xcb/ headers (XCB=1 builds only)",
    )
    p_fetch.add_argument(
        "--subdir",
        help="re-stage only this per-library src subdir (e.g. src-libXt), "
        "pristine, without touching the shared header/src trees or the stamp",
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
