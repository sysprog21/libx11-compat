GTK1_COMMIT := 8aed827458f1eb975aaa61c22f54f3fe46e0372f
GTK1_REPO := https://gitlab.com/robinrowe/gtk1.git
GTK1_SRC_DIR := $(OUT)/upstream/gtk1
GTK1_SOURCE_STAMP := $(GTK1_SRC_DIR)/.source-stamp
GTK1_BUILD_DIR := $(OUT)/gtk1
GTK1_WORK_DIR := $(GTK1_BUILD_DIR)/source
GTK1_CMAKE_DIR := $(GTK1_BUILD_DIR)/cmake
GTK1_BUILD_STAMP := $(GTK1_BUILD_DIR)/.build-stamp
GTK1_LOG := $(abspath $(GTK1_BUILD_DIR))/build.log
GTK1_PATCHES := $(sort $(wildcard compat/gtk1-patches/*.patch))
GTK1_LIB_ALIASES := $(GTK1_BUILD_DIR)/lib-aliases
GTK1_LIB_ALIASES_STAMP := $(GTK1_LIB_ALIASES)/.stamp
GTK1_SMOKE_BIN := $(OUT)/tests/gtk1-hello
GTK1_SMOKE_SRC := tests/ui/gtk1-hello.c

CXX ?= clang++

GTK1_RPATH_FLAGS := -Wl,-rpath,$(abspath $(OUT)) -Wl,-rpath,$(abspath $(GTK1_LIB_ALIASES))
ifeq ($(UNAME_S),Linux)
  GTK1_RPATH_FLAGS += -Wl,-rpath-link,$(abspath $(OUT))
endif

$(GTK1_SOURCE_STAMP): mk/gtk1.mk
	@echo "  GIT     gtk1"
	$(Q)mkdir -p $(dir $(GTK1_SRC_DIR))
	$(Q)if [ -d $(GTK1_SRC_DIR)/.git ]; then \
	    git -C $(GTK1_SRC_DIR) fetch --depth 1 origin $(GTK1_COMMIT); \
	else \
	    rm -rf $(GTK1_SRC_DIR); \
	    git clone --depth 1 $(GTK1_REPO) $(GTK1_SRC_DIR); \
	fi
	$(Q)git -C $(GTK1_SRC_DIR) checkout --detach $(GTK1_COMMIT)
	$(Q)git -C $(GTK1_SRC_DIR) reset --hard $(GTK1_COMMIT) >/dev/null
	$(Q)git -C $(GTK1_SRC_DIR) clean -fdx >/dev/null
	$(Q)touch $@

$(GTK1_LIB_ALIASES_STAMP): $(TARGET) $(XEXT_COMPAT_TARGET) mk/gtk1.mk
	@mkdir -p $(GTK1_LIB_ALIASES)
	$(Q)rm -f $(GTK1_LIB_ALIASES)/libX11.* $(GTK1_LIB_ALIASES)/libXext.*
	$(Q)ln -sf $(abspath $(OUT))/libX11-compat.so $(GTK1_LIB_ALIASES)/libX11.so
	$(Q)ln -sf $(abspath $(OUT))/libXext-compat.so $(GTK1_LIB_ALIASES)/libXext.so
ifeq ($(UNAME_S),Darwin)
	$(Q)ln -sf $(abspath $(OUT))/libX11-compat.so $(GTK1_LIB_ALIASES)/libX11.dylib
	$(Q)ln -sf $(abspath $(OUT))/libXext-compat.so $(GTK1_LIB_ALIASES)/libXext.dylib
endif
	$(Q)touch $@

.PHONY: gtk1 gtk1-probe gtk1-clean gtk1-probe-clean check-smoke-gtk1
## Configure/build GTK1 against libx11-compat
gtk1: $(GTK1_BUILD_STAMP)

gtk1-probe: gtk1

$(GTK1_BUILD_STAMP): $(GTK1_SOURCE_STAMP) $(GTK1_PATCHES) $(GTK1_LIB_ALIASES_STAMP) \
    $(PKGCONFIG_FILES)
	@rm -rf $(GTK1_WORK_DIR) $(GTK1_CMAKE_DIR)
	@mkdir -p $(GTK1_WORK_DIR) $(GTK1_CMAKE_DIR)
	$(Q)git -C $(GTK1_SRC_DIR) reset --hard $(GTK1_COMMIT) >/dev/null
	$(Q)git -C $(GTK1_SRC_DIR) clean -fdx >/dev/null
	$(Q)tar -cf - -C $(GTK1_SRC_DIR) . | tar -xf - -C $(GTK1_WORK_DIR)
	$(Q): > $(GTK1_LOG)
	$(Q)set -e; for patch in $(abspath $(GTK1_PATCHES)); do \
	    patch -d $(GTK1_WORK_DIR) -p1 < "$$patch" >> $(GTK1_LOG) 2>&1; \
	done
	@echo "  CMAKE   gtk1"
	$(Q)cd $(GTK1_CMAKE_DIR) && \
	    PKG_CONFIG_PATH='$(abspath $(PKGCONFIG_DIR))' \
	    cmake $(abspath $(GTK1_WORK_DIR)) \
	        -DCMAKE_C_COMPILER='$(CC)' \
	        -DCMAKE_CXX_COMPILER='$(CXX)' \
	        -DCMAKE_C_FLAGS='-include stdio.h -include glib/gstrfuncs.h -I$(abspath $(GTK1_WORK_DIR)) -I$(abspath include) -I$(abspath $(OUT)/upstream/include) -DNO_SYS_ERRLIST -DNO_SYS_SIGLIST -DHAVE_ATEXIT -Wno-error=implicit-function-declaration -Wno-error=incompatible-function-pointer-types' \
	        -DCMAKE_CXX_FLAGS='-I$(abspath include) -I$(abspath $(OUT)/upstream/include)' \
	        -DCMAKE_LIBRARY_PATH='$(abspath $(GTK1_LIB_ALIASES));$(abspath $(OUT))' \
	        -DCMAKE_SHARED_LINKER_FLAGS='-L$(abspath $(GTK1_LIB_ALIASES)) -L$(abspath $(OUT)) $(GTK1_RPATH_FLAGS)' \
	        -DCMAKE_EXE_LINKER_FLAGS='-L$(abspath $(GTK1_LIB_ALIASES)) -L$(abspath $(OUT)) $(GTK1_RPATH_FLAGS)' \
	        -DBUILD_EXAMPLES=OFF -DTABLET_TEST=OFF -DGTK1_FORCE_X11_BACKEND=ON \
	        >> $(GTK1_LOG) 2>&1 || { \
	            echo "  FAIL    see $(GTK1_LOG)" >&2; tail -60 $(GTK1_LOG) >&2; exit 1; }
	@echo "  MAKE    gtk1"
	$(Q)PKG_CONFIG_PATH='$(abspath $(PKGCONFIG_DIR))' \
	    cmake --build $(GTK1_CMAKE_DIR) >> $(GTK1_LOG) 2>&1 || { \
	        echo "  FAIL    see $(GTK1_LOG)" >&2; tail -60 $(GTK1_LOG) >&2; exit 1; }
	$(Q)touch $@

gtk1-clean:
	@echo "  CLEAN   gtk1"
	$(Q)rm -rf $(GTK1_BUILD_DIR)

gtk1-probe-clean: gtk1-clean

$(GTK1_SMOKE_BIN): $(GTK1_SMOKE_SRC) $(GTK1_BUILD_STAMP)
	@echo "  CC      $<"
	$(Q)mkdir -p $(dir $@)
	$(Q)$(CC) -include stdio.h \
	    -I$(GTK1_WORK_DIR) -I$(GTK1_WORK_DIR)/gtk -I$(GTK1_WORK_DIR)/glib \
	    -I$(GTK1_WORK_DIR)/gdk -I$(GTK1_WORK_DIR)/gdk/x11 \
	    -I$(abspath include) -I$(abspath $(OUT)/upstream/include) \
	    -Wno-implicit-function-declaration -Wno-incompatible-function-pointer-types \
	    $< -L$(GTK1_CMAKE_DIR) -lgtk1 \
	    -L$(GTK1_LIB_ALIASES) -L$(OUT) \
	    -Wl,-rpath,$(abspath $(GTK1_CMAKE_DIR)) $(GTK1_RPATH_FLAGS) \
	    -o $@

check-smoke-gtk1: $(UI_SMOKE_OUT_ROOT)/gtk1-smoke/.stamp

$(UI_SMOKE_OUT_ROOT)/gtk1-smoke/.stamp: FORCE $(GTK1_SMOKE_BIN)
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name gtk1-smoke \
	    --app $(abspath $(GTK1_SMOKE_BIN)) \
	    --workdir $(abspath .) \
	    --replay tests/ui/replays/gtk1-smoke.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/gtk1-smoke \
	    --display $(UI_REPLAY_DISPLAY) \
	    --geometry $(UI_REPLAY_GEOMETRY) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/gtk1-smoke/render-stats.tsv \
	    $(UI_REPLAY_XVFB) \
	    --env DYLD_LIBRARY_PATH=$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@
