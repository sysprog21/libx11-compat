MOTIF_COMMIT := 1f62cee9d8b8bae6a55c8e1dec1c4e8f1ace1d2f
MOTIF_URL := https://github.com/thentenaar/motif
MOTIF_SRC_DIR := $(OUT)/upstream/motif
MOTIF_SRC_STAMP := $(MOTIF_SRC_DIR)/.source-stamp
MOTIF_PATCHES := $(sort $(wildcard compat/motif-patches/*.patch))

# Snapshot the patch list so removals (not just edits) bump an mtime.
# $(MOTIF_PATCHES) alone only re-triggers on edits to files that still
# exist, so dropping a .patch would otherwise leave the patched tree
# stale. Rewrites only when the sorted list changes.
MOTIF_PATCH_LIST_FILE := $(OUT)/upstream/.motif-patch-list
$(shell mkdir -p $(dir $(MOTIF_PATCH_LIST_FILE)); \
        new='$(sort $(notdir $(wildcard compat/motif-patches/*.patch)))'; \
        old=$$(cat $(MOTIF_PATCH_LIST_FILE) 2>/dev/null || true); \
        if [ "$$new" != "$$old" ]; then \
            printf '%s\n' "$$new" > $(MOTIF_PATCH_LIST_FILE); \
        fi)
MOTIF_AUTOGEN_STAMP := $(MOTIF_SRC_DIR)/.autogen-stamp
MOTIF_AUTOGEN_LOG := $(abspath $(MOTIF_SRC_DIR))/autoreconf.log
MOTIF_BUILD_DIR := $(OUT)/motif
MOTIF_CONFIG_STAMP := $(MOTIF_BUILD_DIR)/.configure-stamp
MOTIF_CONFIG_LOG := $(abspath $(MOTIF_BUILD_DIR))/configure.log
MOTIF_BUILD_STAMP := $(MOTIF_BUILD_DIR)/.build-stamp
MOTIF_STAGE_STAMP := $(MOTIF_BUILD_DIR)/.stage-stamp
MOTIF_DEMOS_BUILD_DIR := $(OUT)/motif-demos
MOTIF_DEMOS_CONFIG_STAMP := $(MOTIF_DEMOS_BUILD_DIR)/.configure-stamp
MOTIF_DEMOS_CONFIG_LOG := $(abspath $(MOTIF_DEMOS_BUILD_DIR))/configure.log
MOTIF_DEMOS_BUILD_STAMP := $(MOTIF_DEMOS_BUILD_DIR)/.build-stamp
MOTIF_LIBXM := $(OUT)/libXm.so
MOTIF_LIBMRM := $(OUT)/libMrm.so
MOTIF_TEST_BINS := $(OUT)/tests/test-motif-link \
                   $(OUT)/tests/test-motif-resources \
                   $(OUT)/tests/test-toolkit-probe
MOTIF_YACC ?= $(shell if [ -x /opt/homebrew/opt/bison/bin/bison ]; then printf '%s\n' '/opt/homebrew/opt/bison/bin/bison -y'; else printf '%s\n' yacc; fi)

MOTIF_CFLAGS ?= -g -O0
MOTIF_LIBS ?= -lm
MOTIF_UIL_LIBS = $(MOTIF_LIBS) -lXt-compat -lX11-compat
MOTIF_CONFIGURE_LDFLAGS := -L$(abspath $(OUT))

ifeq ($(UNAME_S),Darwin)
  MOTIF_CONFIGURE_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
  MOTIF_LIBS += -liconv
  # GLw's configure runs PKG_CHECK_MODULES([GL],[gl]) and links whatever gl.pc
  # names for gl*. macOS Homebrew Mesa is a monolithic libGL: it exports both
  # gl* AND glX* and drags in a real libX11, so linking it directly would bind
  # GLw/paperplane's glX* to Mesa (a real-X11 GLX stack) instead of our layer,
  # and the app would crash with no X server. Linux avoids this for free via the
  # GLVND split (gl*-only libOpenGL). We reconstruct that split on macOS with a
  # gl-only reexport shim (see the $(MOTIF_GLSHIM_DIR)/gl.pc rule): it reexports
  # Mesa's gl* but NOT glX*, so glX* can only resolve to libx11-compat while gl*
  # still runs on Mesa's desktop-GL context. Empty when Mesa is absent, in which
  # case GLw configures off and the rest of Motif still builds.
  ifeq ($(GLX),1)
    MOTIF_MESA_PREFIX := $(shell brew --prefix mesa 2>/dev/null)
    ifneq ($(wildcard $(MOTIF_MESA_PREFIX)/lib/libGL.dylib),)
      MOTIF_GLSHIM_DIR := $(OUT)/glshim
      MOTIF_GL_PKGCONFIG := $(MOTIF_GLSHIM_DIR)/gl.pc
    endif
  endif
endif
# Colon-appended to the pinned PKG_CONFIG_PATH so our own .pc files still win.
MOTIF_PKGCONFIG_PATH = $(abspath $(PKGCONFIG_DIR))$(if $(MOTIF_GL_PKGCONFIG),:$(abspath $(dir $(MOTIF_GL_PKGCONFIG))))

# gl-only reexport shim over Mesa's monolithic libGL: exports every gl* symbol
# Mesa defines except glX* (those fall through to libx11-compat at link time,
# order-independently, because the shim simply does not offer them). The gl.pc
# points GLw/paperplane's -lGL at it and carries Mesa's include path for the
# GL headers. Only defined on Darwin+GLX with Mesa present.
ifdef MOTIF_GLSHIM_DIR
$(MOTIF_GLSHIM_DIR)/gl.pc:
	@mkdir -p $(MOTIF_GLSHIM_DIR)
	@echo "  GLSHIM  gl-only reexport over Mesa (glX* -> libx11-compat)"
	$(Q)scripts/gen-mesa-gl-reexport-syms.sh $(MOTIF_MESA_PREFIX)/lib/libGL.dylib \
	    > $(MOTIF_GLSHIM_DIR)/gl.syms
	$(Q)clang -dynamiclib \
	    -o $(MOTIF_GLSHIM_DIR)/libGL.dylib \
	    -L$(MOTIF_MESA_PREFIX)/lib -lGL \
	    -Wl,-reexported_symbols_list,$(MOTIF_GLSHIM_DIR)/gl.syms \
	    -install_name $(abspath $(MOTIF_GLSHIM_DIR)/libGL.dylib)
	$(Q)printf 'Name: gl\nDescription: gl-only shim; glX* routed to libx11-compat\nVersion: 1.0\nCflags: -I%s/include\nLibs: -L%s -lGL\n' \
	    '$(MOTIF_MESA_PREFIX)' '$(abspath $(MOTIF_GLSHIM_DIR))' > $@
endif
motif_runtime_env = \
    DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}

# Verbosity. Default builds hide upstream noise: -s on the recursive
# make silences per-file gcc lines, --quiet on git skips clone/checkout
# progress, and autoreconf / configure stdout+stderr land in per-step
# log files so a failed quiet build still leaves a forensic trail. V=1
# disables all of that.
ifeq ($(V),1)
  MOTIF_SUBMAKE := $(MAKE)
  MOTIF_GIT_Q :=
  motif_log_redirect =
else
  MOTIF_SUBMAKE := $(MAKE) -s
  MOTIF_GIT_Q := --quiet
  motif_log_redirect = >> $(1) 2>&1 || { echo "  FAIL    see $(1)" >&2; tail -40 $(1) >&2; exit 1; }
endif

# GLw is Motif's OpenGL drawing-area widget (GLwDrawingArea/GLwMDrawingArea); it
# uses GLX (glXChooseVisual/MakeCurrent/SwapBuffers). Enable it when the GLX layer
# is built (GLX=1) so libGLw binds to libx11-compat's glX*, disable it otherwise.
# On macOS GLw also needs the gl-only shim's gl.pc (its PKG_CHECK_MODULES([GL],[gl])
# has nothing else to resolve against), so enable it only when Mesa is present and
# MOTIF_GL_PKGCONFIG got set above; otherwise configure would force --enable-glw with
# no gl.pc and the GLw sub-make would fail. Linux has a system gl.pc, so GLX=1 alone
# is enough there. When enabled, lib/GLw must be built before any demo that links it;
# motif_glw_step builds it in the given tree (empty when GLw is off). $(1) is the build dir.
MOTIF_GLW_ENABLED :=
ifeq ($(GLX),1)
  ifeq ($(UNAME_S),Darwin)
    ifdef MOTIF_GL_PKGCONFIG
      MOTIF_GLW_ENABLED := 1
    endif
  else
    MOTIF_GLW_ENABLED := 1
  endif
endif
ifdef MOTIF_GLW_ENABLED
  MOTIF_GLW_FLAG := --enable-glw
  define motif_glw_step
	@echo "  MAKE    motif lib/GLw"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(1)/lib/GLw CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(1))/build.log)
  endef
else
  MOTIF_GLW_FLAG := --disable-glw
  define motif_glw_step
  endef
endif

MOTIF_COMMON_CONFIGURE_FLAGS := \
    --prefix=$(abspath $(OUT)/motif-install) \
    $(MOTIF_GLW_FLAG) \
    --disable-tests \
    --without-xft \
    --with-jpeg=no \
    --with-png=no \
    --with-xrandr=no \
    --with-xrender=no \
    --with-xcursor=no \
    --with-xinerama=no

MOTIF_CONFIGURE_FLAGS := \
    $(MOTIF_COMMON_CONFIGURE_FLAGS) \
    --disable-demos

MOTIF_DEMOS_CONFIGURE_FLAGS := \
    $(MOTIF_COMMON_CONFIGURE_FLAGS) \
    --enable-demos

## Re-trigger on makefile edits, patch edits, or patch additions/removals.
## $(MOTIF_PATCH_LIST_FILE) is rewritten by the $(shell) snapshot above
## whenever the sorted list of compat/motif-patches/*.patch changes, so
## naming it here covers the deletion case that $(MOTIF_PATCHES) misses.
$(MOTIF_SRC_STAMP): mk/motif.mk $(MOTIF_PATCHES) $(MOTIF_PATCH_LIST_FILE)
	@echo "  GIT     $(MOTIF_URL)"
	$(Q)mkdir -p $(dir $(MOTIF_SRC_DIR))
	$(Q)test -d $(MOTIF_SRC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(MOTIF_URL) $(MOTIF_SRC_DIR)
	$(Q)cd $(MOTIF_SRC_DIR) && \
	    { git ls-tree $(MOTIF_COMMIT) >/dev/null 2>&1 || \
	      git fetch $(MOTIF_GIT_Q) origin $(MOTIF_COMMIT) || \
	      git fetch $(MOTIF_GIT_Q) origin; } && \
	    git checkout $(MOTIF_GIT_Q) -f --detach $(MOTIF_COMMIT) && \
	    git reset --hard $(MOTIF_GIT_Q) $(MOTIF_COMMIT) >/dev/null && \
	    git clean $(MOTIF_GIT_Q) -fdx >/dev/null
	$(Q)set -e; for patch in $(abspath $(MOTIF_PATCHES)); do \
	    cd $(abspath $(MOTIF_SRC_DIR)); \
	    if git apply --check "$$patch"; then \
	        git apply "$$patch"; \
	    elif git apply --reverse --check "$$patch"; then \
	        :; \
	    else \
	        echo "  PATCH   failed $$patch" >&2; exit 1; \
	    fi; \
	done
	$(Q)touch $@

$(MOTIF_AUTOGEN_STAMP): $(MOTIF_SRC_STAMP)
	@echo "  AUTOGEN motif"
	$(Q)cd $(MOTIF_SRC_DIR) && autoreconf -fi \
	    $(call motif_log_redirect,$(MOTIF_AUTOGEN_LOG))
	$(Q)touch $@

$(MOTIF_BUILD_DIR):
	@mkdir -p $@

$(MOTIF_DEMOS_BUILD_DIR):
	@mkdir -p $@

$(MOTIF_CONFIG_STAMP): $(MOTIF_AUTOGEN_STAMP) $(PKGCONFIG_FILES) \
    $(MOTIF_GL_PKGCONFIG)     $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(XINERAMA_COMPAT_TARGET) | $(MOTIF_BUILD_DIR)
	@echo "  CONFIG  motif"
	$(Q)cd $(MOTIF_BUILD_DIR) && \
	    $(motif_runtime_env) \
	    PKG_CONFIG_PATH=$(MOTIF_PKGCONFIG_PATH) \
	    CPP="$(CC) -E" \
	    CPPFLAGS="-include stdlib.h" \
	    CFLAGS="$(MOTIF_CFLAGS)" \
	    LDFLAGS="$(MOTIF_CONFIGURE_LDFLAGS)" \
	    YACC="$(MOTIF_YACC)" \
	    $(abspath $(MOTIF_SRC_DIR))/configure $(MOTIF_CONFIGURE_FLAGS) \
	    $(call motif_log_redirect,$(MOTIF_CONFIG_LOG))
	$(Q)touch $@

$(MOTIF_BUILD_STAMP): $(MOTIF_CONFIG_STAMP)
	@echo "  MAKE    motif config"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_BUILD_DIR)/config LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_BUILD_DIR))/build.log)
	@echo "  MAKE    motif lib/Xm"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_BUILD_DIR)/lib/Xm CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_BUILD_DIR))/build.log)
	@echo "  MAKE    motif lib/Mrm"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_BUILD_DIR)/lib/Mrm CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_BUILD_DIR))/build.log)
	$(call motif_glw_step,$(MOTIF_BUILD_DIR))
	$(Q)touch $@

$(MOTIF_DEMOS_CONFIG_STAMP): $(MOTIF_AUTOGEN_STAMP) $(PKGCONFIG_FILES) \
    $(MOTIF_GL_PKGCONFIG)     $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(XINERAMA_COMPAT_TARGET) | $(MOTIF_DEMOS_BUILD_DIR)
	@echo "  CONFIG  motif demos"
	$(Q)cd $(MOTIF_DEMOS_BUILD_DIR) && \
	    $(motif_runtime_env) \
	    PKG_CONFIG_PATH=$(MOTIF_PKGCONFIG_PATH) \
	    CPP="$(CC) -E" \
	    CPPFLAGS="-include stdlib.h" \
	    CFLAGS="$(MOTIF_CFLAGS)" \
	    LDFLAGS="$(MOTIF_CONFIGURE_LDFLAGS)" \
	    YACC="$(MOTIF_YACC)" \
	    $(abspath $(MOTIF_SRC_DIR))/configure $(MOTIF_DEMOS_CONFIGURE_FLAGS) \
	    $(call motif_log_redirect,$(MOTIF_DEMOS_CONFIG_LOG))
	$(Q)touch $@

$(MOTIF_DEMOS_BUILD_STAMP): $(MOTIF_DEMOS_CONFIG_STAMP)
	@echo "  MAKE    motif demos config"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/config LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	@echo "  MAKE    motif demos lib/Xm"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/lib/Xm CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	@echo "  MAKE    motif demos lib/Mrm"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/lib/Mrm CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	@echo "  MAKE    motif uil"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/tools/wml CPP="$(CC) -E" CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/clients/uil CPP="$(CC) -E" CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_UIL_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	$(call motif_glw_step,$(MOTIF_DEMOS_BUILD_DIR))
	@echo "  MAKE    motif demos"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/demos CPP="$(CC) -E" CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	$(Q)touch $@

$(MOTIF_STAGE_STAMP): $(MOTIF_BUILD_STAMP)
	@echo "  STAGE   motif libraries"
	$(Q)cp -f $(MOTIF_BUILD_DIR)/lib/Xm/.libs/libXm*.dylib $(OUT)/ 2>/dev/null || true
	$(Q)cp -f $(MOTIF_BUILD_DIR)/lib/Xm/.libs/libXm.so* $(OUT)/ 2>/dev/null || true
	$(Q)cp -f $(MOTIF_BUILD_DIR)/lib/Mrm/.libs/libMrm*.dylib $(OUT)/ 2>/dev/null || true
	$(Q)cp -f $(MOTIF_BUILD_DIR)/lib/Mrm/.libs/libMrm.so* $(OUT)/ 2>/dev/null || true
ifeq ($(UNAME_S),Darwin)
	$(Q)test ! -f $(OUT)/libXm.5.dylib || \
	    install_name_tool -id @rpath/libXm.5.dylib $(OUT)/libXm.5.dylib
	$(Q)test ! -f $(OUT)/libMrm.5.dylib || \
	    install_name_tool -id @rpath/libMrm.5.dylib \
	        -change $(abspath $(OUT))/motif-install/lib/libXm.5.dylib \
	        @rpath/libXm.5.dylib $(OUT)/libMrm.5.dylib
endif
	$(Q)rm -f $(MOTIF_LIBXM) $(MOTIF_LIBMRM)
	$(Q)if [ -e $(OUT)/libXm.5.dylib ]; then \
	    ln -sf libXm.5.dylib $(MOTIF_LIBXM); \
	elif [ -e $(OUT)/libXm.so.5 ]; then \
	    ln -sf libXm.so.5 $(MOTIF_LIBXM); \
	else \
	    echo "  STAGE   no libXm.5.dylib or libXm.so.5 found" >&2; exit 1; \
	fi
	$(Q)if [ -e $(OUT)/libMrm.5.dylib ]; then \
	    ln -sf libMrm.5.dylib $(MOTIF_LIBMRM); \
	elif [ -e $(OUT)/libMrm.so.5 ]; then \
	    ln -sf libMrm.so.5 $(MOTIF_LIBMRM); \
	else \
	    echo "  STAGE   no libMrm.5.dylib or libMrm.so.5 found" >&2; exit 1; \
	fi
	$(Q)touch $@

$(MOTIF_LIBXM) $(MOTIF_LIBMRM): $(MOTIF_STAGE_STAMP)
	$(Q)test -e $@

$(OUT)/tests/test-motif-%: tests/test-motif-%.c $(MOTIF_LIBXM) \
    $(MOTIF_LIBMRM) $(LIBXT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(LIBXT_CPPFLAGS) -I$(MOTIF_BUILD_DIR)/lib \
	    -I$(MOTIF_SRC_DIR)/lib $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(MOTIF_LIBXM) $(MOTIF_LIBMRM) $(LIBXT_TARGET) $(TARGET) \
	    $(LDLIBS) $(MOTIF_LIBS) $(LIBXT_TEST_LDFLAGS) $(TEST_LDFLAGS) \
	    -o $@

$(OUT)/tests/test-toolkit-probe: tests/test-toolkit-probe.c $(MOTIF_LIBXM) \
    $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) \
	    -DMOTIF_LIB_PATH=\"$(abspath $(MOTIF_LIBXM))\" $< $(TARGET) \
	    $(LDLIBS) $(TEST_LDFLAGS) -o $@

.PHONY: motif motif-demos check-link-motif
## Build thentenaar/motif libXm and libMrm against the compatibility stack
motif: $(MOTIF_LIBXM) $(MOTIF_LIBMRM)

## Build thentenaar/motif demos for differential tests against host libXm/libMrm
motif-demos: $(MOTIF_DEMOS_BUILD_STAMP)

## Run in-process Motif/Xt behavior checks against libx11-compat
check-link-motif: $(MOTIF_TEST_BINS)
	@set -e; for test_bin in $(MOTIF_TEST_BINS); do \
	    printf "$(BLUE)RUN$(RESET) %s\n" "$$test_bin"; \
	    $(motif_runtime_env) SDL_VIDEODRIVER=dummy $$test_bin; \
	done

UI_REPLAY_DISPLAY ?= 121
UI_REPLAY_GEOMETRY ?= 1280x1024x24
UI_REPLAY_SCREENSHOT_COMMAND ?= auto
UI_REPLAY_XVFB ?=
UI_SMOKE_OUT_ROOT ?= $(OUT)/ui-smoke

MOTIF_DIFF_REMOTE ?= node11
MOTIF_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-motif-differential
MOTIF_DIFF_DISPLAY ?= 100
MOTIF_DIFF_JOBS ?= 1
MOTIF_DIFF_FILTER ?=
MOTIF_DIFF_INSTALL_DEPS ?= 0
MOTIF_DIFF_LOCAL ?= 0
# Thresholds calibrated to the GitHub Actions ubuntu-24.04 runner with
# xfonts-base / 100dpi / 75dpi / scalable installed. The system-side
# Motif Xm widget defaults differ from the libx11-compat-built Xm in a
# handful of representative demos, all real library behavior gaps to
# chase in follow-up PRs rather than CI-side regressions.
#
# MAE is 0.11, which the bulk of the representative set clears with margin.
# The two captures that cannot clear it are ColorSel and Ext18List: their
# native side renders helvetica from the adobe-helvetica BITMAP X font,
# which the compat SDL_ttf substitute cannot match at any width (measured
# ~0.133 / ~0.11 on CI, verified independent of SDL/freetype version, DPI,
# and probe-font choice). They are listed in MOTIF_DIFF_ALLOW_DIFF so the
# compare reports them as expected-diff instead of failing the gate; drop
# them from that list once the compat side can render bitmap helvetica.
# changed_ratio stays a coarse backstop because glyph antialiasing alone
# puts correct output at 0.15 to 0.20.
MOTIF_DIFF_MAE_THRESHOLD ?= 0.11
MOTIF_DIFF_ALLOW_DIFF ?= ColorSel,Ext18List
# The allow-list exemption is bounded: an allow-listed capture that clears
# these ceilings is still failed, so a crash frame, a missing widget, or an
# unrelated break in ColorSel/Ext18List cannot hide behind expected-diff.
# The known font-parity floor is ~0.13 MAE / ~0.30 changed, so these sit
# above it with margin but well under a gross regression.
MOTIF_DIFF_ALLOW_MAE_CEILING ?= 0.20
MOTIF_DIFF_ALLOW_CHANGED_CEILING ?= 0.45
MOTIF_DIFF_CHANGED_THRESHOLD ?= 0.32
MOTIF_DIFF_SECONDS ?= 3
MOTIF_DIFF_GEOMETRY ?= 1280x1024x24
MOTIF_DIFF_TOP ?= 12
MOTIF_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(MOTIF_DIFF_LOCAL)),local,remote)
MOTIF_DIFF_OUT_ROOT ?= $(OUT)/motif-differential
MOTIF_DIFF_REPRESENTATIVE_FILTER ?=
MOTIF_DIFF_REPLAY ?= 0
DIFFERENTIAL_REMOTE ?= 0

motif_diff_env = \
    MOTIF_DIFF_REMOTE='$(MOTIF_DIFF_REMOTE)' \
    MOTIF_DIFF_REMOTE_ROOT='$(MOTIF_DIFF_REMOTE_ROOT)' \
    MOTIF_DIFF_DISPLAY='$(MOTIF_DIFF_DISPLAY)' \
    MOTIF_DIFF_JOBS='$(MOTIF_DIFF_JOBS)' \
    MOTIF_DIFF_FILTER='$(MOTIF_DIFF_FILTER)' \
    MOTIF_DIFF_MAE_THRESHOLD='$(MOTIF_DIFF_MAE_THRESHOLD)' \
    MOTIF_DIFF_CHANGED_THRESHOLD='$(MOTIF_DIFF_CHANGED_THRESHOLD)' \
    MOTIF_DIFF_ALLOW_DIFF='$(MOTIF_DIFF_ALLOW_DIFF)' \
    MOTIF_DIFF_ALLOW_MAE_CEILING='$(MOTIF_DIFF_ALLOW_MAE_CEILING)' \
    MOTIF_DIFF_ALLOW_CHANGED_CEILING='$(MOTIF_DIFF_ALLOW_CHANGED_CEILING)' \
    MOTIF_DIFF_SECONDS='$(MOTIF_DIFF_SECONDS)' \
    MOTIF_DIFF_GEOMETRY='$(MOTIF_DIFF_GEOMETRY)' \
    MOTIF_DIFF_TOP='$(MOTIF_DIFF_TOP)' \
    MOTIF_DIFF_COMPARE_LOCATION='$(MOTIF_DIFF_COMPARE_LOCATION)' \
    MOTIF_DIFF_OUT_ROOT='$(abspath $(MOTIF_DIFF_OUT_ROOT))' \
    MOTIF_DIFF_REPRESENTATIVE_FILTER='$(MOTIF_DIFF_REPRESENTATIVE_FILTER)' \
    MOTIF_DIFF_REPLAY='$(MOTIF_DIFF_REPLAY)'

motif_ui_replay_env = \
    --env DYLD_LIBRARY_PATH=$(abspath $(MOTIF_DEMOS_BUILD_DIR))/lib/Xm/.libs:$(abspath $(MOTIF_DEMOS_BUILD_DIR))/lib/Mrm/.libs:$(abspath $(MOTIF_DEMOS_BUILD_DIR))/clients/uil/.libs:$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    --env LD_LIBRARY_PATH=$(abspath $(MOTIF_DEMOS_BUILD_DIR))/lib/Xm/.libs:$(abspath $(MOTIF_DEMOS_BUILD_DIR))/lib/Mrm/.libs:$(abspath $(MOTIF_DEMOS_BUILD_DIR))/clients/uil/.libs:$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}

.PHONY: check-differential-motif check-differential-motif-full check-demos-motif motif-demos-screenshots check-paperplane-macos check-paperplane-linux check-smoke-motif profile-ui FORCE
## Compare representative Motif demo screenshots for system libX11 vs
## libx11-compat. Set MOTIF_DIFF_LOCAL=1 to run on the current host (used
## by the GitHub Actions differential workflow); otherwise the script
## SSHes to MOTIF_DIFF_REMOTE.
check-differential-motif:
	$(Q)$(motif_diff_env) $(PYTHON) scripts/run-motif-differential-tests.py \
	    --mode representative \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_REPLAY)),--replay-smoke) \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_LOCAL)),--local)

## Compare all Motif demo screenshots for system libX11 vs libx11-compat.
check-differential-motif-full:
	$(Q)$(motif_diff_env) $(PYTHON) scripts/run-motif-differential-tests.py \
	    --mode full \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_REPLAY)),--replay-smoke) \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_LOCAL)),--local)

## Run Motif demo process smoke checks
check-demos-motif: $(MOTIF_DEMOS_BUILD_STAMP)
	$(Q)scripts/validate-motif-demos.sh $(MOTIF_DEMOS_BUILD_DIR) $(OUT)

## Capture screenshots for Motif demos built against libx11-compat
motif-demos-screenshots: $(MOTIF_DEMOS_BUILD_STAMP)
	$(Q)scripts/capture-motif-demo-screenshots.sh $(MOTIF_DEMOS_BUILD_DIR) $(OUT)

## Headless GLX render regression: run unmodified paperplane (Motif GLwDrawingArea
## + immediate-mode desktop GL) against libx11-compat and assert its canvas
## actually paints, not a black frame. Guards the whole macOS GLX-over-EGL stack
## (surfaceless Mesa desktop GL, the offscreen pbuffer readback/composite, and the
## GLwDrawingArea resize-delivery wobble). Darwin + GLX=1 + Homebrew Mesa only; a
## no-op elsewhere so it is safe inside the aggregate check.

# Render paperplane and assert its canvas painted (>=30% non-background), retrying
# a few times. The first frame can miss the fixed snapshot delay on a slow/loaded
# host (surfaceless llvmpipe on a shared CI runner), which reads back a blank
# window; the render has no persistent state, so a fresh attempt is the simplest
# robust guard against that transient miss. $(1) is the output png path.
define paperplane_render_check
	$(Q)ok=0; for attempt in 1 2 3; do \
	    scripts/run-paperplane.sh --snapshot $(1) && \
	    $(PYTHON) scripts/assert-image-content.py $(1) 0.30 && { ok=1; break; }; \
	    echo "  RETRY   paperplane produced no valid frame on attempt $$attempt"; \
	done; \
	[ "$$ok" = 1 ] || { echo "  FAIL    paperplane produced no valid frame after 3 attempts"; exit 1; }
endef

ifdef MOTIF_GLSHIM_DIR
check-paperplane-macos: $(MOTIF_DEMOS_BUILD_STAMP)
	@echo "  CHECK   paperplane (macOS GLX headless render)"
	$(call paperplane_render_check,$(OUT)/paperplane-macos.png)
else
check-paperplane-macos:
	@echo "  SKIP    paperplane macOS GLX render (needs Darwin + GLX=1 + Homebrew Mesa)"
endif

## Linux twin of check-paperplane-macos: render unmodified paperplane through the
## same surfaceless-Mesa GLX-over-EGL path Linux CI uses for the mesa demos, and
## assert the GLwDrawingArea canvas paints (not a black frame). GLw is enabled on
## Linux + GLX=1, so the binary is already built by motif-demos; the run picks the
## system Mesa libEGL. A no-op elsewhere so it is safe inside an aggregate check.
ifeq ($(UNAME_S)/$(if $(MOTIF_GLW_ENABLED),on),Linux/on)
# paperplane links the system GLVND libGL (built by Motif's autotools, not us), so
# its immediate-mode gl* never reach our EGL context. run-paperplane.sh preloads
# the gl4es-backed libGL.so ($(OUT)/gl4es/libGL.so) to interpose those gl* and
# route them through gl4es; build it as a prerequisite here.
check-paperplane-linux: $(MOTIF_DEMOS_BUILD_STAMP) $(OUT)/gl4es/libGL.so
	@echo "  CHECK   paperplane (linux GLX headless render)"
	$(call paperplane_render_check,$(OUT)/paperplane-linux.png)
else
check-paperplane-linux:
	@echo "  SKIP    paperplane linux GLX render (needs Linux + GLX=1)"
endif

## Run replay-based local Motif UI smoke checks against libx11-compat
check-smoke-motif: $(UI_SMOKE_OUT_ROOT)/motif-fileview-done/.stamp $(UI_SMOKE_OUT_ROOT)/motif-wsm-labels/.stamp

## Run replay UI smoke checks and collect timing/render profiling artifacts
profile-ui: check-smoke check-smoke-mosaic
	@echo "UI replay profile artifacts under $(abspath $(UI_SMOKE_OUT_ROOT))"
	$(Q)find $(abspath $(UI_SMOKE_OUT_ROOT)) \( -name metrics.tsv -o -name render-stats.tsv \) -print | sort

# Pin fileview's main window to a deterministic origin so the
# in-process snapshot helper and screenshot region line up. Motif
# applies the -geometry hint to the main shell; the language-selection
# dialog may still center per its own resources.
MOTIF_FILEVIEW_GEOMETRY ?= 480x260+0+0

$(UI_SMOKE_OUT_ROOT)/motif-fileview-done/.stamp: FORCE $(MOTIF_DEMOS_BUILD_STAMP)
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name motif-fileview-done \
	    --app $(abspath $(MOTIF_DEMOS_BUILD_DIR))/demos/programs/fileview/fileview \
	    --app-arg=-geometry --app-arg=$(MOTIF_FILEVIEW_GEOMETRY) \
	    --workdir $(abspath $(MOTIF_DEMOS_BUILD_DIR))/demos/programs/fileview \
	    --replay tests/ui/replays/motif-fileview-done.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/motif-fileview-done \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/motif-fileview-done/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    $(motif_ui_replay_env) \
	    --env XAPPLRESDIR=$(abspath $(MOTIF_SRC_DIR))/demos/programs/fileview \
	    --env XFILESEARCHPATH=$(abspath $(MOTIF_SRC_DIR))/demos/programs/fileview/%N.ad:$(abspath $(MOTIF_SRC_DIR))/demos/programs/fileview/%N
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/motif-wsm-labels/.stamp: FORCE $(MOTIF_DEMOS_BUILD_STAMP)
	$(Q)mkdir -p $(abspath $(UI_SMOKE_OUT_ROOT))/motif-wsm-home
	$(Q)printf '%s\n' \
	    'wsm_WSM.WSM.0.linked:True' \
	    'wsm_WSM.WSM.0.allWorkspaces:True' \
	    'wsm_WSM.WSM.0.linkedRoom.hidden:0' \
	    'saveAsShell_WSM*allWorkspaces:True' \
	    'configureShell_WSM*allWorkspaces:True' \
	    'nameShell_WSM*allWorkspaces: True' \
	    'backgroundShell_WSM*allWorkspaces:True' \
	    'deleteShell_WSM*allWorkspaces:True' \
	    'occupyShell_WSM*allWorkspaces:True' \
	    > $(abspath $(UI_SMOKE_OUT_ROOT))/motif-wsm-home/.wsmdb
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name motif-wsm-labels \
	    --app $(abspath $(MOTIF_DEMOS_BUILD_DIR))/demos/programs/workspace/wsm \
	    --workdir $(abspath $(MOTIF_DEMOS_BUILD_DIR))/demos/programs/workspace \
	    --replay tests/ui/replays/motif-wsm-labels.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/motif-wsm-labels \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/motif-wsm-labels/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    $(motif_ui_replay_env) \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/motif-wsm-home \
	    --env XAPPLRESDIR=$(abspath $(MOTIF_SRC_DIR))/demos/programs/workspace \
	    --env XFILESEARCHPATH=$(abspath $(MOTIF_SRC_DIR))/demos/programs/workspace/%N.ad:$(abspath $(MOTIF_SRC_DIR))/demos/programs/workspace/%N
	$(Q)touch $@
