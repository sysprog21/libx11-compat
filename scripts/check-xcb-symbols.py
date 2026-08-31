#!/usr/bin/env python3
"""Check libxcb-compat's exported ABI against an exact symbol manifest."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

XCB_API_RE = re.compile(r"^xcb_[a-z0-9_]+$")


def exported_symbols(library: Path) -> set[str]:
    commands = (("nm", "-D", "-g", str(library)), ("nm", "-g", str(library)))
    output = None
    for command in commands:
        try:
            output = subprocess.check_output(
                command, text=True, stderr=subprocess.DEVNULL
            )
            break
        except (OSError, subprocess.CalledProcessError):
            continue
    if output is None:
        raise SystemExit(f"cannot inspect exported symbols in {library}")

    symbols = set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 2 or " U " in f" {line} ":
            continue
        symbol = (
            fields[-1].removeprefix("_") if sys.platform == "darwin" else fields[-1]
        )
        if XCB_API_RE.fullmatch(symbol):
            symbols.add(symbol)
    return symbols


def manifest_symbols(path: Path) -> set[str]:
    symbols = set()
    for line_number, raw_line in enumerate(path.read_text().splitlines(), 1):
        symbol = raw_line.strip()
        if not symbol or symbol.startswith("#"):
            continue
        if not XCB_API_RE.fullmatch(symbol):
            raise SystemExit(f"{path}:{line_number}: invalid XCB symbol: {symbol}")
        if symbol in symbols:
            raise SystemExit(f"{path}:{line_number}: duplicate symbol: {symbol}")
        symbols.add(symbol)
    return symbols


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: check-xcb-symbols.py <library> <manifest>", file=sys.stderr)
        return 2
    exported = exported_symbols(Path(sys.argv[1]))
    expected = manifest_symbols(Path(sys.argv[2]))
    missing = expected - exported
    unexpected = exported - expected
    for label, symbols in (("missing", missing), ("unexpected", unexpected)):
        if symbols:
            print(f"{label} XCB symbols:")
            for symbol in sorted(symbols):
                print(f"  {symbol}")
    if missing or unexpected:
        return 1
    print(f"XCB symbol coverage complete: {len(exported)} exports")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
