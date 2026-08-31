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

# Codegen helpers that run on the build machine (the libXt makestrs tool, and
# similar toolkit generators) must stay on a native compiler even though CC is
# now emcc for the wasm target. Without this they would be built as wasm and
# could not execute during the build. `?=` here beats libxt.mk's later
# `HOST_CC ?= $(CC)` (this fragment is included first) while still letting a
# command-line or environment HOST_CC override the native default.
HOST_CC ?= cc

# GLX needs a runtime EGL provider we cannot supply in the browser, so
# compile the glX* layer out entirely rather than ship dead dispatch. override
# so a stray command-line GLX=1 cannot drag glx.c / egl-wrapper.c (and their
# missing EGL headers) back into the wasm build.
override GLX := 0

# The XCB shim links the native shared libraries the wasm build does not
# produce, so keep it out regardless of a command-line XCB=1.
override XCB := 0

# Define OUT/TARGET here (config.mk uses ?= and defers to these) so the
# archive path is a static library, not the native .so, from parse time on.
OUT ?= build
TARGET := $(OUT)/libX11-compat.a

# Staged wasm dependency sysroot. Pixman is built into it by mk/wasm-deps.mk;
# the paths are deterministic so the compile flags need no parse-time
# pkg-config against a not-yet-built .pc.
WASM_SYSROOT := $(abspath $(OUT))/wasm-sysroot

# Optimize wasm objects for size (native stays at the -O2 default). Code size is
# the metric that matters for a browser download, so -Oz plus link-time
# optimization: -flto makes each object LLVM bitcode and lets wasm-ld drop
# cross-module dead code at link. A command-line or environment OPTFLAGS still
# wins (e.g. OPTFLAGS=-O0 for a debug wasm build). $(FP_CFLAGS) carries these to
# the link too, which is where LTO actually runs.
#
# Bitcode-consumer coupling: -flto makes the archives LLVM bitcode, so any
# external consumer that links them (an emconfigure/emmake cross-build such as
# Motif) must compile with the SAME OPTFLAGS. A non-flto object linked against
# flto archives crashes wasm-opt (--post-emscripten: "popping from empty stack"),
# which autoconf misreads as a missing symbol. Thread $(OPTFLAGS) into any such
# consumer rather than hardcoding a level.
OPTFLAGS ?= -Oz -flto

endif
