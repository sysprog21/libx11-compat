# WebAssembly Motif build: libXm.a and libMrm.a cross-compiled with emcc.
#
# Reuses the arch-independent front of mk/motif.mk (source clone, patches, and
# autoreconf: MOTIF_SRC_STAMP / MOTIF_AUTOGEN_STAMP) and cross-configures Motif
# with emconfigure against the wasm toolkit archives. Only the library is built
# (lib/Xm + lib/Mrm); the demos, the UIL compilers, and the message catalogs
# are off the critical path for the showcase apps, which link libXm/libMrm.
#
# Two non-obvious pins carry over from the proven recipe:
#   - configure conftests and the Motif objects MUST build with CFLAGS matching
#     the wasm toolkit archives, which are -flto bitcode ($(OPTFLAGS)). A
#     non-flto conftest linked against them crashes wasm-opt, and autoconf
#     misreads the crash as a missing libX11 symbol.
#   - config/makestrs is a build-time codegen tool. Under emmake CC=emcc it
#     would compile to a wasm binary that cannot execute, so it is built with
#     the native HOST_CC and touched so lib/Xm reuses it instead of rebuilding.

ifeq ($(WASM),1)

MOTIF_WASM_BUILD_DIR := $(OUT)/wasm-motif
MOTIF_WASM_PC_DIR := $(MOTIF_WASM_BUILD_DIR)/pkgconfig
# The .pc Libs point here rather than at $(OUT): a mixed tree may hold both the
# native libXm.so/.dylib and the wasm .a, and this dir carries only symlinks to
# the wasm archives so wasm-ld never resolves a native object.
MOTIF_WASM_LIBDIR := $(MOTIF_WASM_BUILD_DIR)/lib-archives
MOTIF_WASM_CONFIG_STAMP := $(MOTIF_WASM_BUILD_DIR)/.configure-stamp
MOTIF_WASM_BUILD_STAMP := $(MOTIF_WASM_BUILD_DIR)/.build-stamp
MOTIF_WASM_HOST_MAKESTRS := $(MOTIF_WASM_BUILD_DIR)/config/makestrs
MOTIF_WASM_LIBXM := $(OUT)/libXm.a
MOTIF_WASM_LIBMRM := $(OUT)/libMrm.a

# Wasm toolkit archives the .pc set and the link smoke resolve against. Xext is
# absent by design: its XShape symbols already live in the core archive.
MOTIF_WASM_ARCHIVES_IN := $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) \
    $(XMU_COMPAT_TARGET)

MOTIF_WASM_CFLAGS := $(OPTFLAGS)
MOTIF_WASM_WL := $(abspath $(MOTIF_WASM_LIBDIR))
MOTIF_WASM_INC := -I$(abspath include) -I$(abspath $(OUT))/upstream/include
MOTIF_WASM_BACKING := $(SDL_PORT_FLAGS) $(PIXMAN_LIBS) -lm

# Emit one wasm .pc: $(1) name, $(2) version, $(3) Requires, $(4) extra Cflags,
# $(5) Libs. x11 carries the SDL/pixman backing so configure's AC_LINK tests
# resolve; the toolkit shims inherit it through Requires: x11.
define motif_wasm_pc
	@{ \
	    echo "Name: $(1)"; \
	    echo "Description: libx11-compat wasm $(1) shim"; \
	    echo "Version: $(2)"; \
	    echo "Requires: $(3)"; \
	    echo "Cflags: $(MOTIF_WASM_INC) $(SDL_PORT_FLAGS) $(PIXMAN_CFLAGS) $(4)"; \
	    echo "Libs: $(5)"; \
	} > $(MOTIF_WASM_PC_DIR)/$(1).pc
endef

# Reconfigure when the recipe or any fragment that feeds the .pc set changes:
# OPTFLAGS (wasm.mk), the pixman flags (config.mk), and SDL_PORT_FLAGS (sdl.mk)
# are all baked into the generated .pc content and the configure environment.
$(MOTIF_WASM_CONFIG_STAMP): $(MOTIF_AUTOGEN_STAMP) $(MOTIF_WASM_ARCHIVES_IN) \
    $(WASM_PIXMAN_LIB) mk/wasm-motif.mk mk/wasm.mk mk/config.mk mk/sdl.mk
	@echo "  CONFIG  motif (wasm)"
	$(Q)mkdir -p $(MOTIF_WASM_BUILD_DIR) $(MOTIF_WASM_PC_DIR) $(MOTIF_WASM_LIBDIR)
	$(Q)for a in $(abspath $(MOTIF_WASM_ARCHIVES_IN)); do \
	    ln -sf $$a $(MOTIF_WASM_LIBDIR)/$$(basename $$a); \
	done
	$(call motif_wasm_pc,x11,1.8.13,,,-L$(MOTIF_WASM_WL) -lX11-compat $(MOTIF_WASM_BACKING))
	$(call motif_wasm_pc,xt,1.3.1,x11,-I$(abspath include/libxt-build),-L$(MOTIF_WASM_WL) -lXt-compat)
	$(call motif_wasm_pc,xpm,3.5.19,x11,,-L$(MOTIF_WASM_WL) -lXpm-compat)
	$(call motif_wasm_pc,xmu,1.0,x11,-I$(abspath include/libxt-build),-L$(MOTIF_WASM_WL) -lXmu-compat)
	$(call motif_wasm_pc,xext,1.0,x11,,)
	$(Q)cd $(MOTIF_WASM_BUILD_DIR) && \
	    emconfigure $(abspath $(MOTIF_SRC_DIR))/configure \
	    --host=wasm32-unknown-emscripten \
	    --enable-static --disable-shared \
	    --disable-glw --disable-demos --disable-tests \
	    --without-xft --with-jpeg=no --with-png=no \
	    --with-xrandr=no --with-xrender=no --with-xcursor=no --with-xinerama=no \
	    PKG_CONFIG_PATH=$(abspath $(MOTIF_WASM_PC_DIR)) \
	    CPP="$(CC) -E" CPPFLAGS="-include stdlib.h" CFLAGS="$(MOTIF_WASM_CFLAGS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_WASM_BUILD_DIR))/configure.log)
	$(Q)touch $@

$(MOTIF_WASM_BUILD_STAMP): $(MOTIF_WASM_CONFIG_STAMP)
	@echo "  CC      motif config/makestrs (host)"
	$(Q)mkdir -p $(dir $(MOTIF_WASM_HOST_MAKESTRS))
	$(Q)$(HOST_CC) -O2 -I$(OUT)/upstream/include -Iinclude \
	    -o $(MOTIF_WASM_HOST_MAKESTRS) $(MOTIF_SRC_DIR)/config/makestrs.c
	@touch $(MOTIF_WASM_HOST_MAKESTRS)
	@echo "  MAKE    motif lib/Xm (wasm)"
	$(Q)emmake $(MOTIF_SUBMAKE) -C $(MOTIF_WASM_BUILD_DIR)/lib/Xm \
	    CFLAGS="$(MOTIF_WASM_CFLAGS)" LIBS="-lm" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_WASM_BUILD_DIR))/build.log)
	@echo "  MAKE    motif lib/Mrm (wasm)"
	$(Q)emmake $(MOTIF_SUBMAKE) -C $(MOTIF_WASM_BUILD_DIR)/lib/Mrm \
	    CFLAGS="$(MOTIF_WASM_CFLAGS)" LIBS="-lm" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_WASM_BUILD_DIR))/build.log)
	$(Q)touch $@

# Harvest each archive in its own rule (not folded into the build stamp) so a
# deleted $(OUT)/libXm.a recopies from the build tree instead of failing a bare
# existence check while the stamp still stands.
$(MOTIF_WASM_LIBXM): $(MOTIF_WASM_BUILD_STAMP)
	@echo "  AR      $@"
	$(Q)cp -f $(MOTIF_WASM_BUILD_DIR)/lib/Xm/.libs/libXm.a $@

$(MOTIF_WASM_LIBMRM): $(MOTIF_WASM_BUILD_STAMP)
	@echo "  AR      $@"
	$(Q)cp -f $(MOTIF_WASM_BUILD_DIR)/lib/Mrm/.libs/libMrm.a $@

# Link smoke: prove the wasm Motif archives are symbol-complete on the toolkit.
# The Motif pair is linked --whole-archive so EVERY member must resolve, not just
# the closure the smoke's calls happen to reach; an unresolved reference in an
# otherwise-unreferenced Xm/Mrm object then fails the link. The toolkit archives
# stay in a normal --start-group (pulled on demand to satisfy those members).
MOTIF_WASM_SMOKE := $(OUT)/tests/test-wasm-motif-link.js
MOTIF_WASM_SMOKE_DEPS := $(MOTIF_WASM_LIBXM) $(MOTIF_WASM_LIBMRM) \
    $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XMU_COMPAT_TARGET) $(TARGET)

$(MOTIF_WASM_SMOKE): tests/test-wasm-motif-link.c $(MOTIF_WASM_SMOKE_DEPS) \
    mk/wasm-motif.mk
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	$(Q)$(CC) -I$(MOTIF_WASM_BUILD_DIR)/lib -I$(MOTIF_SRC_DIR)/lib \
	    -I$(MOTIF_WASM_BUILD_DIR) $(WASM_LIBXT_TEST_CPPFLAGS) \
	    $(SDL_CPPFLAGS) $(PIXMAN_CFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) \
	    tests/test-wasm-motif-link.c \
	    -Wl,--whole-archive $(MOTIF_WASM_LIBXM) $(MOTIF_WASM_LIBMRM) \
	    -Wl,--no-whole-archive \
	    -Wl,--start-group $(LIBXT_TARGET) $(LIBXPM_TARGET) \
	    $(XMU_COMPAT_TARGET) $(TARGET) -Wl,--end-group \
	    $(LDLIBS) -sASYNCIFY -o $@

.PHONY: motif-wasm wasm-motif-test
## Build the wasm Motif archives (libXm.a, libMrm.a)
motif-wasm: $(MOTIF_WASM_LIBXM) $(MOTIF_WASM_LIBMRM)
wasm-motif-test: $(MOTIF_WASM_SMOKE)

endif

# Link-smoke the wasm Motif stack (libXm + libMrm + Xt + Xpm + Xmu on the core).
# Recurses with WASM=1 so a normal make can invoke it; skips cleanly without
# emcc. Separate from check-wasm so the cheap example smoke never triggers the
# Motif cross-build (a source clone plus autoreconf plus a full lib/Xm build).
.PHONY: check-wasm-motif
## Link-smoke the wasm Motif archives (skips when emcc is unavailable)
check-wasm-motif:
	$(Q)if ! command -v emcc >/dev/null 2>&1; then \
	    echo "  SKIP    check-wasm-motif (emcc not found)"; \
	    exit 0; \
	fi; \
	rm -f $(OUT)/tests/test-wasm-motif-link.js \
	      $(OUT)/tests/test-wasm-motif-link.wasm; \
	$(MAKE) WASM=1 wasm-motif-test || exit 1; \
	for f in $(OUT)/libXm.a $(OUT)/libMrm.a \
	         $(OUT)/tests/test-wasm-motif-link.wasm; do \
	    if [ ! -s "$$f" ]; then echo "check-wasm-motif: missing $$f" >&2; exit 1; fi; \
	done; \
	case `file -b $(OUT)/tests/test-wasm-motif-link.wasm` in \
	    *WebAssembly*) ;; \
	    *) echo "check-wasm-motif: not a wasm module" >&2; exit 1;; \
	esac; \
	echo "  OK      check-wasm-motif (Motif archives link on the core)"
