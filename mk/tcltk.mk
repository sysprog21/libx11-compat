# Build a repo-private Tcl and TkX11 8.6.10 for the Tcl/Tk VLSI workload.
#
# Tcl/Tk's macOS install notes allow Homebrew or XQuartz Tcl/Tk, both of which
# leak host X11 and usually select the Aqua Tk backend. Tcl/Tk needs TkX11, so
# this builds Tcl and Tk from pinned source with Tk's unix (X11) backend wired
# to the in-tree libx11-compat stack, never /opt/X11 or Homebrew.
#
# Two symlink farms wire Tk's X detection to the compat libraries, the same
# shape mk/grace.mk uses:
#   1. TCLTK_SYSROOT: header farm with X11/ and X11/extensions/ so Tk's
#      configure resolves <X11/*.h> to the in-tree + staged-upstream headers
#      via --x-includes.
#   2. TCLTK_LIB_ALIASES: soname farm (libX11.so -> libX11-compat.so, ...)
#      passed as --x-libraries so Tk's generated -lX11 resolves to the shim.
#
# Xft is enabled through the libXft-compat shim: --enable-xft plus a shadow
# xft-config stub and a private xft.pc keep Tk's Xft off host fontconfig and
# bound to the compat stack. XScreenSaver stays disabled (--enable-xss=no); the
# gate is that wish maps a Tk window through libx11-compat.

TCLTK_TCL_VERSION := 8.6.10
TCLTK_TK_VERSION := 8.6.10
TCLTK_TCL_SHA256 := 5196dbf6638e3df8d5c87b5815c8c2b758496eb6f0e41446596c9a4e638d87ed
TCLTK_TK_SHA256 := 63df418a859d0a463347f95ded5cd88a3dd3aaa1ceecaeee362194bc30f3e386

TCLTK_CACHE_DIR := $(OUT)/upstream/.cache
TCLTK_TCL_TARBALL := $(TCLTK_CACHE_DIR)/tcl$(TCLTK_TCL_VERSION)-src.tar.gz
TCLTK_TK_TARBALL := $(TCLTK_CACHE_DIR)/tk$(TCLTK_TK_VERSION)-src.tar.gz
TCLTK_TCL_URL := https://prdownloads.sourceforge.net/tcl/tcl$(TCLTK_TCL_VERSION)-src.tar.gz
TCLTK_TK_URL := https://prdownloads.sourceforge.net/tcl/tk$(TCLTK_TK_VERSION)-src.tar.gz

TCLTK_TCL_SRC := $(OUT)/upstream/tcl$(TCLTK_TCL_VERSION)
TCLTK_TK_SRC := $(OUT)/upstream/tk$(TCLTK_TK_VERSION)
TCLTK_TCL_SOURCE_STAMP := $(TCLTK_TCL_SRC)/.source-stamp
TCLTK_TK_SOURCE_STAMP := $(TCLTK_TK_SRC)/.source-stamp

TCLTK_PREFIX := $(OUT)/tcltk
TCLTK_LIBDIR := $(TCLTK_PREFIX)/lib
TCLTK_BINDIR := $(TCLTK_PREFIX)/bin
TCLTK_TCLSH := $(TCLTK_BINDIR)/tclsh$(basename $(TCLTK_TCL_VERSION))
TCLTK_WISH := $(TCLTK_BINDIR)/wish$(basename $(TCLTK_TK_VERSION))
TCLTK_TCL_BUILD_STAMP := $(TCLTK_PREFIX)/.tcl-stamp
TCLTK_TK_BUILD_STAMP := $(TCLTK_PREFIX)/.tk-stamp

TCLTK_BUILD_DIR := $(OUT)/tcltk-build
TCLTK_TCL_LOG := $(abspath $(TCLTK_BUILD_DIR))/tcl-build.log
TCLTK_TK_LOG := $(abspath $(TCLTK_BUILD_DIR))/tk-build.log

TCLTK_SYSROOT := $(TCLTK_BUILD_DIR)/sysroot
TCLTK_SYSROOT_STAMP := $(TCLTK_SYSROOT)/.stamp
TCLTK_LIB_ALIASES := $(TCLTK_BUILD_DIR)/lib-aliases
TCLTK_LIB_ALIASES_STAMP := $(TCLTK_LIB_ALIASES)/.stamp

# Tk's Xft detection probes xft-config first and only falls back to pkg-config
# when that fails. A host xft-config on PATH would win, dragging in host Xft
# cflags/libs and mixing host fontconfig with libXft-compat. It also overwrites
# any XFT_CFLAGS/XFT_LIBS we pass. So a private xft.pc is the only way to point
# Tk's Xft support at libXft-compat, but only once xft-config is out of the way
# (see TCLTK_SHADOW_BIN). The pc carries no Requires, so pkg-config never pulls
# host xrender/fontconfig/freetype.
TCLTK_PKGCONFIG := $(TCLTK_BUILD_DIR)/pkgconfig

# Shadow directory prepended to PATH for the Tk configure step. It holds a
# stub xft-config that always fails, forcing Tk's Xft detection past its
# xft-config probe onto the pkg-config path we control with the private xft.pc.
TCLTK_SHADOW_BIN := $(TCLTK_BUILD_DIR)/shadow-bin

# Relative paths as prerequisites match the in-tree build rules so a clean
# make builds the compat libraries instead of failing on an absolute path.
TCLTK_XLIB_DEPS := $(XEXT_COMPAT_TARGET) $(XFT_COMPAT_TARGET) $(TARGET)

TCLTK_RPATH_FLAGS := -Wl,-rpath,$(abspath $(OUT)) \
                        -Wl,-rpath,$(abspath $(TCLTK_LIB_ALIASES)) \
                        -Wl,-rpath,$(abspath $(TCLTK_LIBDIR))
ifeq ($(UNAME_S),Linux)
  TCLTK_RPATH_FLAGS += -Wl,-rpath-link,$(abspath $(OUT))
endif

# Tk's configure sets LIB_RUNTIME_DIR to "libdir:<x-libraries>" so the extra X
# lib dir lands on the ELF rpath. On Darwin the same colon list is (wrongly)
# reused as the single -install_name, producing a bogus id like
# ".../lib:.../lib-aliases/libtk8.6.dylib" that dyld cannot resolve. Override
# DYLIB_INSTALL_DIR to just the private libdir so libtk gets a valid install
# name and wish records it correctly. Linux keeps the colon list as a real rpath.
TCLTK_MAKE_ARGS :=
ifeq ($(UNAME_S),Darwin)
  TCLTK_MAKE_ARGS += DYLIB_INSTALL_DIR=$(abspath $(TCLTK_LIBDIR))
endif

# Tcl/Tk 8.6.10 predate clang's C99 tightening; a couple of their configure
# conftests rely on implicit declarations that clang 16+ rejects as errors.
# Dropping those to warnings keeps detection working without touching sources.
TCLTK_LEGACY_CFLAGS := -Wno-implicit-function-declaration -Wno-implicit-int

$(TCLTK_TCL_TARBALL): | $(TCLTK_CACHE_DIR)
	@echo "  FETCH   $(TCLTK_TCL_URL)"
	$(Q)curl -fsSL --max-time 600 -o $@.tmp $(TCLTK_TCL_URL)
	$(Q)echo "$(TCLTK_TCL_SHA256)  $@.tmp" | shasum -a 256 -c -
	$(Q)mv $@.tmp $@

$(TCLTK_TK_TARBALL): | $(TCLTK_CACHE_DIR)
	@echo "  FETCH   $(TCLTK_TK_URL)"
	$(Q)curl -fsSL --max-time 600 -o $@.tmp $(TCLTK_TK_URL)
	$(Q)echo "$(TCLTK_TK_SHA256)  $@.tmp" | shasum -a 256 -c -
	$(Q)mv $@.tmp $@

$(TCLTK_TCL_SOURCE_STAMP): $(TCLTK_TCL_TARBALL) mk/tcltk.mk
	@echo "  EXTRACT tcl$(TCLTK_TCL_VERSION)"
	$(Q)rm -rf $(TCLTK_TCL_SRC)
	$(Q)mkdir -p $(dir $(TCLTK_TCL_SRC))
	$(Q)tar -xzf $(TCLTK_TCL_TARBALL) -C $(dir $(TCLTK_TCL_SRC))
	$(Q)touch $@

$(TCLTK_TK_SOURCE_STAMP): $(TCLTK_TK_TARBALL) mk/tcltk.mk
	@echo "  EXTRACT tk$(TCLTK_TK_VERSION)"
	$(Q)rm -rf $(TCLTK_TK_SRC)
	$(Q)mkdir -p $(dir $(TCLTK_TK_SRC))
	$(Q)tar -xzf $(TCLTK_TK_TARBALL) -C $(dir $(TCLTK_TK_SRC))
	$(Q)touch $@

# Header farm: X11/ and X11/extensions/ symlinks to the in-tree and staged
# upstream Xlib headers, same shape as mk/grace.mk minus the Motif subdir.
$(TCLTK_SYSROOT_STAMP): $(UPSTREAM_HEADERS_STAMP) mk/tcltk.mk
	@echo "  SYSROOT tcltk"
	$(Q)rm -rf $(TCLTK_SYSROOT)
	$(Q)mkdir -p $(TCLTK_SYSROOT)/X11/extensions
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ "$$b" = extensions ] && continue; \
	    ln -sf "$$e" "$(TCLTK_SYSROOT)/X11/$$b"; \
	done
	$(Q)for e in $(abspath $(OUT)/upstream/include)/X11/extensions/* \
	             $(abspath include)/X11/extensions/*; do \
	    ln -sf "$$e" "$(TCLTK_SYSROOT)/X11/extensions/$$(basename "$$e")"; \
	done
	$(Q)for e in $(abspath include)/X11/*; do \
	    b=$$(basename "$$e"); \
	    [ -e "$(TCLTK_SYSROOT)/X11/$$b" ] || \
	        ln -sf "$$e" "$(TCLTK_SYSROOT)/X11/$$b"; \
	done
	$(Q)touch $@

# soname farm: Tk's configure emits -lX11 (and -lXext when it probes it); point
# those canonical names at the compat shims so no host X11 is ever linked.
$(TCLTK_LIB_ALIASES_STAMP): $(TCLTK_XLIB_DEPS) mk/tcltk.mk
	@mkdir -p $(TCLTK_LIB_ALIASES)
	$(Q)rm -f $(TCLTK_LIB_ALIASES)/lib*.so $(TCLTK_LIB_ALIASES)/lib*.dylib
	$(Q)set -e; for pair in \
	    libX11.so:libX11-compat.so \
	    libXext.so:libXext-compat.so \
	    libXft.so:libXft-compat.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(TCLTK_LIB_ALIASES)/$$alias"; \
	done
ifeq ($(UNAME_S),Darwin)
	$(Q)set -e; for pair in \
	    libX11.dylib:libX11-compat.so \
	    libXext.dylib:libXext-compat.so \
	    libXft.dylib:libXft-compat.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(TCLTK_LIB_ALIASES)/$$alias"; \
	done
endif
	$(Q)mkdir -p $(TCLTK_PKGCONFIG)
	$(Q)printf '%s\n' \
	    'Name: Xft' \
	    'Description: libx11-compat Xft shim' \
	    'Version: 2.3.4' \
	    'Cflags: -I$(abspath include) -I$(abspath $(OUT)/upstream/include)' \
	    'Libs: -L$(abspath $(TCLTK_LIB_ALIASES)) -lXft' \
	    > $(TCLTK_PKGCONFIG)/xft.pc
	# Tk's Xft detection unconditionally appends `pkg-config fontconfig` on top of
	# xft. The Fc* API lives inside libXft-compat, so shadow host fontconfig with
	# an empty-Libs pc: Tk gets the header path but links no host libfontconfig,
	# keeping every Fc* call bound to the shim (mixing the two FcPattern layouts
	# would crash). Same for freetype, which nothing here calls directly.
	$(Q)printf '%s\n' \
	    'Name: Fontconfig' 'Description: libx11-compat fontconfig shim' \
	    'Version: 2.18.2' 'Cflags: -I$(abspath include)' 'Libs:' \
	    > $(TCLTK_PKGCONFIG)/fontconfig.pc
	$(Q)printf '%s\n' \
	    'Name: FreeType2' 'Description: libx11-compat freetype shim' \
	    'Version: 24.0.0' 'Cflags:' 'Libs:' \
	    > $(TCLTK_PKGCONFIG)/freetype2.pc
	# Stub xft-config so Tk's Xft probe fails it and falls back to the private
	# pkg-config path above instead of finding a host xft-config on PATH.
	$(Q)mkdir -p $(TCLTK_SHADOW_BIN)
	$(Q)printf '%s\n' '#!/bin/sh' 'exit 1' > $(TCLTK_SHADOW_BIN)/xft-config
	$(Q)chmod 0755 $(TCLTK_SHADOW_BIN)/xft-config
	$(Q)touch $@

# Build Tcl from tcl*/unix into the private prefix. Out-of-tree build keeps the
# extracted source clean so a reconfigure does not need a fresh untar.
$(TCLTK_TCL_BUILD_STAMP): $(TCLTK_TCL_SOURCE_STAMP) mk/tcltk.mk
	@mkdir -p $(TCLTK_BUILD_DIR)/tcl $(TCLTK_PREFIX)
	$(Q)rm -f $(TCLTK_TCL_LOG)
	@echo "  CONF    tcl$(TCLTK_TCL_VERSION)"
	$(Q)cd $(TCLTK_BUILD_DIR)/tcl && \
	    CC='$(CC)' CFLAGS='$(TCLTK_LEGACY_CFLAGS)' \
	    $(abspath $(TCLTK_TCL_SRC))/unix/configure \
	        --prefix=$(abspath $(TCLTK_PREFIX)) \
	        --enable-shared \
	        >> $(TCLTK_TCL_LOG) 2>&1 || { \
	        echo "  FAIL    see $(TCLTK_TCL_LOG)" >&2; tail -40 $(TCLTK_TCL_LOG) >&2; exit 1; }
	@echo "  MAKE    tcl$(TCLTK_TCL_VERSION)"
	$(Q)env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(TCLTK_BUILD_DIR)/tcl \
	    >> $(TCLTK_TCL_LOG) 2>&1 || { \
	    echo "  FAIL    see $(TCLTK_TCL_LOG)" >&2; tail -40 $(TCLTK_TCL_LOG) >&2; exit 1; }
	$(Q)env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(TCLTK_BUILD_DIR)/tcl install \
	    >> $(TCLTK_TCL_LOG) 2>&1 || { \
	    echo "  FAIL    see $(TCLTK_TCL_LOG)" >&2; tail -40 $(TCLTK_TCL_LOG) >&2; exit 1; }
	$(Q)touch $@

# Build TkX11 from tk*/unix against the private Tcl and the compat X stack. The
# explicit --x-includes/--x-libraries short-circuit AC_PATH_X so configure never
# reaches for /opt/X11 or Homebrew.
$(TCLTK_TK_BUILD_STAMP): $(TCLTK_TK_SOURCE_STAMP) $(TCLTK_TCL_BUILD_STAMP) \
    $(TCLTK_SYSROOT_STAMP) $(TCLTK_LIB_ALIASES_STAMP) mk/tcltk.mk
	@mkdir -p $(TCLTK_BUILD_DIR)/tk
	$(Q)rm -f $(TCLTK_TK_LOG)
	@echo "  CONF    tk$(TCLTK_TK_VERSION)"
	$(Q)cd $(TCLTK_BUILD_DIR)/tk && \
	    CC='$(CC)' CFLAGS='$(TCLTK_LEGACY_CFLAGS)' \
	    LDFLAGS='-L$(abspath $(TCLTK_LIB_ALIASES)) -L$(abspath $(OUT)) $(TCLTK_RPATH_FLAGS)' \
	    PATH='$(abspath $(TCLTK_SHADOW_BIN))':"$$PATH" \
	    PKG_CONFIG_PATH='$(abspath $(TCLTK_PKGCONFIG))'$${PKG_CONFIG_PATH:+:$$PKG_CONFIG_PATH} \
	    $(abspath $(TCLTK_TK_SRC))/unix/configure \
	        --prefix=$(abspath $(TCLTK_PREFIX)) \
	        --with-tcl=$(abspath $(TCLTK_LIBDIR)) \
	        --enable-shared \
	        --with-x \
	        --x-includes=$(abspath $(TCLTK_SYSROOT)) \
	        --x-libraries=$(abspath $(TCLTK_LIB_ALIASES)) \
	        --enable-xft=yes \
	        --enable-xss=no \
	        >> $(TCLTK_TK_LOG) 2>&1 || { \
	        echo "  FAIL    see $(TCLTK_TK_LOG)" >&2; tail -40 $(TCLTK_TK_LOG) >&2; exit 1; }
	@echo "  MAKE    tk$(TCLTK_TK_VERSION)"
	$(Q)env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(TCLTK_BUILD_DIR)/tk \
	    $(TCLTK_MAKE_ARGS) \
	    >> $(TCLTK_TK_LOG) 2>&1 || { \
	    echo "  FAIL    see $(TCLTK_TK_LOG)" >&2; tail -40 $(TCLTK_TK_LOG) >&2; exit 1; }
	$(Q)env -u MAKEFLAGS -u MFLAGS $(MAKE) -C $(TCLTK_BUILD_DIR)/tk install \
	    $(TCLTK_MAKE_ARGS) \
	    >> $(TCLTK_TK_LOG) 2>&1 || { \
	    echo "  FAIL    see $(TCLTK_TK_LOG)" >&2; tail -40 $(TCLTK_TK_LOG) >&2; exit 1; }
	$(Q)touch $@

.PHONY: tcltk tcltk-clean
## Build repo-private Tcl and TkX11 for Tcl/Tk against libx11-compat
tcltk: $(TCLTK_TK_BUILD_STAMP)

tcltk-clean:
	@echo "  CLEAN   tcltk"
	$(Q)rm -rf $(TCLTK_PREFIX) $(TCLTK_BUILD_DIR) \
	    $(TCLTK_TCL_SRC) $(TCLTK_TK_SRC)
