# Compiler and toolchain detection.

MAKEFLAGS += --no-builtin-rules --no-builtin-variables

UNAME_S := $(shell uname -s)

# GNU make predefines CC=cc. Prefer clang for local builds while preserving
# explicit command-line and environment overrides.
ifeq ($(origin CC),default)
  CC := clang
else
  CC ?= clang
endif

PKG_CONFIG ?= pkg-config
# SDL2_CONFIG and all SDL detection live in mk/sdl.mk.
PYTHON ?= python3
SHFMT ?= shfmt

# clang-format pinned to a single major version. Newer / older releases
# produce different output for the .clang-format style in this tree, so
# the format and check-format targets must use exactly this major.
# Override CLANG_FORMAT on the command line if the binary lives under a
# non-standard name (e.g. CLANG_FORMAT=/opt/llvm-20/bin/clang-format).
CLANG_FORMAT_REQUIRED_VERSION := 20

# Auto-detect a clang-format binary that reports the required major.
# Try the canonically named binary first, then fall back to whatever
# `clang-format` resolves to on PATH (Homebrew's clang-format@20 keg
# installs it as plain `clang-format`, for example). Detection happens
# at parse time and the result is cached so each format target only
# pays for the probe once.
_clang_format_check = $(shell command -v $(1) >/dev/null 2>&1 && \
    $(1) --version 2>/dev/null | \
    awk '/version/ { for (i = 1; i <= NF; i++) if ($$i ~ /^[0-9]+\./) { \
        split($$i, v, "."); if (v[1] == $(CLANG_FORMAT_REQUIRED_VERSION)) print "$(1)"; exit } }')
CLANG_FORMAT ?= $(strip $(or \
    $(call _clang_format_check,clang-format-$(CLANG_FORMAT_REQUIRED_VERSION)), \
    $(call _clang_format_check,clang-format)))
