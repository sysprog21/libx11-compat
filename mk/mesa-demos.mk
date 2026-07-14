# Mesa demos (https://gitlab.freedesktop.org/mesa/demos): unmodified upstream
# desktop-GL + GLX demos built against libx11-compat as a GLX validation workload,
# and the coverage for the in-tree GL translation layer (src/GL). Representative
# set (glxgears, glxdemo, glxheads, sharedtex, multictx, glxswapcontrol) spans the
# visual/immediate-mode, multi-window, multi-context, shared-texture, and
# swap-control paths.
#
# Each is compiled straight from its upstream translation unit (bypassing meson)
# against the in-tree GL headers, and statically links the gl4es loader
# (libgl4es.a) so desktop gl* runs through gl4es -> ANGLE's GLESv2 -> Metal with no
# Mesa; libx11-compat supplies Xlib and glX*. macOS + GLX=1 + ANGLE (build-angle)
# only; a no-op elsewhere. (glxgears_fbconfig is temporarily out: its pbutil helper
# needs a GLXFBConfig signature reconciled against the in-tree headers.)

MESA_DEMOS_URL := https://gitlab.freedesktop.org/mesa/demos.git
MESA_DEMOS_COMMIT := 188fc2354de5a9cbfd73852183a35f4982ca43d5
MESA_DEMOS_SRC_DIR := $(OUT)/upstream/mesa-demos
MESA_DEMOS_SRC_STAMP := $(MESA_DEMOS_SRC_DIR)/.source-stamp
MESA_DEMOS_XDEMOS := $(MESA_DEMOS_SRC_DIR)/src/xdemos
MESA_DEMOS_BUILD_DIR := $(OUT)/mesa-demos
# Single-file demos (one rule); glxgears_fbconfig is special, it also needs pbutil.
#   glxgears/glxdemo        glXChooseVisual / glXChooseFBConfig context paths
#   glxheads                multiple top-level GL windows
#   sharedtex               shared texture across two contexts + windows
#   multictx                multiple GL contexts
#   glxswapcontrol          glXSwapInterval EXT/MESA/SGI variants
MESA_DEMOS_SIMPLE := glxgears glxdemo glxheads sharedtex multictx glxswapcontrol
MESA_DEMOS_NAMES := $(MESA_DEMOS_SIMPLE)
MESA_DEMOS_BINS := $(addprefix $(MESA_DEMOS_BUILD_DIR)/,$(MESA_DEMOS_NAMES))
MESA_DEMOS_GIT_Q := $(if $(filter 1,$(V)),,--quiet)

.PHONY: mesa-demos check-mesa-demos

$(MESA_DEMOS_SRC_STAMP): mk/mesa-demos.mk
	@echo "  GIT     $(MESA_DEMOS_URL)"
	$(Q)mkdir -p $(dir $(MESA_DEMOS_SRC_DIR))
	$(Q)test -d $(MESA_DEMOS_SRC_DIR)/.git || \
	    git clone $(MESA_DEMOS_GIT_Q) $(MESA_DEMOS_URL) $(MESA_DEMOS_SRC_DIR)
	$(Q)cd $(MESA_DEMOS_SRC_DIR) && \
	    git checkout $(MESA_DEMOS_GIT_Q) --detach $(MESA_DEMOS_COMMIT) && \
	    git reset --hard $(MESA_DEMOS_GIT_Q) $(MESA_DEMOS_COMMIT) >/dev/null && \
	    git clean $(MESA_DEMOS_GIT_Q) -fdx >/dev/null
	$(Q)touch $@

# The upstream .c files appear from the clone above; a pattern rule marks any
# xdemos source as produced by the stamp, so make fetches before compiling
# instead of reporting "no rule" for a not-yet-cloned .c.
$(MESA_DEMOS_XDEMOS)/%.c: $(MESA_DEMOS_SRC_STAMP)
	@true

# Desktop gl* runs through the in-tree gl4es translation layer (src/GL, statically
# linked as libgl4es.a) over a GLES2 provider, with glX* resolved to libx11-compat.
# macOS renders over ANGLE's GLESv2 on Metal (no Mesa, needs make build-angle);
# Linux links the system Mesa libGLESv2 and renders through surfaceless Mesa. GLX=1
# only; a no-op on other platforms.
ifeq ($(GLX),1)
ifneq ($(filter Darwin Linux,$(UNAME_S)),)
MESA_DEMOS_GL4ES := 1
endif
endif

# gl.h/glext.h/glxext.h are pristine upstream headers the demos compile against
# (the lib itself uses none of them). Fetch them into third_party/ at build time
# rather than vendoring: glext.h/glxext.h from the Khronos OpenGL-Registry, gl.h
# from Mesa, each pinned; cached under third_party/ so only the first demo build
# needs the network. GL/glx.h stays in include/ -- it is our own hand-trimmed API
# contract, not an upstream copy. Each header self-fetches, so deleting one
# refetches just it. The fetch rule sits outside the GLX=1 gate below so Magic's
# macOS OpenGL user libs can pull the headers when the demo block is off; only
# building the demos themselves is GLX-gated.
GL_HDR_CACHE := third_party/gl-headers
GL_HDR_FILES := $(GL_HDR_CACHE)/GL/gl.h $(GL_HDR_CACHE)/GL/glext.h \
    $(GL_HDR_CACHE)/GL/glxext.h
$(GL_HDR_FILES): scripts/fetch-gl-headers.sh scripts/opengl-registry-pin.txt \
    scripts/mesa-pin.txt
	$(Q)scripts/fetch-gl-headers.sh $(GL_HDR_CACHE) $(@F)

ifdef MESA_DEMOS_GL4ES
$(MESA_DEMOS_BUILD_DIR):
	@mkdir -p $@

# ANGLE GLESv2 dylib the macOS build links and runs against (empty on Linux, which
# uses the system Mesa GLESv2 instead).
MESA_DEMOS_ANGLE := $(if $(filter Darwin,$(UNAME_S)),$(OUT)/angle/libGLESv2.dylib)

# libgl4es.a exports all ~979 desktop gl* (kept general so any unmodified client
# links), but a given demo calls only a handful and needs to EXPOSE only the few
# that libx11-compat resolves by dlsym at runtime. -exported_symbols_list pins the
# binary's global symbol table to exactly those (mk/mesa-demos.exports), and being
# the only export roots they are also the -dead_strip roots, so the gl4es code the
# demo never reaches is dropped with them. Net: 511 exported symbols -> 3, and the
# binary roughly halves, with no change to the general-purpose archive.
#
# The provider link and strip flags below are per platform. macOS links ANGLE's
# GLESv2 and applies that ld64 export-list + -dead_strip trim (plus -Wl,-x to drop
# the local symbol table); Linux links system Mesa GLESv2 with GNU ld
# --gc-sections, which has no export-list equivalent and needs none for CI.
ifeq ($(UNAME_S),Darwin)
MESA_DEMOS_EXPORTS := mk/mesa-demos.exports
MESA_DEMOS_GLES_LINK := -L$(OUT)/angle -lGLESv2
MESA_DEMOS_LDFLAGS := -Wl,-dead_strip -Wl,-exported_symbols_list,$(MESA_DEMOS_EXPORTS) -Wl,-x
else
# --dynamic-list is the GNU-ld counterpart of the macOS export list: it puts the
# same handful of gl4es symbols into the dynamic symtab so the readback path can
# dlsym them (glFlush to drain the batch, the GLState trio for per-context state),
# which a sparse single-frame client like glxdemo needs to render at all.
MESA_DEMOS_EXPORTS := mk/mesa-demos.dynlist
MESA_DEMOS_GLES_LINK := -lGLESv2
MESA_DEMOS_LDFLAGS := -Wl,--dynamic-list=$(MESA_DEMOS_EXPORTS)
endif
define mesa_demo_link
	@echo "  CC      $(notdir $@)"
	$(if $(MESA_DEMOS_ANGLE),$(Q)test -f $(MESA_DEMOS_ANGLE) || { echo "  need ANGLE: run make build-angle" >&2; exit 1; })
	$(Q)$(CC) $(1) -I$(GL_HDR_CACHE) -Iinclude -I$(OUT)/upstream/include \
	    $(TARGET) $(GL4ES_LIB) $(MESA_DEMOS_GLES_LINK) -lm $(MESA_DEMOS_LDFLAGS) -o $@
endef

MESA_DEMO_DEPS := $(GL4ES_LIB) $(TARGET) $(UPSTREAM_HEADERS_STAMP) $(MESA_DEMOS_EXPORTS) \
    $(GL_HDR_FILES)

# Single-file demos: one static pattern rule over MESA_DEMOS_SIMPLE.
$(addprefix $(MESA_DEMOS_BUILD_DIR)/,$(MESA_DEMOS_SIMPLE)): $(MESA_DEMOS_BUILD_DIR)/%: \
    $(MESA_DEMOS_XDEMOS)/%.c $(MESA_DEMO_DEPS) | $(MESA_DEMOS_BUILD_DIR)
	$(call mesa_demo_link,$<)

# glxgears_fbconfig also needs the pbutil helper and its own header dir.
$(MESA_DEMOS_BUILD_DIR)/glxgears_fbconfig: $(MESA_DEMOS_XDEMOS)/glxgears_fbconfig.c \
    $(MESA_DEMOS_XDEMOS)/pbutil.c $(MESA_DEMO_DEPS) | $(MESA_DEMOS_BUILD_DIR)
	$(call mesa_demo_link,$(MESA_DEMOS_XDEMOS)/glxgears_fbconfig.c $(MESA_DEMOS_XDEMOS)/pbutil.c -I$(MESA_DEMOS_XDEMOS))

mesa-demos: $(MESA_DEMOS_BINS)

## Headless GLX render regression: run each unmodified Mesa demo through
## libx11-compat + gl4es and assert it actually paints (not a black frame). Also
## the coverage for the src/GL translation layer. macOS renders gl4es -> ANGLE ->
## Metal (no Mesa); Linux renders gl4es -> system Mesa GLESv2 under a surfaceless
## EGL context (glx-snapshot.sh supplies Xvfb).
ifeq ($(UNAME_S),Darwin)
check-mesa-demos:
	$(Q)if ! test -f $(MESA_DEMOS_ANGLE); then \
	    echo "  SKIP    mesa-demos GLX render (needs ANGLE; run make build-angle)"; \
	    exit 0; \
	fi; \
	$(MAKE) --no-print-directory mesa-demos; \
	set -e; for name in $(MESA_DEMOS_NAMES); do \
	    echo "  CHECK   mesa-demo $$name"; \
	    scripts/run-mesa-demo.sh $$name --snapshot $(OUT)/mesa-demo-$$name.png; \
	    $(PYTHON) scripts/assert-image-content.py $(OUT)/mesa-demo-$$name.png 0.05; \
	done
else
check-mesa-demos:
	$(Q)egl=$$($(CC) -print-file-name=libEGL.so); \
	    gles=$$($(CC) -print-file-name=libGLESv2.so); \
	    if [ ! -f "$$egl" ] || [ ! -f "$$gles" ]; then \
	        echo "  SKIP    mesa-demos GLX render (no system libEGL.so/libGLESv2.so; install libegl-dev libgles-dev)"; \
	        exit 0; \
	    fi; \
	    set -e; \
	    $(MAKE) --no-print-directory mesa-demos; \
	    for name in $(MESA_DEMOS_NAMES); do \
	        echo "  CHECK   mesa-demo $$name (linux/mesa)"; \
	        LIBX11_COMPAT_EGL="$$egl" EGL_PLATFORM=surfaceless \
	            scripts/run-mesa-demo.sh $$name --snapshot $(OUT)/mesa-demo-$$name.png; \
	        $(PYTHON) scripts/assert-image-content.py $(OUT)/mesa-demo-$$name.png 0.05; \
	    done
endif
else
mesa-demos:
	@echo "  SKIP    mesa-demos (needs GLX=1 on Darwin+ANGLE or Linux+Mesa)"
check-mesa-demos:
	@echo "  SKIP    mesa-demos GLX render (needs GLX=1 on Darwin+ANGLE or Linux+Mesa)"
endif
