CINEPAINT_URL := https://gitlab.com/heroic-robots/cinepaint
CINEPAINT_REVISION := 5679f4280614c42c9469a6f642d5367803394aa1
CINEPAINT_CLONE_FLAGS ?= --filter=blob:none --no-checkout
CINEPAINT_SRC_DIR := $(OUT)/upstream/cinepaint
CINEPAINT_SOURCE_STAMP := $(CINEPAINT_SRC_DIR)/.source-stamp
CINEPAINT_PATCHES := $(sort $(wildcard compat/cinepaint-patches/*.patch))

CINEPAINT_BUILD_DIR := $(OUT)/cinepaint
CINEPAINT_WORK_DIR := $(CINEPAINT_BUILD_DIR)/source
CINEPAINT_CMAKE_DIR := $(CINEPAINT_BUILD_DIR)/cmake
CINEPAINT_BUILD_STAMP := $(CINEPAINT_BUILD_DIR)/.build-stamp
CINEPAINT_LOG := $(abspath $(CINEPAINT_BUILD_DIR))/build.log
CINEPAINT_BIN := $(CINEPAINT_CMAKE_DIR)/cinepaint
CINEPAINT_GTK1_LIB := $(GTK1_CMAKE_DIR)/libgtk1.so
CINEPAINT_PLUGIN_LIB_EXT := so
ifeq ($(UNAME_S),Darwin)
  CINEPAINT_GTK1_LIB := $(GTK1_CMAKE_DIR)/libgtk1.dylib
  CINEPAINT_PLUGIN_LIB_EXT := dylib
endif

CINEPAINT_RPATH_FLAGS := -Wl,-rpath,$(abspath $(GTK1_CMAKE_DIR)) \
    -Wl,-rpath,$(abspath $(GTK1_LIB_ALIASES)) -Wl,-rpath,$(abspath $(OUT))
CINEPAINT_PLATFORM_CFLAGS :=
CINEPAINT_PLATFORM_LDFLAGS :=
ifeq ($(UNAME_S),Linux)
  CINEPAINT_RPATH_FLAGS += -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  CINEPAINT_PLATFORM_CFLAGS += -I/opt/homebrew/opt/gettext/include
  CINEPAINT_PLATFORM_LDFLAGS += -L/opt/homebrew/opt/gettext/lib \
      -Wl,-rpath,/opt/homebrew/opt/gettext/lib -lintl
  # Homebrew ships libpng keg-only on the CI runners, so its headers live
  # under $(brew --prefix libpng)/include rather than the main include dir.
  # The png plug-in includes <pnglibconf.h> at top level, so put the keg
  # include/lib dirs on the search path (harmless when libpng is already
  # linked into the main prefix, as on a dev box).
  CINEPAINT_LIBPNG_PREFIX := $(shell brew --prefix libpng 2>/dev/null)
  ifneq ($(CINEPAINT_LIBPNG_PREFIX),)
    CINEPAINT_PLATFORM_CFLAGS += -I$(CINEPAINT_LIBPNG_PREFIX)/include
    CINEPAINT_PLATFORM_LDFLAGS += -L$(CINEPAINT_LIBPNG_PREFIX)/lib
  endif
endif

$(CINEPAINT_SOURCE_STAMP): mk/cinepaint.mk
	@echo "  GIT     $(CINEPAINT_URL)"
	$(Q)mkdir -p $(dir $(CINEPAINT_SRC_DIR))
	$(Q)test -d $(CINEPAINT_SRC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(CINEPAINT_CLONE_FLAGS) $(CINEPAINT_URL) $(CINEPAINT_SRC_DIR)
	$(Q)cd $(CINEPAINT_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin $(CINEPAINT_REVISION) || \
	    (cd $(CINEPAINT_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin)
	$(Q)cd $(CINEPAINT_SRC_DIR) && \
	    git checkout $(MOTIF_GIT_Q) --detach $(CINEPAINT_REVISION) && \
	    git reset --hard $(MOTIF_GIT_Q) $(CINEPAINT_REVISION) >/dev/null && \
	    git clean $(MOTIF_GIT_Q) -fdx >/dev/null
	$(Q)printf '%s\n' '$(CINEPAINT_REVISION)' > $@

.PHONY: cinepaint cinepaint-clean
## Build CinePaint Hollywood against GTK1 and libx11-compat
cinepaint: $(CINEPAINT_BUILD_STAMP)

$(CINEPAINT_BUILD_STAMP): $(CINEPAINT_SOURCE_STAMP) $(CINEPAINT_PATCHES) $(GTK1_BUILD_STAMP)
	@mkdir -p $(CINEPAINT_BUILD_DIR)
	$(Q)rm -rf $(CINEPAINT_WORK_DIR) $(CINEPAINT_CMAKE_DIR)
	$(Q)mkdir -p $(CINEPAINT_WORK_DIR) $(CINEPAINT_CMAKE_DIR)
	$(Q)git -C $(CINEPAINT_SRC_DIR) reset --hard $(CINEPAINT_REVISION) >/dev/null
	$(Q)git -C $(CINEPAINT_SRC_DIR) clean -fdx >/dev/null
	$(Q)git -C $(CINEPAINT_SRC_DIR) archive $(CINEPAINT_REVISION) | \
	    tar -xf - -C $(CINEPAINT_WORK_DIR)
	$(Q): > $(CINEPAINT_LOG)
	$(Q)set -e; for patch in $(abspath $(CINEPAINT_PATCHES)); do \
	    if patch --dry-run --fuzz=0 --no-backup-if-mismatch \
	        -d $(CINEPAINT_WORK_DIR) -p1 < "$$patch" >> $(CINEPAINT_LOG) 2>&1; then \
	        patch --fuzz=0 --no-backup-if-mismatch -d $(CINEPAINT_WORK_DIR) -p1 < "$$patch" \
	            >> $(CINEPAINT_LOG) 2>&1; \
	    elif patch --dry-run --reverse --fuzz=0 --no-backup-if-mismatch \
	        -d $(CINEPAINT_WORK_DIR) -p1 < "$$patch" >> $(CINEPAINT_LOG) 2>&1; then \
	        printf 'patch already applied: %s\n' "$$patch" >> $(CINEPAINT_LOG); \
	    else \
	        exit 1; \
	    fi; \
	done
	$(Q)perl -0pi -e 's/if \(png_get_valid \(pp, info, PNG_INFO_tRNS\) &&\n(\s*)png_get_color_type/if (0 \&\& png_get_valid (pp, info, PNG_INFO_tRNS) &&\n$$1png_get_color_type/' \
	    $(CINEPAINT_WORK_DIR)/hollywood/plugin/png/png.c
	$(Q)perl -0pi -e 's/#include <sys\/types\.h>\n#endif/#include <sys\/types.h>\n#include <spawn.h>\n#endif/' \
	    $(CINEPAINT_WORK_DIR)/hollywood/app/notes_dialog.c
	$(Q)perl -0pi -e 's/#include "notes_dialog\.h"\n(?!\n#ifndef _WIN32\nextern char \*\*environ;\n#endif)/#include "notes_dialog.h"\n\n#ifndef _WIN32\nextern char **environ;\n#endif\n/' \
	    $(CINEPAINT_WORK_DIR)/hollywood/app/notes_dialog.c
	$(Q)perl -0pi -e 's/#define NOTES_PROD_FILENAME "cinenotes"/#define NOTES_PROD_FILENAME "cinenotes1"/; s/#define NOTES_BUGS_FILENAME "data\/BUGS\.md"/#define NOTES_BUGS_FILENAME "BUGS.md"/; s/#define NOTES_CHANGES_FILENAME "data\/CHANGES\.md"/#define NOTES_CHANGES_FILENAME "ChangeLog.md"/; s/#define NOTES_ROADMAP_FILENAME "data\/ROADMAP\.md"/#define NOTES_ROADMAP_FILENAME "ROADMAP.md"/' \
	    $(CINEPAINT_WORK_DIR)/hollywood/app/notes_dialog.c
	$(Q)perl -0pi -e 's/if\('"'"'\/'"'"' != \*p\)/if(len1 \&\& '"'"'\/'"'"' != path[len1 - 1])/' \
	    $(CINEPAINT_WORK_DIR)/hollywood/app/notes_dialog.c
	$(Q)perl -0pi -e 's/char\* args = strdup2path\(docPath, docName,TRUE\);/char* args = strdup2path(docPath, docName,\n#ifdef _WIN32\n        TRUE\n#else\n        FALSE\n#endif\n    );/' \
	    $(CINEPAINT_WORK_DIR)/hollywood/app/notes_dialog.c
	$(Q)perl -0pi -e 's/    pid_t p = fork\(\);\n    if\(p==-1\)\n    \{\treturn;\n    \}\n    if\(p!=0\)\n    \{\treturn;\n    \}\n    execv\(appName,\&args\);/    char* argv[] = {(char*)appName, args, NULL};\n    pid_t p;\n    int err = posix_spawn(\&p, appName, NULL, NULL, argv, environ);\n    if (err)\n    {   printf("ERROR: Cannot launch %s %s: %s\\n", appName, args, strerror(err));\n    }/' \
	    $(CINEPAINT_WORK_DIR)/hollywood/app/notes_dialog.c
	@echo "  CMAKE   cinepaint"
	$(Q)cd $(CINEPAINT_CMAKE_DIR) && \
	    cmake $(abspath $(CINEPAINT_WORK_DIR))/hollywood \
	        -DCMAKE_C_COMPILER='$(CC)' \
	        -DCMAKE_CXX_COMPILER='$(CXX)' \
	        -DCMAKE_C_FLAGS='-include stdio.h -include assert.h -DRETSIGTYPE=void -DLITTLE_ENDIAN -DG_LITTLE_ENDIAN=1234 -DG_BIG_ENDIAN=4321 -I$(abspath include) -I$(abspath $(OUT)/upstream/include) $(CINEPAINT_PLATFORM_CFLAGS) -fcommon -Wno-error=implicit-function-declaration -Wno-error=incompatible-function-pointer-types' \
	        -DCMAKE_CXX_FLAGS='-I$(abspath include) -I$(abspath $(OUT)/upstream/include)' \
	        -DCMAKE_LIBRARY_PATH='$(abspath $(GTK1_CMAKE_DIR));$(abspath $(GTK1_LIB_ALIASES));$(abspath $(OUT))' \
	        -DCMAKE_EXE_LINKER_FLAGS='-L$(abspath $(GTK1_CMAKE_DIR)) -L$(abspath $(GTK1_LIB_ALIASES)) -L$(abspath $(OUT)) $(CINEPAINT_RPATH_FLAGS) $(CINEPAINT_PLATFORM_LDFLAGS)' \
	        -DCMAKE_SHARED_LINKER_FLAGS='-L$(abspath $(GTK1_CMAKE_DIR)) -L$(abspath $(GTK1_LIB_ALIASES)) -L$(abspath $(OUT)) $(CINEPAINT_RPATH_FLAGS) $(CINEPAINT_PLATFORM_LDFLAGS)' \
	        -DCINEPAINT_EXTERNAL_GTK1=ON \
	        -DCINEPAINT_FORCE_X11_BACKEND=ON \
	        -DCINEPAINT_GTK1_SOURCE_DIR='$(abspath $(GTK1_WORK_DIR))' \
	        -DCINEPAINT_GTK1_LIBRARY='$(abspath $(CINEPAINT_GTK1_LIB))' \
	        -DCINEPAINT_BUILD_PLUGINS=ON \
	        -DCINEPAINT_PLUGIN_DIRS='jpeg;png' \
	        >> $(CINEPAINT_LOG) 2>&1 || { \
	            echo "  FAIL    see $(CINEPAINT_LOG)" >&2; tail -60 $(CINEPAINT_LOG) >&2; exit 1; }
	@echo "  MAKE    cinepaint"
	$(Q)cmake --build $(CINEPAINT_CMAKE_DIR) --target cinepaint cinenotes1 plugin-jpeg plugin-png >> $(CINEPAINT_LOG) 2>&1 || { \
	    echo "  FAIL    see $(CINEPAINT_LOG)" >&2; tail -60 $(CINEPAINT_LOG) >&2; exit 1; }
	$(Q)ln -sfn ../source/hollywood/data $(CINEPAINT_CMAKE_DIR)/data
	$(Q)ln -sfn ../source/hollywood/README.md $(CINEPAINT_CMAKE_DIR)/README.md
	$(Q)ln -sfn README.md $(CINEPAINT_CMAKE_DIR)/notes.md
	$(Q)ln -sfn ../source/hollywood/BUGS.md $(CINEPAINT_CMAKE_DIR)/BUGS.md
	$(Q)ln -sfn ../source/hollywood/ChangeLog.md $(CINEPAINT_CMAKE_DIR)/ChangeLog.md
	$(Q)ln -sfn ../source/hollywood/ROADMAP.md $(CINEPAINT_CMAKE_DIR)/ROADMAP.md
	$(Q)ln -sfn libplugin-jpeg.$(CINEPAINT_PLUGIN_LIB_EXT) $(CINEPAINT_CMAKE_DIR)/plugin-jpeg.dll
	$(Q)ln -sfn libplugin-png.$(CINEPAINT_PLUGIN_LIB_EXT) $(CINEPAINT_CMAKE_DIR)/plugin-png.dll
	$(Q)touch $@

cinepaint-clean:
	@echo "  CLEAN   cinepaint"
	$(Q)rm -rf $(CINEPAINT_BUILD_DIR)

.PHONY: check-smoke-cinepaint-startup check-smoke-cinepaint-canvas \
    check-smoke-cinepaint-first-run-ignore \
    check-smoke-cinepaint-about \
    check-smoke-cinepaint-palette-png \
    check-smoke-cinepaint-file-menu \
    check-cinepaint-no-unexpected-stubs check-cinepaint-no-host-x11
check-smoke-cinepaint-startup: $(UI_SMOKE_OUT_ROOT)/cinepaint-startup/.stamp
check-smoke-cinepaint-canvas: $(UI_SMOKE_OUT_ROOT)/cinepaint-canvas/.stamp
check-smoke-cinepaint-first-run-ignore: $(UI_SMOKE_OUT_ROOT)/cinepaint-first-run-ignore/.stamp
check-smoke-cinepaint-about: $(UI_SMOKE_OUT_ROOT)/cinepaint-about/.stamp
check-smoke-cinepaint-palette-png: $(UI_SMOKE_OUT_ROOT)/cinepaint-palette-png/.stamp
check-smoke-cinepaint-file-menu: $(UI_SMOKE_OUT_ROOT)/cinepaint-file-menu/.stamp

$(UI_SMOKE_OUT_ROOT)/cinepaint-startup/.stamp: FORCE $(CINEPAINT_BUILD_STAMP)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home
	$(Q)mkdir -p \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/brushes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/gradients \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/palettes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/patterns \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/plug-ins \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/gfig \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/tmp \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/scripts \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/gflares
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/data/cinepaintrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/cinepaintrc
	$(Q)printf '%s\n' '(dont-show-tips)' >> $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/cinepaintrc
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/docs/gtkrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home/.cinepaint/gtkrc
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name cinepaint-startup \
	    --app $(abspath $(CINEPAINT_BIN)) \
	    --app-arg=--no-splash \
	    --workdir $(abspath $(CINEPAINT_WORK_DIR))/hollywood \
	    --replay tests/ui/replays/cinepaint-startup.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/render-stats.tsv \
	    --env DYLD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/cinepaint-file-menu/.stamp: FORCE $(CINEPAINT_BUILD_STAMP)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home
	$(Q)mkdir -p \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/brushes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/gradients \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/palettes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/patterns \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/plug-ins \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/gfig \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/tmp \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/scripts \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/gflares
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/data/cinepaintrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/cinepaintrc
	$(Q)printf '%s\n' '(dont-show-tips)' >> $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/cinepaintrc
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/docs/gtkrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home/.cinepaint/gtkrc
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name cinepaint-file-menu \
	    --app $(abspath $(CINEPAINT_BIN)) \
	    --app-arg=--no-splash \
	    --workdir $(abspath $(CINEPAINT_WORK_DIR))/hollywood \
	    --replay tests/ui/replays/cinepaint-file-menu.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --env DYLD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@

check-cinepaint-no-unexpected-stubs: check-smoke-gtk1 check-smoke-cinepaint-startup \
	    check-smoke-cinepaint-canvas check-smoke-cinepaint-first-run-ignore \
	    check-smoke-cinepaint-about check-smoke-cinepaint-palette-png \
	    check-smoke-cinepaint-file-menu
	$(Q)! rg -n "WARN_UNIMPLEMENTED" \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/gtk1-smoke/logs \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-startup/logs \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/logs \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-first-run-ignore/logs \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/logs \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/logs \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-file-menu/logs

check-cinepaint-no-host-x11: $(GTK1_SMOKE_BIN) $(CINEPAINT_BUILD_STAMP)
	$(Q)if [ "$(UNAME_S)" = "Darwin" ]; then \
	    deps="$$(otool -L $(GTK1_SMOKE_BIN) $(CINEPAINT_BIN) \
	        $(CINEPAINT_CMAKE_DIR)/libplugin-png.$(CINEPAINT_PLUGIN_LIB_EXT) \
	        $(CINEPAINT_CMAKE_DIR)/libplugin-jpeg.$(CINEPAINT_PLUGIN_LIB_EXT) \
	        $(GTK1_CMAKE_DIR)/libgtk1.$(CINEPAINT_PLUGIN_LIB_EXT))"; \
	else \
	    deps="$$(ldd $(GTK1_SMOKE_BIN) $(CINEPAINT_BIN) \
	        $(CINEPAINT_CMAKE_DIR)/libplugin-png.$(CINEPAINT_PLUGIN_LIB_EXT) \
	        $(CINEPAINT_CMAKE_DIR)/libplugin-jpeg.$(CINEPAINT_PLUGIN_LIB_EXT) \
	        $(GTK1_CMAKE_DIR)/libgtk1.$(CINEPAINT_PLUGIN_LIB_EXT))"; \
	fi; \
	bad="$$(printf '%s\n' "$$deps" | rg 'libX(11|ext|render|shm)[.](so|dylib)' | \
	    rg -v 'libX11-compat|libXext-compat|$(abspath $(GTK1_LIB_ALIASES))/libX(11|ext)[.](so|dylib)' || true)"; \
	if [ -n "$$bad" ]; then \
	    printf '%s\n' "$$bad" >&2; \
	    exit 1; \
	fi

$(UI_SMOKE_OUT_ROOT)/cinepaint-canvas/.stamp: FORCE $(CINEPAINT_BUILD_STAMP)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home
	$(Q)mkdir -p \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/brushes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/gradients \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/palettes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/patterns \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/plug-ins \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/gfig \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/tmp \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/scripts \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/gflares
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/data/cinepaintrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/cinepaintrc
	$(Q)printf '%s\n' '(dont-show-tips)' >> $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/cinepaintrc
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/docs/gtkrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home/.cinepaint/gtkrc
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name cinepaint-canvas \
	    --app $(abspath $(CINEPAINT_BIN)) \
	    --app-arg=--no-splash \
	    --app-arg $(abspath tests/ui/fixtures/gimp-canvas-16x16.png) \
	    --workdir $(abspath $(CINEPAINT_WORK_DIR))/hollywood \
	    --replay tests/ui/replays/cinepaint-canvas.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/render-stats.tsv \
	    --env DYLD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-canvas/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/cinepaint-palette-png/.stamp: FORCE $(CINEPAINT_BUILD_STAMP)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home
	$(Q)mkdir -p \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/brushes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/gradients \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/palettes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/patterns \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/plug-ins \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/gfig \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/tmp \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/scripts \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/gflares
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/data/cinepaintrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/cinepaintrc
	$(Q)printf '%s\n' '(dont-show-tips)' >> $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/cinepaintrc
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/docs/gtkrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home/.cinepaint/gtkrc
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name cinepaint-palette-png \
	    --app $(abspath $(CINEPAINT_BIN)) \
	    --app-arg=--no-splash \
	    --app-arg $(abspath tests/ui/fixtures/cinepaint-palette-550x335.png) \
	    --workdir $(abspath $(CINEPAINT_WORK_DIR))/hollywood \
	    --replay tests/ui/replays/cinepaint-palette-png.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/render-stats.tsv \
	    --env DYLD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-palette-png/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/cinepaint-about/.stamp: FORCE $(CINEPAINT_BUILD_STAMP)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home
	$(Q)mkdir -p \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/brushes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/gradients \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/palettes \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/patterns \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/plug-ins \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/gfig \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/tmp \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/scripts \
	    $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/gflares
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/data/cinepaintrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/cinepaintrc
	$(Q)printf '%s\n' '(dont-show-tips)' >> $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/cinepaintrc
	$(Q)cp $(CINEPAINT_WORK_DIR)/hollywood/docs/gtkrc $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home/.cinepaint/gtkrc
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name cinepaint-about \
	    --app $(abspath $(CINEPAINT_BIN)) \
	    --workdir $(abspath $(CINEPAINT_WORK_DIR))/hollywood \
	    --replay tests/ui/replays/cinepaint-about.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/render-stats.tsv \
	    --env DYLD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-about/home \
	    --env CINEPAINT_TEST_OPEN_ABOUT=1 \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@

$(UI_SMOKE_OUT_ROOT)/cinepaint-first-run-ignore/.stamp: FORCE $(CINEPAINT_BUILD_STAMP)
	$(Q)rm -rf $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-first-run-ignore/home
	$(Q)mkdir -p $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-first-run-ignore/home
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name cinepaint-first-run-ignore \
	    --app $(abspath $(CINEPAINT_BIN)) \
	    --workdir $(abspath $(CINEPAINT_WORK_DIR))/hollywood \
	    --replay tests/ui/replays/cinepaint-first-run-ignore.replay \
	    --out-root $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-first-run-ignore \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --render-stats $(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-first-run-ignore/render-stats.tsv \
	    --env DYLD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(abspath $(CINEPAINT_CMAKE_DIR)):$(abspath $(GTK1_CMAKE_DIR)):$(abspath $(GTK1_LIB_ALIASES)):$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env HOME=$(abspath $(UI_SMOKE_OUT_ROOT))/cinepaint-first-run-ignore/home \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@
