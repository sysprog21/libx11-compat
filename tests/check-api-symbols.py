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


def exported_public_symbols(library: Path) -> set[str]:
    symbols: set[str] = set()
    for line in run_nm(library).splitlines():
        fields = line.split()
        if len(fields) < 2 or " U " in f" {line} ":
            continue

        symbol = fields[-1]
        if sys.platform == "darwin" and symbol.startswith("_"):
            symbol = symbol[1:]

        if PUBLIC_API_RE.match(symbol):
            symbols.add(symbol)

    return symbols


def manifest_symbols(path: Path) -> set[str]:
    symbols: set[str] = set()
    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if not PUBLIC_API_RE.match(line):
            raise SystemExit(f"{path}:{line_number}: not a public X11 API: {line}")
        symbols.add(line)
    return symbols


def print_group(title: str, symbols: set[str]) -> None:
    print(title)
    for symbol in sorted(symbols):
        print(f"  {symbol}")


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: check-api-symbols.py <libX11-compat.so> <api-symbols.txt>",
            file=sys.stderr,
        )
        return 2

    library = Path(sys.argv[1])
    manifest = Path(sys.argv[2])
    exported = _glx_filtered(exported_public_symbols(library))
    covered = _glx_filtered(manifest_symbols(manifest))

    missing = exported - covered
    stale = covered - exported
    if missing or stale:
        if missing:
            print_group("Missing API coverage manifest entries:", missing)
        if stale:
            print_group("Stale API coverage manifest entries:", stale)
        return 1

    print(f"API symbol coverage complete: {len(exported)} public exports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
