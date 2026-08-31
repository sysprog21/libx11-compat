# WebAssembly xnedit build (first real showcase app).
#
# xnedit builds through a hand-written Makefile that takes CC / C_OPT_FLAGS /
# LD_OPT_FLAGS and links -lXm -lXt -lX11 -lXrender -lm -lpthread plus pkg-config
# xft fontconfig. That is toolchain- and library-parameterized, so the wasm
# build reuses the native source clone (XNEDIT_SOURCE_STAMP) and drives the same
# `make linux` with CC=emcc, the wasm toolkit archives, and the app-yield link.
#
# Three wasm-specific arrangements:
#  - Lib aliases: emcc resolves -l<name> to lib<name>.a. libX11/Xt/Xm/Xpm/Xmu/Mrm
#    alias the real wasm archives; libXft/libXrender/libXext/libfontconfig alias
#    EMPTY archives, because those symbols already live in the core (libX11) and
#    aliasing them to the core too would duplicate every core symbol.
#  - --start-group: xnedit's LIBS has a fixed order with circular Motif<->Xt
#    references and no group. Inject -Wl,--start-group (with -lXpm -lXmu -lMrm
#    that Motif needs but xnedit does not name) through LD_OPT_FLAGS, and close it
#    with -Wl,--end-group tucked into the wasm fontconfig.pc Libs, which the
#    Makefile appends last.
#  - Output: the Makefile links `-o xnedit` (emcc emits the JS loader there plus
#    xnedit.wasm and, from --preload-file, xnedit.data). A post step copies the
#    JS to xnedit.js and wraps it in the themed launcher.

# comma for use inside $(call ...) argument lists (a literal , splits args).
comma := ,

ifeq ($(WASM),1)

XNEDIT_WASM_DIR := $(OUT)/wasm-xnedit
XNEDIT_WASM_WORK := $(XNEDIT_WASM_DIR)/source
XNEDIT_WASM_SYSROOT := $(XNEDIT_WASM_DIR)/sysroot
XNEDIT_WASM_LIBDIR := $(XNEDIT_WASM_DIR)/lib-aliases
XNEDIT_WASM_PCDIR := $(XNEDIT_WASM_DIR)/pkgconfig
XNEDIT_WASM_STAMP := $(XNEDIT_WASM_DIR)/.build-stamp
XNEDIT_WASM_HTML := $(OUT)/xnedit.html
XNEDIT_WASM_LOG := $(abspath $(XNEDIT_WASM_DIR))/build.log

# Real archives the aliases point at, and the empty-alias names.
XNEDIT_WASM_REAL := libX11.a:$(TARGET) libXt.a:$(LIBXT_TARGET) \
    libXpm.a:$(LIBXPM_TARGET) libXmu.a:$(XMU_COMPAT_TARGET) \
    libXm.a:$(MOTIF_WASM_LIBXM) libMrm.a:$(MOTIF_WASM_LIBMRM)
XNEDIT_WASM_EMPTY := libXft.a libXrender.a libXext.a libfontconfig.a

# xnedit's own headers resolve <X11/*.h> and <Xm/*.h> from the sysroot; add the
# Motif generated/source headers and the core SDL/pixman flags. Deliberately NOT
# WASM_LIBXT_TEST_CPPFLAGS: that carries -DHAVE_CONFIG_H (libXt-specific), which
# would make xnedit's util/*.c pull a nonexistent ../config.h.
# All -I absolute: xnedit's make cd's into util/ Xlt/ source/ before compiling,
# so a relative path would resolve against the wrong dir (fontconfig.h lives at
# include/fontconfig/, Xft.h at include/X11/Xft/).
XNEDIT_WASM_INC := -I$(abspath $(XNEDIT_WASM_SYSROOT)) \
    -I$(abspath $(MOTIF_WASM_BUILD_DIR)/lib) -I$(abspath $(MOTIF_SRC_DIR)/lib) \
    -I$(abspath $(MOTIF_WASM_BUILD_DIR)) -I$(abspath $(OUT)/upstream/include) \
    -I$(abspath include) -DNARROWPROTO $(SDL_CPPFLAGS) $(PIXMAN_CFLAGS)

# Compile flags fed to xnedit's C_OPT_FLAGS. The Makefile adds -std=gnu99 and its
# own host -I paths (nonexistent here, harmless). -include stdlib.h/sys/stat.h
# mirrors the native xnedit build: they pull sys/types.h so textBuf.h's ssize_t
# resolves under emcc's stricter headers.
XNEDIT_WASM_CFLAGS = $(OPTFLAGS) -D_GNU_SOURCE -include stdlib.h \
    -include sys/types.h -include sys/stat.h \
    $(XNEDIT_WASM_INC) $(FP_CFLAGS) $(CFLAGS_EXTRA)

# Link flags fed to LD_OPT_FLAGS: the yield objects and the browser-yield/ font
# link, then --start-group and the Motif-only archives; the group closes in
# fontconfig.pc.
# The core archive is added by PATH, not -lX11: emcc treats X11 as a host system
# library it does not provide and silently drops the Makefile's -lX11, so the
# alias would never be linked and every core symbol (XSetErrorHandler, the Xrm
# quark functions, ...) would be undefined. A positional archive inside the group
# is immune to that.
# xnedit's own libNUtil/libXmL/libXlt are listed once by the Makefile before the
# libraries, but their members cross-reference each other and the Motif/core
# archives, so a single pass leaves util symbols (NEditMalloc, GetMotifStability,
# ...) undefined. List them again inside the group (harmless duplicate) so
# wasm-ld resolves the cycle.
XNEDIT_WASM_GROUPLIBS = $(abspath $(XNEDIT_WASM_WORK))/util/libNUtil.a \
    $(abspath $(XNEDIT_WASM_WORK))/Microline/XmL/libXmL.a \
    $(abspath $(XNEDIT_WASM_WORK))/Xlt/libXlt.a
# libXm/libMrm are listed positionally because emcc 6.0.4 LTO will not pull the
# XmDisplay/RepType/DropSMgr members through on-demand -lXm alone.
XNEDIT_WASM_LDFLAGS = $(abspath $(WASM_APP_YIELD_OBJS)) -sASYNCIFY \
    -Wl,--wrap=poll -Wl,--wrap=select $(WASM_FONT_PRELOAD) \
    $(WASM_CJK_FONT_PRELOAD) \
    -sALLOW_MEMORY_GROWTH \
    -sEXPORTED_FUNCTIONS=_main,_libx11CompatInjectTextInput,_libx11CompatInjectKey \
    -sEXPORTED_RUNTIME_METHODS=ccall \
    -Wl,--start-group $(XNEDIT_WASM_GROUPLIBS) \
    $(abspath $(MOTIF_WASM_LIBXM)) $(abspath $(MOTIF_WASM_LIBMRM)) \
    $(abspath $(TARGET)) $(PIXMAN_LIBS) \
    -L$(abspath $(XNEDIT_WASM_LIBDIR)) -lXpm -lXmu -lMrm

# Emit one wasm .pc for xnedit's pkg-config xft/fontconfig probe. Description is
# mandatory -- pkg-config silently ignores a package without it (and then falls
# back to the host fontconfig).
define xnedit_wasm_pc
	@{ echo "Name: $(1)"; echo "Description: libx11-compat wasm $(1)"; \
	   echo "Version: 1.0"; echo "Cflags: $(3)"; echo "Libs: $(2)"; } \
	   > $(XNEDIT_WASM_PCDIR)/$(1).pc
endef

$(XNEDIT_WASM_STAMP): $(XNEDIT_SOURCE_STAMP) $(TARGET) $(LIBXT_TARGET) \
    $(LIBXPM_TARGET) $(XMU_COMPAT_TARGET) $(MOTIF_WASM_LIBXM) \
    $(MOTIF_WASM_LIBMRM) $(WASM_APP_YIELD_OBJS) $(WASM_FONT_STAMP) \
    $(NOTO_CJK_CACHE) $(WASM_SHELL) mk/wasm-xnedit.mk
	@echo "  SYSROOT xnedit (wasm)"
	$(Q)rm -rf $(XNEDIT_WASM_DIR)
	$(Q)mkdir -p $(XNEDIT_WASM_SYSROOT)/X11/extensions $(XNEDIT_WASM_SYSROOT)/Xm \
	    $(XNEDIT_WASM_LIBDIR) $(XNEDIT_WASM_PCDIR)
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/*; do \
	    b=$$(basename "$$e"); [ "$$b" = extensions ] && continue; \
	    ln -sf "$$e" "$(XNEDIT_WASM_SYSROOT)/X11/$$b"; done
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/extensions/* \
	             $(abspath include)/X11/extensions/*; do \
	    ln -sf "$$e" "$(XNEDIT_WASM_SYSROOT)/X11/extensions/$$(basename "$$e")"; done
	$(Q)for e in $(abspath include)/X11/*; do \
	    b=$$(basename "$$e"); [ -e "$(XNEDIT_WASM_SYSROOT)/X11/$$b" ] || \
	        ln -sf "$$e" "$(XNEDIT_WASM_SYSROOT)/X11/$$b"; done
	$(Q)for h in $(abspath $(MOTIF_SRC_DIR))/lib/Xm/*.h \
	             $(abspath $(MOTIF_WASM_BUILD_DIR))/lib/Xm/*.h; do \
	    ln -sf "$$h" "$(XNEDIT_WASM_SYSROOT)/Xm/$$(basename "$$h")"; done
	@echo "  ALIAS   xnedit wasm libs"
	$(Q)for pair in $(XNEDIT_WASM_REAL); do \
	    a="$${pair%%:*}"; t="$${pair##*:}"; \
	    ln -sf "$(abspath .)/$$t" "$(XNEDIT_WASM_LIBDIR)/$$a"; done
	$(Q)for a in $(XNEDIT_WASM_EMPTY); do \
	    $(AR) rcs "$(XNEDIT_WASM_LIBDIR)/$$a"; done
	$(call xnedit_wasm_pc,xft,,$(XNEDIT_WASM_INC))
	$(call xnedit_wasm_pc,fontconfig,-Wl$(comma)--end-group,)
	@echo "  MAKE    xnedit (wasm)"
	$(Q)rm -rf $(XNEDIT_WASM_WORK) && mkdir -p $(XNEDIT_WASM_WORK)
	$(Q)tar -cf - -C $(XNEDIT_SRC_DIR) . | tar -xf - -C $(XNEDIT_WASM_WORK)
	$(Q)PKG_CONFIG_LIBDIR=$(abspath $(XNEDIT_WASM_PCDIR)) \
	    PKG_CONFIG_PATH=$(abspath $(XNEDIT_WASM_PCDIR)) \
	    env -u MAKEFLAGS -u MFLAGS \
	    emmake $(MAKE) -C $(XNEDIT_WASM_WORK) linux CC='$(CC)' AR='emar' \
	    C_OPT_FLAGS='$(XNEDIT_WASM_CFLAGS)' \
	    LD_OPT_FLAGS='$(XNEDIT_WASM_LDFLAGS)' \
	    >$(XNEDIT_WASM_LOG) 2>&1; \
	    if [ ! -s $(XNEDIT_WASM_WORK)/source/xnedit.wasm ]; then \
	        echo "  FAIL    see $(XNEDIT_WASM_LOG)" >&2; \
	        tail -50 $(XNEDIT_WASM_LOG) >&2; exit 1; \
	    fi
	$(Q)touch $@

# Wrap the emscripten JS loader the Makefile produced (named "xnedit") in the
# themed launcher: copy it to xnedit.js and fill the shell's {{{ SCRIPT }}} slot
# with a script tag for it.
$(XNEDIT_WASM_HTML): $(XNEDIT_WASM_STAMP) $(WASM_SHELL) mk/wasm-xnedit.mk
	@echo "  HTML    $@"
	$(Q)cp $(XNEDIT_WASM_WORK)/source/xnedit $(OUT)/xnedit.js
	$(Q)cp $(XNEDIT_WASM_WORK)/source/xnedit.wasm $(OUT)/xnedit.wasm
	$(Q)test ! -e $(XNEDIT_WASM_WORK)/source/xnedit.data || \
	    cp $(XNEDIT_WASM_WORK)/source/xnedit.data $(OUT)/xnedit.data
	$(Q)sed 's#{{{ SCRIPT }}}#<script>Module.libx11CompatHasCcallBridge=true;</script><script src="xnedit.js"></script>#' \
	    $(WASM_SHELL) > $@

.PHONY: xnedit-wasm
xnedit-wasm: $(XNEDIT_WASM_HTML)

endif

.PHONY: check-wasm-xnedit
## Build the wasm xnedit browser artifact (skips when emcc is unavailable)
check-wasm-xnedit:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm-xnedit (emcc not found)"; \
	    exit 0; \
	fi; \
	$(MAKE) WASM=1 xnedit-wasm || exit 1; \
	for f in $(OUT)/xnedit.html $(OUT)/xnedit.js $(OUT)/xnedit.wasm; do \
	    if [ ! -s "$$f" ]; then echo "check-wasm-xnedit: missing $$f" >&2; exit 1; fi; \
	done; \
	case `file -b $(OUT)/xnedit.wasm` in \
	    *WebAssembly*) ;; \
	    *) echo "check-wasm-xnedit: not a wasm module" >&2; exit 1;; \
	esac; \
	$(call wasm_paint_check,xnedit) \
	echo "  OK      check-wasm-xnedit (xnedit cross-compiled to a wasm module)"
