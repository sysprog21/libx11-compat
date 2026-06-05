PKGCONFIG_DIR := $(OUT)/pkgconfig
PKGCONFIG_FILES := \
    $(PKGCONFIG_DIR)/x11.pc \
    $(PKGCONFIG_DIR)/xpm.pc \
    $(PKGCONFIG_DIR)/xt.pc \
    $(PKGCONFIG_DIR)/xmu.pc \
    $(PKGCONFIG_DIR)/xext.pc \
    $(PKGCONFIG_DIR)/xinerama.pc

$(PKGCONFIG_DIR):
	@mkdir -p $@

define write_pc
	@{ \
	    echo "prefix=$(abspath $(OUT))"; \
	    echo "exec_prefix=$(abspath $(OUT))"; \
	    echo "libdir=$(abspath $(OUT))"; \
	    echo "includedir=$$(pwd)/include"; \
	    echo "upstreamincludedir=$$(pwd)/$(OUT)/upstream/include"; \
	    echo "libxtbuildincludedir=$$(pwd)/include/libxt-build"; \
	    echo ""; \
	    echo "Name: $(1)"; \
	    echo "Description: libx11-compat $(1) shim"; \
	    echo "Version: $(2)"; \
	    echo "Libs: -L$(abspath $(OUT)) $(3)"; \
	    echo "Cflags: -I$$(pwd)/include -I$$(pwd)/$(OUT)/upstream/include $(4)"; \
	} > $@
endef

$(PKGCONFIG_DIR)/x11.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,x11,1.8.13,-lX11-compat,)

$(PKGCONFIG_DIR)/xpm.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xpm,3.5.19,-lXpm-compat -lX11-compat,)

$(PKGCONFIG_DIR)/xt.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xt,1.3.1,-lXt-compat -lX11-compat,-I$$(pwd)/include/libxt-build)

$(PKGCONFIG_DIR)/xmu.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xmu,1.0,-lXmu-compat -lXt-compat -lX11-compat,-I$$(pwd)/include/libxt-build)

$(PKGCONFIG_DIR)/xext.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xext,1.0,-lXext-compat -lX11-compat,)

$(PKGCONFIG_DIR)/xinerama.pc: $(UPSTREAM_HEADERS_STAMP) mk/pkgconfig.mk | $(PKGCONFIG_DIR)
	@echo "  PC      $@"
	$(call write_pc,xinerama,1.1,-lXinerama-compat -lX11-compat,)

.PHONY: pkgconfig
## Generate pkg-config files for the compatibility libraries
pkgconfig: $(PKGCONFIG_FILES)

all: $(PKGCONFIG_FILES)
