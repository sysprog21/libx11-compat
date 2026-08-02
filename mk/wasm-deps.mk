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

# Defined outside the WASM guard so the check-wasm-prereqs diagnostic (runnable
# from a normal make) can report the pinned version and staged-archive path.
PIXMAN_VERSION := 0.42.2

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
	$(Q)cd $(PIXMAN_WASM_SRC) && \
	    { NOCONFIGURE=1 ./autogen.sh >autogen.log 2>&1 || \
	      { echo "  FAIL    pixman autogen.sh (need host autoconf/automake/libtool)" >&2; \
	        tail -30 autogen.log >&2; exit 1; }; } && \
	    { emconfigure ./configure \
	    --host=wasm32-unknown-emscripten --prefix=$(WASM_SYSROOT) \
	    --enable-static --disable-shared \
	    --disable-mmx --disable-sse2 --disable-ssse3 --disable-vmx \
	    --disable-arm-simd --disable-neon \
	    --disable-gtk --disable-libpng --disable-openmp CFLAGS="-O2" >configure.log 2>&1 || \
	      { echo "  FAIL    pixman configure" >&2; tail -30 configure.log >&2; exit 1; }; }
	$(Q)emmake make -C $(PIXMAN_WASM_SRC) SUBDIRS=pixman install \
	    >$(PIXMAN_WASM_SRC)/build.log 2>&1 || \
	    { echo "  FAIL    pixman build" >&2; tail -30 $(PIXMAN_WASM_SRC)/build.log >&2; exit 1; }

# Every first-party object needs pixman.h, so gate compilation on the staged
# archive. Order-only: the sysroot is deterministic, so a rebuilt archive must
# not force every object to recompile.
$(OBJS): | $(WASM_PIXMAN_LIB)

# DejaVu TTFs preloaded into MEMFS so toolkit apps render text in the
# browser. Xt/Xaw and Motif ask for a scalable ISO8859 font by default (not the
# "fixed" bitmap alias), and XLoadQueryFont -- the path the toolkits use -- only
# resolves fonts the cache scan found, not the hardcoded probe-path fallbacks.
# That scan (src/font.c updateFontCache) is NON-RECURSIVE and reads the search
# dirs (/usr/share/fonts, ...) directly, so the TTFs must sit at the top level of
# one, not nested under truetype/dejavu; nested, the scan misses them and the app
# aborts "no font found". The release zip is fetched and digest-verified like
# pixman; an offline build passes DEJAVU_TTF_ZIP=/path/to/dejavu-fonts-ttf-<ver>
# .zip. Every unmodified toolkit app pulls this (WASM_FONT_PRELOAD is part of the
# app link); the examples draw with core fonts and never touch it.
DEJAVU_VERSION := 2.37
DEJAVU_SHA256 := 7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a
DEJAVU_URLS ?= \
    https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_$(subst .,_,$(DEJAVU_VERSION))/dejavu-fonts-ttf-$(DEJAVU_VERSION).zip
DEJAVU_TTF_ZIP ?=
DEJAVU_CACHE := $(WASM_DEP_DIR)/dejavu-fonts-ttf-$(DEJAVU_VERSION).zip
NOTO_CJK_SHA256 := dce08bd4fd91aa8aa76ed8fea4b694c2dfb8550f67871e326843212ddbeb88b4
NOTO_CJK_URLS ?= \
    https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/TraditionalChinese/NotoSansCJKtc-Regular.otf
NOTO_CJK_FONT ?=
NOTO_CJK_CACHE := $(WASM_DEP_DIR)/NotoSansCJKtc-Regular.otf
WASM_FONT_DIR := $(OUT)/wasm-fonts
WASM_FONT_STAMP := $(WASM_FONT_DIR)/.stamp
# Top level of a search-path dir so the non-recursive font-cache scan finds it.
WASM_FONT_TTF_DIR := $(WASM_FONT_DIR)/usr/share/fonts

$(DEJAVU_CACHE): | $(WASM_DEP_DIR)
	@set -f; sha=$(DEJAVU_SHA256); \
	digest() { (sha256sum "$$1" 2>/dev/null || shasum -a 256 "$$1") | cut -d' ' -f1; }; \
	if [ -n "$(DEJAVU_TTF_ZIP)" ]; then \
	    echo "  COPY    $(notdir $(DEJAVU_TTF_ZIP))"; \
	    cp "$(DEJAVU_TTF_ZIP)" "$@.tmp" || exit 1; \
	    got=`digest "$@.tmp"`; \
	    if [ "$$got" != "$$sha" ]; then \
	        echo "DEJAVU_TTF_ZIP sha256 mismatch: got $$got want $$sha" >&2; \
	        rm -f "$@.tmp"; exit 1; \
	    fi; \
	else \
	    ok=0; \
	    for url in $(DEJAVU_URLS); do \
	        echo "  FETCH   $$url"; \
	        curl -fsSL --max-time 120 -o "$@.tmp" "$$url" || continue; \
	        got=`digest "$@.tmp"`; \
	        if [ "$$got" = "$$sha" ]; then ok=1; break; fi; \
	        echo "  ...digest mismatch from this mirror, trying next" >&2; \
	    done; \
	    if [ "$$ok" != 1 ]; then \
	        echo "no dejavu-$(DEJAVU_VERSION) mirror returned the expected digest $$sha" >&2; \
	        rm -f "$@.tmp"; exit 1; \
	    fi; \
	fi
	@mv "$@.tmp" "$@"

$(NOTO_CJK_CACHE): | $(WASM_DEP_DIR)
	@set -f; sha=$(NOTO_CJK_SHA256); \
	digest() { (sha256sum "$$1" 2>/dev/null || shasum -a 256 "$$1") | cut -d' ' -f1; }; \
	if [ -n "$(NOTO_CJK_FONT)" ]; then \
	    echo "  COPY    $(notdir $(NOTO_CJK_FONT))"; \
	    cp "$(NOTO_CJK_FONT)" "$@.tmp" || exit 1; \
	    got=`digest "$@.tmp"`; \
	    if [ "$$got" != "$$sha" ]; then \
	        echo "NOTO_CJK_FONT sha256 mismatch: got $$got want $$sha" >&2; \
	        rm -f "$@.tmp"; exit 1; \
	    fi; \
	else \
	    ok=0; \
	    for url in $(NOTO_CJK_URLS); do \
	        echo "  FETCH   $$url"; \
	        curl -fL --max-time 180 -o "$@.tmp" "$$url" || continue; \
	        got=`digest "$@.tmp"`; \
	        if [ "$$got" = "$$sha" ]; then ok=1; break; fi; \
	        echo "  ...digest mismatch from this mirror, trying next" >&2; \
	    done; \
	    if [ "$$ok" != 1 ]; then \
	        echo "no Noto CJK mirror returned the expected digest $$sha" >&2; \
	        rm -f "$@.tmp"; exit 1; \
	    fi; \
	fi
	@mv "$@.tmp" "$@"

$(WASM_FONT_STAMP): $(DEJAVU_CACHE) mk/wasm-deps.mk
	@echo "  WASMFONT dejavu-$(DEJAVU_VERSION)"
	$(Q)rm -rf $(WASM_FONT_DIR) && mkdir -p $(WASM_FONT_TTF_DIR)
	$(Q)unzip -o -j $< "*/DejaVuSans.ttf" "*/DejaVuSansMono.ttf" \
	    -d $(WASM_FONT_TTF_DIR) >/dev/null
	$(Q)touch $@

# Preload flags an app adds to render scalable text: mount the staged font tree
# at /usr/share/fonts, matching the core's search path. Requires the FS (kept on
# by default; only the examples strip it). The .data sidecar carries the fonts.
WASM_FONT_PRELOAD = --preload-file \
    $(abspath $(WASM_FONT_DIR))/usr/share/fonts@/usr/share/fonts
WASM_CJK_FONT_PRELOAD = --preload-file \
    $(abspath $(NOTO_CJK_CACHE))@/usr/share/fonts/NotoSansCJKtc-Regular.otf

.PHONY: wasm-fonts
## Stage the wasm DejaVu font tree for --preload-file
wasm-fonts: $(WASM_FONT_STAMP)

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
# Size levers beyond emmalloc, both safe for these browser-only artifacts:
# -sENVIRONMENT=web drops the node/worker JS glue and -sDYNAMIC_EXECUTION=0
# removes eval-based codegen from the loader. -sFILESYSTEM=0 was considered but
# left off: it strips the virtual filesystem entirely, and whether the core
# touches a file syscall at init (font-cache probing) under emscripten is not
# verifiable in the headless build smoke, only in a browser. --closure 1 is a
# further cut but can miscompile the custom shell. Revisit both once a Safari
# runtime pass confirms a canvas still paints.
WASM_EXAMPLE_LDFLAGS := -sMALLOC=emmalloc -sENVIRONMENT=web \
    -sDYNAMIC_EXECUTION=0 --shell-file $(WASM_SHELL)

# Font stack. These examples never open a font file, so they drop the
# SDL_ttf/freetype/harfbuzz stack (about 1.6 MB, over half the artifact): filter
# the TTF/freetype port flags out of the compile+link and satisfy font.c/xft.c's
# residual TTF references (pulled in by XOpenDisplay's font-storage init) with
# no-op stubs (examples/wasm-nofont-stub.c). The example .c files include no SDL
# headers, so the -sUSE_SDL=2 left in the filtered CPPFLAGS still links the
# core's SDL2 use.
#
# life draws text (XDrawString on the default "fixed" font). It used to keep the
# full font stack because loading even the bitmap alias opened a TTF face; the
# core now loads fixed/cursor/6x13 with a NULL face under __EMSCRIPTEN__ and
# renders them from the embedded 6x13 bitmap, so life drops freetype too and
# shares the single pattern rule below.
WASM_EXAMPLE_CPPFLAGS := $(filter-out -sUSE_SDL_TTF=2 -sUSE_FREETYPE=1,$(CPPFLAGS))
WASM_EXAMPLE_LINK_LIBS := $(PIXMAN_LIBS) -lm
WASM_NOFONT_STUB := $(OUT)/examples/wasm-nofont-stub.o

# Compiled with the SDL_ttf header (for the prototypes) but linked without the
# port; the stub bodies stand in for it.
$(WASM_NOFONT_STUB): examples/wasm-nofont-stub.c mk/wasm-deps.mk
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) -sUSE_SDL=2 -sUSE_SDL_TTF=2 $(FP_CFLAGS) -c $< -o $@

# Every example drops the font stack and links the TTF stub.
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

# Browser idle-yield override object. The core's blocking event waits
# (XNextEvent and friends) call a hook that is NULL by default; an unmodified
# showcase app links this object, built with -sASYNCIFY, whose constructor
# installs emscripten_sleep(0) so the app yields to the browser instead of
# freezing the tab. The in-tree examples do NOT link it (they drive
# set_main_loop and stay ASYNCIFY-free); it lives here so the showcase app recipe
# has one place to pull the yield from. Built by the generic object rule.
WASM_IDLE_YIELD_OBJ := $(OBJROOT)/compat/wasm-idle-yield.o

# Link smoke for the yield override: a program that blocks in XNextEvent, linked
# with the override and -sASYNCIFY. A clean link proves the override resolves
# emscripten_sleep (which needs ASYNCIFY) and supersedes the core's default hook,
# so the browser yield mechanism is wired end to end.
WASM_YIELD_TEST := $(OUT)/tests/test-wasm-yield.js
$(WASM_YIELD_TEST): tests/test-wasm-yield.c $(TARGET) $(WASM_IDLE_YIELD_OBJ) \
    mk/wasm-deps.mk
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) tests/test-wasm-yield.c \
	    $(TARGET) $(WASM_IDLE_YIELD_OBJ) $(LDLIBS) -sASYNCIFY -o $@

.PHONY: wasm-yield-test
wasm-yield-test: $(WASM_YIELD_TEST)

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

# Link-smoke the browser idle-yield override. Skips without emcc.
.PHONY: check-wasm-yield
## Link-smoke the wasm idle-yield override (skips when emcc is unavailable)
check-wasm-yield:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm-yield (emcc not found)"; \
	    exit 0; \
	fi; \
	$(MAKE) WASM=1 wasm-yield-test || exit 1; \
	if [ ! -s $(OUT)/tests/test-wasm-yield.wasm ]; then \
	    echo "check-wasm-yield: missing test-wasm-yield.wasm" >&2; exit 1; \
	fi; \
	case `file -b $(OUT)/tests/test-wasm-yield.wasm` in \
	    *WebAssembly*) ;; \
	    *) echo "check-wasm-yield: not a wasm module" >&2; exit 1;; \
	esac; \
	echo "  OK      check-wasm-yield (idle-yield override links with ASYNCIFY)"

# Structural runtime smoke: fully compile each example module in node (parses
# every section and function body, a real step up from the 4-byte magic check
# the link smokes use). It does not instantiate or run main, so it stays
# headless and works with the -sENVIRONMENT=web artifacts; a canvas paint check
# is still manual in Safari. Skips without emcc or node.
.PHONY: check-wasm-node
## Compile-validate the wasm example modules in node (skips without emcc/node)
check-wasm-node:
	$(Q)if ! command -v emcc >/dev/null 2>&1 || ! command -v node >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm-node (emcc or node not found)"; \
	    exit 0; \
	fi; \
	$(MAKE) WASM=1 examples-wasm || exit 1; \
	node scripts/wasm-node-smoke.mjs \
	    $(addprefix $(OUT)/,$(addsuffix .wasm,$(WASM_EXAMPLES)))

# Report the wasm toolchain prerequisites and pixman staging status, so a
# missing tool (emcc/emar, host autotools for the pixman cross-build, node for
# the runtime smoke) is an explicit diagnostic rather than a surprise failure
# mid-build. Emscripten 3.1.45+ is the tested floor: older releases predate some
# ASYNCIFY/-flto flag semantics the leg relies on.
.PHONY: check-wasm-prereqs
## Report WebAssembly toolchain prerequisites and pixman staging status
check-wasm-prereqs:
	@echo "WebAssembly toolchain (tested emcc floor: 3.1.45):"
	@for t in emcc emar node file curl autoreconf automake libtool; do \
	    if command -v $$t >/dev/null 2>&1; then \
	        printf '  %-11s %s\n' "$$t" "`command -v $$t`"; \
	    else \
	        printf '  %-11s MISSING\n' "$$t"; \
	    fi; \
	done
	@emcc --version 2>/dev/null | head -1 | sed 's/^/  emcc:       /' || true
	@if [ -s $(OUT)/wasm-sysroot/lib/libpixman-1.a ]; then \
	    echo "  pixman:     staged ($(OUT)/wasm-sysroot/lib/libpixman-1.a)"; \
	elif [ -n "$(PIXMAN_WASM_TARBALL)" ]; then \
	    echo "  pixman:     offline tarball set (PIXMAN_WASM_TARBALL)"; \
	else \
	    echo "  pixman:     will fetch pixman-$(PIXMAN_VERSION) from the mirror"; \
	fi

# One-command coverage of every wasm smoke. Each target skips cleanly without
# emcc, so this stays green on a toolchain-free CI. Not folded into the default
# `check`: the example smoke fetches/builds pixman (network plus host autotools),
# which the base test gate must not require. check-wasm-motif (mk/wasm-motif.mk)
# is a heavier separate build, so it stays out of the aggregate; run it alongside
# in CI.
.PHONY: check-wasm-all
## Run every WebAssembly smoke (examples + libXt + Xaw + yield + node); skips without emcc
check-wasm-all: check-wasm check-wasm-libxt check-wasm-xaw check-wasm-yield \
    check-wasm-node
