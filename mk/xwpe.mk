XWPE_URL := https://github.com/vejeta/xwpe
XWPE_REVISION := 9192e0c4b531831728ae937c643c45aa7f3a624b
XWPE_CLONE_FLAGS ?= --filter=blob:none --no-checkout
XWPE_SRC_DIR := $(OUT)/upstream/xwpe
XWPE_SOURCE_STAMP := $(XWPE_SRC_DIR)/.source-stamp
XWPE_PATCHES := $(sort $(wildcard compat/xwpe-patches/*.patch))

XWPE_BUILD_DIR := $(OUT)/xwpe
XWPE_WORK_DIR := $(XWPE_BUILD_DIR)/source
XWPE_BIN := $(XWPE_WORK_DIR)/xwpe
XWPE_LOG := $(abspath $(XWPE_BUILD_DIR))/build.log
XWPE_LIB_ALIASES := $(XWPE_BUILD_DIR)/lib-aliases
XWPE_LIB_ALIASES_STAMP := $(XWPE_LIB_ALIASES)/.stamp
XWPE_PKG_CONFIG := $(XWPE_BUILD_DIR)/pkg-config

XWPE_RPATH_FLAGS :=
ifeq ($(UNAME_S),Linux)
  XWPE_RPATH_FLAGS += -Wl,-rpath,$(abspath $(OUT)) \
      -Wl,-rpath,$(abspath $(XWPE_LIB_ALIASES)) \
      -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  XWPE_RPATH_FLAGS += -Wl,-rpath,$(abspath $(OUT)) \
      -Wl,-rpath,$(abspath $(XWPE_LIB_ALIASES))
endif

XWPE_CPPFLAGS := -I$(abspath include) -I$(abspath $(OUT)/upstream/include)
XWPE_LDFLAGS := -L$(abspath $(XWPE_LIB_ALIASES)) -L$(abspath $(OUT)) \
    $(XWPE_RPATH_FLAGS)

$(XWPE_SOURCE_STAMP): mk/xwpe.mk
	@echo "  GIT     $(XWPE_URL)"
	$(Q)mkdir -p $(dir $(XWPE_SRC_DIR))
	$(Q)test -d $(XWPE_SRC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(XWPE_CLONE_FLAGS) $(XWPE_URL) $(XWPE_SRC_DIR)
	$(Q)cd $(XWPE_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin $(XWPE_REVISION) || \
	    (cd $(XWPE_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin)
	$(Q)cd $(XWPE_SRC_DIR) && \
	    git checkout $(MOTIF_GIT_Q) --detach $(XWPE_REVISION) && \
	    git reset --hard $(MOTIF_GIT_Q) $(XWPE_REVISION) >/dev/null && \
	    git clean $(MOTIF_GIT_Q) -fdx >/dev/null
	$(Q)printf '%s\n' '$(XWPE_REVISION)' > $@

$(XWPE_LIB_ALIASES_STAMP): $(TARGET) $(XFT_COMPAT_TARGET) \
    $(XEXT_COMPAT_TARGET) mk/xwpe.mk
	@mkdir -p $(XWPE_LIB_ALIASES)
	$(Q)rm -f $(XWPE_LIB_ALIASES)/libX*.so $(XWPE_LIB_ALIASES)/libX*.dylib
	$(Q)set -e; for pair in \
	    libX11.so:libX11-compat.so \
	    libXft.so:libXft-compat.so \
	    libXext.so:libXext-compat.so \
	    libXrender.so:libX11-compat.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(XWPE_LIB_ALIASES)/$$alias"; \
	done
ifeq ($(UNAME_S),Darwin)
	$(Q)set -e; for pair in \
	    libX11.dylib:libX11-compat.so \
	    libXft.dylib:libXft-compat.so \
	    libXext.dylib:libXext-compat.so \
	    libXrender.dylib:libX11-compat.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(XWPE_LIB_ALIASES)/$$alias"; \
	done
endif
	$(Q)touch $@

.PHONY: xwpe check-smoke-xwpe check-differential-xwpe xwpe-clean
## Build xwpe against the libx11-compat + Xft compatibility stack
xwpe: $(XWPE_BIN)

$(XWPE_BIN): $(XWPE_SOURCE_STAMP) $(XWPE_PATCHES) $(PKGCONFIG_FILES) \
    $(XWPE_LIB_ALIASES_STAMP)
	@mkdir -p $(XWPE_BUILD_DIR)
	$(Q)rm -rf $(XWPE_WORK_DIR)
	$(Q)mkdir -p $(XWPE_WORK_DIR)
	$(Q)tar --exclude .git -cf - -C $(XWPE_SRC_DIR) . | tar -xf - -C $(XWPE_WORK_DIR)
	$(Q)set -e; for patch in $(XWPE_PATCHES); do \
	    patch --fuzz=0 --no-backup-if-mismatch -d $(XWPE_WORK_DIR) -p1 < "$$patch"; \
	done
	$(Q)printf '%s\n' \
	    '#!/bin/sh' \
	    'for arg do' \
	    '  case "$$arg" in *cairo*|*pangocairo*|vterm|vterm03) exit 1;; esac' \
	    'done' \
	    'exec "$(PKG_CONFIG)" "$$@"' > $(XWPE_PKG_CONFIG)
	$(Q)chmod +x $(XWPE_PKG_CONFIG)
	@echo "  CONF    xwpe"
	$(Q)cd $(XWPE_WORK_DIR) && autoreconf -fi >> $(XWPE_LOG) 2>&1 && \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
	    PKG_CONFIG=$(abspath $(XWPE_PKG_CONFIG)) \
	    CC='$(CC)' \
	    CPPFLAGS='$(XWPE_CPPFLAGS)' \
	    CFLAGS='$(CFLAGS) $(CFLAGS_EXTRA)' \
	    LDFLAGS='$(XWPE_LDFLAGS)' \
	    ./configure --prefix=$(abspath $(XWPE_BUILD_DIR))/install \
	        --without-gpm \
	        --x-includes=$(abspath include) \
	        --x-libraries=$(abspath $(XWPE_LIB_ALIASES)) \
	        >> $(XWPE_LOG) 2>&1 || { \
	            echo "  FAIL    see $(XWPE_LOG)" >&2; \
	            tail -60 $(XWPE_LOG) >&2; \
	            exit 1; \
	        }
	@echo "  MAKE    xwpe"
	$(Q)$(MAKE) -C $(XWPE_WORK_DIR) >> $(XWPE_LOG) 2>&1 || { \
	    echo "  FAIL    see $(XWPE_LOG)" >&2; \
	    tail -60 $(XWPE_LOG) >&2; \
	    exit 1; \
	}

XWPE_SMOKE_REGION ?= 0,0,900,650
# The resize test grows the window to 1100x800, so it needs a wider capture
# region than the fixed-geometry startup/input tests to sample the enlarged frame.
XWPE_RESIZE_SMOKE_REGION ?= 0,0,1100,800
xwpe_ui_lib_path = $(abspath $(OUT)):$(abspath $(XWPE_LIB_ALIASES))$(if $(SDL_RUNTIME_LIBDIR),:$(SDL_RUNTIME_LIBDIR))
xwpe_ui_env = \
    --env DYLD_LIBRARY_PATH=$(xwpe_ui_lib_path)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    --env LD_LIBRARY_PATH=$(xwpe_ui_lib_path)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
    --env XWPE_LIB=$(abspath $(XWPE_WORK_DIR))

## Run replay-based xwpe/xwe startup and input smoke checks against libx11-compat
check-smoke-xwpe: $(UI_SMOKE_OUT_ROOT)/xwe-startup/.stamp \
                  $(UI_SMOKE_OUT_ROOT)/xwpe-startup/.stamp \
                  $(UI_SMOKE_OUT_ROOT)/xwpe-mouse-dialog/.stamp \
                  $(UI_SMOKE_OUT_ROOT)/xwpe-popup-close/.stamp \
                  $(UI_SMOKE_OUT_ROOT)/xwpe-keyboard-edit/.stamp \
                  $(UI_SMOKE_OUT_ROOT)/xwpe-resize/.stamp

$(UI_SMOKE_OUT_ROOT)/xwe-startup/.stamp: FORCE $(XWPE_BIN)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/xwe-startup
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xwe-startup \
	    --app $(abspath $(XWPE_WORK_DIR))/xwe \
	    --workdir $(abspath $(XWPE_WORK_DIR)) \
	    --replay tests/ui/replays/xwpe-startup.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xwe-startup \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XWPE_SMOKE_REGION) \
	    --in-process-snapshots \
	    --offscreen \
	    $(xwpe_ui_env)
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/xwpe-startup/.stamp: FORCE $(XWPE_BIN)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-startup
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xwpe-startup \
	    --app $(abspath $(XWPE_BIN)) \
	    --workdir $(abspath $(XWPE_WORK_DIR)) \
	    --replay tests/ui/replays/xwpe-startup.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-startup \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XWPE_SMOKE_REGION) \
	    --in-process-snapshots \
	    --offscreen \
	    $(xwpe_ui_env)
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/xwpe-mouse-dialog/.stamp: FORCE $(XWPE_BIN)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-mouse-dialog
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xwpe-mouse-dialog \
	    --app $(abspath $(XWPE_BIN)) \
	    --workdir $(abspath $(XWPE_WORK_DIR)) \
	    --replay tests/ui/replays/xwpe-mouse-dialog.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-mouse-dialog \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XWPE_SMOKE_REGION) \
	    --in-process-snapshots \
	    --offscreen \
	    $(xwpe_ui_env)
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/xwpe-popup-close/.stamp: FORCE $(XWPE_BIN)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-popup-close
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xwpe-popup-close \
	    --app $(abspath $(XWPE_BIN)) \
	    --workdir $(abspath $(XWPE_WORK_DIR)) \
	    --replay tests/ui/replays/xwpe-popup-close.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-popup-close \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XWPE_SMOKE_REGION) \
	    --in-process-snapshots \
	    --offscreen \
	    $(xwpe_ui_env)
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/xwpe-keyboard-edit/.stamp: FORCE $(XWPE_BIN)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-keyboard-edit
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xwpe-keyboard-edit \
	    --app $(abspath $(XWPE_BIN)) \
	    --app-arg $(abspath tests/ui/fixtures/xnedit-fixture.txt) \
	    --workdir $(abspath $(XWPE_WORK_DIR)) \
	    --replay tests/ui/replays/xwpe-keyboard-edit.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-keyboard-edit \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XWPE_SMOKE_REGION) \
	    --in-process-snapshots \
	    --offscreen \
	    $(xwpe_ui_env)
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/xwpe-resize/.stamp: FORCE $(XWPE_BIN)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-resize
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xwpe-resize \
	    --app $(abspath $(XWPE_BIN)) \
	    --workdir $(abspath $(XWPE_WORK_DIR)) \
	    --replay tests/ui/replays/xwpe-resize.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xwpe-resize \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XWPE_RESIZE_SMOKE_REGION) \
	    --in-process-snapshots \
	    --offscreen \
	    $(xwpe_ui_env)
	$(Q)touch $@

XWPE_DIFF_REMOTE ?= node11
XWPE_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-xwpe-differential
XWPE_DIFF_DISPLAY ?= 114
XWPE_DIFF_JOBS ?= 1
XWPE_DIFF_INSTALL_DEPS ?= 0
XWPE_DIFF_LOCAL ?= 0
# Both sides drive the same patched Xlib chrome render path, but the native
# Xft text path and the compat SDL_ttf path pick slightly different line
# heights, so the File-Manager dialog centers a bit differently and every
# text row carries an antialiasing fringe. That puts the baseline mean
# error near 0.14 and the changed-pixel ratio near 0.24 (measured on
# node11); the thresholds leave headroom for that structural gap plus
# runner-to-runner font variance while still failing on a gross regression
# (a blank pane, an unpainted background, or inverted colors all push the
# error well past these).
XWPE_DIFF_MAE_THRESHOLD ?= 0.24
XWPE_DIFF_CHANGED_THRESHOLD ?= 0.45
XWPE_DIFF_GEOMETRY ?= 1280x1024x24
XWPE_DIFF_TOP ?= 12
XWPE_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(XWPE_DIFF_LOCAL)),local,remote)
XWPE_DIFF_OUT_ROOT ?= $(OUT)/xwpe-differential
XWPE_DIFF_SCREENSHOT_REGION ?= 0,0,660,780

xwpe_diff_env = \
    XWPE_DIFF_REMOTE='$(XWPE_DIFF_REMOTE)' \
    XWPE_DIFF_REMOTE_ROOT='$(XWPE_DIFF_REMOTE_ROOT)' \
    XWPE_DIFF_DISPLAY='$(XWPE_DIFF_DISPLAY)' \
    XWPE_DIFF_JOBS='$(XWPE_DIFF_JOBS)' \
    XWPE_DIFF_MAE_THRESHOLD='$(XWPE_DIFF_MAE_THRESHOLD)' \
    XWPE_DIFF_CHANGED_THRESHOLD='$(XWPE_DIFF_CHANGED_THRESHOLD)' \
    XWPE_DIFF_GEOMETRY='$(XWPE_DIFF_GEOMETRY)' \
    XWPE_DIFF_TOP='$(XWPE_DIFF_TOP)' \
    XWPE_DIFF_COMPARE_LOCATION='$(XWPE_DIFF_COMPARE_LOCATION)' \
    XWPE_DIFF_OUT_ROOT='$(abspath $(XWPE_DIFF_OUT_ROOT))' \
    XWPE_DIFF_SCREENSHOT_REGION='$(XWPE_DIFF_SCREENSHOT_REGION)'

## Build xwpe against system libX11 and libx11-compat, capture the
## File-Manager startup on each, and diff. XWPE_DIFF_LOCAL=1 runs the
## build / capture / compare pipeline on the local host (the CI path);
## otherwise the script SSHes to XWPE_DIFF_REMOTE.
check-differential-xwpe:
	$(Q)$(xwpe_diff_env) $(PYTHON) scripts/run-xwpe-differential-tests.py \
	    $(if $(filter 1 yes true,$(XWPE_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(XWPE_DIFF_LOCAL)),--local)

xwpe-clean:
	@echo "  CLEAN   xwpe"
	$(Q)rm -rf $(XWPE_BUILD_DIR)
