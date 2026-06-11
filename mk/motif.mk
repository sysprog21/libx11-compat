MOTIF_COMMIT := 11cd50b42991e8f11a13522fd820d736cc2f7fcf
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
MOTIF_DEMOS_BUILD_DIR := $(OUT)/motif-demos
MOTIF_DEMOS_CONFIG_STAMP := $(MOTIF_DEMOS_BUILD_DIR)/.configure-stamp
MOTIF_DEMOS_CONFIG_LOG := $(abspath $(MOTIF_DEMOS_BUILD_DIR))/configure.log
MOTIF_DEMOS_BUILD_STAMP := $(MOTIF_DEMOS_BUILD_DIR)/.build-stamp
MOTIF_LIBXM := $(OUT)/libXm.so
MOTIF_LIBMRM := $(OUT)/libMrm.so
MOTIF_TEST_BINS := $(OUT)/tests/test-motif-link \
                   $(OUT)/tests/test-motif-resources
MOTIF_YACC ?= $(shell if [ -x /opt/homebrew/opt/bison/bin/bison ]; then printf '%s\n' '/opt/homebrew/opt/bison/bin/bison -y'; else printf '%s\n' yacc; fi)
MOTIF_CFLAGS ?= -g -O0
MOTIF_LIBS ?= -lm
MOTIF_CONFIGURE_LDFLAGS := -L$(abspath $(OUT))
ifeq ($(UNAME_S),Darwin)
  MOTIF_CONFIGURE_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
  MOTIF_LIBS += -liconv
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

MOTIF_COMMON_CONFIGURE_FLAGS := \
    --prefix=$(abspath $(OUT)/motif-install) \
    --disable-glw \
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
	    git checkout $(MOTIF_GIT_Q) --detach $(MOTIF_COMMIT) && \
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
    $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(XINERAMA_COMPAT_TARGET) | $(MOTIF_BUILD_DIR)
	@echo "  CONFIG  motif"
	$(Q)cd $(MOTIF_BUILD_DIR) && \
	    $(motif_runtime_env) \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
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
	$(Q)touch $@

$(MOTIF_DEMOS_CONFIG_STAMP): $(MOTIF_AUTOGEN_STAMP) $(PKGCONFIG_FILES) \
    $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(XINERAMA_COMPAT_TARGET) | $(MOTIF_DEMOS_BUILD_DIR)
	@echo "  CONFIG  motif demos"
	$(Q)cd $(MOTIF_DEMOS_BUILD_DIR) && \
	    $(motif_runtime_env) \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
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
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/clients/uil CPP="$(CC) -E" CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	@echo "  MAKE    motif demos"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/demos CPP="$(CC) -E" CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	$(Q)touch $@

$(MOTIF_LIBXM) $(MOTIF_LIBMRM): $(MOTIF_BUILD_STAMP)
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

$(OUT)/tests/test-motif-%: tests/test-motif-%.c $(MOTIF_LIBXM) \
    $(MOTIF_LIBMRM) $(LIBXT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(LIBXT_CPPFLAGS) -I$(MOTIF_BUILD_DIR)/lib \
	    -I$(MOTIF_SRC_DIR)/lib $(CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(MOTIF_LIBXM) $(MOTIF_LIBMRM) $(LIBXT_TARGET) $(TARGET) \
	    $(LDLIBS) $(MOTIF_LIBS) $(LIBXT_TEST_LDFLAGS) $(TEST_LDFLAGS) \
	    -o $@

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
MOTIF_DIFF_DISPLAY ?= 119
MOTIF_DIFF_JOBS ?= 1
MOTIF_DIFF_FILTER ?=
MOTIF_DIFF_INSTALL_DEPS ?= 0
MOTIF_DIFF_MAE_THRESHOLD ?= 0.08
MOTIF_DIFF_CHANGED_THRESHOLD ?= 0.20
MOTIF_DIFF_SECONDS ?= 3
MOTIF_DIFF_GEOMETRY ?= 1280x1024x24
MOTIF_DIFF_TOP ?= 12
MOTIF_DIFF_COMPARE_LOCATION ?= remote
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
    MOTIF_DIFF_SECONDS='$(MOTIF_DIFF_SECONDS)' \
    MOTIF_DIFF_GEOMETRY='$(MOTIF_DIFF_GEOMETRY)' \
    MOTIF_DIFF_TOP='$(MOTIF_DIFF_TOP)' \
    MOTIF_DIFF_COMPARE_LOCATION='$(MOTIF_DIFF_COMPARE_LOCATION)' \
    MOTIF_DIFF_OUT_ROOT='$(abspath $(MOTIF_DIFF_OUT_ROOT))' \
    MOTIF_DIFF_REPRESENTATIVE_FILTER='$(MOTIF_DIFF_REPRESENTATIVE_FILTER)' \
    MOTIF_DIFF_REPLAY='$(MOTIF_DIFF_REPLAY)'

motif_ui_replay_env = \
    --env DYLD_LIBRARY_PATH=$(abspath $(MOTIF_DEMOS_BUILD_DIR))/lib/Xm/.libs:$(abspath $(MOTIF_DEMOS_BUILD_DIR))/lib/Mrm/.libs:$(abspath $(MOTIF_DEMOS_BUILD_DIR))/clients/uil/.libs:$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    --env LD_LIBRARY_PATH=$(abspath $(MOTIF_DEMOS_BUILD_DIR))/lib/Xm/.libs:$(abspath $(MOTIF_DEMOS_BUILD_DIR))/lib/Mrm/.libs:$(abspath $(MOTIF_DEMOS_BUILD_DIR))/clients/uil/.libs:$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
    --env LIBX11_COMPAT_FONT_DIR=$(abspath $(OUT))/../fonts

.PHONY: check-differential-motif check-differential-motif-full check-demos-motif motif-demos-screenshots check-smoke-motif profile-ui FORCE
## Compare representative Motif demo screenshots for system libX11 vs libx11-compat on node11
check-differential-motif:
	$(Q)$(motif_diff_env) $(PYTHON) scripts/run-motif-differential-tests.py \
	    --mode representative \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_REPLAY)),--replay-smoke) \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_INSTALL_DEPS)),--install-deps)

## Compare all Motif demo screenshots for system libX11 vs libx11-compat on node11
check-differential-motif-full:
	$(Q)$(motif_diff_env) $(PYTHON) scripts/run-motif-differential-tests.py \
	    --mode full \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_REPLAY)),--replay-smoke) \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_INSTALL_DEPS)),--install-deps)

## Run Motif demo process smoke checks
check-demos-motif: $(MOTIF_DEMOS_BUILD_STAMP)
	$(Q)scripts/validate-motif-demos.sh $(MOTIF_DEMOS_BUILD_DIR) $(OUT)

## Capture screenshots for Motif demos built against libx11-compat
motif-demos-screenshots: $(MOTIF_DEMOS_BUILD_STAMP)
	$(Q)scripts/capture-motif-demo-screenshots.sh $(MOTIF_DEMOS_BUILD_DIR) $(OUT)

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
