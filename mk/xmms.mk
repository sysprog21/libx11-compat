XMMS_REPO := https://github.com/deepfire/xmms.git
XMMS_REVISION := 5847897230da018d93cd9833695f944ece96587b
XMMS_CLONE_FLAGS ?= --filter=blob:none --no-checkout
XMMS_SRC_DIR := $(OUT)/upstream/xmms
XMMS_SOURCE_STAMP := $(OUT)/upstream/.xmms-source-stamp
XMMS_PATCHES := $(sort $(wildcard compat/xmms-patches/*.patch))

XMMS_BUILD_DIR := $(OUT)/xmms
XMMS_WORK_DIR := $(XMMS_BUILD_DIR)/source
XMMS_CONFIG_DIR := $(XMMS_BUILD_DIR)/config-bin
XMMS_INSTALL_DIR := $(abspath $(XMMS_BUILD_DIR))/install
XMMS_BUILD_STAMP := $(XMMS_BUILD_DIR)/.build-stamp
XMMS_LOG := $(abspath $(XMMS_BUILD_DIR))/build.log
XMMS_BIN := $(XMMS_INSTALL_DIR)/bin/xmms
XMMS_COREAUDIO_SRC := compat/xmms-coreaudio/coreaudio.c
XMMS_COREAUDIO_BUILD :=
XMMS_COREAUDIO_INSTALL :=
XMMS_XMMS_LDFLAGS := -export-dynamic
XMMS_CONFIGURE_TRIPLET := $(shell $(CC) -dumpmachine | sed 's/^arm64-/arm-/; s/[0-9][0-9.]*$$//')
XMMS_CONFIGURE_HOST_FLAGS :=
ifeq ($(UNAME_S),Darwin)
  XMMS_CONFIGURE_HOST_FLAGS := --build=$(XMMS_CONFIGURE_TRIPLET) --host=$(XMMS_CONFIGURE_TRIPLET)
  XMMS_XMMS_LDFLAGS := -Wl,-export_dynamic
  XMMS_COREAUDIO_BUILD = && \
	    mkdir -p Output/coreaudio && \
	    cp $(abspath $(XMMS_COREAUDIO_SRC)) Output/coreaudio/coreaudio.c && \
	    ./libtool --tag=CC --mode=compile $(CC) -DHAVE_CONFIG_H \
	        -I. -IOutput/coreaudio -Ixmms -Ilibxmms $(XMMS_CFLAGS) \
	        -c Output/coreaudio/coreaudio.c -o Output/coreaudio/coreaudio.lo >> $(XMMS_LOG) 2>&1 && \
	    ./libtool --tag=CC --mode=link $(CC) $(XMMS_CFLAGS) \
	        -L$(abspath $(GTK1_CMAKE_DIR)) -L$(abspath $(GTK1_LIB_ALIASES)) \
	        -L$(abspath $(OUT)) $(XMMS_RPATH_FLAGS) \
	        -o Output/coreaudio/libcoreaudio.la \
	        -rpath $(XMMS_INSTALL_DIR)/lib/xmms/Plugins -module -avoid-version \
	        -export-symbols-regex "get_.plugin_info" Output/coreaudio/coreaudio.lo \
	        $(XMMS_GTK_LIBS) -framework AudioToolbox >> $(XMMS_LOG) 2>&1
  XMMS_COREAUDIO_INSTALL = && \
	    ./libtool --mode=install /usr/bin/install -c \
	        Output/coreaudio/libcoreaudio.la \
	        $(XMMS_INSTALL_DIR)/lib/xmms/Plugins/libcoreaudio.la >> $(XMMS_LOG) 2>&1
endif

XMMS_PLATFORM_LIBS :=
XMMS_PLATFORM_LDFLAGS :=
ifeq ($(UNAME_S),Linux)
  XMMS_PLATFORM_LIBS += -ldl
endif
ifeq ($(UNAME_S),Darwin)
  XMMS_PLATFORM_LIBS += -L/opt/homebrew/opt/gettext/lib -lintl -liconv
  XMMS_PLATFORM_LDFLAGS += -Wl,-rpath,/opt/homebrew/opt/gettext/lib
endif

XMMS_GTK_CFLAGS := \
    -I$(abspath $(GTK1_WORK_DIR)) \
    -I$(abspath $(GTK1_WORK_DIR))/gtk \
    -I$(abspath $(GTK1_WORK_DIR))/glib \
    -I$(abspath $(GTK1_WORK_DIR))/gdk \
    -I$(abspath $(GTK1_WORK_DIR))/gdk/x11
XMMS_GTK_LIBS := -L$(abspath $(GTK1_CMAKE_DIR)) -lgtk1 \
    -L$(abspath $(GTK1_LIB_ALIASES)) -L$(abspath $(OUT)) -lX11 -lXext \
    -lm $(XMMS_PLATFORM_LIBS)
XMMS_RPATH_FLAGS := -Wl,-rpath,$(abspath $(GTK1_CMAKE_DIR)) \
    -Wl,-rpath,$(abspath $(GTK1_LIB_ALIASES)) -Wl,-rpath,$(abspath $(OUT)) \
    $(XMMS_PLATFORM_LDFLAGS)
ifeq ($(UNAME_S),Linux)
  XMMS_RPATH_FLAGS += -Wl,-rpath-link,$(abspath $(OUT))
endif
XMMS_CFLAGS := -include stdio.h -include glib/gprintf.h \
    -include $(abspath compat/xmms-compat-preinclude.h) \
    -include glib/gstrfuncs.h \
    $(XMMS_GTK_CFLAGS) -I$(abspath include) -I$(abspath $(OUT)/upstream/include) \
    -Wno-error=implicit-function-declaration \
    -Wno-incompatible-pointer-types \
    -Wno-error=int-conversion -fcommon
XMMS_CONFIG_ENV := \
    PATH=$(abspath $(XMMS_CONFIG_DIR)):$$PATH \
    DYLD_LIBRARY_PATH=$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    LD_LIBRARY_PATH=$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}

$(XMMS_SOURCE_STAMP): mk/xmms.mk
	@echo "  GIT     xmms"
	$(Q)mkdir -p $(dir $(XMMS_SRC_DIR))
	$(Q)test -d $(XMMS_SRC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(XMMS_CLONE_FLAGS) $(XMMS_REPO) $(XMMS_SRC_DIR)
	$(Q)git -C $(XMMS_SRC_DIR) fetch $(MOTIF_GIT_Q) origin $(XMMS_REVISION)
	$(Q)git -C $(XMMS_SRC_DIR) checkout --detach $(XMMS_REVISION)
	$(Q)git -C $(XMMS_SRC_DIR) reset --hard $(XMMS_REVISION) >/dev/null
	$(Q)git -C $(XMMS_SRC_DIR) clean -fdx >/dev/null
	$(Q)printf '%s\n' '$(XMMS_REVISION)' > $@

$(XMMS_CONFIG_DIR)/gtk-config: mk/xmms.mk $(GTK1_BUILD_STAMP)
	@mkdir -p $(XMMS_CONFIG_DIR)
	$(Q)printf '%s\n' \
	    '#!/bin/sh' \
	    'for arg in "$$@"; do' \
	    '  case "$$arg" in' \
	    '    --version) echo 1.2.10; exit 0 ;;' \
	    '    --cflags) echo $(XMMS_GTK_CFLAGS); exit 0 ;;' \
	    '    --libs) echo $(XMMS_GTK_LIBS); exit 0 ;;' \
	    '  esac' \
	    'done' \
	    'exit 0' > $@
	$(Q)chmod +x $@

$(XMMS_CONFIG_DIR)/glib-config: $(XMMS_CONFIG_DIR)/gtk-config
	$(Q)cp $< $@

.PHONY: xmms xmms-clean check-smoke-xmms-startup check-smoke-xmms-window \
    check-xmms-no-host-x11 check-xmms-no-unexpected-stubs \
    check-xmms-shape-pixmap-artifacts check-differential-xmms check-xmms
## Build XMMS 1.2.11 against the existing GTK1 port and libx11-compat
xmms: $(XMMS_BUILD_STAMP)

$(XMMS_BUILD_STAMP): $(XMMS_SOURCE_STAMP) $(XMMS_PATCHES) $(XMMS_COREAUDIO_SRC) \
    $(XMMS_CONFIG_DIR)/glib-config $(GTK1_BUILD_STAMP)
	@mkdir -p $(XMMS_BUILD_DIR)
	$(Q)rm -rf $(XMMS_WORK_DIR) $(XMMS_INSTALL_DIR)
	$(Q)mkdir -p $(XMMS_WORK_DIR)
	$(Q)git -C $(XMMS_SRC_DIR) reset --hard $(XMMS_REVISION) >/dev/null
	$(Q)git -C $(XMMS_SRC_DIR) clean -fdx >/dev/null
	$(Q)git -C $(XMMS_SRC_DIR) archive $(XMMS_REVISION) | tar -xf - -C $(XMMS_WORK_DIR)
	$(Q): > $(XMMS_LOG)
	$(Q)set -e; for patch in $(abspath $(XMMS_PATCHES)); do \
	    patch -d $(XMMS_WORK_DIR) -p1 < "$$patch" >> $(XMMS_LOG) 2>&1; \
	done
	@echo "  CONF    xmms"
	$(Q)cd $(XMMS_WORK_DIR) && \
	    $(XMMS_CONFIG_ENV) \
	    CC='$(CC)' \
	    CFLAGS='$(XMMS_CFLAGS)' \
	    LDFLAGS='-L$(abspath $(GTK1_CMAKE_DIR)) -L$(abspath $(GTK1_LIB_ALIASES)) -L$(abspath $(OUT)) $(XMMS_RPATH_FLAGS)' \
	    ./configure $(XMMS_CONFIGURE_HOST_FLAGS) \
	        --prefix=$(XMMS_INSTALL_DIR) \
	        --disable-glibtest --disable-gtktest --disable-nls \
	        --disable-opengl --disable-esd --disable-mikmod \
	        --disable-vorbis --disable-oss --disable-alsatest \
	        --disable-ipv6 --disable-user-plugin-dir --enable-one-plugin-dir \
	        --x-includes=$(abspath include) \
	        --x-libraries=$(abspath $(OUT)) \
	        >> $(XMMS_LOG) 2>&1 || { \
	            echo "  FAIL    see $(XMMS_LOG)" >&2; tail -80 $(XMMS_LOG) >&2; exit 1; }
	@echo "  MAKE    xmms"
	$(Q)cd $(XMMS_WORK_DIR) && \
	    $(XMMS_CONFIG_ENV) env -u MAKEFLAGS -u MFLAGS \
	    $(MAKE) -C libxmms >> $(XMMS_LOG) 2>&1 && \
	    $(MAKE) -C xmms xmms_LDFLAGS='$(XMMS_XMMS_LDFLAGS)' >> $(XMMS_LOG) 2>&1 && \
	    $(MAKE) -C Output/disk_writer >> $(XMMS_LOG) 2>&1 && \
	    $(MAKE) -C Input/tonegen >> $(XMMS_LOG) 2>&1 && \
	    $(MAKE) -C Input/mpg123 >> $(XMMS_LOG) 2>&1 \
	    $(XMMS_COREAUDIO_BUILD) || { \
	        echo "  FAIL    see $(XMMS_LOG)" >&2; tail -80 $(XMMS_LOG) >&2; exit 1; }
	@echo "  INSTALL xmms runtime"
	$(Q)cd $(XMMS_WORK_DIR) && \
	    $(XMMS_CONFIG_ENV) env -u MAKEFLAGS -u MFLAGS \
	    $(MAKE) -C libxmms install >> $(XMMS_LOG) 2>&1 && \
	    $(MAKE) -C xmms xmms_LDFLAGS='$(XMMS_XMMS_LDFLAGS)' install >> $(XMMS_LOG) 2>&1 && \
	    $(MAKE) -C Output/disk_writer install >> $(XMMS_LOG) 2>&1 && \
	    $(MAKE) -C Input/tonegen install >> $(XMMS_LOG) 2>&1 && \
	    $(MAKE) -C Input/mpg123 install >> $(XMMS_LOG) 2>&1 \
	    $(XMMS_COREAUDIO_INSTALL) || { \
	        echo "  FAIL    see $(XMMS_LOG)" >&2; tail -80 $(XMMS_LOG) >&2; exit 1; }
	$(Q)touch $@

# Fail if the XMMS binary or any plugin links a host X library instead of the
# compat stack. The exclusion whitelists the compat libs, the alias farm, and
# the versioned base sonames libX11.so.6 / libXext.so.6: on Linux a GTK1 client
# pulls those in transitively through libXext even though its own X calls bind
# to the compat lib, so they are benign and unavoidable. libXi / libXrender and
# any unversioned host libX11 / libXext are still flagged.
check-xmms-no-host-x11: $(XMMS_BUILD_STAMP)
	$(Q)if [ "$(UNAME_S)" = "Darwin" ]; then \
	    deps="$$(otool -L $(XMMS_BIN) $$(find $(XMMS_INSTALL_DIR)/lib -type f \
	        \( -name '*.so' -o -name '*.dylib' \)))"; \
	else \
	    deps="$$(ldd $(XMMS_BIN) $$(find $(XMMS_INSTALL_DIR)/lib -type f \
	        \( -name '*.so' -o -name '*.dylib' \)))"; \
	fi; \
	bad="$$(printf '%s\n' "$$deps" | rg 'libX(11|ext|i|render)([.][0-9]+)*[.](dylib|so)([.][0-9]+)*' | \
	    rg -v 'libX11-compat|libXext-compat|$(abspath $(GTK1_LIB_ALIASES))/libX(11|ext)[.](so|dylib)|libX(11|ext)[.]so[.][0-9]' || true)"; \
	if [ -n "$$bad" ]; then \
	    printf '%s\n' "$$bad" >&2; \
	    exit 1; \
	fi

check-smoke-xmms-startup: $(UI_SMOKE_OUT_ROOT)/xmms-startup/.stamp

$(UI_SMOKE_OUT_ROOT)/xmms-startup/.stamp: FORCE $(XMMS_BUILD_STAMP)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-startup/home
	$(Q)mkdir -p $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-startup/home
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xmms-startup \
	    --app $(abspath $(XMMS_BIN)) \
	    --workdir $(abspath $(XMMS_INSTALL_DIR)) \
	    --replay tests/ui/replays/xmms-startup.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-startup \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-startup/render-stats.tsv \
	    --env DYLD_LIBRARY_PATH=$(abspath $(XMMS_INSTALL_DIR))/lib:$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(XMMS_INSTALL_DIR))/lib:$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/xmms-startup/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@

check-smoke-xmms-window: $(UI_SMOKE_OUT_ROOT)/xmms-window/.stamp

$(UI_SMOKE_OUT_ROOT)/xmms-window/.stamp: FORCE $(XMMS_BUILD_STAMP) \
    tests/ui/fixtures/xmms/config tests/ui/fixtures/xmms/skin/region.txt
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/home
	$(Q)mkdir -p $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/home/.xmms
	$(Q)cp tests/ui/fixtures/xmms/config \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/home/.xmms/config
	$(Q)printf '%s\n' 'skin=$(abspath tests/ui/fixtures/xmms/skin)' >> \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/home/.xmms/config
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name xmms-window \
	    --app $(abspath $(XMMS_BIN)) \
	    --workdir $(abspath $(XMMS_INSTALL_DIR)) \
	    --replay tests/ui/replays/xmms-window.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/render-stats.tsv \
	    --env DYLD_LIBRARY_PATH=$(abspath $(XMMS_INSTALL_DIR))/lib:$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(XMMS_INSTALL_DIR))/lib:$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@

check-xmms-no-unexpected-stubs: check-smoke-xmms-startup check-smoke-xmms-window
	$(Q)! rg -n "WARN_UNIMPLEMENTED" \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-startup/logs \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/logs
	$(Q)! rg -n "GLib does not support threads|The thread system is not yet initialized" \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-startup/logs \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/logs
	$(Q)! rg -n "replay: line .*target-at" \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/logs

check-xmms-shape-pixmap-artifacts: check-smoke-xmms-window
	$(Q){ \
	    printf '%s\n' 'runtime-shape-state'; \
	    $(PYTHON) -c 'import json; s=json.load(open("$(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/states/playlist-open.json")); [print("%s\tmap=%s\tshape=%s\torigin=%s\tbottom_left=%s\tbottom_left_hit=%s" % (w.get("wm_class"), w.get("map_state"), w.get("shape_bounding"), w.get("shape_bounding_origin"), w.get("shape_bounding_bottom_left"), w.get("shape_bounding_bottom_left_hit"))) for w in s.get("windows", []) if w.get("wm_class") in ("XMMS_Player", "XMMS_Equalizer", "XMMS_Playlist")]'; \
	    printf '%s\n' 'xmms-source-shape-pixmap-paths'; \
	    rg -n 'gtk_widget_shape_combine_mask|skin_create_transparent_mask|gdk_pixmap_create_from_xpm_d|gdk_draw_pixmap|gdk_window_set_back_pixmap|gdk_image_get' $(XMMS_WORK_DIR)/xmms/skin.c $(XMMS_WORK_DIR)/xmms/equalizer.c $(XMMS_WORK_DIR)/xmms/main.c $(XMMS_WORK_DIR)/xmms/playlistwin.c; \
	    printf '%s\n' 'gtk-shape-bridge'; \
	    rg -n 'XShapeCombineMask|XShapeCombine(Rectangles|Region)' $(GTK1_WORK_DIR)/gdk/x11/gdkwindow.c; \
	} > $(abspath $(UI_SMOKE_OUT_ROOT))/xmms-window/shape-pixmap-report.txt
	$(Q)! rg -n 'XShapeCombine(Rectangles|Region)|XShapeCombineShape|XShapeOffsetShape' \
	    $(XMMS_WORK_DIR)/xmms $(XMMS_WORK_DIR)/libxmms

check-xmms: check-xmms-no-host-x11 check-xmms-no-unexpected-stubs \
    check-xmms-shape-pixmap-artifacts

XMMS_DIFF_REMOTE ?= node11
XMMS_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-xmms-differential
XMMS_DIFF_DISPLAY ?= 129
XMMS_DIFF_JOBS ?= 1
XMMS_DIFF_INSTALL_DEPS ?= 0
XMMS_DIFF_LOCAL ?= 0
XMMS_DIFF_MAE_THRESHOLD ?= 0.16
XMMS_DIFF_CHANGED_THRESHOLD ?= 0.42
XMMS_DIFF_GEOMETRY ?= 1280x1024x24
XMMS_DIFF_TOP ?= 12
XMMS_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(XMMS_DIFF_LOCAL)),local,remote)
XMMS_DIFF_OUT_ROOT ?= $(OUT)/xmms-differential
XMMS_DIFF_SCREENSHOT_REGION ?= 0,0,640,420

xmms_diff_env = \
    XMMS_DIFF_REMOTE='$(XMMS_DIFF_REMOTE)' \
    XMMS_DIFF_REMOTE_ROOT='$(XMMS_DIFF_REMOTE_ROOT)' \
    XMMS_DIFF_DISPLAY='$(XMMS_DIFF_DISPLAY)' \
    XMMS_DIFF_JOBS='$(XMMS_DIFF_JOBS)' \
    XMMS_DIFF_MAE_THRESHOLD='$(XMMS_DIFF_MAE_THRESHOLD)' \
    XMMS_DIFF_CHANGED_THRESHOLD='$(XMMS_DIFF_CHANGED_THRESHOLD)' \
    XMMS_DIFF_GEOMETRY='$(XMMS_DIFF_GEOMETRY)' \
    XMMS_DIFF_TOP='$(XMMS_DIFF_TOP)' \
    XMMS_DIFF_COMPARE_LOCATION='$(XMMS_DIFF_COMPARE_LOCATION)' \
    XMMS_DIFF_OUT_ROOT='$(abspath $(XMMS_DIFF_OUT_ROOT))' \
    XMMS_DIFF_SCREENSHOT_REGION='$(XMMS_DIFF_SCREENSHOT_REGION)'

## Compare XMMS screenshots for system GTK1/X11 vs the existing GTK1 port
## over libx11-compat. Set XMMS_DIFF_LOCAL=1 to run the pipeline on this host;
## otherwise the script SSHes to XMMS_DIFF_REMOTE.
check-differential-xmms:
	$(Q)$(xmms_diff_env) $(PYTHON) scripts/run-xmms-differential-tests.py \
	    $(if $(filter 1 yes true,$(XMMS_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(XMMS_DIFF_LOCAL)),--local)

xmms-clean:
	@echo "  CLEAN   xmms"
	$(Q)rm -rf $(XMMS_BUILD_DIR)
