MOSAIC_URL := https://github.com/thentenaar/mosaic-ng
MOSAIC_REVISION ?= 60b9c0e9e76d249d847b459238efcb3a28225af6
MOSAIC_CLONE_FLAGS ?= --filter=blob:none --no-checkout
MOSAIC_DIR := $(OUT)/upstream/mosaic
MOSAIC_SOURCE_STAMP := $(MOSAIC_DIR)/.source-stamp
MOSAIC_BUILD_DIR := $(OUT)/mosaic
MOSAIC_WORK_DIR := $(MOSAIC_BUILD_DIR)/source
MOSAIC_BIN := $(MOSAIC_WORK_DIR)/src/Mosaic
MOSAIC_MOTIF_PREFIX := $(MOSAIC_BUILD_DIR)/motif-prefix
MOSAIC_MOTIF_INCLUDE := $(MOSAIC_MOTIF_PREFIX)/include
MOSAIC_MOTIF_INCLUDE_STAMP := $(MOSAIC_MOTIF_INCLUDE)/.stamp
MOSAIC_LOG := $(abspath $(MOSAIC_BUILD_DIR))/build.log
MOSAIC_HOME_URL ?= http://info.cern.ch/
MOSAIC_HOME_DEFINE := -DHOME_PAGE_DEFAULT=\\\"$(MOSAIC_HOME_URL)\\\"
MOSAIC_SMOKE_URL := file://localhost$(abspath tests/ui/fixtures/mosaic/index.html)
MOSAIC_LINK_SMOKE_URL := file://localhost$(abspath tests/ui/fixtures/mosaic/link-index.html)
MOSAIC_PATCHES := $(sort $(wildcard compat/mosaic-patches/*.patch))

MOSAIC_CFLAGS ?= -O0 -g
MOSAIC_COMMON_CFLAGS := \
    $(MOSAIC_CFLAGS) \
    -std=gnu89 \
    -fcommon \
    -Wno-everything \
    -DMOTIF \
    -DMOTIF1_2 \
    -DLINUX \
    -DXMOSAIC \
    -D_GNU_SOURCE \
    -D_DARWIN_C_SOURCE

MOSAIC_X11_CFLAGS := \
    -I$(abspath include) \
    -I$(abspath $(OUT)/upstream/include) \
    -I$(abspath include/libxt-build) \
    -I$(abspath $(MOSAIC_MOTIF_INCLUDE))

MOSAIC_X11_LIBS := \
    -L$(abspath $(OUT)) \
    -lXm \
    -lXext-compat \
    -lXmu-compat \
    -lXt-compat \
    -lXpm-compat \
    -lX11-compat

MOSAIC_LDFLAGS := -L$(abspath $(OUT))
ifeq ($(UNAME_S),Linux)
  MOSAIC_LDFLAGS += -Wl,-rpath,$(abspath $(OUT)) -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  MOSAIC_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
endif

MOSAIC_SYS_LIBS := -lm
ifeq ($(UNAME_S),Darwin)
  MOSAIC_SYS_LIBS += -liconv
endif

$(MOSAIC_SOURCE_STAMP): mk/mosaic.mk
	@echo "  GIT     $(MOSAIC_URL)"
	$(Q)mkdir -p $(dir $(MOSAIC_DIR))
	$(Q)if [ -e $(MOSAIC_DIR) ] && [ ! -d $(MOSAIC_DIR)/.git ]; then \
	    echo "  GIT     $(MOSAIC_DIR) exists but is not a git checkout" >&2; \
	    exit 1; \
	fi
	$(Q)test -d $(MOSAIC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(MOSAIC_CLONE_FLAGS) $(MOSAIC_URL) $(MOSAIC_DIR)
	$(Q)cd $(MOSAIC_DIR) && git fetch $(MOTIF_GIT_Q) origin $(MOSAIC_REVISION) || \
	    (cd $(MOSAIC_DIR) && git fetch $(MOTIF_GIT_Q) origin)
	$(Q)cd $(MOSAIC_DIR) && \
	    git checkout $(MOTIF_GIT_Q) --detach $(MOSAIC_REVISION) && \
	    git reset --hard $(MOTIF_GIT_Q) $(MOSAIC_REVISION) >/dev/null && \
	    git clean $(MOTIF_GIT_Q) -fdx >/dev/null
	$(Q)printf '%s\n' '$(MOSAIC_REVISION)' > $@

$(MOSAIC_MOTIF_INCLUDE_STAMP): $(MOTIF_LIBXM) $(MOTIF_LIBMRM) mk/mosaic.mk
	@mkdir -p $(@D)
	$(Q)if [ -L $(MOSAIC_MOTIF_INCLUDE)/Xm ]; then rm -f $(MOSAIC_MOTIF_INCLUDE)/Xm; fi
	$(Q)mkdir -p $(MOSAIC_MOTIF_INCLUDE)/Xm
	$(Q)find $(MOSAIC_MOTIF_INCLUDE)/Xm -type l -delete
	$(Q)for header in $(abspath $(MOTIF_SRC_DIR))/lib/Xm/*.h \
	    $(abspath $(MOTIF_BUILD_DIR))/lib/Xm/*.h; do \
	    ln -sf "$$header" "$(MOSAIC_MOTIF_INCLUDE)/Xm/$$(basename "$$header")"; \
	done
	$(Q)touch $@

.PHONY: mosaic check-smoke-mosaic check-differential-mosaic mosaic-clean
## Build NCSA Mosaic against libx11-compat, libXt-compat, and in-tree Motif
mosaic: $(MOSAIC_SOURCE_STAMP) $(MOSAIC_MOTIF_INCLUDE_STAMP) $(MOSAIC_PATCHES) $(PKGCONFIG_FILES) \
    $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XEXT_COMPAT_TARGET) \
    $(XMU_COMPAT_TARGET) $(MOTIF_LIBXM) $(MOTIF_LIBMRM)
	@mkdir -p $(MOSAIC_BUILD_DIR)
	$(Q)rm -rf $(MOSAIC_WORK_DIR)
	$(Q)mkdir -p $(MOSAIC_WORK_DIR)
	$(Q)tar --exclude .git --exclude '*.o' --exclude '*.a' \
	    --exclude '*.dSYM' --exclude src/Mosaic \
	    -cf - -C $(MOSAIC_DIR) . | \
	    tar -xf - -C $(MOSAIC_WORK_DIR)
	$(Q)set -e; for patch in $(MOSAIC_PATCHES); do \
	    patch -d $(MOSAIC_WORK_DIR) -p1 < "$$patch"; \
	done
	$(Q)touch $(MOSAIC_WORK_DIR)/config.h
	$(Q)printf '#!/bin/sh\nexec %s "$$@"\n' '$(CC)' \
	    > $(MOSAIC_WORK_DIR)/cc-shim
	$(Q)chmod +x $(MOSAIC_WORK_DIR)/cc-shim
	@echo "  MAKE    mosaic-ng"
	$(Q)$(motif_runtime_env) \
	    $(MAKE) -C $(MOSAIC_WORK_DIR) -f makefiles/Makefile.linux \
	        MAKEFLAGS= MFLAGS= \
	        CC='$(abspath $(MOSAIC_WORK_DIR))/cc-shim' \
	        RANLIB='ranlib' \
	        CFLAGS='$(MOSAIC_COMMON_CFLAGS)' \
	        sysconfigflags='-DMOTIF -DMOTIF1_2 -DLINUX' \
	        prereleaseflags='' \
	        customflags='$(MOSAIC_HOME_DEFINE)' \
	        xinc='$(MOSAIC_X11_CFLAGS)' \
	        xlibs='$(MOSAIC_X11_LIBS)' \
	        ldflags='$(MOSAIC_LDFLAGS)' \
	        pngflags='' \
	        pnglibs='' \
	        jpegflags='' \
	        jpeglibs='' \
	        krbflags='' \
	        krblibs='' \
	        syslibs='$(MOSAIC_SYS_LIBS)' \
	        default > $(MOSAIC_LOG) 2>&1 || { \
	            echo "  FAIL    see $(MOSAIC_LOG)" >&2; \
	            tail -40 $(MOSAIC_LOG) >&2; \
	            exit 1; \
	        }
ifeq ($(UNAME_S),Darwin)
	$(Q)test ! -x $(MOSAIC_BIN) || install_name_tool \
	    -change $(abspath $(OUT))/motif-install/lib/libXm.5.dylib \
	    @rpath/libXm.5.dylib $(MOSAIC_BIN)
endif

MOSAIC_DIFF_REMOTE ?= node11
MOSAIC_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-mosaic-differential
MOSAIC_DIFF_DISPLAY ?= 104
MOSAIC_DIFF_JOBS ?= 1
MOSAIC_DIFF_INSTALL_DEPS ?= 0
MOSAIC_DIFF_LOCAL ?= 0
MOSAIC_DIFF_MAE_THRESHOLD ?= 0.16
MOSAIC_DIFF_CHANGED_THRESHOLD ?= 0.42
MOSAIC_DIFF_GEOMETRY ?= 1280x1024x24
MOSAIC_DIFF_TOP ?= 12
MOSAIC_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(MOSAIC_DIFF_LOCAL)),local,remote)
MOSAIC_DIFF_OUT_ROOT ?= $(OUT)/mosaic-differential
MOSAIC_DIFF_REPLAY ?= mosaic-differential.replay

mosaic_diff_env = \
    MOSAIC_DIFF_REMOTE='$(MOSAIC_DIFF_REMOTE)' \
    MOSAIC_DIFF_REMOTE_ROOT='$(MOSAIC_DIFF_REMOTE_ROOT)' \
    MOSAIC_DIFF_DISPLAY='$(MOSAIC_DIFF_DISPLAY)' \
    MOSAIC_DIFF_JOBS='$(MOSAIC_DIFF_JOBS)' \
    MOSAIC_DIFF_MAE_THRESHOLD='$(MOSAIC_DIFF_MAE_THRESHOLD)' \
    MOSAIC_DIFF_CHANGED_THRESHOLD='$(MOSAIC_DIFF_CHANGED_THRESHOLD)' \
    MOSAIC_DIFF_GEOMETRY='$(MOSAIC_DIFF_GEOMETRY)' \
    MOSAIC_DIFF_TOP='$(MOSAIC_DIFF_TOP)' \
    MOSAIC_DIFF_COMPARE_LOCATION='$(MOSAIC_DIFF_COMPARE_LOCATION)' \
    MOSAIC_DIFF_OUT_ROOT='$(abspath $(MOSAIC_DIFF_OUT_ROOT))' \
    MOSAIC_DIFF_REPLAY='$(MOSAIC_DIFF_REPLAY)'

## Compare Mosaic screenshots for system libX11 vs libx11-compat. Set
## MOSAIC_DIFF_LOCAL=1 to run on the current host (used by the GitHub
## Actions differential workflow); otherwise the script SSHes to
## MOSAIC_DIFF_REMOTE.
check-differential-mosaic:
	$(Q)$(mosaic_diff_env) $(PYTHON) scripts/run-mosaic-differential-tests.py \
	    $(if $(filter 1 yes true,$(MOSAIC_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(MOSAIC_DIFF_LOCAL)),--local)

MOSAIC_SMOKE_GEOMETRY ?= 900x720+0+0
MOSAIC_SMOKE_REGION ?= 0,0,900,720

## Run replay-based local Mosaic smoke checks against libx11-compat
check-smoke-mosaic: $(UI_SMOKE_OUT_ROOT)/mosaic-home/.stamp \
    $(UI_SMOKE_OUT_ROOT)/mosaic-link/.stamp \
    $(UI_SMOKE_OUT_ROOT)/mosaic-url-edit/.stamp

$(UI_SMOKE_OUT_ROOT)/mosaic-home/.stamp: FORCE mosaic
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-home
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-home-home
	$(Q)mkdir -p $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-home-home
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name mosaic-home \
	    --app $(abspath $(MOSAIC_BIN)) \
	    --app-arg=-geometry --app-arg=$(MOSAIC_SMOKE_GEOMETRY) \
	    --app-arg $(MOSAIC_SMOKE_URL) \
	    --workdir $(abspath $(MOSAIC_WORK_DIR)) \
	    --replay tests/ui/replays/mosaic-home.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-home \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(MOSAIC_SMOKE_REGION) \
	    --in-process-snapshots \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-home/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-home-home \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env XAPPLRESDIR=$(abspath $(MOSAIC_WORK_DIR)) \
	    --env XFILESEARCHPATH=$(abspath $(MOSAIC_WORK_DIR))/%N:$(abspath $(MOSAIC_WORK_DIR))/%N.ad
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/mosaic-link/.stamp: FORCE mosaic \
    $(UI_SMOKE_OUT_ROOT)/mosaic-home/.stamp
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-link
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-link-home
	$(Q)mkdir -p $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-link-home
	$(Q)sleep 2
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name mosaic-link \
	    --app $(abspath $(MOSAIC_BIN)) \
	    --app-arg=-geometry --app-arg=$(MOSAIC_SMOKE_GEOMETRY) \
	    --app-arg $(MOSAIC_LINK_SMOKE_URL) \
	    --workdir $(abspath $(MOSAIC_WORK_DIR)) \
	    --replay tests/ui/replays/mosaic-link.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-link \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(MOSAIC_SMOKE_REGION) \
	    --in-process-snapshots \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-link/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-link-home \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env XAPPLRESDIR=$(abspath $(MOSAIC_WORK_DIR)) \
	    --env XFILESEARCHPATH=$(abspath $(MOSAIC_WORK_DIR))/%N:$(abspath $(MOSAIC_WORK_DIR))/%N.ad
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/mosaic-url-edit/.stamp: FORCE mosaic \
    $(UI_SMOKE_OUT_ROOT)/mosaic-home/.stamp
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-url-edit
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-url-edit-home
	$(Q)mkdir -p $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-url-edit-home
	$(Q)sleep 2
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name mosaic-url-edit \
	    --app $(abspath $(MOSAIC_BIN)) \
	    --app-arg=-geometry --app-arg=$(MOSAIC_SMOKE_GEOMETRY) \
	    --app-arg $(MOSAIC_SMOKE_URL) \
	    --workdir $(abspath $(MOSAIC_WORK_DIR)) \
	    --replay tests/ui/replays/mosaic-url-edit.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-url-edit \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(MOSAIC_SMOKE_REGION) \
	    --in-process-snapshots \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-url-edit/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/mosaic-url-edit-home \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env XAPPLRESDIR=$(abspath $(MOSAIC_WORK_DIR)) \
	    --env XFILESEARCHPATH=$(abspath $(MOSAIC_WORK_DIR))/%N:$(abspath $(MOSAIC_WORK_DIR))/%N.ad
	$(Q)touch $@

mosaic-clean:
	@echo "  CLEAN   mosaic"
	$(Q)rm -rf $(MOSAIC_BUILD_DIR)
