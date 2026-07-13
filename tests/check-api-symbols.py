#!/usr/bin/env python3
"""Verify that the test suite accounts for every exported public X11 API."""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

PUBLIC_API_RE = re.compile(
    r"^(X[A-Z]|XXorRegion|Xkb|Xcms|Xrm|Xmb|Xutf8|Xwc|XShm|XShape|"
    r"XSync|XRender|XFixes|XDamage|XRR|glX)"
)

# libx11-compat exports a small set of non-Xlib shim hooks (all prefixed
# libx11Compat) that clients such as the xwpe live-resize harness resolve at
# runtime. They are not part of the Xlib ABI but are still an exported surface,
# so track them in a dedicated manifest to catch accidental additions/removals.
SHIM_API_RE = re.compile(r"^libx11Compat")

# GLX is an optional build (GLX=0 in the makefiles compiles src/glx.c out). When
# it is disabled the glX* symbols are neither exported nor expected, so drop them
# from both sides of the comparison.
GLX_ENABLED = os.environ.get("LIBX11_COMPAT_GLX", "1") != "0"


def _glx_filtered(symbols: set[str]) -> set[str]:
    if GLX_ENABLED:
        return symbols
    return {s for s in symbols if not s.startswith("glX")}


def run_nm(library: Path) -> str:
    commands = (("nm", "-D", "-g", str(library)), ("nm", "-g", str(library)))
    last_error: Exception | None = None
    for command in commands:
        try:
            return subprocess.check_output(
                command, text=True, stderr=subprocess.DEVNULL
            )
        except (OSError, subprocess.CalledProcessError) as error:
            last_error = error
    raise SystemExit(f"failed to inspect exported symbols: {last_error}")


def exported_symbols(library: Path, pattern: re.Pattern[str]) -> set[str]:
    symbols: set[str] = set()
    for line in run_nm(library).splitlines():
        fields = line.split()
        if len(fields) < 2 or " U " in f" {line} ":
            continue

        symbol = fields[-1]
        if sys.platform == "darwin" and symbol.startswith("_"):
            symbol = symbol[1:]

        if pattern.match(symbol):
            symbols.add(symbol)

    return symbols


def manifest_symbols(path: Path, pattern: re.Pattern[str], kind: str) -> set[str]:
    symbols: set[str] = set()
    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if not pattern.match(line):
            raise SystemExit(f"{path}:{line_number}: not a {kind}: {line}")
        symbols.add(line)
    return symbols


def print_group(title: str, symbols: set[str]) -> None:
    print(title)
    for symbol in sorted(symbols):
        print(f"  {symbol}")


def _diff_manifest(exported: set[str], covered: set[str], label: str) -> bool:
    missing = exported - covered
    stale = covered - exported
    if missing:
        print_group(f"Missing {label} coverage manifest entries:", missing)
    if stale:
        print_group(f"Stale {label} coverage manifest entries:", stale)
    return bool(missing or stale)


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: check-api-symbols.py <libX11-compat.so> <api-symbols.txt>",
            file=sys.stderr,
        )
        return 2

    library = Path(sys.argv[1])
    manifest = Path(sys.argv[2])
    exported = _glx_filtered(exported_symbols(library, PUBLIC_API_RE))
    covered = _glx_filtered(manifest_symbols(manifest, PUBLIC_API_RE, "public X11 API"))

    failed = _diff_manifest(exported, covered, "API")

    # The libx11Compat* shim hooks are only exported on builds that compile the
    # live-resize/present layer (macOS). When none are present (e.g. a Linux CI
    # build), the shim manifest is expected to be empty and the check is a no-op.
    shim_exported = exported_symbols(library, SHIM_API_RE)
    shim_manifest_path = manifest.with_name("shim-symbols.txt")
    shim_covered: set[str] = set()
    if shim_manifest_path.exists():
        shim_covered = manifest_symbols(
            shim_manifest_path, SHIM_API_RE, "libx11Compat shim symbol"
        )
    if shim_exported or shim_covered:
        if _diff_manifest(shim_exported, shim_covered, "shim"):
            failed = True

    if failed:
        return 1

    print(
        f"API symbol coverage complete: {len(exported)} public exports"
        + (f", {len(shim_exported)} shim exports" if shim_exported else "")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
