# WebAssembly build: staged dependencies and browser artifacts.
#
# Included after the object list (mk/sources.mk) and examples exist. The early
# half of the wasm leg (CC=emcc, TARGET, SDL port flags, link-flag gating)
# lives in mk/wasm.mk and the per-file flag edits in config.mk / sdl.mk /
# library.mk; this fragment owns the wasm-only targets.

# Bundled examples built as browser artifacts, one clock.html/js/wasm triple
# each. Defined outside the WASM guard so check-wasm (which recurses with
# WASM=1) can iterate the same list. They draw with core Xlib only, so none
# needs a preloaded font (see the pattern rule below).
WASM_EXAMPLES := clock moire life mandel

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

# Browser example artifacts. A single emcc -o %.html emits three sibling
# files: the HTML shell, the JS loader, and the wasm module. The .html is the
# tracked target; the .js and .wasm fall out beside it.
#
# These examples draw with core Xlib only: lines, arcs, segments, fills, XImage
# blits, and core-font text through the compiled-in 6x13 bitmap. None uses
# Xft/TTF (whose faces are files), MIT-SHM, GLX, or a second window, and the
# core font search path is only walked for text that resolves to a TTF, so the
# wasm MEMFS stays empty and no --preload-file is needed. A face-loading
# example would add --preload-file for the bundled TTFs.
WASM_EXAMPLE_HTML := $(addprefix $(OUT)/,$(addsuffix .html,$(WASM_EXAMPLES)))
# The examples drive their render loop through emscripten_set_main_loop (each
# example's __EMSCRIPTEN__ path), so the browser calls one frame per animation
# frame and the main thread never blocks. That removes the need for ASYNCIFY,
# whose whole-program instrumentation added roughly 260 KB per artifact.
# --shell-file swaps emscripten's stock page for a themed launcher (a canvas
# frame, dark/light theme, loading overlay) shared by every example; it derives
# its title from the page name. emmalloc is smaller than the default dlmalloc.
WASM_SHELL := examples/wasm-shell.html
WASM_EXAMPLE_LDFLAGS := -sMALLOC=emmalloc --shell-file $(WASM_SHELL)

# Font stack. A text-free example (clock, moire, mandel) never loads a font, so
# it drops the SDL_ttf/freetype/harfbuzz stack (about 1.6 MB, over half the
# artifact): filter the TTF/freetype port flags out of the compile+link and
# satisfy font.c/xft.c's residual TTF references (pulled in by XOpenDisplay's
# font-storage init) with no-op stubs (examples/wasm-nofont-stub.c). The example
# .c files include no SDL headers, so the -sUSE_SDL=2 left in the filtered
# CPPFLAGS still links the core's SDL2 use.
#
# life draws text (XDrawString on the "fixed" bitmap font), so it keeps the full
# font stack via an explicit rule below: loading even the bitmap font still opens
# a TTF face today, so the stub's NULL face would make the load fail. Letting
# bitmap fonts load without a TTF face is a core font.c change tracked
# separately; until then only text-free examples shrink.
WASM_EXAMPLE_CPPFLAGS := $(filter-out -sUSE_SDL_TTF=2 -sUSE_FREETYPE=1,$(CPPFLAGS))
WASM_EXAMPLE_LINK_LIBS := $(PIXMAN_LIBS) -lm
WASM_NOFONT_STUB := $(OUT)/examples/wasm-nofont-stub.o

# Compiled with the SDL_ttf header (for the prototypes) but linked without the
# port; the stub bodies stand in for it.
$(WASM_NOFONT_STUB): examples/wasm-nofont-stub.c mk/wasm-deps.mk
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) -sUSE_SDL=2 -sUSE_SDL_TTF=2 $(FP_CFLAGS) -c $< -o $@

# life keeps the full font stack. This explicit rule wins over the text-free
# pattern rule below.
$(OUT)/life.html: examples/life.c $(TARGET) $(WASM_SHELL) mk/wasm-deps.mk
	@echo "  LD      $@"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(TARGET) $(LDLIBS) $(WASM_EXAMPLE_LDFLAGS) -o $@

# Text-free examples: drop the font stack, link the TTF stub.
$(OUT)/%.html: examples/%.c $(TARGET) $(WASM_NOFONT_STUB) $(WASM_SHELL) mk/wasm-deps.mk
	@echo "  LD      $@"
	$(Q)$(CC) $(WASM_EXAMPLE_CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(TARGET) $(WASM_NOFONT_STUB) $(WASM_EXAMPLE_LINK_LIBS) \
	    $(WASM_EXAMPLE_LDFLAGS) -o $@

.PHONY: clock examples-wasm
## Build the wasm clock browser artifact (clock.html, clock.js, clock.wasm)
clock: $(OUT)/clock.html
## Build every wasm example browser artifact (clock, moire, life, mandel)
examples-wasm: $(WASM_EXAMPLE_HTML)

# Toolkit link smoke: link the libXt init test against the wasm libXt and
# libX11-compat archives. A clean link proves the wasm libXt archive is
# symbol-complete on top of the core; a missing symbol fails the emcc link
# loudly. It is not run headless: Emscripten's SDL2 port has no dummy video
# driver, so a functional run needs a browser canvas (covered per app later).
# The test compiles with the same libxt-build config.h as the libXt units.
WASM_LIBXT_TEST := $(OUT)/tests/test-libxt-link.js
WASM_LIBXT_TEST_CPPFLAGS := -DHAVE_CONFIG_H -D_GNU_SOURCE -Iinclude/libxt-build \
    -I$(LIBXT_GEN_DIR) -I$(OUT)/upstream/include -Iinclude \
    -iquote $(OUT)/upstream/include/X11 -iquote include/X11

$(WASM_LIBXT_TEST): tests/test-libxt-link.c $(LIBXT_TARGET) $(TARGET) mk/wasm-deps.mk
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	$(Q)$(CC) $(WASM_LIBXT_TEST_CPPFLAGS) $(SDL_CPPFLAGS) $(PIXMAN_CFLAGS) \
	    $(FP_CFLAGS) $(CFLAGS_EXTRA) tests/test-libxt-link.c \
	    $(LIBXT_TARGET) $(TARGET) $(LDLIBS) -sASYNCIFY -o $@

.PHONY: wasm-libxt-test
wasm-libxt-test: $(WASM_LIBXT_TEST)

# Full Xaw-stack link smoke: Xaw pulls Xmu, Xt, Xpm, and the core, so a clean
# link proves the whole toolkit tier is symbol-complete. Xext is not
# linked: its XShape symbols already live in the core, so adding the separate
# archive would duplicate them. --start-group lets wasm-ld resolve the archives'
# mutual references regardless of order.
WASM_XAW_TEST := $(OUT)/tests/test-libxaw-link.js
WASM_XAW_ARCHIVES := $(LIBXAW_TARGET) $(XMU_COMPAT_TARGET) $(LIBXT_TARGET) \
    $(LIBXPM_TARGET) $(TARGET)

$(WASM_XAW_TEST): tests/test-libxaw-link.c $(WASM_XAW_ARCHIVES) mk/wasm-deps.mk
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	$(Q)$(CC) $(WASM_LIBXT_TEST_CPPFLAGS) -Iinclude/libxaw-build \
	    $(SDL_CPPFLAGS) $(PIXMAN_CFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) \
	    tests/test-libxaw-link.c \
	    -Wl,--start-group $(WASM_XAW_ARCHIVES) -Wl,--end-group \
	    $(LDLIBS) -sASYNCIFY -o $@

.PHONY: wasm-xaw-test
wasm-xaw-test: $(WASM_XAW_TEST)

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
# absent so CI without Emscripten stays green; where emcc exists it builds every
# example artifact and checks the archive plus each example's html/js/wasm are
# produced and that each module really is wasm. The runtime paint/tick check is
# manual in a browser, out of CI scope.
.PHONY: check-wasm
## Build-smoke the WebAssembly leg (skips when emcc is unavailable)
check-wasm:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm (emcc not found)"; \
	    exit 0; \
	fi; \
	$(MAKE) WASM=1 examples-wasm || exit 1; \
	if [ ! -s $(OUT)/libX11-compat.a ]; then \
	    echo "check-wasm: missing $(OUT)/libX11-compat.a" >&2; exit 1; \
	fi; \
	for ex in $(WASM_EXAMPLES); do \
	    for f in $(OUT)/$$ex.html $(OUT)/$$ex.js $(OUT)/$$ex.wasm; do \
	        if [ ! -s "$$f" ]; then echo "check-wasm: missing $$f" >&2; exit 1; fi; \
	    done; \
	    case `file -b $(OUT)/$$ex.wasm` in \
	        *WebAssembly*) ;; \
	        *) echo "check-wasm: $(OUT)/$$ex.wasm is not a wasm module" >&2; exit 1;; \
	    esac; \
	done; \
	echo "  OK      check-wasm (archive + $(WASM_EXAMPLES))"

# Toolkit link smoke: link the wasm libXt archive against the core and confirm
# the result is a symbol-complete wasm module. Separate from check-wasm so the
# cheap example smoke does not pull in the libXt build. Skips without emcc.
.PHONY: check-wasm-libxt
## Link-smoke the wasm libXt archive (skips when emcc is unavailable)
check-wasm-libxt:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm-libxt (emcc not found)"; \
	    exit 0; \
	fi; \
	$(MAKE) WASM=1 wasm-libxt-test || exit 1; \
	for f in $(OUT)/libXt-compat.a $(OUT)/tests/test-libxt-link.wasm; do \
	    if [ ! -s "$$f" ]; then echo "check-wasm-libxt: missing $$f" >&2; exit 1; fi; \
	done; \
	case `file -b $(OUT)/tests/test-libxt-link.wasm` in \
	    *WebAssembly*) ;; \
	    *) echo "check-wasm-libxt: not a wasm module" >&2; exit 1;; \
	esac; \
	echo "  OK      check-wasm-libxt (libXt archive links on the core)"

# Link-smoke the whole wasm Xaw toolkit tier (Xaw + Xmu + Xt + Xpm on the core;
# Xext resolves from the core). Skips without emcc.
.PHONY: check-wasm-xaw
## Link-smoke the wasm Xaw toolkit stack (skips when emcc is unavailable)
check-wasm-xaw:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm-xaw (emcc not found)"; \
	    exit 0; \
	fi; \
	$(MAKE) WASM=1 wasm-xaw-test || exit 1; \
	for f in $(OUT)/libXaw-compat.a $(OUT)/libXmu-compat.a \
	         $(OUT)/libXpm-compat.a \
	         $(OUT)/tests/test-libxaw-link.wasm; do \
	    if [ ! -s "$$f" ]; then echo "check-wasm-xaw: missing $$f" >&2; exit 1; fi; \
	done; \
	case `file -b $(OUT)/tests/test-libxaw-link.wasm` in \
	    *WebAssembly*) ;; \
	    *) echo "check-wasm-xaw: not a wasm module" >&2; exit 1;; \
	esac; \
	echo "  OK      check-wasm-xaw (Xaw stack links on the core)"

# One-command coverage of every wasm smoke. Each target skips cleanly without
# emcc, so this stays green on a toolchain-free CI. Not folded into the default
# `check`: the example smoke fetches/builds pixman (network plus host autotools),
# which the base test gate must not require.
.PHONY: check-wasm-all
## Run every WebAssembly smoke (examples + libXt + Xaw); skips without emcc
check-wasm-all: check-wasm check-wasm-libxt check-wasm-xaw
