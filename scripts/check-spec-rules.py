#!/usr/bin/env python3
"""Check repository invariants documented in spec.md."""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SKIP_DIRS = {
    ".git",
    "__pycache__",
    "build",
}
TEXT_SUFFIXES = {
    ".c",
    ".h",
    ".md",
    ".mk",
    ".py",
}
AMERICAN_ENGLISH = {
    "behaviour": "behavior",
    "initialise": "initialize",
    "serialise": "serialize",
    "colour": "color",
    "cancelled": "canceled",
    "grey": "gray",
    "dialogue": "dialog",
    "analogue": "analog",
}
PROJECT_TEXT_PATHS = {
    "AGENTS.md",
    "CLAUDE.md",
    "README.md",
    "TODO.md",
}
GENERATED_TABLES = {
    "src/display-macros.c": "scripts/gen-display-macros.py",
    "src/std-colors.h": "scripts/update-std-colors.py",
}


def rel(path: Path) -> str:
    return path.relative_to(ROOT).as_posix()


def iter_files() -> list[Path]:
    files: list[Path] = []
    for dirpath, dirnames, filenames in os.walk(ROOT):
        path = Path(dirpath)
        dirnames[:] = [
            name
            for name in dirnames
            if name not in SKIP_DIRS and rel(path / name) != "externals"
        ]
        for filename in filenames:
            file_path = path / filename
            if file_path.suffix in TEXT_SUFFIXES or filename == "Makefile":
                files.append(file_path)
    return sorted(files)


def iter_tracked_files() -> list[Path]:
    import subprocess

    result = subprocess.run(
        ["git", "ls-files"],
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return [ROOT / line for line in result.stdout.splitlines()]


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="latin-1")


def check_required_paths(errors: list[str]) -> None:
    for path in [
        "src",
        "include/X11",
        "tests",
        "mk",
        "scripts",
        "externals",
        "spec.md",
    ]:
        if not (ROOT / path).exists():
            errors.append(f"missing required project path: {path}")


def check_no_android_mk(errors: list[str]) -> None:
    for path in ROOT.rglob("Android.mk"):
        relative = rel(path)
        if relative.startswith("externals/"):
            continue
        errors.append(f"{relative}: Android.mk is not supported; use the root Makefile")


def check_no_sdl2_gpu(errors: list[str]) -> None:
    forbidden = "SDL2" + "_gpu"
    pattern = re.compile(r"\b(?:SDL2_gpu|sdl2_gpu)\b")
    for path in iter_files():
        relative = rel(path)
        if (
            relative == "scripts/check-spec-rules.py"
            or relative.endswith(".md")
            or relative.startswith("include/X11/")
        ):
            continue
        text = read_text(path)
        if pattern.search(text):
            errors.append(
                f"{relative}: {forbidden} is outside the allowed dependency set"
            )


def check_public_headers_do_not_include_private_src(errors: list[str]) -> None:
    include_pattern = re.compile(r'^\s*#\s*include\s+["<]([^">]+)[">]', re.M)
    for path in sorted((ROOT / "include").rglob("*.h")):
        text = read_text(path)
        for match in include_pattern.finditer(text):
            header = match.group(1)
            if header.startswith("src/") or header.startswith("../src/"):
                errors.append(
                    f"{rel(path)}:{text[:match.start()].count(chr(10)) + 1}: "
                    "public headers must not include private src headers"
                )
            if header.startswith("SDL2/") or header in {"SDL.h", "SDL_ttf.h"}:
                errors.append(
                    f"{rel(path)}:{text[:match.start()].count(chr(10)) + 1}: "
                    "public headers must not expose SDL implementation details"
                )


def check_makefile_is_modular(errors: list[str]) -> None:
    makefile = ROOT / "Makefile"
    if not makefile.exists():
        errors.append("missing root Makefile")
        return

    text = read_text(makefile)
    required_includes = [
        "mk/toolchain.mk",
        "mk/config.mk",
        "mk/sources.mk",
        "mk/upstream-headers.mk",
        "mk/common.mk",
        "mk/library.mk",
        "mk/tests.mk",
        "mk/format.mk",
        "mk/help.mk",
        "mk/deps.mk",
    ]
    for include in required_includes:
        if f"include {include}" not in text:
            errors.append(f"Makefile: missing modular include {include}")
    if "spec.md" in text or "check-spec" in text:
        errors.append("Makefile: spec checks must stay outside the build system")


def check_library_target(errors: list[str]) -> None:
    config = ROOT / "mk/config.mk"
    if not config.exists():
        errors.append("missing mk/config.mk")
        return

    text = read_text(config)
    if not re.search(r"^TARGET\s*\?=\s*\$\(OUT\)/libX11-compat\.so\s*$", text, re.M):
        errors.append("mk/config.mk: TARGET must default to $(OUT)/libX11-compat.so")


def check_mk_fragments_do_not_wire_spec(errors: list[str]) -> None:
    for path in sorted((ROOT / "mk").glob("*.mk")):
        text = read_text(path)
        if "spec.md" in text or "check-spec" in text:
            errors.append(f"{rel(path)}: spec checks must stay outside mk/")


def check_allowed_dependency_surface(errors: list[str]) -> None:
    config = ROOT / "mk/config.mk"
    if not config.exists():
        return

    text = read_text(config)
    for forbidden in ["SDL2_gpu", "sdl2_gpu", "SDL2_GFX", "sdl2_gfx"]:
        if forbidden in text:
            errors.append(f"mk/config.mk: forbidden dependency reference {forbidden}")

    pkg_names = set(re.findall(r"\bpkg-config\b.*?\b([A-Za-z0-9_.+-]+)", text))
    allowed_pkg_names = {"SDL2_ttf", "pixman-1"}
    for pkg_name in sorted(pkg_names - allowed_pkg_names):
        if pkg_name not in {"2"}:
            errors.append(f"mk/config.mk: unexpected pkg-config dependency {pkg_name}")


def check_source_list_shape(errors: list[str]) -> None:
    sources = ROOT / "mk/sources.mk"
    if not sources.exists():
        errors.append("missing mk/sources.mk")
        return

    text = read_text(sources)
    if "$(wildcard src/*.c)" not in text:
        errors.append("mk/sources.mk: missing source pattern $(wildcard src/*.c)")
    if "UPSTREAM_SRC_BASES" not in text:
        errors.append(
            "mk/sources.mk: libX11 internals must come from $(UPSTREAM_SRC_BASES) "
            "(see mk/upstream-headers.mk)"
        )


def check_tests_are_registered(errors: list[str]) -> None:
    tests_mk = ROOT / "mk/tests.mk"
    if not tests_mk.exists():
        errors.append("missing mk/tests.mk")
        return

    text = read_text(tests_mk)
    check_bins_match = re.search(r"^CHECK_BINS\s*:?=\s*(.*)$", text, re.M)
    if not check_bins_match:
        errors.append("mk/tests.mk: missing CHECK_BINS")
        return

    registered = set(re.findall(r"\$\(OUT\)/tests/([A-Za-z0-9_./-]+)", text))
    for test_path in sorted((ROOT / "tests").glob("*.c")):
        test_name = test_path.stem
        if test_name not in registered:
            errors.append(f"{rel(test_path)}: test is not registered in CHECK_BINS")


def check_generated_tables_have_generators(errors: list[str]) -> None:
    for generated, generator in GENERATED_TABLES.items():
        if (ROOT / generated).exists() and not (ROOT / generator).exists():
            errors.append(f"{generated}: missing documented generator {generator}")


def check_no_tracked_generated_output(errors: list[str]) -> None:
    for path in iter_tracked_files():
        relative = rel(path)
        if relative.startswith("build/") or "/__pycache__/" in relative:
            errors.append(f"{relative}: generated output must not be tracked")
        if relative.endswith((".log", ".err")):
            errors.append(f"{relative}: logs must not be tracked")


def check_american_english(errors: list[str]) -> None:
    pattern = re.compile(
        r"\b(" + "|".join(re.escape(term) for term in AMERICAN_ENGLISH) + r")\b",
        re.I,
    )
    paths = [ROOT / path for path in PROJECT_TEXT_PATHS]
    paths.extend((ROOT / "src").rglob("*.[ch]"))
    paths.extend((ROOT / "tests").rglob("*.c"))
    paths.extend((ROOT / "mk").rglob("*.mk"))
    paths.extend((ROOT / "scripts").rglob("*.py"))

    skipped = {
        "src/std-colors.h",
        "scripts/check-spec-rules.py",
    }
    for path in sorted(set(paths)):
        if not path.exists() or rel(path) in skipped:
            continue
        text = read_text(path)
        for line_number, line in enumerate(text.splitlines(), 1):
            match = pattern.search(line)
            if match:
                preferred = AMERICAN_ENGLISH[match.group(1).lower()]
                errors.append(
                    f"{rel(path)}:{line_number}: use American English "
                    f"'{preferred}' instead of '{match.group(1)}'"
                )


def main() -> int:
    errors: list[str] = []
    check_required_paths(errors)
    check_no_android_mk(errors)
    check_no_sdl2_gpu(errors)
    check_public_headers_do_not_include_private_src(errors)
    check_makefile_is_modular(errors)
    check_library_target(errors)
    check_mk_fragments_do_not_wire_spec(errors)
    check_allowed_dependency_surface(errors)
    check_source_list_shape(errors)
    check_tests_are_registered(errors)
    check_generated_tables_have_generators(errors)
    check_no_tracked_generated_output(errors)
    check_american_english(errors)

    if errors:
        print("spec rule check failed:", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print("spec rule check passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
