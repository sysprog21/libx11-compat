# Install the shim so a downstream build can link against libx11-compat as an
# ordinary X11.  PREFIX/lib gets the compat shared libraries plus standard-name
# aliases (libX11.so -> libX11-compat.so, ...) so a plain `-lX11 -lXft -lXext`
# resolves to the shim, and PREFIX/include gets the public X11 API headers (the
# staged upstream snapshot with the shim's include/ overrides on top).  This is
# what lets, e.g., xwpe build a native macOS binary with no XQuartz via
# `./configure --with-libx11-compat=$PREFIX`.  The libraries already carry their
# @rpath install_name (see mk/library.mk), so a plain copy is relocatable.
#
# Honour the usual conventions: `make install PREFIX=/usr/local DESTDIR=/pkgroot`.
PREFIX ?= /usr/local
DESTDIR ?=

# Strip local symbols from the installed libraries: the export surface (dynamic
# symbols) is untouched, so only the internal names in .symtab go, shaving ~6%
# off each .so. The in-tree build/ copies keep their local symbols so crash
# backtraces stay symbolic during development; only the deployed artifact slims.
# Override STRIP (e.g. STRIP=: for a packager that strips its own way, or a
# cross strip) to change or disable it.
STRIP ?= strip

# strip -x rewrites the Mach-O, which invalidates the ad-hoc signature ld64 puts
# on arm64 binaries. Apple's own strip re-signs, but lld or an older toolchain
# does not, and dyld then refuses the installed dylib ("code signature invalid").
# Re-sign explicitly on Darwin so the default install is safe on any toolchain;
# elsewhere CODESIGN_RESIGN is the no-op colon builtin that just swallows the path.
CODESIGN_RESIGN := $(if $(filter Darwin,$(UNAME_S)),codesign --force --sign - ,:)

# Libraries a downstream links by their standard X11 SONAME (each gets a
# libNAME.so -> libNAME-compat.so alias).
XCOMPAT_INSTALL_ALIASED := X11 Xft Xext Xt Xmu Xaw Xpm Xinerama ICE SM
ifeq ($(XCB),1)
XCOMPAT_INSTALL_ALIASED += X11-xcb xcb
endif
# Runtime-only wrappers libX11-compat dlopens; installed without an alias. Only
# built for the SDL2 backend -- under SDL_BACKEND=sdl3 the stack links libSDL3
# directly (SDL_USE_WRAPPER=0), so there is nothing to install.
XCOMPAT_INSTALL_WRAPPERS :=
ifeq ($(SDL_USE_WRAPPER),1)
XCOMPAT_INSTALL_WRAPPERS := SDL2-x11compat SDL2_ttf-x11compat
endif
# Public header directories under include/ (the *-build helper dirs are private).
XCOMPAT_INSTALL_HEADER_DIRS := X11 fontconfig GL GLES KHR

XCOMPAT_INSTALL_LIB_FILES := \
    $(foreach l,$(XCOMPAT_INSTALL_ALIASED),$(OUT)/lib$(l)-compat.so) \
    $(foreach w,$(XCOMPAT_INSTALL_WRAPPERS),$(OUT)/lib$(w).so)

.PHONY: install
# x11.pc describes libX11-compat, which every install ships, so it is not an
# XCB decision. It also has to travel with x11-xcb.pc, whose "Requires: x11"
# would otherwise resolve against whatever real libX11 the host has and quietly
# link the system library next to the shim.
XCOMPAT_INSTALL_PC := x11$(if $(filter 1,$(XCB)), xcb x11-xcb)

install: $(XCOMPAT_INSTALL_LIB_FILES) $(UPSTREAM_HEADERS_STAMP) \
    $(addprefix $(PKGCONFIG_DIR)/,$(addsuffix .pc,$(XCOMPAT_INSTALL_PC)))
	@echo "  INSTALL $(DESTDIR)$(PREFIX)"
	$(Q)mkdir -p "$(DESTDIR)$(PREFIX)/lib" "$(DESTDIR)$(PREFIX)/include" \
	    $(if $(XCOMPAT_INSTALL_PC),"$(DESTDIR)$(PREFIX)/lib/pkgconfig")
	$(Q)for l in $(XCOMPAT_INSTALL_ALIASED); do \
	    cp "$(OUT)/lib$$l-compat.so" "$(DESTDIR)$(PREFIX)/lib/" && \
	    $(STRIP) -x "$(DESTDIR)$(PREFIX)/lib/lib$$l-compat.so" && \
	    $(CODESIGN_RESIGN) "$(DESTDIR)$(PREFIX)/lib/lib$$l-compat.so" && \
	    ln -sf "lib$$l-compat.so" "$(DESTDIR)$(PREFIX)/lib/lib$$l.so" || exit 1; \
	done
	$(Q)for w in $(XCOMPAT_INSTALL_WRAPPERS); do \
	    cp "$(OUT)/lib$$w.so" "$(DESTDIR)$(PREFIX)/lib/" && \
	    $(STRIP) -x "$(DESTDIR)$(PREFIX)/lib/lib$$w.so" && \
	    $(CODESIGN_RESIGN) "$(DESTDIR)$(PREFIX)/lib/lib$$w.so" || exit 1; \
	done
	$(Q)cp -R "$(UPSTREAM_HEADERS_DIR)/." "$(DESTDIR)$(PREFIX)/include/"
	$(Q)for d in $(XCOMPAT_INSTALL_HEADER_DIRS); do \
	    if [ -d "include/$$d" ]; then \
	        mkdir -p "$(DESTDIR)$(PREFIX)/include/$$d"; \
	        cp -R "include/$$d/." "$(DESTDIR)$(PREFIX)/include/$$d/"; \
	    fi; \
	done
	$(Q)prefix_escaped=$$(printf '%s' "$(PREFIX)" | sed -e 's/[&|\\]/\\&/g'); \
	for pc in $(XCOMPAT_INSTALL_PC); do \
	    sed -e "s|^prefix=.*|prefix=$$prefix_escaped|" \
	        -e 's|^exec_prefix=.*|exec_prefix=$${prefix}|' \
	        -e 's|^libdir=.*|libdir=$${prefix}/lib|' \
	        -e 's|^includedir=.*|includedir=$${prefix}/include|' \
	        -e 's|^upstreamincludedir=.*|upstreamincludedir=$${includedir}|' \
	        -e 's|^libxtbuildincludedir=.*|libxtbuildincludedir=$${includedir}|' \
	        "$(PKGCONFIG_DIR)/$$pc.pc" \
	        > "$(DESTDIR)$(PREFIX)/lib/pkgconfig/$$pc.pc" || exit 1; \
	done
