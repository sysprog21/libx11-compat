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

# Libraries a downstream links by their standard X11 SONAME (each gets a
# libNAME.so -> libNAME-compat.so alias).
XCOMPAT_INSTALL_ALIASED := X11 Xft Xext Xt Xmu Xaw Xpm Xinerama ICE SM
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
install: $(XCOMPAT_INSTALL_LIB_FILES) $(UPSTREAM_HEADERS_STAMP)
	@echo "  INSTALL $(DESTDIR)$(PREFIX)"
	$(Q)mkdir -p "$(DESTDIR)$(PREFIX)/lib" "$(DESTDIR)$(PREFIX)/include"
	$(Q)for l in $(XCOMPAT_INSTALL_ALIASED); do \
	    cp "$(OUT)/lib$$l-compat.so" "$(DESTDIR)$(PREFIX)/lib/" && \
	    ln -sf "lib$$l-compat.so" "$(DESTDIR)$(PREFIX)/lib/lib$$l.so" || exit 1; \
	done
	$(Q)for w in $(XCOMPAT_INSTALL_WRAPPERS); do \
	    cp "$(OUT)/lib$$w.so" "$(DESTDIR)$(PREFIX)/lib/"; \
	done
	$(Q)cp -R "$(UPSTREAM_HEADERS_DIR)/." "$(DESTDIR)$(PREFIX)/include/"
	$(Q)for d in $(XCOMPAT_INSTALL_HEADER_DIRS); do \
	    if [ -d "include/$$d" ]; then \
	        mkdir -p "$(DESTDIR)$(PREFIX)/include/$$d"; \
	        cp -R "include/$$d/." "$(DESTDIR)$(PREFIX)/include/$$d/"; \
	    fi; \
	done
