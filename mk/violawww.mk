VIOLAWWW_URL := https://github.com/bolknote/violawww
VIOLAWWW_REVISION ?= 9d2a4f8c3c26ad02196e87b1b4e2588946725e0f
VIOLAWWW_CLONE_FLAGS ?= --filter=blob:none --no-checkout
VIOLAWWW_DIR := $(OUT)/upstream/violawww-src
VIOLAWWW_SOURCE_STAMP := $(VIOLAWWW_DIR)/.source-stamp
VIOLAWWW_BUILD_DIR := $(OUT)/violawww
VIOLAWWW_WORK_DIR := $(VIOLAWWW_BUILD_DIR)/source
VIOLAWWW_VW := $(VIOLAWWW_WORK_DIR)/src/vw/vw
VIOLAWWW_VIOLA := $(VIOLAWWW_WORK_DIR)/src/viola/viola
VIOLAWWW_MOTIF_PREFIX := $(VIOLAWWW_BUILD_DIR)/motif-prefix
VIOLAWWW_MOTIF_INCLUDE := $(VIOLAWWW_MOTIF_PREFIX)/include
VIOLAWWW_MOTIF_INCLUDE_STAMP := $(VIOLAWWW_MOTIF_INCLUDE)/.stamp
VIOLAWWW_LOG := $(abspath $(VIOLAWWW_BUILD_DIR))/build.log
VIOLAWWW_PATCHES := $(sort $(wildcard compat/violawww-patches/*.patch))

VIOLAWWW_PLATFORM_CFLAGS :=
ifeq ($(UNAME_S),Darwin)
  VIOLAWWW_PLATFORM_CFLAGS += -DVIOLA_DARWIN
endif
ifeq ($(UNAME_S),Linux)
  VIOLAWWW_PLATFORM_CFLAGS += -DVIOLA_LINUX
endif

VIOLAWWW_CFLAGS ?= -O0 -g
VIOLAWWW_COMMON_CFLAGS := \
    $(VIOLAWWW_CFLAGS) \
    -std=gnu17 \
    -funsigned-char \
    -DVIOLA_LIBX11_COMPAT \
    $(VIOLAWWW_PLATFORM_CFLAGS) \
    -DNO_ALLOCA \
    -Wno-everything

VIOLAWWW_X11_CFLAGS := \
    -I$(abspath include) \
    -I$(abspath $(OUT)/upstream/include) \
    -I$(abspath include/libxt-build)

VIOLAWWW_X11_LIBS := \
    -L$(abspath $(OUT)) \
    -lXext-compat \
    -lXmu-compat \
    -lXt-compat \
    -lXpm-compat \
    -lX11-compat

VIOLAWWW_LDFLAGS := -L$(abspath $(OUT))
ifeq ($(UNAME_S),Linux)
  VIOLAWWW_LDFLAGS += -Wl,-rpath,$(abspath $(OUT)) -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  VIOLAWWW_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
endif

VIOLAWWW_LIBS := \
    -lXm \
    $(VIOLAWWW_X11_LIBS) \
    -lm
ifeq ($(UNAME_S),Darwin)
  VIOLAWWW_LIBS += -liconv -framework AudioToolbox -framework CoreFoundation
endif

$(VIOLAWWW_SOURCE_STAMP): mk/violawww.mk
	@echo "  GIT     $(VIOLAWWW_URL)"
	$(Q)mkdir -p $(dir $(VIOLAWWW_DIR))
	$(Q)if [ -e $(VIOLAWWW_DIR) ] && [ ! -d $(VIOLAWWW_DIR)/.git ]; then \
	    echo "  GIT     $(VIOLAWWW_DIR) exists but is not a git checkout" >&2; \
	    exit 1; \
	fi
	$(Q)test -d $(VIOLAWWW_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(VIOLAWWW_CLONE_FLAGS) $(VIOLAWWW_URL) $(VIOLAWWW_DIR)
	# Fetch before checkout so a VIOLAWWW_REVISION bump in this file
	# stays reproducible against a reused clone: a pinned sha won't
	# exist locally until we fetch it. CI uses --no-checkout clone above
	# so no moving upstream branch is ever built before this checkout.
	# The checkout/reset always names $(VIOLAWWW_REVISION) directly so
	# the fallback path (origin's default-branch tip ends up in
	# FETCH_HEAD when the pinned sha is unreachable on its own) still
	# fails loudly instead of silently building upstream HEAD.
	$(Q)cd $(VIOLAWWW_DIR) && git fetch $(MOTIF_GIT_Q) origin $(VIOLAWWW_REVISION) || \
	    (cd $(VIOLAWWW_DIR) && git fetch $(MOTIF_GIT_Q) origin)
	$(Q)cd $(VIOLAWWW_DIR) && \
	    git checkout $(MOTIF_GIT_Q) --detach $(VIOLAWWW_REVISION) && \
	    git reset --hard $(MOTIF_GIT_Q) $(VIOLAWWW_REVISION) >/dev/null && \
	    git clean $(MOTIF_GIT_Q) -fdx >/dev/null
	$(Q)printf '%s\n' '$(VIOLAWWW_REVISION)' > $@

$(VIOLAWWW_MOTIF_INCLUDE_STAMP): $(MOTIF_LIBXM) $(MOTIF_LIBMRM) mk/violawww.mk
	@mkdir -p $(@D)
	$(Q)if [ -L $(VIOLAWWW_MOTIF_INCLUDE)/Xm ]; then rm -f $(VIOLAWWW_MOTIF_INCLUDE)/Xm; fi
	$(Q)mkdir -p $(VIOLAWWW_MOTIF_INCLUDE)/Xm
	$(Q)find $(VIOLAWWW_MOTIF_INCLUDE)/Xm -type l -delete
	$(Q)for header in $(abspath $(MOTIF_SRC_DIR))/lib/Xm/*.h \
	    $(abspath $(MOTIF_BUILD_DIR))/lib/Xm/*.h; do \
	    ln -sf "$$header" "$(VIOLAWWW_MOTIF_INCLUDE)/Xm/$$(basename "$$header")"; \
	done
	$(Q)touch $@

.PHONY: violawww check-differential-violawww check-smoke-violawww check-smoke violawww-clean
## Build ViolaWWW against libx11-compat, libXt-compat, libXpm-compat, and in-tree Motif
violawww: $(VIOLAWWW_SOURCE_STAMP) $(VIOLAWWW_MOTIF_INCLUDE_STAMP) $(PKGCONFIG_FILES) \
    $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XEXT_COMPAT_TARGET) \
    $(XMU_COMPAT_TARGET) $(MOTIF_LIBXM) $(MOTIF_LIBMRM)
	@mkdir -p $(VIOLAWWW_BUILD_DIR)
	$(Q)rm -rf $(VIOLAWWW_WORK_DIR)
	$(Q)mkdir -p $(VIOLAWWW_WORK_DIR)
	$(Q)tar --exclude .git --exclude '*.o' --exclude '*.d' \
	    --exclude '*.a' --exclude '*.dSYM' --exclude src/vw/vw \
	    --exclude src/viola/viola --exclude vplot_dir/vplot \
	    -cf - -C $(VIOLAWWW_DIR) . | \
	    tar -xf - -C $(VIOLAWWW_WORK_DIR)
	$(Q)set -e; for patch in $(VIOLAWWW_PATCHES); do \
	    patch -d $(VIOLAWWW_WORK_DIR) -p1 < "$$patch"; \
	done
	@echo "  MAKE    violawww"
	$(Q)$(motif_runtime_env) \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
	    $(MAKE) -C $(VIOLAWWW_WORK_DIR) \
	        CC='$(CC)' \
	        PKG_CONFIG='$(PKG_CONFIG)' \
	        OPENMOTIF_PREFIX='$(abspath $(VIOLAWWW_MOTIF_PREFIX))' \
	        CFLAGS='$(VIOLAWWW_COMMON_CFLAGS)' \
	        CFLAGS_LIBS='$(VIOLAWWW_COMMON_CFLAGS)' \
	        X11_CFLAGS='$(VIOLAWWW_X11_CFLAGS)' \
	        X11_LIBS='$(VIOLAWWW_X11_LIBS)' \
	        LDFLAGS='$(VIOLAWWW_LDFLAGS)' \
	        LIBS='$(VIOLAWWW_LIBS)' \
	        all > $(VIOLAWWW_LOG) 2>&1 || { \
	            echo "  FAIL    see $(VIOLAWWW_LOG)" >&2; \
	            tail -40 $(VIOLAWWW_LOG) >&2; \
	            exit 1; \
	        }
ifeq ($(UNAME_S),Darwin)
	$(Q)for bin in $(VIOLAWWW_VW) $(VIOLAWWW_VIOLA); do \
	    test ! -x "$$bin" || install_name_tool \
	        -change $(abspath $(OUT))/motif-install/lib/libXm.5.dylib \
	        @rpath/libXm.5.dylib "$$bin"; \
	done
endif

VIOLAWWW_DIFF_REMOTE ?= node11
VIOLAWWW_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-violawww-differential
VIOLAWWW_DIFF_DISPLAY ?= 120
VIOLAWWW_DIFF_JOBS ?= 1
VIOLAWWW_DIFF_INSTALL_DEPS ?= 0
VIOLAWWW_DIFF_LOCAL ?= 0
VIOLAWWW_DIFF_MAE_THRESHOLD ?= 0.12
VIOLAWWW_DIFF_CHANGED_THRESHOLD ?= 0.35
VIOLAWWW_DIFF_SECONDS ?= 5
VIOLAWWW_DIFF_GEOMETRY ?= 1280x1024x24
VIOLAWWW_DIFF_TOP ?= 12
VIOLAWWW_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(VIOLAWWW_DIFF_LOCAL)),local,remote)
VIOLAWWW_DIFF_OUT_ROOT ?= $(OUT)/violawww-differential
VIOLAWWW_SMOKE_DEPS ?= violawww

violawww_diff_env = \
    VIOLAWWW_DIFF_REMOTE='$(VIOLAWWW_DIFF_REMOTE)' \
    VIOLAWWW_DIFF_REMOTE_ROOT='$(VIOLAWWW_DIFF_REMOTE_ROOT)' \
    VIOLAWWW_DIFF_DISPLAY='$(VIOLAWWW_DIFF_DISPLAY)' \
    VIOLAWWW_DIFF_JOBS='$(VIOLAWWW_DIFF_JOBS)' \
    VIOLAWWW_DIFF_MAE_THRESHOLD='$(VIOLAWWW_DIFF_MAE_THRESHOLD)' \
    VIOLAWWW_DIFF_CHANGED_THRESHOLD='$(VIOLAWWW_DIFF_CHANGED_THRESHOLD)' \
    VIOLAWWW_DIFF_SECONDS='$(VIOLAWWW_DIFF_SECONDS)' \
    VIOLAWWW_DIFF_GEOMETRY='$(VIOLAWWW_DIFF_GEOMETRY)' \
    VIOLAWWW_DIFF_TOP='$(VIOLAWWW_DIFF_TOP)' \
    VIOLAWWW_DIFF_COMPARE_LOCATION='$(VIOLAWWW_DIFF_COMPARE_LOCATION)' \
    VIOLAWWW_DIFF_OUT_ROOT='$(abspath $(VIOLAWWW_DIFF_OUT_ROOT))'

## Compare ViolaWWW screenshots for system libX11 vs libx11-compat. Set
## VIOLAWWW_DIFF_LOCAL=1 to run the pipeline on the current host (used
## by the GitHub Actions differential workflow); otherwise the script
## SSHes to VIOLAWWW_DIFF_REMOTE.
check-differential-violawww:
	$(Q)$(violawww_diff_env) $(PYTHON) scripts/run-violawww-differential-tests.py \
	    $(if $(filter 1 yes true,$(VIOLAWWW_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(VIOLAWWW_DIFF_LOCAL)),--local)

# Pin vw's window at a known origin/size so screenshot crops capture
# the X client's pixels instead of whatever the host desktop happens to
# show. Xt parses -geometry WxH+X+Y itself, so this works without any
# WM cooperation.
VIOLAWWW_SMOKE_GEOMETRY ?= 800x720+0+0
VIOLAWWW_SMOKE_REGION   ?= 0,0,800,720
VIOLAWWW_HELP_SMOKE_REGION ?= 0,0,1024,720

## Run replay-based local ViolaWWW smoke checks against libx11-compat
## (scroll + Help-menu popup). Stamp-prereq pattern matches check-smoke-motif
## in mk/motif.mk so each replay runs even when its output directory already
## exists from a previous invocation (FORCE makes the stamp always rebuild).
check-smoke-violawww: $(UI_SMOKE_OUT_ROOT)/violawww-scroll/.stamp \
                     $(UI_SMOKE_OUT_ROOT)/violawww-help/.stamp

$(UI_SMOKE_OUT_ROOT)/violawww-scroll/.stamp: FORCE $(VIOLAWWW_SMOKE_DEPS)
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name violawww-scroll \
	    --app $(abspath $(VIOLAWWW_VW)) \
	    --app-arg=-geometry --app-arg=$(VIOLAWWW_SMOKE_GEOMETRY) \
	    --app-arg file:$(abspath tests/ui/fixtures/violawww-scroll.html) \
	    --workdir $(abspath $(VIOLAWWW_WORK_DIR)) \
	    --replay tests/ui/replays/violawww-scroll.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/violawww-scroll \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(VIOLAWWW_SMOKE_REGION) \
	    --in-process-snapshots \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/violawww-scroll/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env WWW_HOME=file:$(abspath tests/ui/fixtures/violawww-scroll.html)
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/violawww-help/.stamp: FORCE $(VIOLAWWW_SMOKE_DEPS)
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name violawww-help \
	    --app $(abspath $(VIOLAWWW_VW)) \
	    --app-arg=-geometry --app-arg=$(VIOLAWWW_SMOKE_GEOMETRY) \
	    --app-arg file:$(abspath tests/ui/fixtures/violawww-scroll.html) \
	    --workdir $(abspath $(VIOLAWWW_WORK_DIR)) \
	    --replay tests/ui/replays/violawww-help.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/violawww-help \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(VIOLAWWW_HELP_SMOKE_REGION) \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/violawww-help/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env WWW_HOME=file:$(abspath tests/ui/fixtures/violawww-scroll.html)
	$(Q)touch $@

## Run local replay-only UI smoke checks that do not require node11
check-smoke: check-smoke-violawww check-smoke-motif

violawww-clean:
	@echo "  CLEAN   violawww"
	$(Q)rm -rf $(VIOLAWWW_BUILD_DIR)
