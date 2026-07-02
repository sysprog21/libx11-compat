# Build xfig-3.2.9a against the libx11-compat shim stack. xfig's
# configure.ac hardcodes XLIBS="-lXft -lXt -lX11" and stacks on
# -lXmu / -lXaw as needed; it has no PKG_CHECK_MODULES escape for the
# X libraries. Two pieces of glue make this build:
#
#   1. A symlink farm under build/lib-aliases/ that maps each compat
#      soname back to its canonical xorg name (libXft-compat.so -> libXft.so
#      etc). LDFLAGS -L points the linker at that directory before
#      anything else, so -lXft resolves to our shim.
#   2. PKG_CONFIG_PATH points at build/pkgconfig/, so the small set of
#      auxiliary PKG_CHECK_MODULES calls (fontconfig) still succeed.
#
# Many runtime dependencies (libpng/libjpeg/libtiff/ghostscript) are
# optional; we skip them with --without-* / --disable-*. The first cut
# is gated by:
#   --without-xaw3d        no libXaw3d on this host; fall back to xaw7
#   --without-xi           XInput1 is greenfield in libx11-compat
#   --without-xfig-libraries skip the optional xfig-libraries pkg-config
#   --disable-tablet       depends on xi
# Per TODO.md "Athena widget stack" Tier 3, any additional blockers
# (missing Xmu helpers, XGrabServer canvas tearing, XSendEvent routing)
# surface here and get filed as top-level TODO entries; this fragment
# does not paper over them.

XFIG_VERSION := 3.2.9a
XFIG_URL := https://prdownloads.sourceforge.net/mcj/xfig-$(XFIG_VERSION).tar.xz
XFIG_SHA256 := bc572a1881e5e20987ac590158b041ab7803845a9691036d3ba5e982f66d9ca3
XFIG_CACHE_DIR := $(OUT)/upstream/.cache
XFIG_TARBALL := $(XFIG_CACHE_DIR)/xfig-$(XFIG_VERSION).tar.xz
XFIG_DIR := $(OUT)/upstream/xfig-$(XFIG_VERSION)
XFIG_SOURCE_STAMP := $(XFIG_DIR)/.source-stamp
XFIG_BUILD_DIR := $(OUT)/xfig
XFIG_WORK_DIR := $(XFIG_BUILD_DIR)/source
XFIG_BIN := $(XFIG_WORK_DIR)/src/xfig
XFIG_LOG := $(abspath $(XFIG_BUILD_DIR))/build.log
XFIG_PATCHES := $(sort $(wildcard compat/xfig-patches/*.patch))

# Path where xfig's configure expects to find -lXft / -lX11 etc by their
# canonical xorg names. We stage symlinks here that point back at our
# *-compat.so artifacts. LDFLAGS -L this directory first so the linker
# resolves to the shim before any host /usr/lib copies.
XFIG_LIB_ALIASES := $(XFIG_BUILD_DIR)/lib-aliases
XFIG_LIB_ALIASES_STAMP := $(XFIG_LIB_ALIASES)/.stamp

XFIG_LDFLAGS := \
    -L$(abspath $(XFIG_LIB_ALIASES)) \
    -L$(abspath $(OUT))
# Bake both build/ and the lib-aliases farm into the rpath. xfig's
# DT_NEEDED list embeds the SONAMEs the linker saw (libXmu.so,
# libXt.so, libXaw7.so, libXft.so, libX11.so) - those soname
# resolutions only succeed at runtime when the symlinks under
# lib-aliases/ are on the search path, not just the *-compat.so
# files in build/. Without the second -rpath, ld.so prints
# "cannot open shared object file" and xfig exits with 127 in CI.
ifeq ($(UNAME_S),Linux)
  XFIG_LDFLAGS += -Wl,-rpath,$(abspath $(OUT)) \
                  -Wl,-rpath,$(abspath $(XFIG_LIB_ALIASES)) \
                  -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  XFIG_LDFLAGS += -Wl,-rpath,$(abspath $(OUT)) \
                  -Wl,-rpath,$(abspath $(XFIG_LIB_ALIASES))
endif

XFIG_CPPFLAGS := \
    -I$(abspath include) \
    -I$(abspath $(OUT)/upstream/include) \
    -I$(abspath include/libxt-build)
# resources.h uses caddr_t, which on glibc is gated behind _DEFAULT_SOURCE
# (or the legacy _BSD_SOURCE). Apple's libc exposes it unconditionally
# via <sys/types.h>, so local builds pass without the macro. Set it
# here so Ubuntu's clang stops failing d_arc / d_ellipse with "unknown
# type name 'caddr_t'".
XFIG_CPPFLAGS += -D_DEFAULT_SOURCE

XFIG_CONFIGURE_FLAGS := \
    --without-xaw3d \
    --without-xi \
    --without-xfig-libraries \
    --disable-tablet

# XFIG_CACHE_DIR shares the build/upstream/.cache directory with
# mk/xclock.mk; the directory rule lives in mk/xclock.mk so no duplicate
# target warning is emitted.
$(XFIG_TARBALL): | $(XFIG_CACHE_DIR)
	@echo "  FETCH   $(XFIG_URL)"
	$(Q)curl -fsSL -o $@.tmp $(XFIG_URL)
	$(Q)echo "$(XFIG_SHA256)  $@.tmp" | shasum -a 256 -c -
	$(Q)mv $@.tmp $@

$(XFIG_SOURCE_STAMP): $(XFIG_TARBALL) mk/xfig.mk
	@echo "  EXTRACT xfig-$(XFIG_VERSION)"
	$(Q)rm -rf $(XFIG_DIR)
	$(Q)mkdir -p $(dir $(XFIG_DIR))
	$(Q)tar -xJf $(XFIG_TARBALL) -C $(dir $(XFIG_DIR))
	$(Q)touch $@

# Build the symlink farm: -lXft -> libXft-compat.so, etc. Keeping this
# stamp dependent on $(TARGET) etc means the symlinks always point at
# fresh shim builds.
$(XFIG_LIB_ALIASES_STAMP): $(LIBXAW_TARGET) $(LIBXT_TARGET) \
    $(XFT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(LIBXPM_TARGET) \
    $(XEXT_COMPAT_TARGET) $(TARGET) mk/xfig.mk
	@mkdir -p $(XFIG_LIB_ALIASES)
	$(Q)rm -f $(XFIG_LIB_ALIASES)/libX*.so $(XFIG_LIB_ALIASES)/libX*.dylib
	$(Q)set -e; for pair in \
	    libX11.so:libX11-compat.so \
	    libXt.so:libXt-compat.so \
	    libXmu.so:libXmu-compat.so \
	    libXaw.so:libXaw-compat.so \
	    libXaw7.so:libXaw-compat.so \
	    libXpm.so:libXpm-compat.so \
	    libXft.so:libXft-compat.so \
	    libXext.so:libXext-compat.so; do \
	    alias="$${pair%%:*}"; \
	    target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" \
	        "$(XFIG_LIB_ALIASES)/$$alias"; \
	done
ifeq ($(UNAME_S),Darwin)
	$(Q)set -e; for pair in \
	    libX11.dylib:libX11-compat.so \
	    libXt.dylib:libXt-compat.so \
	    libXmu.dylib:libXmu-compat.so \
	    libXaw.dylib:libXaw-compat.so \
	    libXaw7.dylib:libXaw-compat.so \
	    libXpm.dylib:libXpm-compat.so \
	    libXft.dylib:libXft-compat.so \
	    libXext.dylib:libXext-compat.so; do \
	    alias="$${pair%%:*}"; \
	    target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" \
	        "$(XFIG_LIB_ALIASES)/$$alias"; \
	done
endif
	$(Q)touch $@

.PHONY: xfig check-differential-xfig xfig-clean
## Build xfig against the libx11-compat shim stack
xfig: $(XFIG_BIN)

$(XFIG_BIN): $(XFIG_SOURCE_STAMP) $(XFIG_PATCHES) $(PKGCONFIG_FILES) \
    $(XFIG_LIB_ALIASES_STAMP)
	@mkdir -p $(XFIG_BUILD_DIR)
	$(Q)rm -rf $(XFIG_WORK_DIR)
	$(Q)mkdir -p $(XFIG_WORK_DIR)
	$(Q)tar -cf - -C $(XFIG_DIR) . | tar -xf - -C $(XFIG_WORK_DIR)
	$(Q)set -e; for patch in $(XFIG_PATCHES); do \
	    patch -d $(XFIG_WORK_DIR) -p1 < "$$patch"; \
	done
	@echo "  CONF    xfig"
	$(Q)cd $(XFIG_WORK_DIR) && \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
	    CC='$(CC)' \
	    CFLAGS='$(CFLAGS) $(CFLAGS_EXTRA)' \
	    CPPFLAGS='$(XFIG_CPPFLAGS)' \
	    LDFLAGS='$(XFIG_LDFLAGS)' \
	    XCPPFLAGS='$(XFIG_CPPFLAGS)' \
	    XLDFLAGS='-L$(abspath $(XFIG_LIB_ALIASES))' \
	    ./configure --prefix=$(abspath $(XFIG_BUILD_DIR))/install \
	        $(XFIG_CONFIGURE_FLAGS) \
	        >> $(XFIG_LOG) 2>&1 || { \
	            echo "  FAIL    see $(XFIG_LOG)" >&2; \
	            tail -60 $(XFIG_LOG) >&2; \
	            exit 1; \
	        }
	@echo "  MAKE    xfig"
	$(Q)$(MAKE) -C $(XFIG_WORK_DIR) >> $(XFIG_LOG) 2>&1 || { \
	    echo "  FAIL    see $(XFIG_LOG)" >&2; \
	    tail -60 $(XFIG_LOG) >&2; \
	    exit 1; \
	}

xfig-clean:
	@echo "  CLEAN   xfig"
	$(Q)rm -rf $(XFIG_BUILD_DIR)

XFIG_DIFF_REMOTE ?= node11
XFIG_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-xfig-differential
XFIG_DIFF_DISPLAY ?= 125
XFIG_DIFF_JOBS ?= 1
XFIG_DIFF_INSTALL_DEPS ?= 0
XFIG_DIFF_LOCAL ?= 0
XFIG_DIFF_MAE_THRESHOLD ?= 0.16
XFIG_DIFF_CHANGED_THRESHOLD ?= 0.42
XFIG_DIFF_GEOMETRY ?= 1280x1024x24
XFIG_DIFF_TOP ?= 12
XFIG_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(XFIG_DIFF_LOCAL)),local,remote)
XFIG_DIFF_OUT_ROOT ?= $(OUT)/xfig-differential
XFIG_DIFF_SCREENSHOT_REGION ?= 0,0,1024,768

xfig_diff_env = \
    XFIG_DIFF_REMOTE='$(XFIG_DIFF_REMOTE)' \
    XFIG_DIFF_REMOTE_ROOT='$(XFIG_DIFF_REMOTE_ROOT)' \
    XFIG_DIFF_DISPLAY='$(XFIG_DIFF_DISPLAY)' \
    XFIG_DIFF_JOBS='$(XFIG_DIFF_JOBS)' \
    XFIG_DIFF_MAE_THRESHOLD='$(XFIG_DIFF_MAE_THRESHOLD)' \
    XFIG_DIFF_CHANGED_THRESHOLD='$(XFIG_DIFF_CHANGED_THRESHOLD)' \
    XFIG_DIFF_GEOMETRY='$(XFIG_DIFF_GEOMETRY)' \
    XFIG_DIFF_TOP='$(XFIG_DIFF_TOP)' \
    XFIG_DIFF_COMPARE_LOCATION='$(XFIG_DIFF_COMPARE_LOCATION)' \
    XFIG_DIFF_OUT_ROOT='$(abspath $(XFIG_DIFF_OUT_ROOT))' \
    XFIG_DIFF_SCREENSHOT_REGION='$(XFIG_DIFF_SCREENSHOT_REGION)'

## Compare Xfig screenshots for system libX11 vs libx11-compat. Set
## XFIG_DIFF_LOCAL=1 to run the build / capture / compare pipeline on
## the current host (used by the GitHub Actions differential workflow);
## otherwise the script SSHes to XFIG_DIFF_REMOTE.
check-differential-xfig:
	$(Q)$(xfig_diff_env) $(PYTHON) scripts/run-xfig-differential-tests.py \
	    $(if $(filter 1 yes true,$(XFIG_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(XFIG_DIFF_LOCAL)),--local)

XFIG_SMOKE_GEOMETRY ?= 1024x768+0+0
XFIG_SMOKE_REGION   ?= 0,0,1024,768

.PHONY: check-smoke-xfig
## Run replay-based xfig smoke checks against libx11-compat
## (startup paint + line-draw rubber-band). Opt-in for now: TODO.md
## "Athena widget stack" Tier 3 requires 20 consecutive runs across
## local + node11 before promotion into the main check-smoke flow.
check-smoke-xfig: $(UI_SMOKE_OUT_ROOT)/xfig-startup/.stamp \
                  $(UI_SMOKE_OUT_ROOT)/xfig-draw-line/.stamp

$(UI_SMOKE_OUT_ROOT)/xfig-startup/.stamp: FORCE $(XFIG_BIN)
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xfig-startup \
	    --app $(abspath $(XFIG_BIN)) \
	    --app-arg=-geometry --app-arg=$(XFIG_SMOKE_GEOMETRY) \
	    --workdir $(abspath $(XFIG_WORK_DIR)) \
	    --replay tests/ui/replays/xfig-startup.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xfig-startup \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XFIG_SMOKE_REGION) \
	    --in-process-snapshots \
	    $(UI_REPLAY_XVFB) \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/xfig-draw-line/.stamp: FORCE $(XFIG_BIN)
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xfig-draw-line \
	    --app $(abspath $(XFIG_BIN)) \
	    --app-arg=-geometry --app-arg=$(XFIG_SMOKE_GEOMETRY) \
	    --workdir $(abspath $(XFIG_WORK_DIR)) \
	    --replay tests/ui/replays/xfig-draw-line.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xfig-draw-line \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(XFIG_SMOKE_REGION) \
	    --in-process-snapshots \
	    $(UI_REPLAY_XVFB) \
	    --env DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}
	$(Q)touch $@
