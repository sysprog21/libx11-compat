# Build Grace 5.1.25 (xmgrace) against the Motif + libx11-compat stack.
#
# Grace ships its own autoconf-flavor configure that writes conf/Make.conf.
# Its GUI link line is
#     GUI_LIBS = @X_LIBS@ -lXt -lXext -lX11 [-lXmu] [-lXpm] $MOTIF_LIB $XBAE_LIB
# so it needs the same canonical-name resolution trick as mk/xfig.mk: a
# lib-aliases farm mapping libXt.so -> libXt-compat.so etc, plus libXm.so
# pointed at the in-tree Motif build (mk/motif.mk). T1lib, Xbae, and cephes
# are bundled in the tarball, so no external font/widget/math deps are pulled.
#
# Two symlink farms make this build:
#   1. GRACE_SYSROOT: header farm with X11/, X11/extensions/, and Xm/ subdirs
#      (same shape as mk/xephem.mk) so Grace's <Xm/Xm.h> / <X11/*.h> resolve to
#      the in-tree Motif + compat headers via --with-extra-incpath.
#   2. GRACE_LIB_ALIASES: soname farm (libXm/libXt/libXext/libX11/libXpm/libXmu)
#      wired in first via --with-extra-ldpath so -lXm and friends resolve to the
#      shim before any host /usr/lib copies (critical on node11).
#
# Optional drivers and helpers that would pull new deps are disabled up front:
# netCDF, JPEG, PNG, PDF, FFTW, the Fortran wrapper, editres (-lXmu path),
# and XmHTML online help. T1lib uses the bundled copy so no host -lt1 is needed.

GRACE_VERSION := 5.1.25
# Fetch the Grace source from the Debian mirror rather than the upstream
# plasma-gate host, which is slow and times out. Debian's grace_5.1.25.orig.tar.gz
# is byte-identical to the upstream grace-latest.tar.gz (same 2957689 bytes, same
# SHA256, same grace-5.1.25/ top dir) and lives on a fast, versioned, stable path.
GRACE_URL := https://deb.debian.org/debian/pool/main/g/grace/grace_5.1.25.orig.tar.gz
GRACE_SHA256 := 751ab9917ed0f6232073c193aba74046037e185d73b77bab0f5af3e3ff1da2ac
GRACE_CACHE_DIR := $(OUT)/upstream/.cache
GRACE_TARBALL := $(GRACE_CACHE_DIR)/grace-$(GRACE_VERSION).tar.gz
GRACE_DIR := $(OUT)/upstream/grace-$(GRACE_VERSION)
GRACE_SOURCE_STAMP := $(GRACE_DIR)/.source-stamp
GRACE_BUILD_DIR := $(OUT)/grace
GRACE_WORK_DIR := $(GRACE_BUILD_DIR)/source
GRACE_BIN := $(GRACE_WORK_DIR)/src/xmgrace
GRACE_LOG := $(abspath $(GRACE_BUILD_DIR))/build.log
GRACE_PATCHES := $(sort $(wildcard compat/grace-patches/*.patch))

# Repo-local runtime home baked into the binary as the compiled default.
GRACE_HOME_DIR := $(abspath $(GRACE_BUILD_DIR))/runtime
GRACE_RUNTIME_STAMP := $(GRACE_HOME_DIR)/.stamp
# Runtime data Grace reads from GRACE_HOME at startup: fonts (FontDataBase plus
# the Type1 set and encodings), the new-plot templates, and the sample plots.
# doc and auxiliary are help-viewer and extra-tool payloads, not needed to bring
# the canvas up, so they are left out to keep the staged tree minimal.
GRACE_RUNTIME_SUBDIRS := fonts templates examples

GRACE_SYSROOT := $(GRACE_BUILD_DIR)/sysroot
GRACE_SYSROOT_STAMP := $(GRACE_SYSROOT)/.stamp
GRACE_LIB_ALIASES := $(GRACE_BUILD_DIR)/lib-aliases
GRACE_LIB_ALIASES_STAMP := $(GRACE_LIB_ALIASES)/.stamp

# Relative paths as prerequisites match the in-tree build rules so a clean
# make grace builds the libraries instead of failing on an absolute path.
GRACE_XLIB_DEPS := \
    $(MOTIF_LIBXM) \
    $(LIBXT_TARGET) \
    $(XEXT_COMPAT_TARGET) \
    $(XMU_COMPAT_TARGET) \
    $(LIBXPM_TARGET) \
    $(ICE_COMPAT_TARGET) \
    $(SM_COMPAT_TARGET) \
    $(TARGET)

GRACE_INCPATH := \
    $(abspath $(GRACE_SYSROOT)):$(abspath include):$(abspath $(OUT)/upstream/include):$(abspath include/libxt-build)
GRACE_LDPATH := $(abspath $(GRACE_LIB_ALIASES)):$(abspath $(OUT))

GRACE_RPATH_FLAGS :=
ifeq ($(UNAME_S),Linux)
  GRACE_RPATH_FLAGS += -Wl,-rpath,$(abspath $(OUT)) \
                       -Wl,-rpath,$(abspath $(GRACE_LIB_ALIASES)) \
                       -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  GRACE_RPATH_FLAGS += -Wl,-rpath,$(abspath $(OUT)) \
                       -Wl,-rpath,$(abspath $(GRACE_LIB_ALIASES))
endif

# Grace 5.1.25 predates C99; its configure conftests (and much of its own
# source) rely on implicit function declarations and implicit int, which clang
# 16+ rejects as hard errors. Without this the Motif detection conftest fails to
# compile and configure aborts with "M*tif has not been found". These flags feed
# configure via CFLAGS and propagate to the build through Make.conf's CFLAGS0.
# -D_DEFAULT_SOURCE is the glibc side: the project builds with -std=c99, and
# under strict c99 glibc hides signgam (used by cephes/cmath.h) and caddr_t,
# which breaks the build on Linux even though Apple's libc exposes them; the
# explicit macro re-enables the __USE_MISC declarations regardless of -std.
GRACE_LEGACY_CFLAGS := -Wno-implicit-function-declaration -Wno-implicit-int \
    -D_DEFAULT_SOURCE

GRACE_CONFIGURE_FLAGS := \
    --enable-grace-home=$(GRACE_HOME_DIR) \
    --with-extra-incpath=$(GRACE_INCPATH) \
    --with-extra-ldpath=$(GRACE_LDPATH) \
    --with-motif-library=-lXm \
    --with-bundled-t1lib \
    --enable-netcdf=no \
    --enable-jpegdrv=no \
    --enable-pngdrv=no \
    --enable-pdfdrv=no \
    --enable-f77-wrapper=no \
    --enable-editres=no \
    --enable-xmhtml=no \
    --with-fftw=no

# GRACE_CACHE_DIR shares build/upstream/.cache with mk/xclock.mk; the directory
# rule lives there, so no duplicate-target warning is emitted here.
$(GRACE_TARBALL): | $(GRACE_CACHE_DIR)
	@echo "  FETCH   $(GRACE_URL)"
	$(Q)curl -fsSL --max-time 600 -o $@.tmp $(GRACE_URL)
	$(Q)echo "$(GRACE_SHA256)  $@.tmp" | shasum -a 256 -c -
	$(Q)mv $@.tmp $@

$(GRACE_SOURCE_STAMP): $(GRACE_TARBALL) mk/grace.mk
	@echo "  EXTRACT grace-$(GRACE_VERSION)"
	$(Q)rm -rf $(GRACE_DIR)
	$(Q)mkdir -p $(dir $(GRACE_DIR))
	$(Q)tar -xzf $(GRACE_TARBALL) -C $(dir $(GRACE_DIR))
	$(Q)touch $@

$(GRACE_SYSROOT_STAMP): $(LIBXT_TARGET) $(MOTIF_STAGE_STAMP) mk/grace.mk
	@echo "  SYSROOT grace"
	$(Q)rm -rf $(GRACE_SYSROOT)
	$(Q)mkdir -p $(GRACE_SYSROOT)/X11/extensions $(GRACE_SYSROOT)/Xm
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ "$$b" = extensions ] && continue; \
	    ln -sf "$$e" "$(GRACE_SYSROOT)/X11/$$b"; \
	done
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/extensions/* \
	             $(abspath include)/X11/extensions/*; do \
	    ln -sf "$$e" "$(GRACE_SYSROOT)/X11/extensions/$$(basename "$$e")"; \
	done
	$(Q)for e in $(abspath include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ -e "$(GRACE_SYSROOT)/X11/$$b" ] || \
	        ln -sf "$$e" "$(GRACE_SYSROOT)/X11/$$b"; \
	done
	$(Q)for h in $(abspath $(MOTIF_SRC_DIR))/lib/Xm/*.h \
	             $(abspath $(MOTIF_BUILD_DIR))/lib/Xm/*.h; do \
	    ln -sf "$$h" "$(GRACE_SYSROOT)/Xm/$$(basename "$$h")"; \
	done
	$(Q)touch $@

$(GRACE_LIB_ALIASES_STAMP): $(GRACE_XLIB_DEPS) mk/grace.mk
	@mkdir -p $(GRACE_LIB_ALIASES)
	$(Q)rm -f $(GRACE_LIB_ALIASES)/lib*.so $(GRACE_LIB_ALIASES)/lib*.dylib
	$(Q)set -e; for pair in \
	    libX11.so:libX11-compat.so \
	    libXt.so:libXt-compat.so \
	    libXmu.so:libXmu-compat.so \
	    libXpm.so:libXpm-compat.so \
	    libXext.so:libXext-compat.so \
	    libICE.so:libICE-compat.so \
	    libSM.so:libSM-compat.so \
	    libXm.so:libXm.so; do \
	    alias="$${pair%%:*}"; \
	    target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(GRACE_LIB_ALIASES)/$$alias"; \
	done
ifeq ($(UNAME_S),Darwin)
	$(Q)set -e; for pair in \
	    libX11.dylib:libX11-compat.so \
	    libXt.dylib:libXt-compat.so \
	    libXmu.dylib:libXmu-compat.so \
	    libXpm.dylib:libXpm-compat.so \
	    libXext.dylib:libXext-compat.so \
	    libICE.dylib:libICE-compat.so \
	    libSM.dylib:libSM-compat.so \
	    libXm.dylib:libXm.so; do \
	    alias="$${pair%%:*}"; \
	    target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(GRACE_LIB_ALIASES)/$$alias"; \
	done
endif
	$(Q)touch $@

.PHONY: grace xmgrace grace-clean
## Build Grace (xmgrace) and stage its repo-local runtime tree
grace: $(GRACE_BIN) $(GRACE_RUNTIME_STAMP)
xmgrace: $(GRACE_BIN) $(GRACE_RUNTIME_STAMP)

$(GRACE_BIN): $(GRACE_SOURCE_STAMP) $(GRACE_PATCHES) $(GRACE_SYSROOT_STAMP) \
    $(GRACE_LIB_ALIASES_STAMP)
	@mkdir -p $(GRACE_BUILD_DIR)
	$(Q)rm -f $(GRACE_LOG)
	$(Q)rm -rf $(GRACE_WORK_DIR)
	$(Q)mkdir -p $(GRACE_WORK_DIR)
	$(Q)tar -cf - -C $(GRACE_DIR) . | tar -xf - -C $(GRACE_WORK_DIR)
	$(Q)set -e; for patch in $(GRACE_PATCHES); do \
	    patch -d $(GRACE_WORK_DIR) -p1 < "$$patch"; \
	done
	@echo "  CONF    grace"
	$(Q)cd $(GRACE_WORK_DIR) && \
	    $(motif_runtime_env) \
	    CC='$(CC)' \
	    CFLAGS='$(CFLAGS) $(CFLAGS_EXTRA) $(GRACE_LEGACY_CFLAGS)' \
	    LDFLAGS='$(GRACE_RPATH_FLAGS) -L$(abspath $(OUT))' \
	    ./configure --prefix=$(abspath $(GRACE_BUILD_DIR))/install \
	        $(GRACE_CONFIGURE_FLAGS) \
	        >> $(GRACE_LOG) 2>&1 || { \
	            echo "  FAIL    see $(GRACE_LOG)" >&2; \
	            tail -60 $(GRACE_LOG) >&2; exit 1; \
	        }
	@echo "  MAKE    grace"
	$(Q)$(motif_runtime_env) env -u MAKEFLAGS -u MFLAGS \
	    $(MAKE) -C $(GRACE_WORK_DIR) >> $(GRACE_LOG) 2>&1 || { \
	    echo "  FAIL    see $(GRACE_LOG)" >&2; \
	    tail -60 $(GRACE_LOG) >&2; exit 1; \
	}

# Stage the runtime tree into GRACE_HOME_DIR, the compiled-in default home.
# Each data subdir ships an install target that copies into $(DESTDIR)$(GRACE_HOME);
# with DESTDIR empty and GRACE_HOME pointed at the repo-local dir, nothing touches
# the system. The tree is wiped first so re-staging is clean rather than tripping
# the upstream "file exists, install only the .sample" branch.
$(GRACE_RUNTIME_STAMP): $(GRACE_BIN) mk/grace.mk
	@echo "  STAGE   grace runtime"
	$(Q)rm -rf $(GRACE_HOME_DIR)
	$(Q)mkdir -p $(GRACE_HOME_DIR)
	$(Q)set -e; for d in $(GRACE_RUNTIME_SUBDIRS); do \
	    env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(GRACE_WORK_DIR)/$$d install \
	        GRACE_HOME='$(GRACE_HOME_DIR)' DESTDIR= >> $(GRACE_LOG) 2>&1 || { \
	        echo "  FAIL    see $(GRACE_LOG)" >&2; tail -40 $(GRACE_LOG) >&2; exit 1; \
	    }; \
	done
	$(Q)cp $(GRACE_WORK_DIR)/gracerc $(GRACE_WORK_DIR)/gracerc.user $(GRACE_HOME_DIR)/
	$(Q)touch $@

# Replay-based smoke. Launches xmgrace headless with the repo-local
# GRACE_HOME, a disposable HOME, and the checked-in fixture, then drives the
# startup and interaction replays through the in-process offscreen engine. The
# startup replay fails if the main window is blank or off-screen; the
# interaction replay fails if the plot canvas does not stay visible, the File
# menu does not post and render (grace-file-menu.json changed-region), or the
# process dies during menu and canvas input.
GRACE_FIXTURE := tests/ui/fixtures/grace/simple.agr
GRACE_SMOKE_REGION ?= 0,0,680,700
grace_ui_lib_path = $(abspath $(OUT))$(if $(SDL_RUNTIME_LIBDIR),:$(SDL_RUNTIME_LIBDIR))
grace_ui_env = \
    --env DYLD_LIBRARY_PATH=$(grace_ui_lib_path)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    --env LD_LIBRARY_PATH=$(grace_ui_lib_path)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
    --env GRACE_HOME=$(GRACE_HOME_DIR)
# Shell env (not the runner --env form) putting the compat libs and, when set,
# the SDL runtime dir on the loader path. For targets that exec a Grace binary
# directly rather than through run-ui-replay.py, so a host whose SDL sits outside
# the default loader paths still resolves it. motif_runtime_env only covers OUT.
grace_shell_lib_env = \
    DYLD_LIBRARY_PATH=$(grace_ui_lib_path)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    LD_LIBRARY_PATH=$(grace_ui_lib_path)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}

# Absolute smoke out-root, shared by the replay bodies and the export target so
# the path is spelled once.
grace_out_root = $(abspath $(UI_SMOKE_OUT_ROOT))

# One recipe body shared by every replay: $(1) is the replay name (also the
# .replay basename and out-root subdir). Each replay gets its own disposable
# HOME (grace-home-$(1)) so concurrent runs under make -j never race on the
# .grace preferences xmgrace writes there.
define grace_smoke_run
	$(Q)rm -rf $(grace_out_root)/$(1) \
	    $(grace_out_root)/grace-home-$(1)
	$(Q)mkdir -p $(grace_out_root)/grace-home-$(1)
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name $(1) \
	    --app $(abspath $(GRACE_BIN)) \
	    --app-arg=$(abspath $(GRACE_FIXTURE)) \
	    --workdir $(abspath $(GRACE_WORK_DIR))/src \
	    --replay tests/ui/replays/$(1).replay \
	    --out-root $(grace_out_root)/$(1) \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --screenshot-region $(GRACE_SMOKE_REGION) \
	    --in-process-snapshots \
	    $(if $(UI_REPLAY_XVFB),$(UI_REPLAY_XVFB),--offscreen) \
	    $(grace_ui_env) \
	    --env HOME=$(grace_out_root)/grace-home-$(1)
	$(Q)touch $@
endef

.PHONY: check-smoke-grace
## Run replay-based Grace startup, interaction, and resize smoke checks
check-smoke-grace: $(UI_SMOKE_OUT_ROOT)/grace-startup/.stamp \
                   $(UI_SMOKE_OUT_ROOT)/grace-interact/.stamp \
                   $(UI_SMOKE_OUT_ROOT)/grace-resize/.stamp

$(UI_SMOKE_OUT_ROOT)/grace-startup/.stamp: FORCE $(GRACE_BIN) $(GRACE_RUNTIME_STAMP)
	$(call grace_smoke_run,grace-startup)

$(UI_SMOKE_OUT_ROOT)/grace-interact/.stamp: FORCE $(GRACE_BIN) $(GRACE_RUNTIME_STAMP)
	$(call grace_smoke_run,grace-interact)

$(UI_SMOKE_OUT_ROOT)/grace-resize/.stamp: FORCE $(GRACE_BIN) $(GRACE_RUNTIME_STAMP)
	$(call grace_smoke_run,grace-resize)

# Export smoke: render the fixture to PostScript with -hardcopy (no new repo
# deps - PostScript is Grace's built-in default device) and assert a non-empty
# file with a valid PostScript header. This exercises the custom canvas draw path
# plus the font engine end to end without the GUI. -hardcopy still initializes Xt
# (XOpenDisplay), so a video backend is needed. GRACE_HARDCOPY_ENV defaults to
# SDL_VIDEODRIVER=dummy, which is enough on macOS where SDL's dummy driver brings
# up a display with no X server. On a headless Linux runner Grace's startup does
# not come up under the dummy driver, so CI runs this target under Xvfb with
# GRACE_HARDCOPY_ENV cleared, letting SDL bind its x11 backend to the Xvfb server.
GRACE_HARDCOPY_ENV ?= SDL_VIDEODRIVER=dummy
GRACE_EXPORT_OUT := $(grace_out_root)/grace-export
.PHONY: check-grace-export
## Render the Grace fixture to PostScript and check the output
check-grace-export: $(GRACE_BIN) $(GRACE_RUNTIME_STAMP)
	$(Q)rm -rf $(GRACE_EXPORT_OUT); mkdir -p $(GRACE_EXPORT_OUT)/home
	$(Q)$(grace_shell_lib_env) $(GRACE_HARDCOPY_ENV) GRACE_HOME=$(GRACE_HOME_DIR) \
	    HOME=$(GRACE_EXPORT_OUT)/home \
	    $(abspath $(GRACE_BIN)) -hardcopy \
	        -printfile $(GRACE_EXPORT_OUT)/plot.ps \
	        $(abspath $(GRACE_FIXTURE))
	$(Q)test -s $(GRACE_EXPORT_OUT)/plot.ps || { \
	    echo "  FAIL    grace export produced no PostScript" >&2; exit 1; }
	$(Q)head -1 $(GRACE_EXPORT_OUT)/plot.ps | grep -q '^%!PS' || { \
	    echo "  FAIL    grace export is not a PostScript file" >&2; exit 1; }
	@echo "  OK      grace export: $(GRACE_EXPORT_OUT)/plot.ps"

# Differential: build Grace on the node11 host against native OpenMotif
# 2.3.8 (the baseline) and against libx11-compat, capture the startup screenshot
# on two Xvfb servers, and diff them with scripts/compare-motif-reference.py.
# Set GRACE_DIFF_LOCAL=1 to run the whole pipeline on the current host instead of
# SSHing to the remote.
#
# Calibrated on node11 (OpenMotif 2.3.8 baseline): the startup capture measures
# MAE 0.038 and changed-pixel 0.123 against the libx11-compat stack. Grace mixes
# Motif chrome with a custom-drawn plot canvas rendered through two different X
# stacks (native X11 vs SDL), so the residual is mostly scrollbar and antialiased
# text or axis differences. The thresholds keep headroom over the measured values
# for runner-to-runner hinting and timing variance; the whole-frame pixel diff
# catches gross layout shifts, missing chrome, and a fully blank window. Because
# the canvas is mostly white background, a diff alone would dilute a plot-content
# regression, so grace-startup-differential.replay also asserts grace-canvas-
# content.json (region_non_background over the plot rect) on both sides, which is
# what actually guards the axes/curve/labels from silently going blank.
GRACE_DIFF_REMOTE ?= node11
GRACE_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-grace-differential
GRACE_DIFF_DISPLAY ?= 131
GRACE_DIFF_JOBS ?= 1
GRACE_DIFF_INSTALL_DEPS ?= 0
GRACE_DIFF_LOCAL ?= 0
GRACE_DIFF_MAE_THRESHOLD ?= 0.10
GRACE_DIFF_CHANGED_THRESHOLD ?= 0.25
GRACE_DIFF_GEOMETRY ?= 1280x1024x24
GRACE_DIFF_TOP ?= 12
GRACE_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(GRACE_DIFF_LOCAL)),local,remote)
GRACE_DIFF_OUT_ROOT ?= $(OUT)/grace-differential
GRACE_DIFF_SCREENSHOT_REGION ?= $(GRACE_SMOKE_REGION)

grace_diff_env = \
    GRACE_DIFF_REMOTE='$(GRACE_DIFF_REMOTE)' \
    GRACE_DIFF_REMOTE_ROOT='$(GRACE_DIFF_REMOTE_ROOT)' \
    GRACE_DIFF_DISPLAY='$(GRACE_DIFF_DISPLAY)' \
    GRACE_DIFF_JOBS='$(GRACE_DIFF_JOBS)' \
    GRACE_DIFF_MAE_THRESHOLD='$(GRACE_DIFF_MAE_THRESHOLD)' \
    GRACE_DIFF_CHANGED_THRESHOLD='$(GRACE_DIFF_CHANGED_THRESHOLD)' \
    GRACE_DIFF_GEOMETRY='$(GRACE_DIFF_GEOMETRY)' \
    GRACE_DIFF_TOP='$(GRACE_DIFF_TOP)' \
    GRACE_DIFF_COMPARE_LOCATION='$(GRACE_DIFF_COMPARE_LOCATION)' \
    GRACE_DIFF_OUT_ROOT='$(abspath $(GRACE_DIFF_OUT_ROOT))' \
    GRACE_DIFF_SCREENSHOT_REGION='$(GRACE_DIFF_SCREENSHOT_REGION)'

.PHONY: check-differential-grace
## Compare Grace startup screenshots for system OpenMotif vs libx11-compat.
check-differential-grace:
	$(Q)$(grace_diff_env) $(PYTHON) scripts/run-grace-differential-tests.py \
	    $(if $(filter 1 yes true,$(GRACE_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(GRACE_DIFF_LOCAL)),--local)

grace-clean:
	@echo "  CLEAN   grace"
	$(Q)rm -rf $(GRACE_BUILD_DIR)
