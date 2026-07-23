# Build Magic VLSI layout against the repo-private Tcl/TkX11 (mk/tcltk.mk)
# and the libx11-compat stack.
#
# Magic's own scripts/configure honors --with-tcl / --with-tk (pointing at the
# dirs that hold tclConfig.sh / tkConfig.sh) plus AC_PATH_X's --x-includes /
# --x-libraries. We reuse the header sysroot and soname alias farms already
# staged for the private Tcl/Tk so Magic's X detection resolves to the compat
# shims, never /opt/X11 or Homebrew. The Tcl/Tk GUI (tclmagic.so + the wish
# launcher) is the target, not a non-Tk batch binary.
#
# Cairo, OpenGL, readline, and compression are disabled in this first pass: they
# pull host libraries or block the initial GUI build. The X11 graphics driver is
# the one that exercises TkX11 through libx11-compat.

MAGIC_URL := https://github.com/RTimothyEdwards/magic
MAGIC_REVISION := 17ac06a24a952380ade3a7d33cd2f0c3943dfc12
MAGIC_SRC_DIR := $(OUT)/upstream/magic-src
MAGIC_SOURCE_STAMP := $(MAGIC_SRC_DIR)/.source-stamp
MAGIC_PATCHES := $(sort $(wildcard compat/magic-patches/*.patch))

MAGIC_BUILD_DIR := $(OUT)/magic
MAGIC_WORK_DIR := $(MAGIC_BUILD_DIR)/source
MAGIC_PREFIX := $(MAGIC_BUILD_DIR)/install
MAGIC_LOG := $(abspath $(MAGIC_BUILD_DIR))/build.log
MAGIC_BIN := $(MAGIC_PREFIX)/bin/magic
MAGIC_BUILD_STAMP := $(MAGIC_BUILD_DIR)/.build-stamp

# Reuse the Tcl/Tk sysroot + alias farms from mk/tcltk.mk (included first).
MAGIC_INCPATH := $(abspath $(TCLTK_SYSROOT))
MAGIC_LIBPATH := $(abspath $(TCLTK_LIB_ALIASES))

MAGIC_RPATH_FLAGS := -Wl,-rpath,$(abspath $(OUT)) \
                     -Wl,-rpath,$(abspath $(TCLTK_LIB_ALIASES)) \
                     -Wl,-rpath,$(abspath $(TCLTK_LIBDIR))
ifeq ($(UNAME_S),Linux)
  MAGIC_RPATH_FLAGS += -Wl,-rpath-link,$(abspath $(OUT))
endif

# Magic's configure does not persist LDFLAGS into defs.mak, and its module link
# (LDDL_FLAGS = $(LDFLAGS) -dynamiclib ...) expands $(LDFLAGS) at build time. So
# the rpath has to be supplied to the make step too, not just to configure, or
# tclmagic.so links -lX11 (compat) with no LC_RPATH and dyld cannot resolve
# @rpath/libX11-compat.so at load. Pass the same LDFLAGS to both.
MAGIC_LDFLAGS := -L$(MAGIC_LIBPATH) -L$(abspath $(OUT)) $(MAGIC_RPATH_FLAGS)

# Magic 8.3.x still uses a few pre-C99 idioms modern clang rejects as errors;
# drop those to warnings so configure conftests and the build proceed.
MAGIC_LEGACY_CFLAGS := -Wno-implicit-function-declaration -Wno-implicit-int \
    -Wno-int-conversion

# OpenGL 3D display: MAGIC_OPENGL=1 links Magic's OpenGL graphics
# against the in-tree gl4es GL provider (build/libGL), the vendored Mesa GLU
# tessellator (build/libGLU), and libx11-compat for glX*. Desktop gl* run
# through gl4es -> GLESv2 on Darwin. Linux may use system OpenGL, but only
# after an explicit header+link probe so configure never silently falls through
# to whatever host GL happens to be present.
MAGIC_OPENGL ?= auto
ANGLE_ROOT ?= third_party/angle
MAGIC_ANGLE_AVAILABLE := $(if $(wildcard $(ANGLE_ROOT)/src/libGLESv2.gni),1,0)
ifeq ($(MAGIC_OPENGL),auto)
  ifeq ($(UNAME_S),Darwin)
    MAGIC_OPENGL_RESOLVED := $(MAGIC_ANGLE_AVAILABLE)
  else ifeq ($(UNAME_S),Linux)
    MAGIC_OPENGL_RESOLVED := $(shell printf '%s\n' '\#include <GL/gl.h>' \
        '\#include <GL/glu.h>' | \
        $(CC) -shared -x c - -lGL -lGLU -o /dev/null >/dev/null 2>&1 && \
        printf 1 || printf 0)
  else
    MAGIC_OPENGL_RESOLVED := 0
  endif
else
  MAGIC_OPENGL_RESOLVED := $(MAGIC_OPENGL)
endif
MAGIC_GL_DEPS :=
ifeq ($(MAGIC_OPENGL_RESOLVED),1)
  MAGIC_OGL_FLAG := --with-opengl
  ifeq ($(UNAME_S),Darwin)
    ifneq ($(MAGIC_ANGLE_AVAILABLE),1)
      $(error MAGIC_OPENGL=1 requested, but ANGLE sources are missing; run make fetch-angle build-angle or use MAGIC_OPENGL=0)
    endif
    MAGIC_GL_CFLAGS := -I$(abspath third_party/gl-headers) \
        -I$(abspath third_party/glu/include) -I$(abspath include)
    MAGIC_GL_DEPS := $(OUT)/libGL.dylib $(OUT)/libGLU.dylib \
        $(OUT)/plugins/macOS/libEGL.dylib \
        $(OUT)/plugins/macOS/libGLESv2.dylib
  else ifeq ($(UNAME_S),Linux)
    MAGIC_GL_AVAILABLE := $(shell printf '%s\n' '\#include <GL/gl.h>' \
        '\#include <GL/glu.h>' | \
        $(CC) -shared -x c - -lGL -lGLU -o /dev/null >/dev/null 2>&1 && \
        printf 1 || printf 0)
    ifneq ($(MAGIC_GL_AVAILABLE),1)
      $(error MAGIC_OPENGL=1 requested, but Linux OpenGL headers/libs were not detected)
    endif
    MAGIC_GL_CFLAGS :=
  else
    $(error MAGIC_OPENGL=1 is unsupported on $(UNAME_S))
  endif
else
  MAGIC_OGL_FLAG := --without-opengl
  MAGIC_GL_CFLAGS :=
endif

MAGIC_CONFIGURE_FLAGS := \
    --with-tcl=$(abspath $(TCLTK_LIBDIR)) \
    --with-tk=$(abspath $(TCLTK_LIBDIR)) \
    --with-interpreter=tcl \
    --with-x \
    --x-includes=$(MAGIC_INCPATH) \
    --x-libraries=$(MAGIC_LIBPATH) \
    --without-cairo \
    $(MAGIC_OGL_FLAG) \
    --disable-readline \
    --disable-compression

$(MAGIC_SOURCE_STAMP): mk/magic.mk
	@echo "  GIT     $(MAGIC_URL)"
	$(Q)mkdir -p $(dir $(MAGIC_SRC_DIR))
	$(Q)test -d $(MAGIC_SRC_DIR)/.git || \
	    git clone $(MAGIC_URL) $(MAGIC_SRC_DIR)
	$(Q)cd $(MAGIC_SRC_DIR) && git fetch -q origin $(MAGIC_REVISION) || \
	    (cd $(MAGIC_SRC_DIR) && git fetch -q origin)
	$(Q)cd $(MAGIC_SRC_DIR) && \
	    git checkout -q --detach $(MAGIC_REVISION) && \
	    git reset --hard -q $(MAGIC_REVISION) && \
	    git clean -fdxq
	$(Q)printf '%s\n' '$(MAGIC_REVISION)' > $@

.PHONY: magic check-magic-ogl-wrapper \
    check-magic-no-unexpected-stubs check-smoke-magic magic-clean
## Build Magic against the private Tcl/TkX11 and libx11-compat
magic: $(MAGIC_BUILD_STAMP)

## Verify the default TkCon wrapper can start the OpenGL display.
check-magic-ogl-wrapper: $(MAGIC_BUILD_STAMP) $(MAGIC_GL_DEPS)
	$(Q)$(PYTHON) scripts/check-magic-ogl-wrapper.py \
	    --magic $(abspath $(MAGIC_BIN)) \
	    --cad-root $(abspath $(MAGIC_PREFIX))/lib \
	    --out $(abspath $(OUT)) \
	    --tcltk-libdir $(abspath $(TCLTK_LIBDIR))

MAGIC_SMOKE_OUT := $(UI_SMOKE_OUT_ROOT)/magic-startup
# The alias farm must be on the loader path: libtk carries a NEEDED libX11.so
# (unversioned soname) that lives only there as a symlink to libX11-compat.so.
# Without it Tk resolves a real libX11 and Tk_Init dies with "couldn't connect
# to display", so Magic never maps its layout window on a headless host.
magic_smoke_lib_path = $(abspath $(OUT)):$(abspath $(TCLTK_LIBDIR)):$(abspath $(TCLTK_LIB_ALIASES))$(if $(SDL_RUNTIME_LIBDIR),:$(SDL_RUNTIME_LIBDIR))
MAGIC_SMOKE_DEPS := $(MAGIC_BUILD_STAMP)
ifeq ($(MAGIC_OPENGL_RESOLVED),1)
  MAGIC_SMOKE_DEPS += check-magic-ogl-wrapper
endif

## Run a replay-based Magic layout startup smoke against libx11-compat.
## Kept as a standalone target rather than folded into the aggregate check-smoke:
## the smoke builds a private Tcl/Tk plus Magic, which is far heavier and more
## network-dependent than the Motif + ViolaWWW gate check-smoke is meant to stay.
## CI runs it in its own job, the same split mosaic uses.
check-smoke-magic: $(MAGIC_SMOKE_OUT)/.stamp

## Gate the Magic replay: no unexpected unimplemented Xlib calls, and no host
## X11 / Aqua Tk linked into the Magic or private Tcl/Tk stack. The replay smoke
## already proved Magic starts, maps, and paints a real x11 layout window, so
## the audit is a static link check over the built artifacts, not a relaunch.
check-magic-no-unexpected-stubs: check-smoke-magic
	$(Q)$(PYTHON) scripts/check-magic.py \
	    --scan-log $(abspath $(MAGIC_SMOKE_OUT))/logs/magic-startup.log
	$(Q)$(PYTHON) scripts/check-magic.py --audit \
	    --cad-root $(abspath $(MAGIC_PREFIX))/lib \
	    --out $(abspath $(OUT)) \
	    --tcltk-prefix $(abspath $(TCLTK_PREFIX))

$(MAGIC_SMOKE_OUT)/.stamp: FORCE $(MAGIC_SMOKE_DEPS)
	$(Q)rm -rf $(abspath $(MAGIC_SMOKE_OUT))/work $(abspath $(MAGIC_SMOKE_OUT))/home
	$(Q)mkdir -p $(abspath $(MAGIC_SMOKE_OUT))/work $(abspath $(MAGIC_SMOKE_OUT))/home
	$(Q)cp tests/ui/fixtures/magic/minimal.mag $(abspath $(MAGIC_SMOKE_OUT))/work/
	$(Q)$(PYTHON) scripts/run-ui-replay.py \
	    --name magic-startup \
	    --app $(abspath $(MAGIC_BIN)) \
	    --app-arg=-noconsole --app-arg=-nowrapper \
	    --app-arg=-rcfile \
	    --app-arg $(abspath tests/ui/fixtures/magic/smoke-setup.tcl) \
	    --workdir $(abspath $(MAGIC_SMOKE_OUT))/work \
	    --replay tests/ui/replays/magic-startup.replay \
	    --out-root $(abspath $(MAGIC_SMOKE_OUT)) \
	    --screenshot-command $(UI_REPLAY_SCREENSHOT_COMMAND) \
	    --in-process-snapshots \
	    --offscreen \
	    --env DYLD_LIBRARY_PATH=$(magic_smoke_lib_path)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
	    --env LD_LIBRARY_PATH=$(magic_smoke_lib_path)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH} \
	    --env CAD_ROOT=$(abspath $(MAGIC_PREFIX))/lib \
	    --env HOME=$(abspath $(MAGIC_SMOKE_OUT))/home \
	    --env DISPLAY=:0 \
	    --env LIBX11_COMPAT_WARN_UNIMPLEMENTED=1
	$(Q)touch $@

$(MAGIC_BUILD_STAMP): $(MAGIC_SOURCE_STAMP) $(MAGIC_PATCHES) \
    $(TCLTK_TK_BUILD_STAMP) $(MAGIC_GL_DEPS) mk/magic.mk
	@mkdir -p $(MAGIC_BUILD_DIR)
	$(Q)rm -f $(MAGIC_LOG)
	$(Q)rm -rf $(MAGIC_WORK_DIR)
	$(Q)mkdir -p $(MAGIC_WORK_DIR)
	$(Q)tar --exclude .git -cf - -C $(MAGIC_SRC_DIR) . | tar -xf - -C $(MAGIC_WORK_DIR)
	$(Q)set -e; for patch in $(MAGIC_PATCHES); do \
	    patch -d $(MAGIC_WORK_DIR) -p1 < "$$patch"; \
	done
	@echo "  CONF    magic"
	$(Q)cd $(MAGIC_WORK_DIR) && \
	    CC='$(CC)' \
	    CFLAGS='-g $(MAGIC_LEGACY_CFLAGS) $(MAGIC_GL_CFLAGS)' \
	    LDFLAGS='$(MAGIC_LDFLAGS)' \
	    ./configure --prefix=$(abspath $(MAGIC_PREFIX)) \
	        $(MAGIC_CONFIGURE_FLAGS) \
	        >> $(MAGIC_LOG) 2>&1 || { \
	        echo "  FAIL    see $(MAGIC_LOG)" >&2; tail -50 $(MAGIC_LOG) >&2; exit 1; }
	@echo "  MAKE    magic"
	$(Q)env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(MAGIC_WORK_DIR) \
	    LDFLAGS='$(MAGIC_LDFLAGS)' \
	    >> $(MAGIC_LOG) 2>&1 || { \
	    echo "  FAIL    see $(MAGIC_LOG)" >&2; tail -50 $(MAGIC_LOG) >&2; exit 1; }
	$(Q)env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(MAGIC_WORK_DIR) install \
	    LDFLAGS='$(MAGIC_LDFLAGS)' \
	    >> $(MAGIC_LOG) 2>&1 || { \
	    echo "  FAIL    see $(MAGIC_LOG)" >&2; tail -50 $(MAGIC_LOG) >&2; exit 1; }
	$(Q)touch $@

# build-ogl-userlibs.sh compiles the vendored Mesa GLU tessellator
# (third_party/glu/src/libtess) against the desktop-GL headers under
# third_party/gl-headers. Neither tree is vendored: third_party/ is git-ignored
# and wiped by clean, so a clean checkout must refetch both before the script
# can start. gl.h reuses the mesa-demos header cache (its fetch rule lives in
# mk/mesa-demos.mk); glu's source and its own glu.h are fetched from Mesa at the
# pin in scripts/glu-pin.txt. The gl.h prerequisite is spelled as a literal path
# rather than $(GL_HDR_FILES): that variable is defined in mesa-demos.mk, which
# is included after this file, so it would expand empty here. The rule that
# builds it is resolved at build time, so the literal path still finds it.
MAGIC_GL_HDR := third_party/gl-headers/GL/gl.h
MAGIC_GLU_SRC := third_party/glu/include/GL/glu.h
$(MAGIC_GLU_SRC): scripts/fetch-glu.sh scripts/glu-pin.txt
	$(Q)scripts/fetch-glu.sh

$(OUT)/.magic-gl-libs-stamp: $(OUT)/libgl4es.a $(OUT)/libX11-compat.so \
    $(MAGIC_GL_HDR) $(MAGIC_GLU_SRC) scripts/build-ogl-userlibs.sh
	$(Q)scripts/build-ogl-userlibs.sh
	$(Q)touch $@

$(OUT)/libGL.dylib $(OUT)/libGLU.dylib: $(OUT)/.magic-gl-libs-stamp
	$(Q)test -e $@ || { rm -f $(OUT)/.magic-gl-libs-stamp; \
	    $(MAKE) --no-print-directory $(OUT)/.magic-gl-libs-stamp; \
	    test -e $@; }

magic-clean:
	@echo "  CLEAN   magic"
	$(Q)rm -rf $(MAGIC_BUILD_DIR)
