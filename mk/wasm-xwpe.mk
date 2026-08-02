# WebAssembly xwpe build (showcase app).

comma := ,

ifeq ($(WASM),1)

XWPE_WASM_DIR := $(OUT)/wasm-xwpe
XWPE_WASM_WORK := $(XWPE_WASM_DIR)/source
XWPE_WASM_SYSROOT := $(XWPE_WASM_DIR)/sysroot
XWPE_WASM_LIBDIR := $(XWPE_WASM_DIR)/lib-aliases
XWPE_WASM_PCDIR := $(XWPE_WASM_DIR)/pkgconfig
XWPE_WASM_STAMP := $(XWPE_WASM_DIR)/.build-stamp
XWPE_WASM_HTML := $(OUT)/xwpe.html
XWPE_WASM_LOG := $(abspath $(XWPE_WASM_DIR))/build.log
XWPE_WASM_COMPAT := compat/xwpe-wasm
XWPE_WASM_SAMPLE_SOURCES := \
    examples/2048.c \
    examples/clock.c \
    examples/mandel.c \
    examples/paint.c \
    compat/wasm-idle-yield.c \
    compat/wasm-poll-yield.c

XWPE_WASM_REAL := libX11.a:$(abspath $(TARGET)) \
    libXmu.a:$(abspath $(XMU_COMPAT_TARGET))
XWPE_WASM_EMPTY := libXext.a libXft.a libfontconfig.a

XWPE_WASM_INC := -I$(abspath $(XWPE_WASM_WORK)) \
    -I$(abspath $(XWPE_WASM_SYSROOT)) \
    -I$(abspath $(OUT)/upstream/include) -I$(abspath include) \
    -I$(abspath include/libxt-build) -DNARROWPROTO \
    $(SDL_CPPFLAGS) $(PIXMAN_CFLAGS)

XWPE_WASM_CFLAGS := $(OPTFLAGS) -std=gnu17 -Wno-everything \
    $(XWPE_WASM_INC) $(CFLAGS_EXTRA)

XWPE_WASM_DATA_PRELOAD := \
    --preload-file $(abspath $(XWPE_WASM_WORK))/syntax_def@/usr/local/lib/xwpe/syntax_def \
    --preload-file $(abspath $(XWPE_WASM_WORK))/help.xwpe_eng@/usr/local/lib/xwpe/help.xwpe_eng \
    --preload-file $(abspath $(XWPE_WASM_WORK))/help.key_eng@/usr/local/lib/xwpe/help.key_eng \
    $(foreach src,$(XWPE_WASM_SAMPLE_SOURCES),--preload-file $(abspath $(src))@/home/web/src/$(notdir $(src)))

XWPE_WASM_LDFLAGS := $(abspath $(WASM_APP_YIELD_OBJS)) $(SDL_PORT_FLAGS) \
    -sASYNCIFY -Wl,--wrap=poll -Wl,--wrap=select -sALLOW_MEMORY_GROWTH \
    -sSTACK_SIZE=1048576 -sASYNCIFY_STACK_SIZE=24576 \
    -sEMULATE_FUNCTION_POINTER_CASTS=1 -sEXPORTED_RUNTIME_METHODS=ccall \
    $(WASM_FONT_PRELOAD) \
    $(XWPE_WASM_DATA_PRELOAD) -L$(abspath $(XWPE_WASM_LIBDIR)) \
    -Wl,--start-group

XWPE_WASM_LIBS := $(abspath $(XMU_COMPAT_TARGET)) $(abspath $(TARGET)) \
    $(PIXMAN_LIBS) -lm -lz -Wl,--end-group

define xwpe_wasm_pc
	@{ echo "Name: $(1)"; echo "Description: libx11-compat wasm $(1)"; \
	   echo "Version: 1.0"; echo "Cflags: $(2)"; echo "Libs: $(3)"; } \
	   > $(XWPE_WASM_PCDIR)/$(1).pc
endef

$(XWPE_WASM_STAMP): $(XWPE_SOURCE_STAMP) $(XWPE_PATCHES) $(TARGET) \
    $(XMU_COMPAT_TARGET) $(WASM_APP_YIELD_OBJS) $(WASM_FONT_STAMP) \
    $(WASM_SHELL) mk/wasm-xwpe.mk $(XWPE_WASM_COMPAT)/curses.h \
    $(XWPE_WASM_COMPAT)/term.h $(XWPE_WASM_COMPAT)/execinfo.h \
    $(XWPE_WASM_COMPAT)/json-c/json.h $(XWPE_WASM_SAMPLE_SOURCES)
	@echo "  SYSROOT xwpe (wasm)"
	$(Q)rm -rf $(XWPE_WASM_DIR)
	$(Q)mkdir -p $(XWPE_WASM_SYSROOT)/X11/extensions $(XWPE_WASM_LIBDIR) \
	    $(XWPE_WASM_PCDIR)
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/*; do \
	    b=$$(basename "$$e"); [ "$$b" = extensions ] && continue; \
	    ln -sf "$$e" "$(XWPE_WASM_SYSROOT)/X11/$$b"; done
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/extensions/* \
	             $(abspath include)/X11/extensions/*; do \
	    ln -sf "$$e" "$(XWPE_WASM_SYSROOT)/X11/extensions/$$(basename "$$e")"; done
	$(Q)for e in $(abspath include)/X11/*; do \
	    b=$$(basename "$$e"); [ -e "$(XWPE_WASM_SYSROOT)/X11/$$b" ] || \
	        ln -sf "$$e" "$(XWPE_WASM_SYSROOT)/X11/$$b"; done
	@echo "  ALIAS   xwpe wasm libs"
	$(Q)for pair in $(XWPE_WASM_REAL); do \
	    a="$${pair%%:*}"; t="$${pair##*:}"; ln -sf "$$t" "$(XWPE_WASM_LIBDIR)/$$a"; \
	done
	$(Q)for a in $(XWPE_WASM_EMPTY); do $(AR) rcs "$(XWPE_WASM_LIBDIR)/$$a"; done
	$(call xwpe_wasm_pc,ncursesw,$(XWPE_WASM_INC),)
	$(call xwpe_wasm_pc,ncurses,$(XWPE_WASM_INC),)
	$(call xwpe_wasm_pc,json-c,$(XWPE_WASM_INC),)
	$(call xwpe_wasm_pc,xft,$(XWPE_WASM_INC),-L$(abspath $(XWPE_WASM_LIBDIR)) -lXft -lfontconfig)
	$(call xwpe_wasm_pc,fontconfig,$(XWPE_WASM_INC),-L$(abspath $(XWPE_WASM_LIBDIR)) -lfontconfig)
	@echo "  STAGE   xwpe source (wasm)"
	$(Q)rm -rf $(XWPE_WASM_WORK) && mkdir -p $(XWPE_WASM_WORK)
	$(Q)tar --exclude .git --exclude '*.o' --exclude '*.d' --exclude we \
	    --exclude we.wasm --exclude we.data -cf - -C $(XWPE_SRC_DIR) . | \
	    tar -xf - -C $(XWPE_WASM_WORK)
	$(Q)set -e; for patch in $(XWPE_PATCHES); do \
	    patch --fuzz=0 --no-backup-if-mismatch -d $(XWPE_WASM_WORK) -p1 < "$$patch"; \
	done
	$(Q)cp $(XWPE_WASM_COMPAT)/curses.h $(XWPE_WASM_WORK)/curses.h
	$(Q)cp $(XWPE_WASM_COMPAT)/term.h $(XWPE_WASM_WORK)/term.h
	$(Q)cp $(XWPE_WASM_COMPAT)/execinfo.h $(XWPE_WASM_WORK)/execinfo.h
	$(Q)mkdir -p $(XWPE_WASM_WORK)/json-c
	$(Q)cp $(XWPE_WASM_COMPAT)/json-c/json.h $(XWPE_WASM_WORK)/json-c/json.h
	@echo "  CONF    xwpe (wasm)"
	$(Q)cd $(XWPE_WASM_WORK) && autoreconf -fi >>$(XWPE_WASM_LOG) 2>&1 && \
	    PKG_CONFIG_LIBDIR=$(abspath $(XWPE_WASM_PCDIR)) \
	    PKG_CONFIG_PATH=$(abspath $(XWPE_WASM_PCDIR)) \
	    env -u MAKEFLAGS -u MFLAGS \
	    emconfigure ./configure --host=wasm32-unknown-emscripten \
	        --prefix=/usr/local --without-gpm \
	        --x-includes=$(abspath $(XWPE_WASM_SYSROOT)) \
	        --x-libraries=$(abspath $(XWPE_WASM_LIBDIR)) \
	        CC='$(CC)' AR='$(AR)' RANLIB='emranlib' \
	        CPPFLAGS='$(XWPE_WASM_INC)' CFLAGS='$(XWPE_WASM_CFLAGS)' \
	        LDFLAGS='-L$(abspath $(XWPE_WASM_LIBDIR))' \
	        NCURSESW_CFLAGS='$(XWPE_WASM_INC)' NCURSESW_LIBS=' ' \
	        JSONC_CFLAGS='$(XWPE_WASM_INC)' JSONC_LIBS=' ' \
	        XFT_CFLAGS='$(XWPE_WASM_INC)' \
	        XFT_LIBS='-L$(abspath $(XWPE_WASM_LIBDIR)) -lXft -lfontconfig' \
	        ac_cv_search_openpty='none required' ac_cv_lib_z_inflate=yes \
	        ac_cv_header_pty_h=no ac_cv_header_util_h=no ac_cv_header_libutil_h=no \
	        >>$(XWPE_WASM_LOG) 2>&1 || { \
	            echo "  FAIL    see $(XWPE_WASM_LOG)" >&2; \
	            tail -60 $(XWPE_WASM_LOG) >&2; exit 1; }
	@echo "  MAKE    xwpe (wasm)"
	$(Q)env -u MAKEFLAGS -u MFLAGS emmake $(MAKE) -C $(XWPE_WASM_WORK) \
	    AR='$(AR)' RANLIB='emranlib' \
	    LDFLAGS='$(XWPE_WASM_LDFLAGS)' LIBS='$(XWPE_WASM_LIBS)' \
	    >>$(XWPE_WASM_LOG) 2>&1 || { \
	        echo "  FAIL    see $(XWPE_WASM_LOG)" >&2; \
	        tail -60 $(XWPE_WASM_LOG) >&2; exit 1; }
	$(Q)if [ ! -s $(XWPE_WASM_WORK)/we.wasm ]; then \
	    echo "  FAIL    we.wasm not produced; see $(XWPE_WASM_LOG)" >&2; \
	    tail -60 $(XWPE_WASM_LOG) >&2; exit 1; fi
	$(Q)touch $@

# The stamp builds we{,.wasm,.data} once; each browser artifact is a cheap copy
# off it. One rule per file (not one recipe emitting all four) so deleting any
# single artifact restores just it. Explicit targets cannot share a recipe
# without GNU make 4.3's &:, which the macOS 3.81 floor lacks.
$(OUT)/xwpe.js: $(XWPE_WASM_STAMP)
	@echo "  COPY    $@"
	$(Q)cp $(XWPE_WASM_WORK)/we $@
	$(Q)perl -0pi -e 's/we\.(wasm|data)/xwpe.$$1/g' $@
$(OUT)/xwpe.wasm: $(XWPE_WASM_STAMP)
	@echo "  COPY    $@"
	$(Q)cp $(XWPE_WASM_WORK)/we.wasm $@
$(OUT)/xwpe.data: $(XWPE_WASM_STAMP)
	@echo "  COPY    $@"
	$(Q)test ! -e $(XWPE_WASM_WORK)/we.data || cp $(XWPE_WASM_WORK)/we.data $@

$(XWPE_WASM_HTML): $(XWPE_WASM_STAMP) $(WASM_SHELL) mk/wasm-xwpe.mk
	@echo "  HTML    $@"
	$(Q)sed 's#{{{ SCRIPT }}}#<script>Module.thisProgram="xwpe";Module.libx11CompatDisableImeBridge=true;Module.libx11CompatBridgeDomEvents=true;Module.libx11CompatHasCcallBridge=true;Module.libx11CompatAssetVersion="20260802-src-preload";Module.locateFile=function(path,prefix){return /\\.(wasm|data)$$/.test(path)?(prefix||"")+path+"?v="+Module.libx11CompatAssetVersion:(prefix||"")+path;};Module.preRun.push(function(){ENV.XWPE_LIB="/usr/local/lib/xwpe";});</script><script src="xwpe.js?v=20260802-src-preload"></script>#' \
	    $(WASM_SHELL) > $@

.PHONY: xwpe-wasm
xwpe-wasm: $(XWPE_WASM_HTML) $(OUT)/xwpe.js $(OUT)/xwpe.wasm $(OUT)/xwpe.data

endif

.PHONY: check-wasm-xwpe
## Build the wasm xwpe browser artifact (skips when emcc is unavailable)
check-wasm-xwpe:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm-xwpe (emcc not found)"; exit 0; fi; \
	$(MAKE) WASM=1 xwpe-wasm || exit 1; \
	for f in $(OUT)/xwpe.html $(OUT)/xwpe.js $(OUT)/xwpe.wasm $(OUT)/xwpe.data; do \
	    if [ ! -s "$$f" ]; then echo "check-wasm-xwpe: missing $$f" >&2; exit 1; fi; \
	done; \
	case `file -b $(OUT)/xwpe.wasm` in *WebAssembly*) ;; \
	    *) echo "check-wasm-xwpe: not a wasm module" >&2; exit 1;; esac; \
	if command -v node >/dev/null 2>&1; then \
	    node scripts/wasm-node-smoke.mjs $(OUT)/xwpe.wasm || exit 1; \
	    node scripts/wasm-paint-check.mjs $(OUT) xwpe || exit 1; \
	else \
	    if [ "$${WASM_PAINT_REQUIRED:-0}" = 1 ]; then \
	        echo "check-wasm-xwpe: node missing for required paint check" >&2; exit 1; fi; \
	    echo "  WARN    check-wasm-xwpe: node missing, skipped module smoke" >&2; \
	fi; \
	echo "  OK      check-wasm-xwpe (xwpe cross-compiled to a wasm module)"
