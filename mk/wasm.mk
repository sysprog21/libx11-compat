# WebAssembly / Emscripten target.
#
# WASM=1 retargets the build to emcc, producing a static libX11-compat
# archive plus a browser artifact for examples/clock.c. Only the core
# library and that one example build for wasm; the toolkit libraries, the
# upstream WM apps, and the native differential harness stay native-only.
#
# The leg is deliberately narrow (see the WebAssembly scope guard in
# TODO.md): SDL2 backend over Emscripten's SDL2 port, no MIT-SHM, no
# multi-window WM, no GLX/OpenGL, single-threaded (ASYNCIFY, no pthreads).
#
# This fragment is included first so CC and the wasm target selection land
# before mk/toolchain.mk applies its native clang default. The wasm-only
# targets (staged pixman, the static archive, the clock browser artifact) live
# in mk/wasm-deps.mk, which is included after the object list exists.

ifeq ($(WASM),1)

# emcc drives both compile and link. A command-line CC=... still wins: a
# command-line assignment overrides this file-level one.
CC := emcc
AR := emar

# GLX needs a runtime EGL provider we cannot supply in the browser, so
# compile the glX* layer out entirely rather than ship dead dispatch. override
# so a stray command-line GLX=1 cannot drag glx.c / egl-wrapper.c (and their
# missing EGL headers) back into the wasm build.
override GLX := 0

# Define OUT/TARGET here (config.mk uses ?= and defers to these) so the
# archive path is a static library, not the native .so, from parse time on.
OUT ?= build
TARGET := $(OUT)/libX11-compat.a

# Staged wasm dependency sysroot. Pixman is built into it by mk/wasm-deps.mk;
# the paths are deterministic so the compile flags need no parse-time
# pkg-config against a not-yet-built .pc.
WASM_SYSROOT := $(abspath $(OUT))/wasm-sysroot

endif
