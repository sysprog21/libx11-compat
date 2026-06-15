OSIRIS_URL := https://centre.libranext.com/libranext/osiris.git
OSIRIS_REVISION ?= 18037370a0b4fd95d495bce83bfccc7c09650f98
OSIRIS_CLONE_FLAGS ?= --filter=blob:none --no-checkout
OSIRIS_SRC_DIR := $(OUT)/upstream/osiris
OSIRIS_SRC_STAMP := $(OSIRIS_SRC_DIR)/.source-stamp
OSIRIS_PATCHES := $(sort $(wildcard compat/osiris-patches/*.patch))
OSIRIS_PATCH_LIST_FILE := $(OUT)/upstream/.osiris-patch-list
$(shell mkdir -p $(dir $(OSIRIS_PATCH_LIST_FILE)); \
        new='$(sort $(notdir $(wildcard compat/osiris-patches/*.patch)))'; \
        old=$$(cat $(OSIRIS_PATCH_LIST_FILE) 2>/dev/null || true); \
        if [ "$$new" != "$$old" ]; then \
            printf '%s\n' "$$new" > $(OSIRIS_PATCH_LIST_FILE); \
        fi)

OSIRIS_BUILD_ROOT := $(OUT)/osiris
OSIRIS_BUILD_DIR := $(OSIRIS_BUILD_ROOT)/build
OSIRIS_CONFIG_STAMP := $(OSIRIS_BUILD_DIR)/.configure-stamp
OSIRIS_BUILD_STAMP := $(OSIRIS_BUILD_DIR)/.build-stamp
OSIRIS_LOG := $(abspath $(OSIRIS_BUILD_ROOT))/build.log
OSIRIS_MESON_LOG := $(abspath $(OSIRIS_BUILD_DIR))/meson-logs/meson-log.txt
OSIRIS_LIB := $(OSIRIS_BUILD_ROOT)/libosiris.so
ifeq ($(UNAME_S),Linux)
  OSIRIS_MT_LIB := $(OSIRIS_BUILD_ROOT)/libosiris-mt.so.2.4.4
  OSIRIS_MT_RUNTIME_LIB := $(OUT)/libosiris-mt.so.2.4.4
else
  OSIRIS_MT_LIB := $(OSIRIS_BUILD_ROOT)/libosiris-mt.2.4.4.dylib
  OSIRIS_MT_RUNTIME_LIB := $(OUT)/libosiris-mt.2.4.4.dylib
endif
OSIRIS_T1 := $(OUT)/examples/osiris/t1

OSIRIS_MESON ?= meson
OSIRIS_NINJA ?= ninja

OSIRIS_MESON_FLAGS := \
    --prefix=$(abspath $(OSIRIS_BUILD_ROOT))/install \
    -Dxft=disabled \
    -Dopengl=disabled \
    -Dsm=disabled \
    -Dmng=disabled \
    -Dgif=disabled \
    -Dexamples=enabled \
    -Dtutorial=enabled

OSIRIS_ENV = \
    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
    DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}

OSIRIS_LDFLAGS := -L$(abspath $(OUT))
ifeq ($(UNAME_S),Linux)
  OSIRIS_LDFLAGS += -Wl,-rpath,$(abspath $(OUT)) -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  OSIRIS_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
  OSIRIS_LDFLAGS += -lresolv
endif

.PHONY: osiris-fetch osiris-configure osiris check-demos-osiris check-smoke-osiris check-differential-osiris osiris-clean

$(OSIRIS_SRC_STAMP): mk/osiris.mk $(OSIRIS_PATCHES) $(OSIRIS_PATCH_LIST_FILE)
	@echo "  GIT     $(OSIRIS_URL)"
	$(Q)mkdir -p $(dir $(OSIRIS_SRC_DIR))
	$(Q)if [ -e $(OSIRIS_SRC_DIR) ] && [ ! -d $(OSIRIS_SRC_DIR)/.git ]; then \
	    echo "  GIT     $(OSIRIS_SRC_DIR) exists but is not a git checkout" >&2; \
	    exit 1; \
	fi
	$(Q)test -d $(OSIRIS_SRC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(OSIRIS_CLONE_FLAGS) $(OSIRIS_URL) $(OSIRIS_SRC_DIR)
	$(Q)cd $(OSIRIS_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin $(OSIRIS_REVISION) || \
	    (cd $(OSIRIS_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin)
	$(Q)cd $(OSIRIS_SRC_DIR) && \
	    git checkout $(MOTIF_GIT_Q) --detach $(OSIRIS_REVISION) && \
	    git reset --hard $(MOTIF_GIT_Q) $(OSIRIS_REVISION) >/dev/null && \
	    git clean $(MOTIF_GIT_Q) -fdx >/dev/null
	$(Q)set -e; for patch in $(abspath $(OSIRIS_PATCHES)); do \
	    cd $(abspath $(OSIRIS_SRC_DIR)); \
	    if git apply --check "$$patch"; then \
	        git apply "$$patch"; \
	    elif git apply --reverse --check "$$patch"; then \
	        :; \
	    else \
	        echo "  PATCH   failed $$patch" >&2; exit 1; \
	    fi; \
	done
	$(Q)touch $@

## Fetch and patch the pinned Osiris source tree
osiris-fetch: $(OSIRIS_SRC_STAMP)

$(OSIRIS_CONFIG_STAMP): mk/osiris.mk $(OSIRIS_SRC_STAMP) $(PKGCONFIG_FILES) \
    $(TARGET) $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(ICE_COMPAT_TARGET) \
    $(SM_COMPAT_TARGET)
	@echo "  MESON   osiris"
	$(Q)rm -rf $(OSIRIS_BUILD_DIR)
	$(Q)mkdir -p $(OSIRIS_BUILD_ROOT)
	$(Q)$(OSIRIS_ENV) \
	    CPPFLAGS="-I$(abspath include) -I$(abspath $(OUT)/upstream/include)" \
	    LDFLAGS="$(OSIRIS_LDFLAGS)" \
	    $(OSIRIS_MESON) setup $(OSIRIS_BUILD_DIR) $(OSIRIS_SRC_DIR) \
	        $(OSIRIS_MESON_FLAGS) > $(OSIRIS_LOG) 2>&1 || { \
	            echo "  FAIL    see $(OSIRIS_LOG)" >&2; \
	            tail -40 $(OSIRIS_LOG) >&2; \
	            exit 1; \
	        }
	$(Q)! grep -E "Run-time dependency (xft|fontconfig|sm|ice|glu|gl|glx)" \
	    $(OSIRIS_MESON_LOG) >/dev/null || { \
	        echo "  FAIL    disabled Osiris dependency was probed" >&2; \
	        grep -E "Run-time dependency (xft|fontconfig|sm|ice|glu|gl|glx)" \
	            $(OSIRIS_MESON_LOG) >&2; \
	        exit 1; \
	    }
	$(Q)touch $@

## Configure Osiris against the compatibility stack
osiris-configure: $(OSIRIS_CONFIG_STAMP)

$(OSIRIS_BUILD_STAMP): mk/osiris.mk $(OSIRIS_CONFIG_STAMP)
	@echo "  NINJA   osiris"
	$(Q)$(OSIRIS_ENV) $(OSIRIS_NINJA) -C $(OSIRIS_BUILD_DIR) > $(OSIRIS_LOG) 2>&1 || { \
	    echo "  FAIL    see $(OSIRIS_LOG)" >&2; \
	    tail -40 $(OSIRIS_LOG) >&2; \
	    exit 1; \
	}
	$(Q)mkdir -p $(dir $(OSIRIS_T1))
	$(Q)lib=$$(find $(OSIRIS_BUILD_DIR) -maxdepth 2 \
	    \( -name 'libosiris.so*' -o -name 'libosiris*.dylib' \) \
	    -type f | grep -v 'libosiris-mt' | sort | head -1); \
	    test -n "$$lib"; \
	    cp "$$lib" $(OSIRIS_LIB)
	$(Q)mtlib=$$(find $(OSIRIS_BUILD_DIR) -maxdepth 2 \
	    \( -name 'libosiris-mt.so*' -o -name 'libosiris-mt*.dylib' \) \
	    -type f | sort | head -1); \
	    test -z "$$mtlib" || { cp "$$mtlib" $(OSIRIS_MT_LIB); \
	        cp "$$mtlib" $(OSIRIS_MT_RUNTIME_LIB); }
	$(Q)t1=$$(find $(OSIRIS_BUILD_DIR) -path '*/tutorial/t1/t1' \
	    -type f -perm -111 | head -1); \
	    test -n "$$t1"; \
	    cp "$$t1" $(OSIRIS_T1)
	$(Q)touch $@

## Build Osiris tutorials and non-GL examples against libx11-compat
osiris: $(OSIRIS_BUILD_STAMP)

## Launch every built Osiris tutorial/example long enough to catch crashes
check-demos-osiris: osiris
	$(Q)scripts/validate-osiris-demos.sh \
	    $(abspath $(OSIRIS_BUILD_DIR)) \
	    $(abspath $(OSIRIS_SRC_DIR)) \
	    $(abspath $(OUT))

OSIRIS_SMOKE_REGION ?= 0,0,360,180
OSIRIS_DIFF_REMOTE ?= node11
OSIRIS_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-osiris-differential
OSIRIS_DIFF_DISPLAY ?= 124
OSIRIS_DIFF_JOBS ?= 1
OSIRIS_DIFF_INSTALL_DEPS ?= 0
OSIRIS_DIFF_LOCAL ?= 0
OSIRIS_DIFF_MAE_THRESHOLD ?= 0.16
OSIRIS_DIFF_CHANGED_THRESHOLD ?= 0.46
OSIRIS_DIFF_GEOMETRY ?= 1280x1024x24
OSIRIS_DIFF_TOP ?= 12
OSIRIS_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(OSIRIS_DIFF_LOCAL)),local,remote)
OSIRIS_DIFF_OUT_ROOT ?= $(OUT)/osiris-differential
OSIRIS_DIFF_SCREENSHOT_REGION ?= $(OSIRIS_SMOKE_REGION)
OSIRIS_REFERENCE_SCREEN_DIR ?= $(OUT)/osiris-reference-screens

osiris_diff_env = \
    OSIRIS_DIFF_REMOTE='$(OSIRIS_DIFF_REMOTE)' \
    OSIRIS_DIFF_REMOTE_ROOT='$(OSIRIS_DIFF_REMOTE_ROOT)' \
    OSIRIS_DIFF_DISPLAY='$(OSIRIS_DIFF_DISPLAY)' \
    OSIRIS_DIFF_JOBS='$(OSIRIS_DIFF_JOBS)' \
    OSIRIS_DIFF_MAE_THRESHOLD='$(OSIRIS_DIFF_MAE_THRESHOLD)' \
    OSIRIS_DIFF_CHANGED_THRESHOLD='$(OSIRIS_DIFF_CHANGED_THRESHOLD)' \
    OSIRIS_DIFF_GEOMETRY='$(OSIRIS_DIFF_GEOMETRY)' \
    OSIRIS_DIFF_TOP='$(OSIRIS_DIFF_TOP)' \
    OSIRIS_DIFF_COMPARE_LOCATION='$(OSIRIS_DIFF_COMPARE_LOCATION)' \
    OSIRIS_DIFF_OUT_ROOT='$(abspath $(OSIRIS_DIFF_OUT_ROOT))' \
    OSIRIS_DIFF_SCREENSHOT_REGION='$(OSIRIS_DIFF_SCREENSHOT_REGION)' \
    OSIRIS_REFERENCE_SCREEN_DIR='$(abspath $(OSIRIS_REFERENCE_SCREEN_DIR))'

## Run replay-based local Osiris tutorial/t1 smoke checks
check-smoke-osiris: $(UI_SMOKE_OUT_ROOT)/osiris-t1/.stamp $(UI_SMOKE_OUT_ROOT)/osiris-designer-menu/.stamp

$(UI_SMOKE_OUT_ROOT)/osiris-t1/.stamp: FORCE osiris
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/osiris-t1
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name osiris-t1 \
	    --app $(abspath $(OSIRIS_T1)) \
	    --app-arg=-geometry --app-arg=100x30+0+0 \
	    --workdir $(abspath tests/ui/fixtures/osiris) \
	    --replay tests/ui/replays/osiris-t1.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/osiris-t1 \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(OSIRIS_SMOKE_REGION) \
	    --in-process-snapshots \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/osiris-t1/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env LIBX11_COMPAT_FONT_DIR=$(abspath $(OUT))/../fonts
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/osiris-designer-menu/.stamp: FORCE osiris
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/osiris-designer-menu
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name osiris-designer-menu \
	    --app $(abspath $(OSIRIS_BUILD_DIR))/designer \
	    --app-arg=-geometry --app-arg=940x740+0+0 \
	    --workdir $(abspath $(OSIRIS_SRC_DIR))/tools/designer/designer \
	    --replay tests/ui/replays/osiris-designer-menu.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/osiris-designer-menu \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    $(UI_REPLAY_XVFB) \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OSIRIS_BUILD_DIR)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OSIRIS_BUILD_DIR)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env LIBX11_COMPAT_FONT_DIR=$(abspath $(OUT))/../fonts
	$(Q)touch $@

## Compare Osiris screenshots for system libX11 vs libx11-compat. Set
## OSIRIS_DIFF_LOCAL=1 to run on the current host (used by the GitHub
## Actions differential workflow); otherwise the script SSHes to
## OSIRIS_DIFF_REMOTE.
check-differential-osiris:
	$(Q)$(osiris_diff_env) $(PYTHON) scripts/run-osiris-differential-tests.py \
	    $(if $(filter 1 yes true,$(OSIRIS_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(OSIRIS_DIFF_LOCAL)),--local)

osiris-clean:
	@echo "  CLEAN   osiris"
	$(Q)rm -rf $(OSIRIS_BUILD_ROOT)
