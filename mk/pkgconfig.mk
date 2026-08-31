PKGCONFIG_DIR := $(OUT)/pkgconfig
PKGCONFIG_FILES := \
    $(PKGCONFIG_DIR)/x11.pc \
    $(PKGCONFIG_DIR)/xpm.pc \
    $(PKGCONFIG_DIR)/xt.pc \
    $(PKGCONFIG_DIR)/xmu.pc \
    $(PKGCONFIG_DIR)/xext.pc \
    $(PKGCONFIG_DIR)/xinerama.pc \
    $(PKGCONFIG_DIR)/ice.pc \
    $(PKGCONFIG_DIR)/sm.pc \
    $(PKGCONFIG_DIR)/xaw7.pc \
    $(PKGCONFIG_DIR)/xproto.pc \
    $(PKGCONFIG_DIR)/fontconfig.pc \
    $(PKGCONFIG_DIR)/xrender.pc \
    $(PKGCONFIG_DIR)/xft.pc
ifeq ($(XCB),1)
PKGCONFIG_FILES += $(PKGCONFIG_DIR)/xcb.pc $(PKGCONFIG_DIR)/x11-xcb.pc
endif

$(PKGCONFIG_DIR):
	@mkdir -p $@

# Libs and Cflags refer to the variables above rather than repeating absolute
# paths, so `make install` can relocate a file by rewriting the variable lines
# alone (mk/install.mk). pkg-config expands them, so an in-tree consumer reading
# build/pkgconfig/ sees exactly the paths it saw before.
define write_pc
	@{ \
	    echo "prefix=$(abspath $(OUT))"; \
	    echo "exec_prefix=\$${prefix}"; \
	    echo "libdir=\$${prefix}"; \
	    echo "includedir=$$(pwd)/include"; \
	    echo "upstreamincludedir=$$(pwd)/$(OUT)/upstream/include"; \
	    echo "libxtbuildincludedir=$$(pwd)/include/libxt-build"; \
	    echo ""; \
	    echo "Name: $(1)"; \
	    echo "Description: libx11-compat $(1) shim"; \
	    echo "Version: $(2)"; \
	    $(if $(5),echo "Requires: $(5)";) \
	    echo "Libs: -L\$${libdir} $(3)"; \
	    echo "Cflags: -I\$${includedir} -I\$${upstreamincludedir} $(4)"; \
	} > $@
endef

$(PKGCONFIG_DIR)/x11.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,x11,1.8.13,-lX11-compat,)

$(PKGCONFIG_DIR)/xcb.pc: mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xcb,1.15,-lxcb-compat,)

$(PKGCONFIG_DIR)/x11-xcb.pc: mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,x11-xcb,1.8.0,-lX11-xcb-compat,,x11 xcb)

$(PKGCONFIG_DIR)/xpm.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xpm,3.5.19,-lXpm-compat -lX11-compat,)

$(PKGCONFIG_DIR)/xt.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xt,1.3.1,-lXt-compat -lX11-compat,-I\$${libxtbuildincludedir})

$(PKGCONFIG_DIR)/xmu.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xmu,1.0,-lXmu-compat -lXt-compat -lX11-compat,-I\$${libxtbuildincludedir})

$(PKGCONFIG_DIR)/xext.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xext,1.0,-lXext-compat -lX11-compat,)

$(PKGCONFIG_DIR)/xinerama.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xinerama,1.1,-lXinerama-compat -lX11-compat,)

$(PKGCONFIG_DIR)/ice.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,ice,1.1,-lICE-compat,)

$(PKGCONFIG_DIR)/sm.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,sm,1.2,-lSM-compat -lICE-compat,)

$(PKGCONFIG_DIR)/xaw7.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xaw7,1.0.16,-lXaw-compat -lXt-compat -lXmu-compat -lXpm-compat -lX11-compat,-I\$${libxtbuildincludedir})

# xproto.pc covers protocol-only headers. Apps like xclock pkg-config it
# to get the X11/Xproto.h include path; the upstream-headers stamp already
# stages every xproto header under build/upstream/include/X11/, so there
# is no library to link against - just the same -I paths the other shims
# emit.
$(PKGCONFIG_DIR)/xproto.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xproto,7.0.31,,)

# fontconfig.pc routes clients (Xfig) through libXft-compat.so for the
# whole Fc* surface instead of the host libfontconfig. libXft-compat
# exports its own FcPatternCreate / FcPatternDestroy / etc with a struct
# layout that has nothing in common with upstream fontconfig's; linking
# both at once corrupts patterns the moment one library walks an object
# created by the other. Provide this .pc so a pkg-config probe lands on
# the shim instead of the host package, and PKG_CONFIG_PATH wins over
# system /opt/homebrew on Darwin and /usr/lib/x86_64-linux-gnu on Linux.
$(PKGCONFIG_DIR)/fontconfig.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,fontconfig,2.13.99,-lXft-compat -lX11-compat,)

# Xrender.pc is header-only on this shim; the actual XRender* entry
# points reject any call with WARN_UNIMPLEMENTED. Xfig and Osiris pkg-
# config xrender as a transitive dep of Xft; the cflags get them the
# Xrender.h shim and the empty libs line stops them from accidentally
# linking the host libXrender.
$(PKGCONFIG_DIR)/xrender.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xrender,0.9.10,-lX11-compat,)

# xft.pc lets a downstream that pkg-configs xft (Osiris -Dxft=enabled,
# Xfig's PKG_CHECK_MODULES) land on libXft-compat instead of host libXft.
$(PKGCONFIG_DIR)/xft.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xft,2.3.99,-lXft-compat -lX11-compat,)

.PHONY: pkgconfig
## Generate pkg-config files for the compatibility libraries
pkgconfig: $(PKGCONFIG_FILES)

all: $(PKGCONFIG_FILES)
