# Build xclock-1.1.1 against the libx11-compat / libXt-compat /
# libXaw-compat / libXmu-compat shim soname set. xclock is an autoconf
# project, so the recipe stages a fresh source tree per make invocation
# then runs the bundled configure with PKG_CONFIG_PATH pointing at
# build/pkgconfig/ so it finds xaw7.pc / xmu.pc / xt.pc / x11.pc /
# xproto.pc rather than the host's system libX11.

XCLOCK_VERSION := 1.1.1
XCLOCK_URL := https://www.x.org/releases/individual/app/xclock-$(XCLOCK_VERSION).tar.xz
XCLOCK_SHA256 := df7ceabf8f07044a2fde4924d794554996811640a45de40cb12c2cf1f90f742c
XCLOCK_CACHE_DIR := $(OUT)/upstream/.cache
XCLOCK_TARBALL := $(XCLOCK_CACHE_DIR)/xclock-$(XCLOCK_VERSION).tar.xz
XCLOCK_DIR := $(OUT)/upstream/xclock-$(XCLOCK_VERSION)
XCLOCK_SOURCE_STAMP := $(XCLOCK_DIR)/.source-stamp
XCLOCK_BUILD_DIR := $(OUT)/xclock
XCLOCK_WORK_DIR := $(XCLOCK_BUILD_DIR)/source
XCLOCK_BIN := $(XCLOCK_WORK_DIR)/xclock
XCLOCK_LOG := $(abspath $(XCLOCK_BUILD_DIR))/build.log
XCLOCK_PATCHES := $(sort $(wildcard compat/xclock-patches/*.patch))

XCLOCK_LDFLAGS := -L$(abspath $(OUT))
ifeq ($(UNAME_S),Linux)
  XCLOCK_LDFLAGS += -Wl,-rpath,$(abspath $(OUT)) -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  XCLOCK_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
endif

# The xkb path needs xkbfile.pc which we do not stub, and the Xft path
# pulls libXrender + libXft into the link line; libXrender is not
# packaged here (the Xrender.h shim is header-only) so xft pkg-config
# would resolve but the final link would fail. Disable both at configure
# time; xclock falls back to its non-Xft glyph rendering.
XCLOCK_CONFIGURE_FLAGS := \
    --without-xkb \
    --without-xft

$(XCLOCK_TARBALL): | $(XCLOCK_CACHE_DIR)
	@echo "  FETCH   $(XCLOCK_URL)"
	$(Q)curl -fsSL -o $@.tmp $(XCLOCK_URL)
	$(Q)echo "$(XCLOCK_SHA256)  $@.tmp" | shasum -a 256 -c -
	$(Q)mv $@.tmp $@

$(XCLOCK_CACHE_DIR):
	@mkdir -p $@

$(XCLOCK_SOURCE_STAMP): $(XCLOCK_TARBALL) mk/xclock.mk
	@echo "  EXTRACT xclock-$(XCLOCK_VERSION)"
	$(Q)rm -rf $(XCLOCK_DIR)
	$(Q)mkdir -p $(dir $(XCLOCK_DIR))
	$(Q)tar -xJf $(XCLOCK_TARBALL) -C $(dir $(XCLOCK_DIR))
	$(Q)touch $@

.PHONY: xclock xclock-clean
## Build xclock against the libx11-compat shim stack
xclock: $(XCLOCK_BIN)

$(XCLOCK_BIN): $(XCLOCK_SOURCE_STAMP) $(XCLOCK_PATCHES) $(PKGCONFIG_FILES) \
    $(LIBXAW_TARGET) $(LIBXT_TARGET) $(XMU_COMPAT_TARGET) \
    $(LIBXPM_TARGET) $(TARGET)
	@mkdir -p $(XCLOCK_BUILD_DIR)
	$(Q)rm -rf $(XCLOCK_WORK_DIR)
	$(Q)mkdir -p $(XCLOCK_WORK_DIR)
	$(Q)tar -cf - -C $(XCLOCK_DIR) . | tar -xf - -C $(XCLOCK_WORK_DIR)
	$(Q)set -e; for patch in $(XCLOCK_PATCHES); do \
	    patch -d $(XCLOCK_WORK_DIR) -p1 < "$$patch"; \
	done
	@echo "  CONF    xclock"
	$(Q)cd $(XCLOCK_WORK_DIR) && \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
	    CC='$(CC)' \
	    CFLAGS='$(CFLAGS) $(CFLAGS_EXTRA)' \
	    LDFLAGS='$(XCLOCK_LDFLAGS)' \
	    ./configure --prefix=$(abspath $(XCLOCK_BUILD_DIR))/install \
	        $(XCLOCK_CONFIGURE_FLAGS) \
	        >> $(XCLOCK_LOG) 2>&1 || { \
	            echo "  FAIL    see $(XCLOCK_LOG)" >&2; \
	            tail -40 $(XCLOCK_LOG) >&2; \
	            exit 1; \
	        }
	@echo "  MAKE    xclock"
	$(Q)$(MAKE) -C $(XCLOCK_WORK_DIR) >> $(XCLOCK_LOG) 2>&1 || { \
	    echo "  FAIL    see $(XCLOCK_LOG)" >&2; \
	    tail -40 $(XCLOCK_LOG) >&2; \
	    exit 1; \
	}

xclock-clean:
	@echo "  CLEAN   xclock"
	$(Q)rm -rf $(XCLOCK_BUILD_DIR)

# Pin xclock at a known geometry so the dial-region assertion crops the
# right pixels regardless of host WM. 160x160 matches xclock's default
# square preferred geometry.
XCLOCK_SMOKE_GEOMETRY ?= 160x160+0+0
XCLOCK_SMOKE_REGION   ?= 0,0,160,160

.PHONY: check-smoke-xclock
## Run replay-based xclock smoke checks against libx11-compat
## (startup dial + per-tick second-hand redraw). Opt-in for now: the
## 20-consecutive-run gate per TODO.md "Athena widget stack" Tier 2 is
## not yet cleared, so this target is NOT promoted into the main
## check-smoke aggregate.
check-smoke-xclock: $(UI_SMOKE_OUT_ROOT)/xclock-startup/.stamp \
                    $(UI_SMOKE_OUT_ROOT)/xclock-tick/.stamp

$(UI_SMOKE_OUT_ROOT)/xclock-startup/.stamp: FORCE $(XCLOCK_BIN)
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xclock-startup \
	    --app $(abspath $(XCLOCK_BIN)) \
	    --app-arg=-geometry --app-arg=$(XCLOCK_SMOKE_GEOMETRY) \
	    --workdir $(abspath $(XCLOCK_WORK_DIR)) \
	    --replay tests/ui/replays/xclock-startup.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xclock-startup \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XCLOCK_SMOKE_REGION) \
	    --in-process-snapshots \
	    $(UI_REPLAY_XVFB) \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env LIBX11_COMPAT_FONT_DIR=$(abspath $(OUT))/../fonts
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/xclock-tick/.stamp: FORCE $(XCLOCK_BIN)
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xclock-tick \
	    --app $(abspath $(XCLOCK_BIN)) \
	    --app-arg=-geometry --app-arg=$(XCLOCK_SMOKE_GEOMETRY) \
	    --workdir $(abspath $(XCLOCK_WORK_DIR)) \
	    --replay tests/ui/replays/xclock-tick.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xclock-tick \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XCLOCK_SMOKE_REGION) \
	    --in-process-snapshots \
	    $(UI_REPLAY_XVFB) \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env LIBX11_COMPAT_FONT_DIR=$(abspath $(OUT))/../fonts
	$(Q)touch $@
