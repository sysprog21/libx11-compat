XEPHEM_URL := https://github.com/XEphem/XEphem
XEPHEM_REVISION := c8a0157284aa898c5308d4dfa8de9e2ff1660c89
XEPHEM_CLONE_FLAGS ?= --filter=blob:none --no-checkout
XEPHEM_SRC_DIR := $(OUT)/upstream/xephem-src
XEPHEM_SOURCE_STAMP := $(XEPHEM_SRC_DIR)/.source-stamp
XEPHEM_PATCHES := $(sort $(wildcard compat/xephem-patches/*.patch))

XEPHEM_BUILD_DIR := $(OUT)/xephem
XEPHEM_WORK_DIR := $(XEPHEM_BUILD_DIR)/source
XEPHEM_GUI_DIR := $(XEPHEM_WORK_DIR)/GUI/xephem
XEPHEM_BIN := $(XEPHEM_GUI_DIR)/xephem
XEPHEM_LOG := $(abspath $(XEPHEM_BUILD_DIR))/build.log
XEPHEM_SYSROOT := $(XEPHEM_BUILD_DIR)/sysroot
XEPHEM_SYSROOT_STAMP := $(XEPHEM_SYSROOT)/.stamp

XEPHEM_OPENSSL_CFLAGS := $(shell $(PKG_CONFIG) --cflags openssl 2>/dev/null)
XEPHEM_OPENSSL_LIBS := $(shell $(PKG_CONFIG) --libs openssl 2>/dev/null || printf '%s\n' '-lssl -lcrypto')

XEPHEM_RPATH_FLAGS :=
ifeq ($(UNAME_S),Linux)
  XEPHEM_RPATH_FLAGS += -Wl,-rpath,$(abspath $(OUT)) -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  XEPHEM_RPATH_FLAGS += -Wl,-rpath,$(abspath $(OUT))
endif

# Relative paths used as prerequisites: they match the in-tree build rules
# (mk/motif.mk for libXm, mk/library.mk for the compat libs), so a clean
# `make xephem` builds those libraries instead of failing with "No rule to make
# target" on an absolute path that matches nothing.
XEPHEM_XLIB_DEPS := \
    $(MOTIF_LIBXM) \
    $(LIBXT_TARGET) \
    $(XEXT_COMPAT_TARGET) \
    $(XMU_COMPAT_TARGET) \
    $(TARGET)
# Absolute paths for the link line: the GUI sub-make runs in XEPHEM_GUI_DIR.
XEPHEM_XLIBS := $(foreach lib,$(XEPHEM_XLIB_DEPS),$(abspath $(lib)))

$(XEPHEM_SOURCE_STAMP): mk/xephem.mk
	@echo "  GIT     $(XEPHEM_URL)"
	$(Q)mkdir -p $(dir $(XEPHEM_SRC_DIR))
	$(Q)test -d $(XEPHEM_SRC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(XEPHEM_CLONE_FLAGS) $(XEPHEM_URL) $(XEPHEM_SRC_DIR)
	$(Q)cd $(XEPHEM_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin $(XEPHEM_REVISION) || \
	    (cd $(XEPHEM_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin)
	$(Q)cd $(XEPHEM_SRC_DIR) && \
	    git checkout $(MOTIF_GIT_Q) --detach $(XEPHEM_REVISION) && \
	    git reset --hard $(MOTIF_GIT_Q) $(XEPHEM_REVISION) >/dev/null && \
	    git clean $(MOTIF_GIT_Q) -fdx >/dev/null
	$(Q)printf '%s\n' '$(XEPHEM_REVISION)' > $@

$(XEPHEM_SYSROOT_STAMP): $(LIBXT_TARGET) $(MOTIF_STAGE_STAMP) mk/xephem.mk
	@echo "  SYSROOT xephem"
	$(Q)rm -rf $(XEPHEM_SYSROOT)
	$(Q)mkdir -p $(XEPHEM_SYSROOT)/X11/extensions $(XEPHEM_SYSROOT)/Xm
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ "$$b" = extensions ] && continue; \
	    ln -sf "$$e" "$(XEPHEM_SYSROOT)/X11/$$b"; \
	done
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/extensions/* \
	             $(abspath include)/X11/extensions/*; do \
	    ln -sf "$$e" "$(XEPHEM_SYSROOT)/X11/extensions/$$(basename "$$e")"; \
	done
	$(Q)for e in $(abspath include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ -e "$(XEPHEM_SYSROOT)/X11/$$b" ] || \
	        ln -sf "$$e" "$(XEPHEM_SYSROOT)/X11/$$b"; \
	done
	$(Q)for h in $(abspath $(MOTIF_SRC_DIR))/lib/Xm/*.h \
	             $(abspath $(MOTIF_BUILD_DIR))/lib/Xm/*.h; do \
	    ln -sf "$$h" "$(XEPHEM_SYSROOT)/Xm/$$(basename "$$h")"; \
	done
	$(Q)touch $@

.PHONY: xephem check-smoke-xephem check-differential-xephem xephem-clean
## Build XEphem against the Motif + libx11-compat stack
xephem: $(XEPHEM_BIN)

$(XEPHEM_BIN): $(XEPHEM_SOURCE_STAMP) $(XEPHEM_PATCHES) $(XEPHEM_SYSROOT_STAMP) \
    $(XEPHEM_XLIB_DEPS)
	@mkdir -p $(XEPHEM_BUILD_DIR)
	$(Q)rm -f $(XEPHEM_LOG)
	$(Q)rm -rf $(XEPHEM_WORK_DIR)
	$(Q)mkdir -p $(XEPHEM_WORK_DIR)
	$(Q)tar --exclude .git --exclude '*.o' --exclude '*.a' \
	    --exclude GUI/xephem/xephem \
	    -cf - -C $(XEPHEM_SRC_DIR) . | tar -xf - -C $(XEPHEM_WORK_DIR)
	$(Q)set -e; for patch in $(XEPHEM_PATCHES); do \
	    patch -d $(XEPHEM_WORK_DIR) -p1 < "$$patch"; \
	done
	@echo "  MAKE    xephem support libs"
	$(Q)for dir in libastro libip libjpegd liblilxml libpng libz; do \
	    env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(XEPHEM_WORK_DIR)/$$dir \
	        CC='$(CC)' >> $(XEPHEM_LOG) 2>&1 || { \
	        echo "  FAIL    see $(XEPHEM_LOG)" >&2; tail -60 $(XEPHEM_LOG) >&2; exit 1; \
	    }; \
	done
	@echo "  MAKE    xephem"
	$(Q)$(motif_runtime_env) env -u MAKEFLAGS -u MFLAGS \
	    $(MAKE) -C $(XEPHEM_GUI_DIR) xephem \
	        CC='$(CC)' \
	        MOTIFI='$(abspath $(XEPHEM_SYSROOT))' \
	        MOTIFL='$(abspath $(OUT))' \
	        PLATI='$(XEPHEM_OPENSSL_CFLAGS) -DXEPHEM_DEFAULT_SHAREDIR=\"$(abspath $(XEPHEM_GUI_DIR))\"' \
	        PLATL='$(XEPHEM_RPATH_FLAGS)' \
	        XLIBS='$(XEPHEM_XLIBS)' \
	        LIBS='$$(XLIBS) $$(LIBLIB) -lm $(XEPHEM_OPENSSL_LIBS)' \
	        >> $(XEPHEM_LOG) 2>&1 || { \
	            echo "  FAIL    see $(XEPHEM_LOG)" >&2; \
	            tail -60 $(XEPHEM_LOG) >&2; \
	            exit 1; \
	        }

XEPHEM_SMOKE_REGION ?= 0,0,1000,700
xephem_ui_lib_path = $(abspath $(OUT))$(if $(SDL_RUNTIME_LIBDIR),:$(SDL_RUNTIME_LIBDIR))
xephem_ui_env = \
    --env DYLD_LIBRARY_PATH=$(xephem_ui_lib_path)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    --env LD_LIBRARY_PATH=$(xephem_ui_lib_path)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/xephem-home

## Run a replay-based XEphem startup smoke check against libx11-compat
check-smoke-xephem: $(UI_SMOKE_OUT_ROOT)/xephem-startup/.stamp

$(UI_SMOKE_OUT_ROOT)/xephem-startup/.stamp: FORCE $(XEPHEM_BIN)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/xephem-startup
	$(Q)mkdir -p $(abspath $(UI_SMOKE_OUT_ROOT))/xephem-home/Library
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xephem-startup \
	    --app $(abspath $(XEPHEM_BIN)) \
	    --workdir $(abspath $(XEPHEM_GUI_DIR)) \
	    --replay tests/ui/replays/xephem-startup.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xephem-startup \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XEPHEM_SMOKE_REGION) \
	    --in-process-snapshots \
	    --offscreen \
	    $(xephem_ui_env)
	$(Q)touch $@

XEPHEM_DIFF_REMOTE ?= node11
XEPHEM_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-xephem-differential
XEPHEM_DIFF_DISPLAY ?= 118
XEPHEM_DIFF_JOBS ?= 1
XEPHEM_DIFF_INSTALL_DEPS ?= 0
XEPHEM_DIFF_LOCAL ?= 0
# Calibrated on node11 (OpenMotif 2.3.8): the startup control window
# measures MAE 0.140, changed-pixel 0.387 against the libx11-compat
# stack. XEphem mixes Motif chrome with a custom-drawn sky canvas and
# the two sides render text through different font engines (system core
# / Xft vs SDL_ttf), so the baseline diff sits higher than a chrome-only
# client. Thresholds carry ~40-57% headroom over the measured values for
# runner-to-runner antialiasing / hinting / timing variance while still
# tripping on a blank-window or gross-layout regression.
XEPHEM_DIFF_MAE_THRESHOLD ?= 0.22
XEPHEM_DIFF_CHANGED_THRESHOLD ?= 0.55
XEPHEM_DIFF_GEOMETRY ?= 1280x1024x24
XEPHEM_DIFF_TOP ?= 12
XEPHEM_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(XEPHEM_DIFF_LOCAL)),local,remote)
XEPHEM_DIFF_OUT_ROOT ?= $(OUT)/xephem-differential
XEPHEM_DIFF_SCREENSHOT_REGION ?= $(XEPHEM_SMOKE_REGION)

xephem_diff_env = \
    XEPHEM_DIFF_REMOTE='$(XEPHEM_DIFF_REMOTE)' \
    XEPHEM_DIFF_REMOTE_ROOT='$(XEPHEM_DIFF_REMOTE_ROOT)' \
    XEPHEM_DIFF_DISPLAY='$(XEPHEM_DIFF_DISPLAY)' \
    XEPHEM_DIFF_JOBS='$(XEPHEM_DIFF_JOBS)' \
    XEPHEM_DIFF_MAE_THRESHOLD='$(XEPHEM_DIFF_MAE_THRESHOLD)' \
    XEPHEM_DIFF_CHANGED_THRESHOLD='$(XEPHEM_DIFF_CHANGED_THRESHOLD)' \
    XEPHEM_DIFF_GEOMETRY='$(XEPHEM_DIFF_GEOMETRY)' \
    XEPHEM_DIFF_TOP='$(XEPHEM_DIFF_TOP)' \
    XEPHEM_DIFF_COMPARE_LOCATION='$(XEPHEM_DIFF_COMPARE_LOCATION)' \
    XEPHEM_DIFF_OUT_ROOT='$(abspath $(XEPHEM_DIFF_OUT_ROOT))' \
    XEPHEM_DIFF_SCREENSHOT_REGION='$(XEPHEM_DIFF_SCREENSHOT_REGION)'

## Compare XEphem screenshots for system OpenMotif vs libx11-compat.
check-differential-xephem:
	$(Q)$(xephem_diff_env) $(PYTHON) scripts/run-xephem-differential-tests.py \
	    $(if $(filter 1 yes true,$(XEPHEM_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(XEPHEM_DIFF_LOCAL)),--local)

xephem-clean:
	@echo "  CLEAN   xephem"
	$(Q)rm -rf $(XEPHEM_BUILD_DIR)
