MICROPOLIS_URL := https://github.com/stargo/micropolis
MICROPOLIS_REVISION := c24fb94b88efa4689338543a8f7bb046fe5a1aaf
MICROPOLIS_SRC_DIR := $(OUT)/upstream/micropolis-src
MICROPOLIS_SOURCE_STAMP := $(MICROPOLIS_SRC_DIR)/.source-stamp
MICROPOLIS_PATCHES := $(sort $(wildcard compat/micropolis-patches/*.patch))

MICROPOLIS_BUILD_DIR := $(OUT)/micropolis
MICROPOLIS_WORK_DIR := $(MICROPOLIS_BUILD_DIR)/source
MICROPOLIS_LOG := $(abspath $(MICROPOLIS_BUILD_DIR))/build.log
MICROPOLIS_BIN := $(MICROPOLIS_WORK_DIR)/src/sim/sim
MICROPOLIS_TK_WISH := $(MICROPOLIS_WORK_DIR)/src/tk/wish
MICROPOLIS_TCLX_WISH := $(MICROPOLIS_WORK_DIR)/src/tclx/wish
MICROPOLIS_FIXTURE_CITY := tests/ui/fixtures/micropolis/neatmap.cty
MICROPOLIS_SYSROOT := $(MICROPOLIS_BUILD_DIR)/sysroot
MICROPOLIS_SYSROOT_STAMP := $(MICROPOLIS_SYSROOT)/.stamp
MICROPOLIS_LIB_ALIASES := $(MICROPOLIS_BUILD_DIR)/lib-aliases
MICROPOLIS_LIB_ALIASES_STAMP := $(MICROPOLIS_LIB_ALIASES)/.stamp
MICROPOLIS_CC := $(MICROPOLIS_BUILD_DIR)/bin/cc-legacy

MICROPOLIS_YACC ?= $(or $(shell command -v byacc 2>/dev/null),yacc)
MICROPOLIS_LEGACY_CFLAGS := -Wno-unknown-warning-option -Wno-implicit-int \
    -Wno-implicit-function-declaration -Wno-return-type \
    -Wno-return-mismatch -Wno-int-conversion \
    -Wno-incompatible-pointer-types -Wno-deprecated-non-prototype

MICROPOLIS_RPATH_FLAGS := -Wl,-rpath,$(abspath $(OUT)) \
    -Wl,-rpath,$(abspath $(MICROPOLIS_LIB_ALIASES))
ifeq ($(UNAME_S),Linux)
  MICROPOLIS_RPATH_FLAGS += -Wl,-rpath-link,$(abspath $(OUT))
endif

MICROPOLIS_LDFLAGS := -L$(abspath $(MICROPOLIS_LIB_ALIASES)) \
    -L$(abspath $(OUT)) $(MICROPOLIS_RPATH_FLAGS)
# Micropolis links wish/sim with XLIB (not LDFLAGS), so the flags that let the
# linker resolve libX11-compat.so's SDL/TTF shim dependency have to ride here
# too. On the SDL2 backend the compat lib pulls SDL_*/TTF_* from the in-tree
# libSDL2-x11compat / libSDL2_ttf-x11compat shims in $(OUT); GNU ld needs
# -L$(OUT) plus -rpath-link to follow that transitive DT_NEEDED at link time
# (macOS ld defers it, so this only bites the Linux build).
MICROPOLIS_XLIBS := -L$(abspath $(MICROPOLIS_LIB_ALIASES)) -L$(abspath $(OUT)) \
    $(MICROPOLIS_RPATH_FLAGS) -lX11 -lXext -lXpm
MICROPOLIS_TCLLIB := $(abspath $(MICROPOLIS_WORK_DIR))/src/tclx/tcllib
MICROPOLIS_TKLIB := $(abspath $(MICROPOLIS_WORK_DIR))/src/tclx/tcllib

$(MICROPOLIS_SOURCE_STAMP): mk/micropolis.mk
	@echo "  GIT     $(MICROPOLIS_URL)"
	$(Q)mkdir -p $(dir $(MICROPOLIS_SRC_DIR))
	$(Q)test -d $(MICROPOLIS_SRC_DIR)/.git || \
	    git clone --filter=blob:none $(MICROPOLIS_URL) $(MICROPOLIS_SRC_DIR)
	$(Q)cd $(MICROPOLIS_SRC_DIR) && git fetch -q origin $(MICROPOLIS_REVISION) || \
	    (cd $(MICROPOLIS_SRC_DIR) && git fetch -q origin)
	$(Q)cd $(MICROPOLIS_SRC_DIR) && \
	    git checkout -q --detach $(MICROPOLIS_REVISION) && \
	    git reset --hard -q $(MICROPOLIS_REVISION) && \
	    git clean -fdxq
	$(Q)printf '%s\n' '$(MICROPOLIS_REVISION)' > $@

$(MICROPOLIS_SYSROOT_STAMP): $(UPSTREAM_HEADERS_STAMP) mk/micropolis.mk
	@echo "  SYSROOT micropolis"
	$(Q)rm -rf $(MICROPOLIS_SYSROOT)
	$(Q)mkdir -p $(MICROPOLIS_SYSROOT)/X11/extensions
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ "$$b" = extensions ] && continue; \
	    ln -sf "$$e" "$(MICROPOLIS_SYSROOT)/X11/$$b"; \
	done
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/extensions/* \
	             $(abspath include)/X11/extensions/*; do \
	    ln -sf "$$e" "$(MICROPOLIS_SYSROOT)/X11/extensions/$$(basename "$$e")"; \
	done
	$(Q)for e in $(abspath include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ -e "$(MICROPOLIS_SYSROOT)/X11/$$b" ] || \
	        ln -sf "$$e" "$(MICROPOLIS_SYSROOT)/X11/$$b"; \
	done
	$(Q)touch $@

$(MICROPOLIS_LIB_ALIASES_STAMP): $(TARGET) $(XEXT_COMPAT_TARGET) \
    $(LIBXPM_TARGET) mk/micropolis.mk
	@mkdir -p $(MICROPOLIS_LIB_ALIASES)
	$(Q)rm -f $(MICROPOLIS_LIB_ALIASES)/libX*.so \
	    $(MICROPOLIS_LIB_ALIASES)/libX*.dylib
	$(Q)set -e; for pair in \
	    libX11.so:libX11-compat.so \
	    libXext.so:libXext-compat.so \
	    libXpm.so:libXpm-compat.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(MICROPOLIS_LIB_ALIASES)/$$alias"; \
	done
ifeq ($(UNAME_S),Darwin)
	$(Q)set -e; for pair in \
	    libX11.dylib:libX11-compat.so \
	    libXext.dylib:libXext-compat.so \
	    libXpm.dylib:libXpm-compat.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(MICROPOLIS_LIB_ALIASES)/$$alias"; \
	done
endif
	$(Q)touch $@

$(MICROPOLIS_CC): mk/micropolis.mk
	@mkdir -p $(dir $@)
	$(Q)printf '%s\n' '#!/bin/sh' \
	    'exec "$${DIFF_CC:-$(CC)}" $(MICROPOLIS_LEGACY_CFLAGS) "$$@"' > $@
	$(Q)chmod +x $@

.PHONY: micropolis micropolis-check check-smoke-micropolis \
    check-smoke-micropolis-shm check-smoke-micropolis-launcher \
    micropolis-clean
## Build Micropolis against its bundled Tcl/Tk/TclX and libx11-compat
micropolis: $(MICROPOLIS_BIN)

$(MICROPOLIS_BIN): $(MICROPOLIS_SOURCE_STAMP) $(MICROPOLIS_PATCHES) \
    $(MICROPOLIS_SYSROOT_STAMP) $(MICROPOLIS_LIB_ALIASES_STAMP) \
    $(MICROPOLIS_CC) mk/micropolis.mk
	@mkdir -p $(MICROPOLIS_BUILD_DIR)
	$(Q)rm -f $(MICROPOLIS_LOG)
	$(Q)rm -rf $(MICROPOLIS_WORK_DIR)
	$(Q)mkdir -p $(MICROPOLIS_WORK_DIR)
	$(Q)tar --exclude .git -cf - -C $(MICROPOLIS_SRC_DIR) . | \
	    tar -xf - -C $(MICROPOLIS_WORK_DIR)
	$(Q)set -e; for patch in $(MICROPOLIS_PATCHES); do \
	    patch --fuzz=0 --no-backup-if-mismatch -d $(MICROPOLIS_WORK_DIR) -p1 < "$$patch"; \
	done
	@echo "  MAKE    micropolis"
	$(Q)env -u MAKEFLAGS -u MFLAGS \
	    SOURCE_DATE_EPOCH=$${SOURCE_DATE_EPOCH:-1704067200} \
	    $(MAKE) -C $(MICROPOLIS_WORK_DIR)/src all \
	    CC='$(abspath $(MICROPOLIS_CC))' \
	    XINCLUDE='$(abspath $(MICROPOLIS_SYSROOT))' \
	    XLIB='$(MICROPOLIS_XLIBS)' \
	    XPM_LIBS='-L$(abspath $(MICROPOLIS_LIB_ALIASES)) -lXpm' \
	    TCL_TK_LIBS='-L$(abspath $(MICROPOLIS_LIB_ALIASES)) -lX11 -lm -lXpm' \
	    XLDFLAGS='$(MICROPOLIS_LDFLAGS)' \
	    LDFLAGS='$(MICROPOLIS_LDFLAGS)' \
	    TCLLIBRARY='$(MICROPOLIS_TCLLIB)' \
	    TKLIBRARY='$(MICROPOLIS_TKLIB)' \
	    YACC='$(MICROPOLIS_YACC)' \
	    >> $(MICROPOLIS_LOG) 2>&1 || { \
	    echo "  FAIL    see $(MICROPOLIS_LOG)" >&2; tail -60 $(MICROPOLIS_LOG) >&2; exit 1; }

$(MICROPOLIS_TK_WISH) $(MICROPOLIS_TCLX_WISH): $(MICROPOLIS_BIN)
	$(Q)test -x $@

micropolis-check: $(MICROPOLIS_BIN) $(MICROPOLIS_TK_WISH) \
    $(MICROPOLIS_TCLX_WISH) check-smoke-micropolis \
    check-smoke-micropolis-shm check-smoke-micropolis-launcher
	$(Q)deps=$(MICROPOLIS_BUILD_DIR)/link-deps.txt; \
	bad=$(MICROPOLIS_BUILD_DIR)/bad-links.txt; \
	rm -f "$$deps" "$$bad"; \
	set -e; for bin in $(MICROPOLIS_BIN) $(MICROPOLIS_TK_WISH) $(MICROPOLIS_TCLX_WISH); do \
	    test -x "$$bin"; \
	    $(if $(filter Darwin,$(UNAME_S)),otool -L,ldd) "$$bin"; \
	done > "$$deps"; \
	if rg '(/opt/X11|/usr/X11|XQuartz|libX(11|ext|pm)[.](so|dylib))' "$$deps" | \
	    rg -v 'libX(11|ext|pm)-compat|$(abspath $(MICROPOLIS_LIB_ALIASES))/libX(11|ext|pm)[.](so|dylib)' > "$$bad"; then \
	    cat "$$bad" >&2; \
	    exit 1; \
	fi
	@echo "  OK      micropolis links use compat X11"

MICROPOLIS_SMOKE_OUT := $(UI_SMOKE_OUT_ROOT)/micropolis-startup
MICROPOLIS_SHM_SMOKE_OUT := $(UI_SMOKE_OUT_ROOT)/micropolis-startup-shm
MICROPOLIS_LAUNCHER_SMOKE_OUT := $(UI_SMOKE_OUT_ROOT)/micropolis-scenario-launcher
micropolis_ui_lib_path = $(abspath $(OUT)):$(abspath $(MICROPOLIS_LIB_ALIASES))$(if $(SDL_RUNTIME_LIBDIR),:$(SDL_RUNTIME_LIBDIR))

check-smoke-micropolis: $(MICROPOLIS_BIN) $(MICROPOLIS_FIXTURE_CITY)
	$(Q)rm -rf $(abspath $(MICROPOLIS_SMOKE_OUT))/home
	$(Q)mkdir -p $(abspath $(MICROPOLIS_SMOKE_OUT))/home
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name micropolis-startup \
	    --app $(abspath $(MICROPOLIS_BIN)) \
	    --app-arg=-w \
	    --app-arg=$(abspath $(MICROPOLIS_FIXTURE_CITY)) \
	    --workdir $(abspath $(MICROPOLIS_WORK_DIR)) \
	    --replay tests/ui/replays/micropolis-startup.replay \
	    --out-root $(abspath $(MICROPOLIS_SMOKE_OUT)) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --env DYLD_LIBRARY_PATH=$(micropolis_ui_lib_path)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(micropolis_ui_lib_path)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env DISPLAY=:0 \
	    --env SIMHOME=$(abspath $(MICROPOLIS_WORK_DIR)) \
	    --env HOME=$(abspath $(MICROPOLIS_SMOKE_OUT))/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)test -s $(abspath $(MICROPOLIS_SMOKE_OUT))/logs/micropolis-startup.log
	$(Q)! rg 'Hit unimplemented function|XAllocColorCells|XStoreColor|XStoreColors|XAllocColorPlanes' \
	    $(abspath $(MICROPOLIS_SMOKE_OUT))/logs/micropolis-startup.log

check-smoke-micropolis-shm: $(MICROPOLIS_BIN) $(MICROPOLIS_FIXTURE_CITY)
	$(Q)rm -rf $(abspath $(MICROPOLIS_SHM_SMOKE_OUT))/home
	$(Q)mkdir -p $(abspath $(MICROPOLIS_SHM_SMOKE_OUT))/home
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name micropolis-startup-shm \
	    --app $(abspath $(MICROPOLIS_BIN)) \
	    --app-arg=$(abspath $(MICROPOLIS_FIXTURE_CITY)) \
	    --workdir $(abspath $(MICROPOLIS_WORK_DIR)) \
	    --replay tests/ui/replays/micropolis-startup.replay \
	    --out-root $(abspath $(MICROPOLIS_SHM_SMOKE_OUT)) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --env DYLD_LIBRARY_PATH=$(micropolis_ui_lib_path)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(micropolis_ui_lib_path)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env DISPLAY=:0 \
	    --env SIMHOME=$(abspath $(MICROPOLIS_WORK_DIR)) \
	    --env HOME=$(abspath $(MICROPOLIS_SHM_SMOKE_OUT))/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)test -s $(abspath $(MICROPOLIS_SHM_SMOKE_OUT))/logs/micropolis-startup-shm.log
	$(Q)rg 'too lame to support shared memory pixmaps' \
	    $(abspath $(MICROPOLIS_SHM_SMOKE_OUT))/logs/micropolis-startup-shm.log >/dev/null
	$(Q)! rg 'Hit unimplemented function|XAllocColorCells|XStoreColor|XStoreColors|XAllocColorPlanes' \
	    $(abspath $(MICROPOLIS_SHM_SMOKE_OUT))/logs/micropolis-startup-shm.log

check-smoke-micropolis-launcher: $(MICROPOLIS_BIN)
	$(Q)rm -rf $(abspath $(MICROPOLIS_LAUNCHER_SMOKE_OUT))/home
	$(Q)mkdir -p $(abspath $(MICROPOLIS_LAUNCHER_SMOKE_OUT))/home
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name micropolis-scenario-launcher \
	    --app $(abspath $(MICROPOLIS_WORK_DIR))/Micropolis \
	    --workdir $(abspath .) \
	    --replay tests/ui/replays/micropolis-scenario-launcher.replay \
	    --out-root $(abspath $(MICROPOLIS_LAUNCHER_SMOKE_OUT)) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --env DISPLAY=:0 \
	    --env HOME=$(abspath $(MICROPOLIS_LAUNCHER_SMOKE_OUT))/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)test -s $(abspath $(MICROPOLIS_LAUNCHER_SMOKE_OUT))/logs/micropolis-scenario-launcher.log

micropolis-clean:
	@echo "  CLEAN   micropolis"
	$(Q)rm -rf $(MICROPOLIS_BUILD_DIR)
