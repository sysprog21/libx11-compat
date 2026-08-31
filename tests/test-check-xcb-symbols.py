#!/usr/bin/env python3
"""Unit tests for scripts/check-xcb-symbols.py."""

from __future__ import annotations

import importlib.util
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "check-xcb-symbols.py"


def load_checker():
    spec = importlib.util.spec_from_file_location("check_xcb_symbols", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


def main() -> None:
    checker = load_checker()
    with tempfile.TemporaryDirectory() as temporary:
        manifest = Path(temporary) / "symbols.txt"
        manifest.write_text("# comment\nxcb_connect\nxcb_flush\n")
        assert checker.manifest_symbols(manifest) == {"xcb_connect", "xcb_flush"}

        manifest.write_text("xcb_connect\nxcb_connect\n")
        try:
            checker.manifest_symbols(manifest)
        except SystemExit as error:
            assert "duplicate symbol" in str(error)
        else:
            raise AssertionError("duplicate manifest symbol was accepted")

        manifest.write_text("XOpenDisplay\n")
        try:
            checker.manifest_symbols(manifest)
        except SystemExit as error:
            assert "invalid XCB symbol" in str(error)
        else:
            raise AssertionError("non-XCB manifest symbol was accepted")

    print("test-check-xcb-symbols: ok")


if __name__ == "__main__":
    main()
