# WebAssembly Mosaic build (showcase app).

ifeq ($(WASM),1)

MOSAIC_WASM_DIR := $(OUT)/wasm-mosaic
MOSAIC_WASM_WORK := $(MOSAIC_WASM_DIR)/source
MOSAIC_WASM_SYSROOT := $(MOSAIC_WASM_DIR)/sysroot
MOSAIC_WASM_LIBDIR := $(MOSAIC_WASM_DIR)/lib-aliases
MOSAIC_WASM_STAMP := $(MOSAIC_WASM_DIR)/.build-stamp
MOSAIC_WASM_HTML := $(OUT)/mosaic.html
MOSAIC_WASM_LOG := $(abspath $(MOSAIC_WASM_DIR))/build.log

MOSAIC_WASM_REAL := libX11.a:$(abspath $(TARGET)) \
    libXt.a:$(abspath $(LIBXT_TARGET)) libXpm.a:$(abspath $(LIBXPM_TARGET)) \
    libXmu.a:$(abspath $(XMU_COMPAT_TARGET)) \
    libXm.a:$(abspath $(MOTIF_WASM_LIBXM)) \
    libMrm.a:$(abspath $(MOTIF_WASM_LIBMRM))
MOSAIC_WASM_EMPTY := libXext.a

MOSAIC_WASM_HOME := file:///usr/local/share/mosaic/index.html
MOSAIC_WASM_HOME_DEFINE := -DHOME_PAGE_DEFAULT=\\\"$(MOSAIC_WASM_HOME)\\\"
MOSAIC_WASM_DATA_PRELOAD := \
    --preload-file $(abspath tests/ui/fixtures/mosaic)@/usr/local/share/mosaic

MOSAIC_WASM_INC := -I$(abspath $(MOSAIC_WASM_SYSROOT)) \
    -I$(abspath $(OUT)/upstream/include) -I$(abspath include) \
    -I$(abspath $(MOTIF_WASM_BUILD_DIR)/lib) -I$(abspath $(MOTIF_SRC_DIR)/lib) \
    -I$(abspath $(MOTIF_WASM_BUILD_DIR)) -DNARROWPROTO \
    $(SDL_CPPFLAGS) $(PIXMAN_CFLAGS)

MOSAIC_WASM_USER_CFLAGS := $(filter-out -std=c99 -Wall -Wextra,$(CFLAGS) $(CFLAGS_EXTRA))
MOSAIC_WASM_CFLAGS := $(OPTFLAGS) -std=gnu89 -fcommon -Wno-everything \
    -DMOTIF -DMOTIF1_2 -DLINUX -DXMOSAIC -D_GNU_SOURCE -D_DARWIN_C_SOURCE \
    $(MOSAIC_WASM_INC) $(MOSAIC_WASM_USER_CFLAGS)

MOSAIC_WASM_XLIBS := $(abspath $(MOTIF_WASM_LIBXM)) \
    $(abspath $(MOTIF_WASM_LIBMRM)) $(abspath $(LIBXT_TARGET)) \
    $(abspath $(LIBXPM_TARGET)) $(abspath $(XMU_COMPAT_TARGET)) \
    $(abspath $(TARGET)) $(PIXMAN_LIBS)

# Mosaic embeds libXpm-derived parser internals but still calls public libXpm.
MOSAIC_WASM_LDFLAGS := $(abspath $(WASM_APP_YIELD_OBJS)) $(SDL_PORT_FLAGS) \
    -sASYNCIFY -Wl,--wrap=poll -Wl,--wrap=select \
    -Wl,--allow-multiple-definition -sALLOW_MEMORY_GROWTH \
    -sSTACK_SIZE=1048576 -sASYNCIFY_STACK_SIZE=24576 \
    -sEMULATE_FUNCTION_POINTER_CASTS=1 $(WASM_FONT_PRELOAD) \
    $(MOSAIC_WASM_DATA_PRELOAD) -L$(abspath $(MOSAIC_WASM_LIBDIR)) \
    -Wl,--start-group

$(MOSAIC_WASM_STAMP): $(MOSAIC_SOURCE_STAMP) $(MOSAIC_PATCHES) $(TARGET) \
    $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XMU_COMPAT_TARGET) \
    $(MOTIF_WASM_LIBXM) $(MOTIF_WASM_LIBMRM) $(WASM_APP_YIELD_OBJS) \
    $(WASM_FONT_STAMP) $(WASM_SHELL) mk/wasm-mosaic.mk
	@echo "  SYSROOT mosaic (wasm)"
	$(Q)rm -rf $(MOSAIC_WASM_DIR)
	$(Q)mkdir -p $(MOSAIC_WASM_SYSROOT)/X11/extensions $(MOSAIC_WASM_SYSROOT)/Xm \
	    $(MOSAIC_WASM_LIBDIR)
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/*; do \
	    b=$$(basename "$$e"); [ "$$b" = extensions ] && continue; \
	    ln -sf "$$e" "$(MOSAIC_WASM_SYSROOT)/X11/$$b"; done
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/extensions/* \
	             $(abspath include)/X11/extensions/*; do \
	    ln -sf "$$e" "$(MOSAIC_WASM_SYSROOT)/X11/extensions/$$(basename "$$e")"; done
	$(Q)for e in $(abspath include)/X11/*; do \
	    b=$$(basename "$$e"); [ -e "$(MOSAIC_WASM_SYSROOT)/X11/$$b" ] || \
	        ln -sf "$$e" "$(MOSAIC_WASM_SYSROOT)/X11/$$b"; done
	$(Q)for h in $(abspath $(MOTIF_SRC_DIR))/lib/Xm/*.h \
	             $(abspath $(MOTIF_WASM_BUILD_DIR))/lib/Xm/*.h; do \
	    ln -sf "$$h" "$(MOSAIC_WASM_SYSROOT)/Xm/$$(basename "$$h")"; done
	@echo "  ALIAS   mosaic wasm libs"
	$(Q)for pair in $(MOSAIC_WASM_REAL); do \
	    a="$${pair%%:*}"; t="$${pair##*:}"; ln -sf "$$t" "$(MOSAIC_WASM_LIBDIR)/$$a"; \
	done
	$(Q)for a in $(MOSAIC_WASM_EMPTY); do $(AR) rcs "$(MOSAIC_WASM_LIBDIR)/$$a"; done
	@echo "  MAKE    mosaic (wasm)"
	$(Q)rm -rf $(MOSAIC_WASM_WORK) && mkdir -p $(MOSAIC_WASM_WORK)
	$(Q)tar --exclude .git --exclude '*.o' --exclude '*.a' --exclude src/Mosaic \
	    -cf - -C $(MOSAIC_DIR) . | tar -xf - -C $(MOSAIC_WASM_WORK)
	$(Q)set -e; for patch in $(MOSAIC_PATCHES); do \
	    patch -d $(MOSAIC_WASM_WORK) -p1 < "$$patch"; done
	$(Q)touch $(MOSAIC_WASM_WORK)/config.h
	$(Q)env -u MAKEFLAGS -u MFLAGS emmake $(MAKE) -C $(MOSAIC_WASM_WORK) \
	    -f makefiles/Makefile.linux MAKEFLAGS= MFLAGS= \
	    CC='$(CC)' AR='$(AR)' RANLIB='emranlib' \
	    CFLAGS='$(MOSAIC_WASM_CFLAGS)' \
	    sysconfigflags='-DMOTIF -DMOTIF1_2 -DLINUX' \
	    prereleaseflags='' \
	    customflags='$(MOSAIC_WASM_HOME_DEFINE)' \
	    xinc='$(MOSAIC_WASM_INC)' \
	    xlibs='$(MOSAIC_WASM_XLIBS) -lm -Wl,--end-group' \
	    ldflags='$(MOSAIC_WASM_LDFLAGS)' \
	    pngflags='' pnglibs='' jpegflags='' jpeglibs='' \
	    krbflags='' krblibs='' syslibs='-lm' \
	    default > $(MOSAIC_WASM_LOG) 2>&1 || { \
	        echo "  FAIL    see $(MOSAIC_WASM_LOG)" >&2; \
	        tail -60 $(MOSAIC_WASM_LOG) >&2; exit 1; }
	$(Q)if [ ! -s $(MOSAIC_WASM_WORK)/src/Mosaic.wasm ]; then \
	    echo "  FAIL    Mosaic.wasm not produced; see $(MOSAIC_WASM_LOG)" >&2; \
	    tail -60 $(MOSAIC_WASM_LOG) >&2; exit 1; fi
	$(Q)touch $@

$(MOSAIC_WASM_HTML): $(MOSAIC_WASM_STAMP) $(WASM_SHELL) mk/wasm-mosaic.mk
	@echo "  HTML    $@"
	$(Q)cp $(MOSAIC_WASM_WORK)/src/Mosaic $(OUT)/mosaic.js
	$(Q)cp $(MOSAIC_WASM_WORK)/src/Mosaic.wasm $(OUT)/mosaic.wasm
	$(Q)test ! -e $(MOSAIC_WASM_WORK)/src/Mosaic.data || \
	    cp $(MOSAIC_WASM_WORK)/src/Mosaic.data $(OUT)/mosaic.data
	$(Q)sed 's#{{{ SCRIPT }}}#<script src="mosaic.js"></script>#' \
	    $(WASM_SHELL) > $@

.PHONY: mosaic-wasm
mosaic-wasm: $(MOSAIC_WASM_HTML)

endif

.PHONY: check-wasm-mosaic
## Build the wasm Mosaic browser artifact (skips when emcc is unavailable)
check-wasm-mosaic:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm-mosaic (emcc not found)"; exit 0; fi; \
	$(MAKE) WASM=1 mosaic-wasm || exit 1; \
	for f in $(OUT)/mosaic.html $(OUT)/mosaic.js $(OUT)/mosaic.wasm \
	         $(OUT)/mosaic.data; do \
	    if [ ! -s "$$f" ]; then echo "check-wasm-mosaic: missing $$f" >&2; exit 1; fi; \
	done; \
	case `file -b $(OUT)/mosaic.wasm` in *WebAssembly*) ;; \
	    *) echo "check-wasm-mosaic: not a wasm module" >&2; exit 1;; esac; \
	if command -v node >/dev/null 2>&1; then \
	    node scripts/wasm-node-smoke.mjs $(OUT)/mosaic.wasm || exit 1; \
	    node scripts/wasm-paint-check.mjs $(OUT) mosaic || exit 1; \
	else \
	    if [ "$${WASM_PAINT_REQUIRED:-0}" = 1 ]; then \
	        echo "check-wasm-mosaic: node missing for required paint check" >&2; exit 1; fi; \
	    echo "  WARN    check-wasm-mosaic: node missing, skipped module smoke" >&2; \
	fi; \
	echo "  OK      check-wasm-mosaic (Mosaic cross-compiled to a wasm module)"
