# Compiler and toolchain detection.

MAKEFLAGS += --no-builtin-rules --no-builtin-variables

# GNU make predefines CC=cc. Prefer clang for local builds while preserving
# explicit command-line and environment overrides.
ifeq ($(origin CC),default)
  CC := clang
else
  CC ?= clang
endif

PKG_CONFIG ?= pkg-config
SDL2_CONFIG ?= sdl2-config
CLANG_FORMAT ?= clang-format
PYTHON ?= python3
