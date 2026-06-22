# Build libXt-compat.so from the pinned libXt-1.3.1 source tree staged
# under $(OUT)/upstream/src-libXt/ (see scripts/sync-upstream-headers.py).
#
# libXt is a pure Xlib client: every symbol it calls outside libc reaches
# libX11-compat.so. Compile its 53 .c files against the staged upstream
# headers + a hand-synthesized config.h, generate StringDefs.c from
# string.list via the upstream makestrs helper compiled as a host tool,
# and link the result as build/libXt-compat.so.
#
# The build is intentionally always-on. The dependency closure adds no
# new system packages -- only header staging and a host-side helper that
# is rebuilt from upstream source on every fetch.

LIBXT_SRC_DIR     := $(OUT)/upstream/src-libXt
LIBXT_TOPDIR      := $(OUT)/upstream/topdir-libXt
LIBXT_UTIL_DIR    := $(LIBXT_TOPDIR)/util
LIBXT_GEN_DIR     := $(OUT)/libxt-gen
LIBXT_OBJ_DIR     := $(OUT)/libxt
LIBXT_HOST_DIR    := $(OUT)/host
LIBXT_TARGET      := $(OUT)/libXt-compat.so

# mk/libxt.mk is included before mk/upstream-headers.mk, but its rules need
# this stamp while make parses prerequisites. Keep the value aligned with the
# authoritative upstream fragment.
UPSTREAM_HEADERS_DIR ?= $(OUT)/upstream/include
UPSTREAM_HEADERS_STAMP ?= $(UPSTREAM_HEADERS_DIR)/.upstream-stamp

LIBXT_HOST_MAKESTRS := $(LIBXT_HOST_DIR)/makestrs
LIBXT_STRING_LIST   := $(LIBXT_UTIL_DIR)/string.list
LIBXT_TEMPLATES     := \
    $(LIBXT_UTIL_DIR)/StrDefs.ct \
    $(LIBXT_UTIL_DIR)/StrDefs.ht \
    $(LIBXT_UTIL_DIR)/Shell.ht

LIBXT_GEN_STAMP    := $(LIBXT_GEN_DIR)/.stringdefs-stamp
LIBXT_GEN_C        := $(LIBXT_GEN_DIR)/StringDefs.c
LIBXT_GEN_HEADERS  := $(LIBXT_GEN_DIR)/StringDefs.h $(LIBXT_GEN_DIR)/Shell.h
LIBXT_STAGED_H     := \
    $(OUT)/upstream/include/X11/StringDefs.h \
    $(OUT)/upstream/include/X11/Shell.h

# Enumerated rather than wildcarded: $(wildcard) evaluates at parse time,
# before the first sync has created the directory. The list comes from the
# libXt-1.3.1 src/ tree (53 files). Keep alphabetized so version bumps that
# add or remove a file produce a small, readable diff.
LIBXT_SRC_BASES := \
    ActionHook.c Alloc.c ArgList.c Callback.c ClickTime.c Composite.c \
    Constraint.c Convert.c Converters.c Core.c Create.c Destroy.c \
    Display.c Error.c Event.c EventUtil.c Functions.c GCManager.c \
    Geometry.c GetActKey.c GetResList.c GetValues.c HookObj.c Hooks.c \
    Initialize.c Intrinsic.c Keyboard.c Manage.c NextEvent.c Object.c \
    PassivGrab.c Pointer.c Popup.c PopupCB.c RectObj.c ResConfig.c \
    Resources.c Selection.c SetSens.c SetValues.c SetWMCW.c Shell.c \
    TMaction.c TMgrab.c TMkey.c TMparse.c TMprint.c TMstate.c Threads.c \
    VarCreate.c VarGet.c Varargs.c Vendor.c
LIBXT_SRCS := $(addprefix $(LIBXT_SRC_DIR)/,$(LIBXT_SRC_BASES))
LIBXT_OBJS := $(patsubst $(LIBXT_SRC_DIR)/%.c,$(LIBXT_OBJ_DIR)/%.o,$(LIBXT_SRCS)) \
              $(LIBXT_OBJ_DIR)/StringDefs.o

# libXt include path layering:
#   include/libxt-build  - hand-synthesized config.h (libXt only)
#   $(LIBXT_GEN_DIR)     - generated StringDefs.h / Shell.h (quoted form)
#   $(OUT)/upstream/...  - libX11 + xorgproto + libXt headers (already
#                          covered by the global -iquote setup in
#                          mk/config.mk, repeated here for clarity)
#   include              - tracked headers (X11/Xmu/Editres.h stub)
LIBXT_CPPFLAGS := \
    -DHAVE_CONFIG_H -DLIBXT_COMPILATION \
    -DXFILESEARCHPATHDEFAULT='"/usr/share/X11/app-defaults/%N%C"' \
    -DERRORDB='"/usr/share/X11/XtErrorDB"' \
    -Iinclude/libxt-build \
    -I$(LIBXT_GEN_DIR) \
    -I$(OUT)/upstream/include \
    -Iinclude \
    -iquote $(LIBXT_GEN_DIR) \
    -iquote include/X11 \
    -iquote $(OUT)/upstream/include/X11 \
    $(if $(SDL2_PREFIX),-I$(SDL2_PREFIX)/include) \
    -D_GNU_SOURCE -D_DARWIN_C_SOURCE

# Upstream libXt was written before -Wall / strict-prototypes were common,
# so silence the noise specific to that tree without weakening our own
# warning set on libx11-compat sources. -Wno-incompatible-pointer-types-
# discards-qualifiers is the clang spelling for the gcc-only
# -Wno-discarded-qualifiers and covers the cases where libXt drops a
# `const` while assigning to a non-const callback argument.
LIBXT_CFLAGS := -std=c99 -Wall -fPIC \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
    -Wno-missing-braces -Wno-sign-compare -Wno-deprecated-declarations \
    -Wno-incompatible-pointer-types-discards-qualifiers \
    -Wno-incompatible-pointer-types

# Host compiler for makestrs. We do not cross-compile today, so reuse CC.
HOST_CC ?= $(CC)

$(LIBXT_HOST_DIR) $(LIBXT_GEN_DIR) $(LIBXT_OBJ_DIR):
	@mkdir -p $@

# Compile makestrs as a host-side tool. It is a single-file standalone
# generator that depends only on libc; no need for any libx11-compat
# include paths.
$(LIBXT_HOST_MAKESTRS): $(UPSTREAM_HEADERS_STAMP) | $(LIBXT_HOST_DIR)
	@echo "  HOSTCC  $(LIBXT_UTIL_DIR)/makestrs.c"
	$(Q)$(HOST_CC) -O2 -o $@ $(LIBXT_UTIL_DIR)/makestrs.c

# The upstream sync stages libXt sources and util files as side effects.
# Declare them so a clean build knows how to fetch these normal
# prerequisites before trying to build libXt objects or the host generator.
$(LIBXT_SRCS) $(LIBXT_UTIL_DIR)/makestrs.c $(LIBXT_STRING_LIST) $(LIBXT_TEMPLATES): $(UPSTREAM_HEADERS_STAMP)

# Run makestrs from the topdir so the "util/StrDefs.ct" / "util/StrDefs.ht"
# paths embedded in string.list resolve against cwd. The generated headers
# land in cwd alongside StringDefs.c, then move to LIBXT_GEN_DIR. GNU Make
# 3.81 does not support grouped targets, so a portable stamp ensures the
# multi-output generator runs once under parallel make.
$(LIBXT_GEN_STAMP): $(LIBXT_HOST_MAKESTRS) $(UPSTREAM_HEADERS_STAMP) | $(LIBXT_GEN_DIR)
	@echo "  GEN     StringDefs.{c,h} Shell.h"
	$(Q)rm -f $(LIBXT_GEN_C) $(LIBXT_GEN_HEADERS)
	$(Q)cd $(LIBXT_TOPDIR) && \
	    $(abspath $(LIBXT_HOST_MAKESTRS)) < util/string.list \
	    > $(abspath $(LIBXT_GEN_C))
	$(Q)mv $(LIBXT_TOPDIR)/StringDefs.h $(LIBXT_GEN_DIR)/StringDefs.h
	$(Q)mv $(LIBXT_TOPDIR)/Shell.h $(LIBXT_GEN_DIR)/Shell.h
	$(Q)touch $@

$(LIBXT_GEN_C) $(LIBXT_GEN_HEADERS): $(LIBXT_GEN_STAMP)
	$(Q)test -f $@ || { \
	    echo "  ERROR   $@ missing despite stamp present; remove $(LIBXT_GEN_STAMP) and retry"; \
	    exit 1; \
	}

# Stage StringDefs.h and Shell.h into the upstream X11/ include tree so
# consumers that say `#include <X11/Shell.h>` (e.g. Motif's lib/Xm/*.c)
# resolve them through the standard include path, not just the libXt
# build's private generation dir.
$(OUT)/upstream/include/X11/StringDefs.h: $(LIBXT_GEN_DIR)/StringDefs.h
	$(Q)cp $< $@
$(OUT)/upstream/include/X11/Shell.h: $(LIBXT_GEN_DIR)/Shell.h
	$(Q)cp $< $@

# Compile libXt translation units. The upstream stamp guarantees the .c
# files have been staged before the recipe reads them; the explicit
# dependency on LIBXT_GEN_HEADERS ensures StringDefs.h is available before
# any unit that quotes it compiles.
$(LIBXT_OBJ_DIR)/%.o: $(UPSTREAM_HEADERS_STAMP) $(LIBXT_GEN_HEADERS) \
    $(LIBXT_STAGED_H) $(SDL_BACKEND_STAMP) | $(LIBXT_OBJ_DIR)
	@echo "  CC      $(LIBXT_SRC_DIR)/$*.c"
	$(Q)$(CC) $(LIBXT_CPPFLAGS) $(CFLAGS) $(LIBXT_CFLAGS) $(CFLAGS_EXTRA) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d) \
	    -c $(LIBXT_SRC_DIR)/$*.c -o $@

# StringDefs.c lives in $(LIBXT_GEN_DIR), not LIBXT_SRC_DIR.
$(LIBXT_OBJ_DIR)/StringDefs.o: $(LIBXT_GEN_C) $(LIBXT_GEN_HEADERS) \
    $(LIBXT_STAGED_H) $(SDL_BACKEND_STAMP) | $(LIBXT_OBJ_DIR) \
    $(UPSTREAM_HEADERS_STAMP)
	@echo "  CC      $<"
	$(Q)$(CC) $(LIBXT_CPPFLAGS) $(CFLAGS) $(LIBXT_CFLAGS) $(CFLAGS_EXTRA) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d) -c $< -o $@

# Link as a shared library. libXt's own dependency closure is just
# libX11-compat (which provides Xlib, Xrm, atoms, ...) and libc; SDL2 is
# pulled in transitively through libX11-compat.so. -lm and -pthread match
# what libX11-compat uses so XTHREADS-enabled mutex code links cleanly.
#
# Compat stack rpath / install_name discipline lives in
# common.mk:shared_lib_rpath_ldflags so every dependent library
# (libXt-compat, libXpm-compat, libXmu-compat, libXext-compat) ends up
# with the same in-tree resolution behavior.
LIBXT_LDFLAGS := $(call shared_lib_rpath_ldflags,$(notdir $(LIBXT_TARGET)))

$(LIBXT_TARGET): $(LIBXT_OBJS) $(TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(LIBXT_LDFLAGS) -shared -o $@ $(LIBXT_OBJS) \
	    -L$(OUT) -lX11-compat -lm -pthread

.PHONY: libxt
## Build the libXt compatibility shared library
libxt: $(LIBXT_TARGET)

all: $(LIBXT_TARGET)

# Header-dependency files are -included by mk/deps.mk via $(ALL_DEPS),
# which picks up $(LIBXT_OBJS:.o=.d). The generator (-MMD -MP) above is
# what creates them on each object build.
