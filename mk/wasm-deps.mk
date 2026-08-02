# WebAssembly build: staged dependencies and browser artifacts.
#
# Included after the object list (mk/sources.mk) and examples exist. The early
# half of the wasm leg (CC=emcc, TARGET, SDL port flags, link-flag gating)
# lives in mk/wasm.mk and the per-file flag edits in config.mk / sdl.mk /
# library.mk; this fragment owns the wasm-only targets.

ifeq ($(WASM),1)

# Pixman is not an Emscripten built-in port, so it is fetched and cross-built
# from source into WASM_SYSROOT (set in mk/wasm.mk), static-only with every
# CPU-specific fast path disabled. The main build reads it through the
# deterministic sysroot paths pinned in mk/config.mk, never host pkg-config, so
# no host pixman can leak into a wasm compile or link line.
#
# Fetch-and-verify mirrors mk/font-data.mk: every source (the mirror or a local
# PIXMAN_WASM_TARBALL) must match the pinned digest or it is rejected. The source
# is pixman's freedesktop GitLab archive. 0.42.2 is the last pixman release with
# an autotools build; 0.44+ is meson-only, which emconfigure does not drive. The
# GitLab archive is the git tree, so it carries configure.ac but no generated
# configure; the build recipe runs autogen.sh first, which needs host autotools
# (autoconf/automake/libtool). The pinned digest is that archive's, not the
# cairographics release tarball's (different bytes).
WASM_DEP_DIR := $(OUT)/wasm-deps
PIXMAN_VERSION := 0.42.2
PIXMAN_WASM_SHA256 := 4191a5084bae000a61e3513b06027b6f8f559d17d61769ed9de27dfb0cec8699
PIXMAN_WASM_URLS ?= \
    https://gitlab.freedesktop.org/pixman/pixman/-/archive/pixman-$(PIXMAN_VERSION)/pixman-pixman-$(PIXMAN_VERSION).tar.gz
# Offline or air-gapped build: pass PIXMAN_WASM_TARBALL=/path/to/pixman.tar.gz
# to skip the network fetch. It is digest-checked like any mirror.
PIXMAN_WASM_TARBALL ?=

PIXMAN_WASM_CACHE := $(WASM_DEP_DIR)/pixman-$(PIXMAN_VERSION).tar.gz
PIXMAN_WASM_SRC := $(WASM_DEP_DIR)/pixman-$(PIXMAN_VERSION)
WASM_PIXMAN_LIB := $(WASM_SYSROOT)/lib/libpixman-1.a

$(WASM_DEP_DIR):
	@mkdir -p $@

# Fetch (or copy) the tarball and content-verify it before use, failing closed.
$(PIXMAN_WASM_CACHE): | $(WASM_DEP_DIR)
	@set -f; sha=$(PIXMAN_WASM_SHA256); \
	digest() { (sha256sum "$$1" 2>/dev/null || shasum -a 256 "$$1") | cut -d' ' -f1; }; \
	if [ -n "$(PIXMAN_WASM_TARBALL)" ]; then \
	    echo "  COPY    $(notdir $(PIXMAN_WASM_TARBALL))"; \
	    cp "$(PIXMAN_WASM_TARBALL)" "$@.tmp" || exit 1; \
	    got=`digest "$@.tmp"`; \
	    if [ "$$got" != "$$sha" ]; then \
	        echo "PIXMAN_WASM_TARBALL sha256 mismatch: got $$got want $$sha" >&2; \
	        rm -f "$@.tmp"; exit 1; \
	    fi; \
	else \
	    ok=0; \
	    for url in $(PIXMAN_WASM_URLS); do \
	        echo "  FETCH   $$url"; \
	        curl -fsSL --max-time 120 -o "$@.tmp" "$$url" || continue; \
	        got=`digest "$@.tmp"`; \
	        if [ "$$got" = "$$sha" ]; then ok=1; break; fi; \
	        echo "  ...digest mismatch from this mirror, trying next" >&2; \
	    done; \
	    if [ "$$ok" != 1 ]; then \
	        echo "no pixman-$(PIXMAN_VERSION) mirror returned the expected digest $$sha" >&2; \
	        rm -f "$@.tmp"; exit 1; \
	    fi; \
	fi
	@mv "$@.tmp" "$@"

# Cross-build static pixman into the sysroot. strip-components=1 normalizes the
# archive's top-level directory name into one source dir. autogen.sh with
# NOCONFIGURE=1 generates configure from configure.ac; emconfigure then
# cross-configures it. The SUBDIRS=pixman override builds and installs only the
# library (plus the .pc), skipping pixman's test and demo programs.
# Depend on this fragment so a change to the version, digest, mirror, or
# configure flags restages pixman instead of reporting nothing to do.
$(WASM_PIXMAN_LIB): $(PIXMAN_WASM_CACHE) mk/wasm-deps.mk
	@echo "  WASMDEP pixman-$(PIXMAN_VERSION)"
	$(Q)rm -rf $(PIXMAN_WASM_SRC) && mkdir -p $(PIXMAN_WASM_SRC)
	$(Q)tar xzf $< -C $(PIXMAN_WASM_SRC) --strip-components=1
	$(Q)cd $(PIXMAN_WASM_SRC) && NOCONFIGURE=1 ./autogen.sh >autogen.log 2>&1 && \
	    emconfigure ./configure \
	    --host=wasm32-unknown-emscripten --prefix=$(WASM_SYSROOT) \
	    --enable-static --disable-shared \
	    --disable-mmx --disable-sse2 --disable-ssse3 --disable-vmx \
	    --disable-arm-simd --disable-neon \
	    --disable-gtk --disable-libpng --disable-openmp CFLAGS="-O2" >configure.log 2>&1
	$(Q)emmake make -C $(PIXMAN_WASM_SRC) SUBDIRS=pixman install >$(PIXMAN_WASM_SRC)/build.log 2>&1

# Every first-party object needs pixman.h, so gate compilation on the staged
# archive. Order-only: the sysroot is deterministic, so a rebuilt archive must
# not force every object to recompile.
$(OBJS): | $(WASM_PIXMAN_LIB)

.PHONY: wasm-deps
## Stage the wasm dependency sysroot (pixman) without compiling the library
wasm-deps: $(WASM_PIXMAN_LIB)

# Static archive: the browser has no shared object, so the wasm library is a
# plain emar archive of the first-party objects. Remove any prior archive
# first so a dropped object cannot survive as a stale member.
$(TARGET): $(OBJS)
	@echo "  AR      $@"
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $(OBJS)

# clock browser artifact. A single emcc -o clock.html emits three sibling
# files: the HTML shell, the JS loader, and the wasm module. The .html is the
# tracked target; clock.js and clock.wasm fall out beside it.
#
# clock draws only lines and arcs, no text, and the core "fixed" font is the
# 6x13 bitmap compiled into the archive. The font search path is only walked
# when text is actually drawn, so this example requires no preloaded font
# files: the wasm MEMFS stays empty and no --preload-file is needed. A
# text-drawing example would add --preload-file for the bundled TTFs.
WASM_CLOCK_HTML := $(OUT)/clock.html
# ASYNCIFY lets the example's blocking event loop (emscripten_sleep in place of
# select) yield to the browser each tick so the SDL canvas paints, keeping all
# SDL work on the main thread with no SharedArrayBuffer/COOP/COEP requirement.
WASM_CLOCK_LDFLAGS := -sASYNCIFY

$(WASM_CLOCK_HTML): examples/clock.c $(TARGET) mk/wasm-deps.mk
	@echo "  LD      $@"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) examples/clock.c \
	    $(TARGET) $(LDLIBS) $(WASM_CLOCK_LDFLAGS) -o $@

.PHONY: clock
## Build the wasm clock browser artifact (clock.html, clock.js, clock.wasm)
clock: $(WASM_CLOCK_HTML)

# The wasm leg gets its own default goal instead of `all`. GNU Make accumulates
# prerequisites across every `all:` rule, and the toolkit fragments (libXt,
# libXpm, libXaw, the Xext/Xmu/... compat libs, pkg-config files) each attach
# native targets to `all`; reusing `all` here would drag the whole native
# toolkit into a `make WASM=1`. A distinct goal sidesteps that with no need to
# gate each fragment.
#
# Default goal for the wasm leg: the static library archive.
.PHONY: wasm
wasm: $(TARGET)
.DEFAULT_GOAL := wasm

endif

# Build smoke for the wasm leg, invokable from a normal make (it recurses with
# WASM=1 rather than requiring the caller to set it). Skips cleanly when emcc is
# absent so CI without Emscripten stays green; where emcc exists it builds the
# clock artifacts and checks all three are produced and that the module really
# is wasm. The runtime paint/tick check is manual in a browser, out of CI scope.
.PHONY: check-wasm
## Build-smoke the WebAssembly leg (skips when emcc is unavailable)
check-wasm:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm (emcc not found)"; \
	    exit 0; \
	fi; \
	$(MAKE) WASM=1 clock || exit 1; \
	for f in $(OUT)/clock.html $(OUT)/clock.js $(OUT)/clock.wasm $(OUT)/libX11-compat.a; do \
	    if [ ! -s "$$f" ]; then echo "check-wasm: missing $$f" >&2; exit 1; fi; \
	done; \
	case `file -b $(OUT)/clock.wasm` in \
	    *WebAssembly*) ;; \
	    *) echo "check-wasm: $(OUT)/clock.wasm is not a wasm module" >&2; exit 1;; \
	esac; \
	echo "  OK      check-wasm (archive + clock.html/js/wasm)"
