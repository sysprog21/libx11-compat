XNEDIT_URL := https://github.com/unixwork/xnedit
XNEDIT_REVISION := 7f775fce45a24cb2fa910063bd8c0efb727e1668
XNEDIT_CLONE_FLAGS ?= --filter=blob:none --no-checkout
XNEDIT_SRC_DIR := $(OUT)/upstream/xnedit
XNEDIT_SOURCE_STAMP := $(XNEDIT_SRC_DIR)/.source-stamp
XNEDIT_PATCHES := $(sort $(wildcard compat/xnedit-patches/*.patch))

XNEDIT_BUILD_DIR := $(OUT)/xnedit
XNEDIT_WORK_DIR := $(XNEDIT_BUILD_DIR)/source
XNEDIT_BIN := $(XNEDIT_WORK_DIR)/source/xnedit
XNEDIT_LOG := $(abspath $(XNEDIT_BUILD_DIR))/build.log
XNEDIT_LIB_ALIASES := $(XNEDIT_BUILD_DIR)/lib-aliases
XNEDIT_LIB_ALIASES_STAMP := $(XNEDIT_LIB_ALIASES)/.stamp
XNEDIT_SYSROOT := $(XNEDIT_BUILD_DIR)/sysroot
XNEDIT_SYSROOT_STAMP := $(XNEDIT_SYSROOT)/.stamp

XNEDIT_RPATH_FLAGS :=
ifeq ($(UNAME_S),Linux)
  # --disable-new-dtags emits DT_RPATH rather than DT_RUNPATH. DT_RUNPATH is not
  # consulted when the loader resolves a dependency's own NEEDED entries, so with
  # the default new dtags the binary finds libXm/libXt/libX11 but not their
  # transitive deps (libXft-compat.so is NEEDED by libXm, not by xnedit), and the
  # bare binary fails to start without LD_LIBRARY_PATH. DT_RPATH is searched for
  # the whole dependency tree, so it resolves the transitive compat libs the way
  # macOS @rpath already does, letting ./xnedit run with no LD_LIBRARY_PATH.
  XNEDIT_RPATH_FLAGS += -Wl,--disable-new-dtags \
      -Wl,-rpath,$(abspath $(OUT)) \
      -Wl,-rpath,$(abspath $(XNEDIT_LIB_ALIASES)) \
      -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  XNEDIT_RPATH_FLAGS += -Wl,-rpath,$(abspath $(OUT)) \
      -Wl,-rpath,$(abspath $(XNEDIT_LIB_ALIASES)) -liconv \
      -framework CoreFoundation
endif

XNEDIT_C_OPT_FLAGS := $(CFLAGS) $(CFLAGS_EXTRA) \
    -include stdlib.h -include sys/stat.h -I$(abspath $(XNEDIT_SYSROOT))
XNEDIT_LD_OPT_FLAGS := -L$(abspath $(XNEDIT_LIB_ALIASES)) \
    -L$(abspath $(OUT)) $(XNEDIT_RPATH_FLAGS)

$(XNEDIT_SOURCE_STAMP): mk/xnedit.mk $(XNEDIT_PATCHES)
	@echo "  GIT     $(XNEDIT_URL)"
	$(Q)mkdir -p $(dir $(XNEDIT_SRC_DIR))
	$(Q)test -d $(XNEDIT_SRC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(XNEDIT_CLONE_FLAGS) $(XNEDIT_URL) $(XNEDIT_SRC_DIR)
	$(Q)cd $(XNEDIT_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin $(XNEDIT_REVISION) || \
	    (cd $(XNEDIT_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin)
	$(Q)cd $(XNEDIT_SRC_DIR) && \
	    git checkout $(MOTIF_GIT_Q) --detach $(XNEDIT_REVISION) && \
	    git reset --hard $(MOTIF_GIT_Q) $(XNEDIT_REVISION) >/dev/null && \
	    git clean $(MOTIF_GIT_Q) -fdx >/dev/null
	$(Q)set -e; for patch in $(abspath $(XNEDIT_PATCHES)); do \
	    cd $(abspath $(XNEDIT_SRC_DIR)); \
	    if git apply --ignore-space-change --unidiff-zero --check "$$patch"; then \
	        git apply --ignore-space-change --unidiff-zero "$$patch"; \
	    elif git apply --ignore-space-change --unidiff-zero --reverse --check "$$patch"; then \
	        :; \
	    else \
	        echo "  PATCH   failed $$patch" >&2; exit 1; \
	    fi; \
	done
	$(Q)touch $@

$(XNEDIT_SYSROOT_STAMP): $(LIBXT_TARGET) $(MOTIF_STAGE_STAMP) mk/xnedit.mk
	@echo "  SYSROOT xnedit"
	$(Q)rm -rf $(XNEDIT_SYSROOT)
	$(Q)mkdir -p $(XNEDIT_SYSROOT)/X11/extensions $(XNEDIT_SYSROOT)/Xm
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ "$$b" = extensions ] && continue; \
	    ln -sf "$$e" "$(XNEDIT_SYSROOT)/X11/$$b"; \
	done
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/extensions/* \
	             $(abspath include)/X11/extensions/*; do \
	    ln -sf "$$e" "$(XNEDIT_SYSROOT)/X11/extensions/$$(basename "$$e")"; \
	done
	$(Q)for e in $(abspath include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ -e "$(XNEDIT_SYSROOT)/X11/$$b" ] || \
	        ln -sf "$$e" "$(XNEDIT_SYSROOT)/X11/$$b"; \
	done
	$(Q)for h in $(abspath $(MOTIF_SRC_DIR))/lib/Xm/*.h \
	             $(abspath $(MOTIF_BUILD_DIR))/lib/Xm/*.h; do \
	    ln -sf "$$h" "$(XNEDIT_SYSROOT)/Xm/$$(basename "$$h")"; \
	done
	$(Q)touch $@

$(XNEDIT_LIB_ALIASES_STAMP): $(TARGET) $(LIBXT_TARGET) $(XEXT_COMPAT_TARGET) \
    $(XFT_COMPAT_TARGET) $(MOTIF_STAGE_STAMP) mk/xnedit.mk
	@mkdir -p $(XNEDIT_LIB_ALIASES)
	$(Q)rm -f $(XNEDIT_LIB_ALIASES)/libX*.so $(XNEDIT_LIB_ALIASES)/libX*.dylib \
	    $(XNEDIT_LIB_ALIASES)/libXm.so $(XNEDIT_LIB_ALIASES)/libXm.dylib \
	    $(XNEDIT_LIB_ALIASES)/libSDL2*.so
	$(Q)set -e; for pair in \
	    libX11.so:libX11-compat.so \
	    libXt.so:libXt-compat.so \
	    libXext.so:libXext-compat.so \
	    libXft.so:libXft-compat.so \
	    libXrender.so:libX11-compat.so \
	    libXm.so:libXm.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(XNEDIT_LIB_ALIASES)/$$alias"; \
	done
	# The SDL2 shims are NEEDED by libX11-compat.so, which is loaded through the
	# libX11.so alias symlink above, so its RUNPATH=$$ORIGIN resolves to this
	# lib-aliases dir rather than $(OUT). Mirror the shims here too so DT_RPATH
	# handoff to the shim resolves without LD_LIBRARY_PATH. Guarded on existence:
	# the SDL3 backend links libSDL3 directly and ships no shim (also true on
	# macOS), so there is nothing to alias there.
	$(Q)set -e; for shim in libSDL2-x11compat.so libSDL2_ttf-x11compat.so; do \
	    if [ -e "$(abspath $(OUT))/$$shim" ]; then \
	        ln -sf "$(abspath $(OUT))/$$shim" "$(XNEDIT_LIB_ALIASES)/$$shim"; \
	    fi; \
	done
ifeq ($(UNAME_S),Darwin)
	$(Q)set -e; for pair in \
	    libX11.dylib:libX11-compat.so \
	    libXt.dylib:libXt-compat.so \
	    libXext.dylib:libXext-compat.so \
	    libXft.dylib:libXft-compat.so \
	    libXrender.dylib:libX11-compat.so \
	    libXm.dylib:libXm.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(XNEDIT_LIB_ALIASES)/$$alias"; \
	done
endif
	$(Q)touch $@

.PHONY: xnedit xnedit-clean check-smoke-xnedit check-differential-xnedit
## Build unixwork/xnedit against the Motif + Xft compatibility stack
xnedit: $(XNEDIT_BIN)

$(XNEDIT_BIN): $(XNEDIT_SOURCE_STAMP) $(PKGCONFIG_FILES) \
    $(XNEDIT_SYSROOT_STAMP) $(XNEDIT_LIB_ALIASES_STAMP)
	@mkdir -p $(XNEDIT_BUILD_DIR)
	$(Q)rm -rf $(XNEDIT_WORK_DIR)
	$(Q)mkdir -p $(XNEDIT_WORK_DIR)
	$(Q)tar -cf - -C $(XNEDIT_SRC_DIR) . | tar -xf - -C $(XNEDIT_WORK_DIR)
	@echo "  MAKE    xnedit"
	$(Q)env -u MAKEFLAGS -u MFLAGS \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
	    DYLD_LIBRARY_PATH=$(abspath $(OUT)):$(abspath $(XNEDIT_LIB_ALIASES))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    LD_LIBRARY_PATH=$(abspath $(OUT)):$(abspath $(XNEDIT_LIB_ALIASES))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    $(MAKE) -C $(XNEDIT_WORK_DIR) linux \
	        CC='$(CC)' \
	        C_OPT_FLAGS='$(XNEDIT_C_OPT_FLAGS)' \
	        LD_OPT_FLAGS='$(XNEDIT_LD_OPT_FLAGS)' \
	        >> $(XNEDIT_LOG) 2>&1 || { \
	            echo "  FAIL    see $(XNEDIT_LOG)" >&2; \
	            tail -60 $(XNEDIT_LOG) >&2; \
	            exit 1; \
	        }

xnedit-clean:
	@echo "  CLEAN   xnedit"
	$(Q)rm -rf $(XNEDIT_BUILD_DIR)

XNEDIT_UI_LIB_PATH = $(abspath $(OUT)):$(abspath $(XNEDIT_LIB_ALIASES))$(if $(SDL_RUNTIME_LIBDIR),:$(SDL_RUNTIME_LIBDIR))
XNEDIT_SMOKE_GEOMETRY ?= 120x45+0+0
XNEDIT_SMOKE_REGION ?= 0,0,900,650
XNEDIT_FIXTURE := $(abspath tests/ui/fixtures/xnedit-fixture.txt)

xnedit_ui_env = \
    --env DYLD_LIBRARY_PATH=$(XNEDIT_UI_LIB_PATH)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    --env LD_LIBRARY_PATH=$(XNEDIT_UI_LIB_PATH)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
    --env LIBX11_COMPAT_SCREEN_GEOMETRY=1280x1024 \
    --env XNEDIT_HOME=$(abspath $(@D))/home

## Run replay-based XNEdit smoke checks against libx11-compat
check-smoke-xnedit: $(UI_SMOKE_OUT_ROOT)/xnedit-startup/.stamp \
                    $(UI_SMOKE_OUT_ROOT)/xnedit-fixture/.stamp \
                    $(UI_SMOKE_OUT_ROOT)/xnedit-resize/.stamp \
                    $(UI_SMOKE_OUT_ROOT)/xnedit-resize-file-menu/.stamp \
                    $(UI_SMOKE_OUT_ROOT)/xnedit-file-menu/.stamp \
                    $(UI_SMOKE_OUT_ROOT)/xnedit-preferences-menu/.stamp \
                    $(UI_SMOKE_OUT_ROOT)/xnedit-text-fonts-dialog/.stamp \
                    $(UI_SMOKE_OUT_ROOT)/xnedit-typing/.stamp

# Every runner drives the same UI_REPLAY_DISPLAY and, under --xvfb, unlinks the
# shared /tmp/.X<display>-lock before starting its own Xvfb. Running them in
# parallel (make -j check-smoke-xnedit) makes them fight over that display and
# fail nondeterministically. GNU Make 3.81 has no per-target .NOTPARALLEL, so
# serialize the scenarios with an order-only chain: each stamp waits for the
# prior one without treating its timestamp as a rebuild trigger.
$(UI_SMOKE_OUT_ROOT)/xnedit-fixture/.stamp: | $(UI_SMOKE_OUT_ROOT)/xnedit-startup/.stamp
$(UI_SMOKE_OUT_ROOT)/xnedit-resize/.stamp: | $(UI_SMOKE_OUT_ROOT)/xnedit-fixture/.stamp
$(UI_SMOKE_OUT_ROOT)/xnedit-resize-file-menu/.stamp: | $(UI_SMOKE_OUT_ROOT)/xnedit-resize/.stamp
$(UI_SMOKE_OUT_ROOT)/xnedit-file-menu/.stamp: | $(UI_SMOKE_OUT_ROOT)/xnedit-resize-file-menu/.stamp
$(UI_SMOKE_OUT_ROOT)/xnedit-preferences-menu/.stamp: | $(UI_SMOKE_OUT_ROOT)/xnedit-file-menu/.stamp
$(UI_SMOKE_OUT_ROOT)/xnedit-text-fonts-dialog/.stamp: | $(UI_SMOKE_OUT_ROOT)/xnedit-preferences-menu/.stamp
$(UI_SMOKE_OUT_ROOT)/xnedit-typing/.stamp: | $(UI_SMOKE_OUT_ROOT)/xnedit-text-fonts-dialog/.stamp

# One smoke scenario is one run-ui-replay.py invocation that only varies by
# scenario name, an optional fixture app-arg, and an optional timeline latency
# pass. Factor the recipe into a macro so a new scenario is one call, not a
# copied 20-line block. Call with:
#   $1 scenario suffix (stamp dir and --name are xnedit-$1, replay is
#      tests/ui/replays/xnedit-$1.replay, latency baseline is
#      tests/ui/baselines/xnedit-$1-latency.json)
#   $2 extra order/build prerequisites (e.g. the fixture file), or empty
#   $3 extra app-arg lines (e.g. the fixture argument), or empty
#   $4 non-empty enables --timeline plus the mine-timeline-latency.py pass
define xnedit_smoke_target
$$(UI_SMOKE_OUT_ROOT)/xnedit-$(1)/.stamp: FORCE $$(XNEDIT_BIN) $(2)
	$$(Q)rm -rf $$(abspath $$(UI_SMOKE_OUT_ROOT))/xnedit-$(1)
	$$(Q)$$(PYTHON) scripts/run-ui-replay.py \
	    --name xnedit-$(1) \
	    --app $$(abspath $$(XNEDIT_BIN)) \
	    --app-arg=-svrname --app-arg=xnedit-$(1) \
	    --app-arg=-geometry --app-arg=$$(XNEDIT_SMOKE_GEOMETRY) \
	    $(3) \
	    --workdir $$(abspath $$(XNEDIT_WORK_DIR))/source \
	    --replay tests/ui/replays/xnedit-$(1).replay \
	    --out-root $$(abspath $$(UI_SMOKE_OUT_ROOT))/xnedit-$(1) \
	    --display $$(UI_REPLAY_DISPLAY) \
	    --geometry $$(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $$(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $$(XNEDIT_SMOKE_REGION) \
	    --in-process-snapshots \
	    $(if $(4),--timeline) \
	    $$(UI_REPLAY_XVFB) \
	    $$(xnedit_ui_env)
$(if $(4),	$$(Q)$$(PYTHON) scripts/mine-timeline-latency.py \
	    $$(abspath $$(UI_SMOKE_OUT_ROOT))/xnedit-$(1)/timeline.jsonl \
	    --out $$(abspath $$(UI_SMOKE_OUT_ROOT))/xnedit-$(1)/latency.json \
	    --baseline tests/ui/baselines/xnedit-$(1)-latency.json)
	$$(Q)touch $$@
endef

$(eval $(call xnedit_smoke_target,startup))
$(eval $(call xnedit_smoke_target,fixture,$(XNEDIT_FIXTURE),--app-arg=$(XNEDIT_FIXTURE)))
$(eval $(call xnedit_smoke_target,resize,$(XNEDIT_FIXTURE),--app-arg=$(XNEDIT_FIXTURE)))
$(eval $(call xnedit_smoke_target,resize-file-menu,$(XNEDIT_FIXTURE),--app-arg=$(XNEDIT_FIXTURE)))
$(eval $(call xnedit_smoke_target,file-menu,,,timeline))
$(eval $(call xnedit_smoke_target,preferences-menu,,,timeline))
$(eval $(call xnedit_smoke_target,text-fonts-dialog))
$(eval $(call xnedit_smoke_target,typing))

XNEDIT_DIFF_REMOTE ?= node11
XNEDIT_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-xnedit-differential
XNEDIT_DIFF_DISPLAY ?= 112
XNEDIT_DIFF_JOBS ?= 1
XNEDIT_DIFF_INSTALL_DEPS ?= 0
XNEDIT_DIFF_LOCAL ?= 0
XNEDIT_DIFF_MAE_THRESHOLD ?= 0.18
XNEDIT_DIFF_CHANGED_THRESHOLD ?= 0.46
XNEDIT_DIFF_GEOMETRY ?= 1280x1024x24
XNEDIT_DIFF_TOP ?= 12
XNEDIT_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(XNEDIT_DIFF_LOCAL)),local,remote)
XNEDIT_DIFF_OUT_ROOT ?= $(OUT)/xnedit-differential
XNEDIT_DIFF_SCREENSHOT_REGION ?= $(XNEDIT_SMOKE_REGION)

xnedit_diff_env = \
    XNEDIT_DIFF_REMOTE='$(XNEDIT_DIFF_REMOTE)' \
    XNEDIT_DIFF_REMOTE_ROOT='$(XNEDIT_DIFF_REMOTE_ROOT)' \
    XNEDIT_DIFF_DISPLAY='$(XNEDIT_DIFF_DISPLAY)' \
    XNEDIT_DIFF_JOBS='$(XNEDIT_DIFF_JOBS)' \
    XNEDIT_DIFF_MAE_THRESHOLD='$(XNEDIT_DIFF_MAE_THRESHOLD)' \
    XNEDIT_DIFF_CHANGED_THRESHOLD='$(XNEDIT_DIFF_CHANGED_THRESHOLD)' \
    XNEDIT_DIFF_GEOMETRY='$(XNEDIT_DIFF_GEOMETRY)' \
    XNEDIT_DIFF_TOP='$(XNEDIT_DIFF_TOP)' \
    XNEDIT_DIFF_COMPARE_LOCATION='$(XNEDIT_DIFF_COMPARE_LOCATION)' \
    XNEDIT_DIFF_OUT_ROOT='$(abspath $(XNEDIT_DIFF_OUT_ROOT))' \
    XNEDIT_DIFF_SCREENSHOT_REGION='$(XNEDIT_DIFF_SCREENSHOT_REGION)'

## Compare XNEdit screenshots for system libX11 vs libx11-compat.
check-differential-xnedit:
	$(Q)$(xnedit_diff_env) $(PYTHON) scripts/run-xnedit-differential-tests.py \
	    $(if $(filter 1 yes true,$(XNEDIT_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(XNEDIT_DIFF_LOCAL)),--local)
