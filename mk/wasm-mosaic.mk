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

# Preloaded fixtures are baked into the .data bundle, so a fixture change must
# rebuild. The wildcard catches edits and additions; the directory itself is
# listed too so a deletion or rename (which drops a name from the wildcard but
# bumps the directory mtime) also invalidates the stamp. Flat tree today.
MOSAIC_WASM_FIXTURE_DIR := tests/ui/fixtures/mosaic
MOSAIC_WASM_FIXTURES := $(MOSAIC_WASM_FIXTURE_DIR) \
    $(wildcard $(MOSAIC_WASM_FIXTURE_DIR)/*)

MOSAIC_WASM_INC := -I$(abspath $(MOSAIC_WASM_SYSROOT)) \
    -I$(abspath $(OUT)/upstream/include) -I$(abspath include) \
    -I$(abspath $(MOTIF_WASM_BUILD_DIR)/lib) -I$(abspath $(MOTIF_SRC_DIR)/lib) \
    -I$(abspath $(MOTIF_WASM_BUILD_DIR)) -DNARROWPROTO \
    $(SDL_CPPFLAGS) $(PIXMAN_CFLAGS)

# Mosaic bundles its own copy of the libXpm parser internals in src/xpmread.c
# and src/xpmhash.c. Motif's ImageCache calls public libXpm (XpmReadFileToImage),
# which drags libXpm-compat.a's parser objects into the link, and the internal
# xpm* helpers they carry are the same strong globals mosaic defines. wasm-ld
# before LLVM 20 (the version emsdk 3.1.64 bundles) cannot tolerate duplicate
# definitions at all: --allow-multiple-definition, -z muldefs, and
# --noinhibit-exec all landed together in LLVM 20, so none of them exist there.
# Rather than tolerate the clash, remove it: rename mosaic's copies to a private
# prefix. The rename rides CFLAGS, which reaches every mosaic translation unit,
# so mosaic's definitions and the one cross-file reference (xpmread.c calling into
# xpmhash.c) stay consistent while libXpm keeps the originals. No linker flag is
# needed on any emcc, so this builds on both the pinned 3.1.64 and newer toolchains.
#
# This list is only the strong symbols that actually collide today. mosaic and
# libXpm-compat also both define xpmReadFile, xpmDataClose, xpmFreeInternAttrib,
# xpmInitInternAttrib, and xpmOpenArray, which do not collide now only because
# the post-rename reference graph does not archive-pull the libXpm object that
# defines them under emsdk 3.1.64. If a toolchain bump or a libXpm-compat object
# split pulls one of those and the link fails with a duplicate strong symbol,
# add the offending name here; the rename is the fix, not a linker flag.
MOSAIC_WASM_XPM_DUPES := xpmColorKeys xpmDataTypes xpmGetCmt xpmHashIntern \
    xpmHashSlot xpmHashTableFree xpmHashTableInit xpmNextString xpmNextUI \
    xpmNextWord xpmParseData xpmParseHeader
MOSAIC_WASM_XPM_RENAMES := \
    $(foreach s,$(MOSAIC_WASM_XPM_DUPES),-D$(s)=mosaic_$(s))

MOSAIC_WASM_USER_CFLAGS := $(filter-out -std=c99 -Wall -Wextra,$(CFLAGS) $(CFLAGS_EXTRA))
MOSAIC_WASM_CFLAGS := $(OPTFLAGS) -std=gnu89 -fcommon -Wno-everything \
    -DMOTIF -DMOTIF1_2 -DLINUX -DXMOSAIC -D_GNU_SOURCE -D_DARWIN_C_SOURCE \
    $(MOSAIC_WASM_XPM_RENAMES) \
    $(MOSAIC_WASM_INC) $(MOSAIC_WASM_USER_CFLAGS)

MOSAIC_WASM_XLIBS := $(abspath $(MOTIF_WASM_LIBXM)) \
    $(abspath $(MOTIF_WASM_LIBMRM)) $(abspath $(LIBXT_TARGET)) \
    $(abspath $(LIBXPM_TARGET)) $(abspath $(XMU_COMPAT_TARGET)) \
    $(abspath $(TARGET)) $(PIXMAN_LIBS)

# Mosaic embeds libXpm-derived parser internals but still pulls public libXpm in
# through Motif. The duplicate strong symbols that used to require a link-time
# override are renamed away in MOSAIC_WASM_CFLAGS above, so no such flag is needed.
MOSAIC_WASM_LDFLAGS := $(abspath $(WASM_APP_YIELD_OBJS)) $(SDL_PORT_FLAGS) \
    -sASYNCIFY -Wl,--wrap=poll -Wl,--wrap=select \
    -sALLOW_MEMORY_GROWTH \
    -sSTACK_SIZE=1048576 -sASYNCIFY_STACK_SIZE=24576 \
    -sASYNCIFY_IMPORTS=__call_sighandler -sNO_EXIT_RUNTIME=1 \
    -sEMULATE_FUNCTION_POINTER_CASTS=1 -sEXPORTED_RUNTIME_METHODS=ccall \
    $(WASM_FONT_PRELOAD) \
    $(MOSAIC_WASM_DATA_PRELOAD) -L$(abspath $(MOSAIC_WASM_LIBDIR)) \
    -Wl,--start-group

$(MOSAIC_WASM_STAMP): $(MOSAIC_SOURCE_STAMP) $(MOSAIC_PATCHES) $(TARGET) \
    $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XMU_COMPAT_TARGET) \
    $(MOTIF_WASM_LIBXM) $(MOTIF_WASM_LIBMRM) $(WASM_APP_YIELD_OBJS) \
    $(WASM_FONT_STAMP) $(MOSAIC_WASM_FIXTURES) $(WASM_SHELL) mk/wasm-mosaic.mk
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

# The stamp builds Mosaic.{js,wasm,data} once; each browser artifact is a cheap
# copy off it. One rule per file (not one recipe emitting all four) so deleting
# any single artifact restores just it. Explicit targets cannot share a recipe
# without GNU make 4.3's &:, which the macOS 3.81 floor lacks.
$(OUT)/mosaic.js: $(MOSAIC_WASM_STAMP)
	@echo "  COPY    $@"
	$(Q)cp $(MOSAIC_WASM_WORK)/src/Mosaic $@
$(OUT)/mosaic.wasm: $(MOSAIC_WASM_STAMP)
	@echo "  COPY    $@"
	$(Q)cp $(MOSAIC_WASM_WORK)/src/Mosaic.wasm $@
$(OUT)/mosaic.data: $(MOSAIC_WASM_STAMP)
	@echo "  COPY    $@"
	$(Q)test ! -e $(MOSAIC_WASM_WORK)/src/Mosaic.data || \
	    cp $(MOSAIC_WASM_WORK)/src/Mosaic.data $@

# The pages bundle depends only on each app's .html. xcircuit and xnedit copy
# their module into $(OUT) from inside the .html recipe, but mosaic emits the
# module as separate .js/.wasm/.data targets, so list them as prerequisites
# here; without this, building (and bundling) mosaic.html stages no module and
# the deployed showcase 404s on mosaic.js/.wasm.
$(MOSAIC_WASM_HTML): $(MOSAIC_WASM_STAMP) $(WASM_SHELL) mk/wasm-mosaic.mk \
    $(OUT)/mosaic.js $(OUT)/mosaic.wasm $(OUT)/mosaic.data
	@echo "  HTML    $@"
	$(Q)sed 's#{{{ SCRIPT }}}#<script>document.body.classList.add("fullscreen-app");Module.libx11CompatFullscreenShell=true;Module.libx11CompatSkipInitialFocus=true;Module.libx11CompatBridgeDomEvents=true;Module.libx11CompatHasCcallBridge=true;Module.libx11CompatAssetVersion="20260802-mosaic-input";Module.locateFile=function(path,prefix){var p=path.replace(/^Mosaic\\./,"mosaic.");return /\\.(wasm|data)$$/.test(p)?(prefix||"")+p+"?v="+Module.libx11CompatAssetVersion:(prefix||"")+p;};</script><script src="mosaic.js?v=20260802-mosaic-input"></script>#' \
	    $(WASM_SHELL) > $@

.PHONY: mosaic-wasm
mosaic-wasm: $(MOSAIC_WASM_HTML) $(OUT)/mosaic.js $(OUT)/mosaic.wasm \
    $(OUT)/mosaic.data

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
	$(call wasm_paint_check,mosaic,smoke) \
	echo "  OK      check-wasm-mosaic (Mosaic cross-compiled to a wasm module)"
