# Build the historical Motif GIMP 0.54.1 (1996) against the libx11-compat
# stack. 0.54.1 is the last Motif-based GIMP; later releases moved to GTK.
# The patch set under compat/gimp-patches/ is the GNOME-hosted rework
# gitlab.gnome.org/balooii/gimp-0.54 (LP64 / modern-toolchain fixes); it is
# reused verbatim here. We reuse the in-tree thentenaar/motif (libXm/libMrm,
# already linked against libXpm-compat) rather than building a second
# OpenMotif, and we do not need libXp: this Motif is built --without the print
# shell.
#
# Two pieces of glue make GIMP's 1996 autoconf build resolve against the
# compat stack:
#
#   1. A symlink farm under build/gimp-motif/lib-aliases/ mapping each compat
#      soname back to its canonical xorg/Motif name (libX11-compat.so ->
#      libX11.so etc). configure's AC_CHECK_LIB(Xm, ...) and the final link
#      resolve -lXm / -lXt / -lX11 / -lXext through it; LDFLAGS -L points there
#      first.
#   2. A merged include sysroot under build/gimp-motif/sysroot/. GIMP's
#      app/Makefile only carries configure's X_CFLAGS (one --x-includes dir),
#      so we stage a single tree unioning the generated X11/Xt headers
#      (build/upstream/include), the in-tree X11 stubs (include/X11: SM/, ICE/,
#      extensions/XShm.h), and the Motif headers (source + generated). On
#      Darwin the sysroot also carries malloc.h and values.h shims for two
#      glibc-isms GIMP includes; Linux (the differential runner) uses the real
#      system headers, so the shims are Darwin-only and no source patch is
#      needed beyond the balooii set.
#
# XShm: libx11-compat implements MIT-SHM (src/xshm.c) and the sysroot supplies
# X11/extensions/XShm.h, so HAVE_XSHM_H is defined and the SHM canvas path is
# compiled in. GIMP falls back to plain XPutImage at runtime if XShmAttach
# fails.

GIMP_VERSION := 0.54.1
GIMP_URL := https://download.gimp.org/gimp/historical/gimp-$(GIMP_VERSION).fixed.tar.gz
GIMP_SHA256 := 74fcd9671ce7bdbb274171fb7b4326e82541222e17c153d1ee40659544eafa7f
GIMP_CACHE_DIR := $(OUT)/upstream/.cache
GIMP_TARBALL := $(GIMP_CACHE_DIR)/gimp-$(GIMP_VERSION).fixed.tar.gz
GIMP_SRC_DIR := $(OUT)/upstream/gimp-$(GIMP_VERSION)
GIMP_SOURCE_STAMP := $(GIMP_SRC_DIR)/.source-stamp
GIMP_PATCHES := $(sort $(wildcard compat/gimp-patches/*.patch))

GIMP_BUILD_DIR := $(OUT)/gimp-motif
GIMP_WORK_DIR := $(GIMP_BUILD_DIR)/source
GIMP_BIN := $(GIMP_WORK_DIR)/app/gimp
GIMP_BUILD_STAMP := $(GIMP_BUILD_DIR)/.build-stamp
GIMP_LOG := $(abspath $(GIMP_BUILD_DIR))/build.log

# Merged include sysroot (single -I via configure's --x-includes).
GIMP_SYSROOT := $(GIMP_BUILD_DIR)/sysroot
GIMP_SYSROOT_STAMP := $(GIMP_SYSROOT)/.stamp

# Symlink farm: -lX11 -> libX11-compat.so, -lXm -> libXm.so, etc.
GIMP_LIB_ALIASES := $(GIMP_BUILD_DIR)/lib-aliases
GIMP_LIB_ALIASES_STAMP := $(GIMP_LIB_ALIASES)/.stamp

GIMP_UPSTREAM_INC := $(OUT)/upstream/include

GIMP_LDFLAGS := \
    -L$(abspath $(GIMP_LIB_ALIASES)) \
    -L$(abspath $(OUT))
ifeq ($(UNAME_S),Linux)
  GIMP_LDFLAGS += -Wl,-rpath,$(abspath $(OUT)) \
                  -Wl,-rpath,$(abspath $(GIMP_LIB_ALIASES)) \
                  -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  GIMP_LDFLAGS += -Wl,-rpath,$(abspath $(OUT)) \
                  -Wl,-rpath,$(abspath $(GIMP_LIB_ALIASES))
endif

# Image-format plug-ins (jpeg/png) link against system format libs. These
# resolve through pkg-config on both macOS (Homebrew) and Linux (system), and
# link only into the standalone plug-in executables, never into the compat
# libraries, so the SDL2/SDL2_ttf/pixman core-dep rule is preserved. The
# plug-ins Makefile hardcodes -ljpeg/-lpng/-lz/-lXpm/-lX11, so we only need
# the -I and -L paths; the -l flags stay as the upstream Makefile spells them.
# We rebuild plug-ins with these in an isolated second make so the INCLUDE /
# LINCLUDE override never reaches app/gimp's own link. TIFF is intentionally
# dropped (compat/gimp-patches/0012-plugins-drop-tiff.patch removes the tiff
# plug-in) so the build needs no libtiff; TIFF is an uncommon format here.
GIMP_PLUGIN_FMT_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpng libjpeg 2>/dev/null)
GIMP_PLUGIN_FMT_LDPATHS := $(shell $(PKG_CONFIG) --libs-only-L libpng libjpeg 2>/dev/null)
GIMP_PLUGIN_INCLUDE := -I$(abspath $(GIMP_SYSROOT)) $(GIMP_PLUGIN_FMT_CFLAGS)
GIMP_PLUGIN_LINCLUDE := -L$(abspath $(GIMP_LIB_ALIASES)) -L$(abspath $(OUT)) \
    $(GIMP_PLUGIN_FMT_LDPATHS)
ifeq ($(UNAME_S),Linux)
  GIMP_PLUGIN_LINCLUDE += -Wl,-rpath,$(abspath $(OUT)) \
      -Wl,-rpath,$(abspath $(GIMP_LIB_ALIASES)) \
      -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  # Homebrew keeps a libpng.dylib -> libpng16 symlink and libz here, so the
  # upstream -lpng / -lz resolve; rpath them for the spawned plug-in runtime.
  GIMP_PLUGIN_LINCLUDE += -L/opt/homebrew/lib \
      -Wl,-rpath,/opt/homebrew/lib -Wl,-rpath,$(abspath $(OUT)) \
      -Wl,-rpath,$(abspath $(GIMP_LIB_ALIASES))
endif

# GIMP_CACHE_DIR shares build/upstream/.cache with mk/xclock.mk / mk/xfig.mk;
# the directory rule lives in mk/xclock.mk so no duplicate-target warning is
# emitted.
$(GIMP_TARBALL): | $(GIMP_CACHE_DIR)
	@echo "  FETCH   $(GIMP_URL)"
	$(Q)curl -fsSL -o $@.tmp $(GIMP_URL)
	$(Q)echo "$(GIMP_SHA256)  $@.tmp" | shasum -a 256 -c -
	$(Q)mv $@.tmp $@

$(GIMP_SOURCE_STAMP): $(GIMP_TARBALL) mk/gimp-motif.mk
	@echo "  EXTRACT gimp-$(GIMP_VERSION)"
	$(Q)rm -rf $(GIMP_SRC_DIR)
	$(Q)mkdir -p $(dir $(GIMP_SRC_DIR))
	$(Q)tar -xzf $(GIMP_TARBALL) -C $(dir $(GIMP_SRC_DIR))
	$(Q)touch $@

# Merged include sysroot. Union order: generated X11/Xt headers, then in-tree
# X11 stubs (SM/, ICE/, extensions/XShm.h) without clobbering, then Motif
# (source + generated). Darwin adds malloc.h / values.h shims.
$(GIMP_SYSROOT_STAMP): $(LIBXT_TARGET) $(MOTIF_STAGE_STAMP) mk/gimp-motif.mk
	@echo "  SYSROOT gimp-motif"
	$(Q)rm -rf $(GIMP_SYSROOT)
	$(Q)mkdir -p $(GIMP_SYSROOT)/X11/extensions $(GIMP_SYSROOT)/Xm
	$(Q)for e in $(abspath $(GIMP_UPSTREAM_INC))/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ "$$b" = extensions ] && continue; \
	    ln -sf "$$e" "$(GIMP_SYSROOT)/X11/$$b"; \
	done
	$(Q)for e in $(abspath $(GIMP_UPSTREAM_INC))/X11/extensions/* \
	             $(abspath include)/X11/extensions/*; do \
	    ln -sf "$$e" "$(GIMP_SYSROOT)/X11/extensions/$$(basename "$$e")"; \
	done
	$(Q)for e in $(abspath include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ -e "$(GIMP_SYSROOT)/X11/$$b" ] || ln -sf "$$e" "$(GIMP_SYSROOT)/X11/$$b"; \
	done
	$(Q)for h in $(abspath $(MOTIF_SRC_DIR))/lib/Xm/*.h \
	             $(abspath $(MOTIF_BUILD_DIR))/lib/Xm/*.h; do \
	    ln -sf "$$h" "$(GIMP_SYSROOT)/Xm/$$(basename "$$h")"; \
	done
ifeq ($(UNAME_S),Darwin)
	$(Q)printf '#include <stdlib.h>\n' > $(GIMP_SYSROOT)/malloc.h
	$(Q)printf '%s\n' \
	    '#include <limits.h>' \
	    '#ifndef MAXINT' '#define MAXINT INT_MAX' '#endif' \
	    '#ifndef MININT' '#define MININT INT_MIN' '#endif' \
	    > $(GIMP_SYSROOT)/values.h
endif
	$(Q)touch $@

# Symlink farm mapping compat sonames to canonical xorg/Motif names.
$(GIMP_LIB_ALIASES_STAMP): $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) \
    $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(MOTIF_STAGE_STAMP) \
    mk/gimp-motif.mk
	@mkdir -p $(GIMP_LIB_ALIASES)
	$(Q)rm -f $(GIMP_LIB_ALIASES)/libX*.so $(GIMP_LIB_ALIASES)/libX*.dylib \
	    $(GIMP_LIB_ALIASES)/libMrm.so $(GIMP_LIB_ALIASES)/libMrm.dylib
	$(Q)set -e; for pair in \
	    libX11.so:libX11-compat.so \
	    libXt.so:libXt-compat.so \
	    libXext.so:libXext-compat.so \
	    libXpm.so:libXpm-compat.so \
	    libXmu.so:libXmu-compat.so \
	    libXm.so:libXm.so \
	    libMrm.so:libMrm.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(GIMP_LIB_ALIASES)/$$alias"; \
	done
ifeq ($(UNAME_S),Darwin)
	$(Q)set -e; for pair in \
	    libX11.dylib:libX11-compat.so \
	    libXt.dylib:libXt-compat.so \
	    libXext.dylib:libXext-compat.so \
	    libXpm.dylib:libXpm-compat.so \
	    libXmu.dylib:libXmu-compat.so \
	    libXm.dylib:libXm.so \
	    libMrm.dylib:libMrm.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(GIMP_LIB_ALIASES)/$$alias"; \
	done
endif
	$(Q)touch $@

.PHONY: gimp-motif gimp-motif-clean check-differential-gimp-motif
## Build the historical Motif GIMP 0.54.1 against the libx11-compat stack
gimp-motif: $(GIMP_BUILD_STAMP)

# The recursive make unsets MAKEFLAGS / MFLAGS so the parent's
# --no-builtin-rules (mk/toolchain.mk) does not propagate into GIMP's 1996
# Makefiles, which depend on the built-in %.o: %.c rule to compile every
# object. Without this the app link fires with zero objects. Same guard as
# mk/mosaic.mk.
$(GIMP_BUILD_STAMP): $(GIMP_SOURCE_STAMP) $(GIMP_PATCHES) $(GIMP_SYSROOT_STAMP) \
    $(GIMP_LIB_ALIASES_STAMP)
	@mkdir -p $(GIMP_BUILD_DIR)
	$(Q)rm -rf $(GIMP_WORK_DIR)
	$(Q)mkdir -p $(GIMP_WORK_DIR)
	$(Q)tar -cf - -C $(GIMP_SRC_DIR) . | tar -xf - -C $(GIMP_WORK_DIR)
	$(Q)set -e; for patch in $(abspath $(GIMP_PATCHES)); do \
	    patch -d $(GIMP_WORK_DIR) -p1 < "$$patch" >> $(GIMP_LOG) 2>&1; \
	done
	@echo "  AUTOCONF gimp-motif"
	$(Q)cd $(GIMP_WORK_DIR) && autoconf -f -o configure configure.in \
	    >> $(GIMP_LOG) 2>&1 || { \
	        echo "  FAIL    see $(GIMP_LOG)" >&2; tail -60 $(GIMP_LOG) >&2; exit 1; }
	@echo "  CONF    gimp-motif"
	$(Q)cd $(GIMP_WORK_DIR) && \
	    CC='$(CC)' \
	    LDFLAGS='$(GIMP_LDFLAGS)' \
	    ./configure \
	        --x-includes='$(abspath $(GIMP_SYSROOT))' \
	        --x-libraries='$(abspath $(GIMP_LIB_ALIASES))' \
	        >> $(GIMP_LOG) 2>&1 || { \
	            echo "  FAIL    see $(GIMP_LOG)" >&2; tail -60 $(GIMP_LOG) >&2; exit 1; }
	@echo "  MAKE    gimp-motif app"
	$(Q)env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(GIMP_WORK_DIR)/app \
	    >> $(GIMP_LOG) 2>&1 || { \
	    echo "  FAIL    see $(GIMP_LOG)" >&2; tail -60 $(GIMP_LOG) >&2; exit 1; }
	@echo "  MAKE    gimp-motif plug-ins"
	# The plug-ins Makefile hardcodes CC = gcc, unlike app/Makefile which
	# picks up configure's CC. On macOS gcc resolves to clang so it built,
	# but on a host with real gcc the balooii -Wno-error=return-mismatch
	# flag (GCC 14+/clang only) breaks older gcc. Override CC with the
	# toolchain compiler so plug-ins build with the same clang as the rest.
	$(Q)env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(GIMP_WORK_DIR)/plug-ins \
	    CC='$(CC)' \
	    INCLUDE='$(GIMP_PLUGIN_INCLUDE)' LINCLUDE='$(GIMP_PLUGIN_LINCLUDE)' \
	    >> $(GIMP_LOG) 2>&1 || { \
	        echo "  FAIL    see $(GIMP_LOG)" >&2; tail -60 $(GIMP_LOG) >&2; exit 1; }
	$(Q)touch $@

gimp-motif-clean:
	@echo "  CLEAN   gimp-motif"
	$(Q)rm -rf $(GIMP_BUILD_DIR)

# Runtime env: resolve the compat sonames embedded in app/gimp's DT_NEEDED
# (libXm.5, libXt-compat.so, ...) from build/ and the lib-aliases farm.
gimp_ui_replay_lib_path = $(abspath $(OUT)):$(abspath $(GIMP_LIB_ALIASES))$(if $(SDL_RUNTIME_LIBDIR),:$(SDL_RUNTIME_LIBDIR))
gimp_ui_replay_env = \
    --env DYLD_LIBRARY_PATH=$(gimp_ui_replay_lib_path)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    --env LD_LIBRARY_PATH=$(gimp_ui_replay_lib_path)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
    --env LIBX11_COMPAT_FONT_DIR=$(abspath $(OUT))/../fonts

# GIMP 0.54 searches "~/:/usr/local/lib/gimp" for gimprc, so a generated
# HOME/gimprc with an absolute brush-path is the deterministic config.
GIMP_SMOKE_HOME := $(UI_SMOKE_OUT_ROOT)/gimp-motif-home

# Opt-in: like xfig/mosaic, promote into check-smoke after 20 consecutive
# runs across local + node11 (see TODO.md "GIMP 0.54 Motif validation").
.PHONY: check-smoke-gimp-motif
## Run the replay-based Motif GIMP startup smoke against libx11-compat
check-smoke-gimp-motif: $(UI_SMOKE_OUT_ROOT)/gimp-motif-startup/.stamp

# A clean empty HOME (no gimprc) exercises the gimprc-default-build-paths
# patch: GIMP falls back to the gimprc shipped beside the binary and resolves
# plug-in / brush paths from the build tree, so the smoke needs no generated
# config.
$(UI_SMOKE_OUT_ROOT)/gimp-motif-startup/.stamp: FORCE $(GIMP_BUILD_STAMP)
	$(Q)rm -rf $(abspath $(GIMP_SMOKE_HOME))
	$(Q)mkdir -p $(abspath $(GIMP_SMOKE_HOME))
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name gimp-motif-startup \
	    --app $(abspath $(GIMP_BIN)) \
	    --workdir $(abspath $(GIMP_WORK_DIR))/app \
	    --replay tests/ui/replays/gimp-motif-startup.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/gimp-motif-startup \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/gimp-motif-startup/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    $(gimp_ui_replay_env) \
	    --env HOME=$(abspath $(GIMP_SMOKE_HOME))
	$(Q)touch $@

# Differential: build GIMP on a Linux host against system libX11 + OpenMotif
# and against libx11-compat + bundled Motif, capture the toolbox startup
# screen on each, and compare. GIMP is the heaviest Motif client here, so the
# system OpenMotif baseline diverges more than xfig/mosaic; thresholds start
# loose and tighten as Xm parity work lands. node11 ships OpenMotif 2.3.8,
# which is the same toolkit the balooii patch set targets. See TODO.md
# "GIMP 0.54 Motif validation" for the promotion gate.
GIMP_DIFF_REMOTE ?= node11
GIMP_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-gimp-differential
GIMP_DIFF_DISPLAY ?= 126
GIMP_DIFF_JOBS ?= 1
GIMP_DIFF_INSTALL_DEPS ?= 0
GIMP_DIFF_LOCAL ?= 0
# Calibrated on node11 (OpenMotif 2.3.8 vs compat: MAE 0.072, changed 0.194 for
# the toolbox startup screen); thresholds carry headroom over that baseline.
GIMP_DIFF_MAE_THRESHOLD ?= 0.16
GIMP_DIFF_CHANGED_THRESHOLD ?= 0.42
GIMP_DIFF_GEOMETRY ?= 1280x1024x24
GIMP_DIFF_TOP ?= 12
GIMP_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(GIMP_DIFF_LOCAL)),local,remote)
GIMP_DIFF_OUT_ROOT ?= $(OUT)/gimp-differential
GIMP_DIFF_SCREENSHOT_REGION ?= 0,0,1024,768

gimp_diff_env = \
    GIMP_DIFF_REMOTE='$(GIMP_DIFF_REMOTE)' \
    GIMP_DIFF_REMOTE_ROOT='$(GIMP_DIFF_REMOTE_ROOT)' \
    GIMP_DIFF_DISPLAY='$(GIMP_DIFF_DISPLAY)' \
    GIMP_DIFF_JOBS='$(GIMP_DIFF_JOBS)' \
    GIMP_DIFF_MAE_THRESHOLD='$(GIMP_DIFF_MAE_THRESHOLD)' \
    GIMP_DIFF_CHANGED_THRESHOLD='$(GIMP_DIFF_CHANGED_THRESHOLD)' \
    GIMP_DIFF_GEOMETRY='$(GIMP_DIFF_GEOMETRY)' \
    GIMP_DIFF_TOP='$(GIMP_DIFF_TOP)' \
    GIMP_DIFF_COMPARE_LOCATION='$(GIMP_DIFF_COMPARE_LOCATION)' \
    GIMP_DIFF_OUT_ROOT='$(abspath $(GIMP_DIFF_OUT_ROOT))' \
    GIMP_DIFF_SCREENSHOT_REGION='$(GIMP_DIFF_SCREENSHOT_REGION)'

## GIMP_DIFF_LOCAL=1 to run the build / capture / compare pipeline on the
## local host (used by CI); otherwise the script SSHes to GIMP_DIFF_REMOTE.
check-differential-gimp-motif:
	$(Q)$(gimp_diff_env) $(PYTHON) scripts/run-gimp-differential-tests.py \
	    $(if $(filter 1 yes true,$(GIMP_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(GIMP_DIFF_LOCAL)),--local)
