# Build ANGLE's Metal libEGL/libGLESv2 from source with plain GNU make: no gn,
# gclient, or depot_tools. Ported from ../wilco (mk/angle-direct.mk). The source
# lists are awk-extracted from ANGLE's own .gni files so they track the pinned
# upstream, and the handful of files gn would generate are replaced by the shims
# in tools/angle-compat plus the -D macro set below.
#
# This is an OPT-IN target (make build-angle), never part of the default build.
# Fetch first (make fetch-angle), because the .gni parsing happens at make parse
# time and needs the tree present. Output dylibs land in $(ANGLE_OUT_DIR); point
# LIBX11_COMPAT_EGL at $(ANGLE_OUT_DIR)/libEGL.dylib to run the GLX layer on them.

ANGLE_ROOT := third_party/angle

.PHONY: fetch-angle build-angle check-angle-sources clean-angle

## Fetch the pinned ANGLE source tree (plain git, no gn/depot_tools).
fetch-angle:
	$(Q)scripts/fetch-angle.sh

# The rest only materializes once the tree is fetched; a plain build otherwise
# never pays for the parse-time awk over the .gni files.
ifneq ($(wildcard $(ANGLE_ROOT)/src/libGLESv2.gni),)

CXX ?= clang++
AR ?= ar
MKDIR_P ?= mkdir -p

# Single source of truth for the pinned commit (also read by scripts/fetch-angle.sh).
ANGLE_PIN := $(shell cat scripts/angle-pin.txt 2>/dev/null)

# The 327 ANGLE translation units are independent, so the build self-parallelizes
# to the host CPU count. Override with ANGLE_JOBS=N (or pass -j yourself).
ANGLE_JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

ANGLE_OUT_DIR := $(OUT)/angle
ANGLE_OBJ_DIR := $(OUT)/angle-obj
ANGLE_LIBEGL := $(ANGLE_OUT_DIR)/libEGL.dylib
ANGLE_LIBGLESV2 := $(ANGLE_OUT_DIR)/libGLESv2.dylib

# Plugin drop that the EGL loader finds by itself, with no LIBX11_COMPAT_EGL
# override: src/egl-wrapper.c derives "<dir of libX11-compat>/plugins/macOS/"
# via dladdr, so the loader picks up libEGL here. The entries are symlinks back
# into ANGLE_OUT_DIR rather than copies, so both the loader's dlopened libGLESv2
# and the copy glxgears links resolve to one realpath -> one dyld image -> a
# shared current context. The subdir is macOS because ANGLE is built only here.
ANGLE_PLUGIN_DIR := $(OUT)/plugins/macOS
ANGLE_PLUGIN_EGL := $(ANGLE_PLUGIN_DIR)/libEGL.dylib
ANGLE_PLUGIN_GLESV2 := $(ANGLE_PLUGIN_DIR)/libGLESv2.dylib
ANGLE_COMMON_LIB := $(ANGLE_OUT_DIR)/libangle-common.a
ANGLE_TRANSLATOR_LIB := $(ANGLE_OUT_DIR)/libangle-translator.a
ANGLE_LIBANGLE_LIB := $(ANGLE_OUT_DIR)/libangle-core.a
ANGLE_METAL_LIB := $(ANGLE_OUT_DIR)/libangle-metal.a
ANGLE_ENTRY_LIB := $(ANGLE_OUT_DIR)/libangle-entry.a
ANGLE_EGL_LOADER_LIB := $(ANGLE_OUT_DIR)/libangle-egl-loader.a
ANGLE_LIBGLESV2_GNI := $(ANGLE_ROOT)/src/libGLESv2.gni
ANGLE_COMPILER_GNI := $(ANGLE_ROOT)/src/compiler.gni
ANGLE_METAL_GNI := $(ANGLE_ROOT)/src/libANGLE/renderer/metal/metal_backend.gni

# Extract the quoted C/C++/ObjC++ source names from a named list in a .gni file.
define angle_gni_sources
$(shell awk 'BEGIN { on = 0 } \
	$$0 ~ "^$(1)[[:space:]]*(\\+)?=.*\\[" { on = 1 } \
	on { line = $$0; while (match(line, /"[^"]+\.(c|cc|cpp|mm)"/)) { \
		print substr(line, RSTART + 1, RLENGTH - 2); \
		line = substr(line, RSTART + RLENGTH); \
	} } \
	on && $$0 ~ /]/ { on = 0 }' $(2) 2>/dev/null)
endef

define angle_objs
$(patsubst %.c,$(ANGLE_OBJ_DIR)/%.o,\
$(patsubst %.cc,$(ANGLE_OBJ_DIR)/%.o,\
$(patsubst %.cpp,$(ANGLE_OBJ_DIR)/%.o,\
$(patsubst %.mm,$(ANGLE_OBJ_DIR)/%.o,$(addprefix $(ANGLE_ROOT)/,$(1))))))
endef

ANGLE_COMMON_SRCS := \
	$(call angle_gni_sources,libangle_common_sources,$(ANGLE_LIBGLESV2_GNI)) \
	$(call angle_gni_sources,libangle_common_shader_state_sources,$(ANGLE_LIBGLESV2_GNI)) \
	$(call angle_gni_sources,xxhash_sources,$(ANGLE_LIBGLESV2_GNI)) \
	src/common/CompiledShaderState.cpp \
	src/common/angle_version_info.cpp \
	src/common/apple_platform_utils.mm \
	src/common/system_utils_apple.cpp \
	src/common/system_utils_mac.cpp \
	src/common/system_utils_posix.cpp
ANGLE_IMAGE_SRCS := \
	$(call angle_gni_sources,libangle_image_util_sources,$(ANGLE_LIBGLESV2_GNI)) \
	src/image_util/AstcDecompressorNoOp.cpp
ANGLE_GPU_INFO_SRCS := \
	$(call angle_gni_sources,libangle_gpu_info_util_sources,$(ANGLE_LIBGLESV2_GNI)) \
	$(call angle_gni_sources,libangle_gpu_info_util_mac_sources,$(ANGLE_LIBGLESV2_GNI))
ANGLE_LIBANGLE_SRCS := \
	$(call angle_gni_sources,libangle_sources,$(ANGLE_LIBGLESV2_GNI)) \
	$(call angle_gni_sources,libangle_mac_sources,$(ANGLE_LIBGLESV2_GNI)) \
	src/libANGLE/capture/FrameCapture_mock.cpp \
	src/libANGLE/capture/serialize_mock.cpp
ANGLE_TRANSLATOR_SRCS := \
	$(call angle_gni_sources,angle_translator_sources,$(ANGLE_COMPILER_GNI)) \
	$(call angle_gni_sources,angle_translator_essl_symbol_table_sources,$(ANGLE_COMPILER_GNI)) \
	$(call angle_gni_sources,angle_translator_lib_msl_sources,$(ANGLE_COMPILER_GNI)) \
	$(call angle_gni_sources,angle_preprocessor_sources,$(ANGLE_COMPILER_GNI))
ANGLE_METAL_SRCS := $(addprefix src/libANGLE/renderer/metal/,\
	$(call angle_gni_sources,metal_backend_sources,$(ANGLE_METAL_GNI)))
ANGLE_ENTRY_SRCS := \
	$(call angle_gni_sources,libglesv2_entry_point_sources,$(ANGLE_LIBGLESV2_GNI)) \
	$(call angle_gni_sources,libglesv2_sources,$(ANGLE_LIBGLESV2_GNI))
ANGLE_EGL_LOADER_SRCS := \
	$(call angle_gni_sources,libegl_sources,$(ANGLE_LIBGLESV2_GNI)) \
	src/libEGL/egl_loader_autogen.cpp

ANGLE_DIRECT_SRCS := $(sort \
	$(ANGLE_COMMON_SRCS) $(ANGLE_IMAGE_SRCS) $(ANGLE_GPU_INFO_SRCS) \
	$(ANGLE_LIBANGLE_SRCS) $(ANGLE_TRANSLATOR_SRCS) $(ANGLE_METAL_SRCS) \
	$(ANGLE_ENTRY_SRCS) $(ANGLE_EGL_LOADER_SRCS))

ANGLE_COMMON_OBJS := $(call angle_objs,$(ANGLE_COMMON_SRCS))
ANGLE_IMAGE_OBJS := $(call angle_objs,$(ANGLE_IMAGE_SRCS))
ANGLE_GPU_INFO_OBJS := $(call angle_objs,$(ANGLE_GPU_INFO_SRCS))
ANGLE_LIBANGLE_OBJS := $(call angle_objs,$(ANGLE_LIBANGLE_SRCS))
ANGLE_TRANSLATOR_OBJS := $(call angle_objs,$(ANGLE_TRANSLATOR_SRCS))
ANGLE_METAL_OBJS := $(call angle_objs,$(ANGLE_METAL_SRCS))
ANGLE_ENTRY_OBJS := $(call angle_objs,$(ANGLE_ENTRY_SRCS))
ANGLE_COMMON_CORE_OBJS := $(ANGLE_COMMON_OBJS) $(ANGLE_IMAGE_OBJS) $(ANGLE_GPU_INFO_OBJS)
ANGLE_EGL_LOADER_OBJS := $(call angle_objs,$(ANGLE_EGL_LOADER_SRCS))

# Compiler flags. Kept independent of the library's global CPPFLAGS/CFLAGS so the
# X11/SDL/pixman include set does not leak into the ANGLE build.
ANGLE_BASE_CPPFLAGS := \
	-Itools/angle-compat \
	-I$(ANGLE_ROOT)/include \
	-I$(ANGLE_ROOT)/src \
	-I$(ANGLE_ROOT)/src/common/base \
	-I$(ANGLE_ROOT)/src/common/third_party/xxhash \
	-DANGLE_ENABLE_METAL=1 \
	-DANGLE_ENABLE_SHARE_CONTEXT_LOCK=1 \
	-DANGLE_ENABLE_CONTEXT_MUTEX=1 \
	-DANGLE_CAPTURE_ENABLED=0 \
	-DANGLE_EGL_LIBRARY_NAME=\"libEGL\" \
	-DANGLE_GLESV2_LIBRARY_NAME=\"libGLESv2\" \
	-DGL_GLES_PROTOTYPES=1 \
	-DEGL_EGL_PROTOTYPES=1 \
	-DGL_GLEXT_PROTOTYPES \
	-DEGL_EGLEXT_PROTOTYPES
# Size optimization: -Os shrinks the code. Symbol exposure is trimmed at link
# time with an exported-symbols allowlist (ANGLE_EXPORTS below), not with
# -fvisibility=hidden: hidden visibility also hides ANGLE's public egl*/gl* entry
# points (their KHRONOS export macros are not active in this build config), which
# breaks the libEGL loader's dlsym of eglGetDisplay. The allowlist plus
# -dead_strip keeps only the real entry points exported and lets the linker
# remove everything unreachable from them.
# -Oz is clang's most aggressive size optimization; ThinLTO adds cross-module
# dead-code elimination and inlining (removing internal code that per-TU -Oz +
# -dead_strip cannot reach) and parallelizes its backend phase. The
# -exported_symbols_list allowlist is the LTO/strip root set, so ld64 keeps the
# exported GLES/EGL entry points and everything they reach; probe-angle is the
# smoke check that the trimmed provider still creates a Metal context after any
# flag or pin change.
ANGLE_OPT := -Oz -flto=thin
ANGLE_CXXFLAGS := -std=c++20 -Wno-error -Wno-pedantic -fPIC $(ANGLE_OPT)
ANGLE_OBJCXXFLAGS := -std=c++20 -Wno-error -Wno-pedantic -fno-objc-arc -fPIC $(ANGLE_OPT)
ANGLE_EXPORTS := mk/angle.exports

# Per-implementation-unit visibility defines, matched by object sub-path.
$(ANGLE_OBJ_DIR)/$(ANGLE_ROOT)/src/libANGLE/%.o: ANGLE_EXTRA := -DLIBANGLE_IMPLEMENTATION
$(ANGLE_OBJ_DIR)/$(ANGLE_ROOT)/src/libGLESv2/%.o: ANGLE_EXTRA := -DLIBGLESV2_IMPLEMENTATION
$(ANGLE_OBJ_DIR)/$(ANGLE_ROOT)/src/libEGL/%.o: ANGLE_EXTRA := \
	-DLIBEGL_IMPLEMENTATION -DANGLE_USE_EGL_LOADER \
	-DANGLE_DISPATCH_LIBRARY=\"libGLESv2\"

$(ANGLE_OBJ_DIR)/%.o: %.cpp
	@$(MKDIR_P) $(dir $@)
	@echo "  CXX     $<"
	$(Q)$(CXX) $(ANGLE_BASE_CPPFLAGS) $(ANGLE_EXTRA) $(ANGLE_CXXFLAGS) -c $< -o $@
$(ANGLE_OBJ_DIR)/%.o: %.cc
	@$(MKDIR_P) $(dir $@)
	@echo "  CXX     $<"
	$(Q)$(CXX) $(ANGLE_BASE_CPPFLAGS) $(ANGLE_EXTRA) $(ANGLE_CXXFLAGS) -c $< -o $@
$(ANGLE_OBJ_DIR)/%.o: %.mm
	@$(MKDIR_P) $(dir $@)
	@echo "  OBJCXX  $<"
	$(Q)$(CXX) $(ANGLE_BASE_CPPFLAGS) $(ANGLE_EXTRA) $(ANGLE_OBJCXXFLAGS) -c $< -o $@
$(ANGLE_OBJ_DIR)/%.o: %.c
	@$(MKDIR_P) $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(ANGLE_BASE_CPPFLAGS) $(ANGLE_EXTRA) -fPIC $(ANGLE_OPT) -c $< -o $@

# Sanity gate: a broken .gni extraction yields far too few files. The count is
# validated at build time (parse-time awk failures collapse to empty lists).
check-angle-sources:
	@head="$$(git -C $(ANGLE_ROOT) rev-parse HEAD 2>/dev/null)"; \
	test "$$head" = "$(ANGLE_PIN)" || { \
		echo "third_party/angle at $$head, expected $(ANGLE_PIN); run make fetch-angle" >&2; \
		exit 69; \
	}
	@test "$(words $(ANGLE_DIRECT_SRCS))" -gt 250 || { \
		echo "ANGLE source extraction incomplete: $(words $(ANGLE_DIRECT_SRCS)) files (need >250). A pin bump can reshuffle the .gni files; re-validate the awk extraction in mk/angle.mk" >&2; \
		exit 69; \
	}
	@printf '  CHECK   ANGLE @ %s: %s direct sources\n' "$(ANGLE_PIN)" "$(words $(ANGLE_DIRECT_SRCS))"

$(ANGLE_COMMON_LIB): $(ANGLE_COMMON_CORE_OBJS)
	@$(MKDIR_P) $(dir $@)
	@echo "  AR      $@"
	$(Q)rm -f $@ && $(AR) rcs $@ $^
$(ANGLE_TRANSLATOR_LIB): $(ANGLE_TRANSLATOR_OBJS)
	@$(MKDIR_P) $(dir $@)
	@echo "  AR      $@"
	$(Q)rm -f $@ && $(AR) rcs $@ $^
$(ANGLE_LIBANGLE_LIB): $(ANGLE_LIBANGLE_OBJS)
	@$(MKDIR_P) $(dir $@)
	@echo "  AR      $@"
	$(Q)rm -f $@ && $(AR) rcs $@ $^
$(ANGLE_METAL_LIB): $(ANGLE_METAL_OBJS)
	@$(MKDIR_P) $(dir $@)
	@echo "  AR      $@"
	$(Q)rm -f $@ && $(AR) rcs $@ $^
$(ANGLE_ENTRY_LIB): $(ANGLE_ENTRY_OBJS)
	@$(MKDIR_P) $(dir $@)
	@echo "  AR      $@"
	$(Q)rm -f $@ && $(AR) rcs $@ $^
$(ANGLE_EGL_LOADER_LIB): $(ANGLE_EGL_LOADER_OBJS)
	@$(MKDIR_P) $(dir $@)
	@echo "  AR      $@"
	$(Q)rm -f $@ && $(AR) rcs $@ $^

$(ANGLE_LIBGLESV2): $(ANGLE_EXPORTS)
$(ANGLE_LIBGLESV2): $(ANGLE_COMMON_LIB) $(ANGLE_TRANSLATOR_LIB) \
                    $(ANGLE_LIBANGLE_LIB) $(ANGLE_METAL_LIB) $(ANGLE_ENTRY_LIB)
	@$(MKDIR_P) $(dir $@)
	@echo "  DYLIB   $@"
	$(Q)$(CXX) -flto=thin -dynamiclib -install_name @rpath/libGLESv2.dylib -Wl,-all_load \
		-Wl,-dead_strip -Wl,-exported_symbols_list,$(ANGLE_EXPORTS) \
		$(ANGLE_COMMON_LIB) $(ANGLE_TRANSLATOR_LIB) $(ANGLE_LIBANGLE_LIB) \
		$(ANGLE_METAL_LIB) $(ANGLE_ENTRY_LIB) \
		-framework Metal -framework IOSurface -framework QuartzCore \
		-framework Cocoa -framework CoreFoundation -framework CoreGraphics \
		-framework IOKit -o $@ -lz
	$(Q)strip -x $@

# libEGL is the thin loader that dlopens libGLESv2 at runtime, so it does not
# link libGLESv2. force_load pulls in all its egl* entry points; libangle-common
# is linked on demand for the loader's system-library helpers
# (OpenSystemLibraryAndGetError / GetLibrarySymbol), which used to be satisfied by
# libGLESv2's now-trimmed export table.
$(ANGLE_LIBEGL): $(ANGLE_EXPORTS)
$(ANGLE_LIBEGL): $(ANGLE_EGL_LOADER_LIB) $(ANGLE_COMMON_LIB) $(ANGLE_LIBGLESV2)
	@$(MKDIR_P) $(dir $@)
	@echo "  DYLIB   $@"
	$(Q)$(CXX) -flto=thin -dynamiclib -install_name @rpath/libEGL.dylib \
		-Wl,-force_load,$(ANGLE_EGL_LOADER_LIB) $(ANGLE_COMMON_LIB) \
		-Wl,-dead_strip -Wl,-exported_symbols_list,$(ANGLE_EXPORTS) \
		-Wl,-rpath,@loader_path -o $@ -lz
	$(Q)strip -x $@

## Build ANGLE's Metal libEGL + libGLESv2 (opt-in; run after make fetch-angle).
## Self-parallelizes over ANGLE_JOBS so a plain `make build-angle` uses all cores.
build-angle: check-angle-sources
	$(Q)$(MAKE) -j$(ANGLE_JOBS) $(ANGLE_LIBEGL) $(ANGLE_LIBGLESV2)
	$(Q)$(MAKE) install-angle-plugin
	@echo "ANGLE built: $(ANGLE_LIBEGL) $(ANGLE_LIBGLESV2)"

clean-angle:
	$(Q)rm -rf $(ANGLE_OBJ_DIR) $(ANGLE_OUT_DIR)

# glxgears GLES2 showcase: renders through the GLX layer onto the built ANGLE
# provider. Links libX11-compat for glX*/X* and ANGLE's libGLESv2 for gl* (same
# libGLESv2 dyld resolves for the libEGL loader, so the current context is
# shared); GLES2 headers come from ANGLE's include tree.
.PHONY: glxgears
$(OUT)/examples/glxgears: examples/glxgears.c $(TARGET) $(ANGLE_LIBGLESV2)
	@$(MKDIR_P) $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) -I$(ANGLE_ROOT)/include $(CFLAGS) $(CFLAGS_EXTRA) \
	    $< $(TARGET) $(ANGLE_LIBGLESV2) $(LDLIBS) \
	    -Wl,-rpath,$(abspath $(OUT)) -Wl,-rpath,$(abspath $(ANGLE_OUT_DIR)) \
	    -lm -o $@

.PHONY: glxgears-headless
ANGLE_RUN_ENV = LIBX11_COMPAT_EGL=$(abspath $(ANGLE_LIBEGL)) \
	DYLD_LIBRARY_PATH=$(abspath $(OUT)):$(abspath $(ANGLE_OUT_DIR))

## Run the glxgears showcase in a real on-screen window (three spinning gears,
## presented via a CAMetalLayer surface; prints FPS; close the window to quit).
glxgears: $(OUT)/examples/glxgears $(ANGLE_LIBEGL)
	$(Q)$(ANGLE_RUN_ENV) $(OUT)/examples/glxgears

## Same gears rendered offscreen (pbuffer) for headless CI: bounded frame count
## plus a silent readback sanity check. Identical rendering, no window.
glxgears-headless: $(OUT)/examples/glxgears $(ANGLE_LIBEGL)
	$(Q)SDL_VIDEODRIVER=dummy GLXGEARS_FRAMES=6 $(ANGLE_RUN_ENV) \
	    $(OUT)/examples/glxgears

# Symlink the built provider into the plugin dir so the loader finds it with no
# env override. rm before ln so a stale/wrong link is always replaced (ln -sf
# alone would keep it when make thinks the target is up to date). abspath keeps
# the link valid regardless of cwd; it points inside this build tree, which is
# not relocatable anyway (the ANGLE objects live there too).
$(ANGLE_PLUGIN_EGL): $(ANGLE_LIBEGL)
	@$(MKDIR_P) $(ANGLE_PLUGIN_DIR)
	$(Q)rm -f $@ && ln -s $(abspath $<) $@
$(ANGLE_PLUGIN_GLESV2): $(ANGLE_LIBGLESV2)
	@$(MKDIR_P) $(ANGLE_PLUGIN_DIR)
	$(Q)rm -f $@ && ln -s $(abspath $<) $@

## Install the built ANGLE provider into plugins/macOS/ (symlinks) so a client
## linking libX11-compat finds libEGL through the dladdr plugin search alone.
.PHONY: install-angle-plugin
install-angle-plugin: $(ANGLE_PLUGIN_EGL) $(ANGLE_PLUGIN_GLESV2)

## Prove the plugin path: run glxgears headless with NO LIBX11_COMPAT_EGL, so the
## provider is resolved only via plugins/macOS/. Baked rpaths locate libX11-compat
## and the linked libGLESv2; the loader finds libEGL through dladdr. Assert (via
## DYLD_PRINT_LIBRARIES) that the libEGL dyld actually loaded lives under $(OUT),
## so a silent fallback to a system libEGL cannot make this a false pass.
.PHONY: check-angle-plugin
check-angle-plugin: $(OUT)/examples/glxgears install-angle-plugin
	$(Q)out=$$(SDL_VIDEODRIVER=dummy GLXGEARS_FRAMES=6 DYLD_PRINT_LIBRARIES=1 \
	    $(OUT)/examples/glxgears 2>&1) || { echo "$$out"; exit 1; }; \
	  echo "$$out" | grep 'libEGL.dylib' | grep -q '$(abspath $(OUT))' \
	    && echo "plugin path OK (loaded our libEGL under $(OUT), no override)" \
	    || { echo "FAIL: libEGL not loaded from our tree (system fallback?)"; \
	         echo "$$out" | grep -i 'egl.*dylib'; exit 1; }

# Approach-A GLX differential: one binary renders a fixed GLES2 scene through our
# GLX layer and through direct EGL on the same provider; the two readbacks must
# match. Links libX11-compat (glX*/X*), ANGLE's libGLESv2 (gl*) and libEGL (the
# direct-EGL path); GLES2/EGL headers come from ANGLE's include tree.
$(OUT)/tests/glx-egl-diff: tests/glx-egl-diff.c $(TARGET) $(ANGLE_LIBGLESV2) \
                           $(ANGLE_LIBEGL)
	@$(MKDIR_P) $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) -I$(ANGLE_ROOT)/include $(CFLAGS) $(CFLAGS_EXTRA) \
	    $< $(TARGET) $(ANGLE_LIBGLESV2) $(ANGLE_LIBEGL) $(LDLIBS) \
	    -Wl,-rpath,$(abspath $(OUT)) -Wl,-rpath,$(abspath $(ANGLE_OUT_DIR)) \
	    -o $@

## GLX differential: render the same scene via our GLX layer and via direct EGL
## on the same driver, then diff the pixels. Any difference is our translation.
GLX_DIFF_LIBS = DYLD_LIBRARY_PATH=$(abspath $(OUT)):$(abspath $(ANGLE_OUT_DIR))
.PHONY: check-differential-glx
check-differential-glx: $(OUT)/tests/glx-egl-diff
	$(Q)SDL_VIDEODRIVER=dummy LIBX11_COMPAT_EGL=$(abspath $(ANGLE_LIBEGL)) \
	    $(GLX_DIFF_LIBS) $(OUT)/tests/glx-egl-diff glx > $(OUT)/glx-diff-glx.rgba \
	    || { echo "FAIL: glx backend"; exit 1; }
	$(Q)$(GLX_DIFF_LIBS) $(OUT)/tests/glx-egl-diff egl > $(OUT)/glx-diff-egl.rgba \
	    || { echo "FAIL: egl backend"; exit 1; }
	$(Q)python3 scripts/compare-rgba.py $(OUT)/glx-diff-glx.rgba \
	    $(OUT)/glx-diff-egl.rgba 0 0

else

build-angle:
	@echo "third_party/angle not fetched; run: make fetch-angle" >&2
	@exit 1

endif
