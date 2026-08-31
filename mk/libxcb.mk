# Core XCB ABI shim over libX11-compat. Built only for XCB=1 (mk/config.mk).
#
# The public xcb/ headers are staged into $(OUT)/upstream/include by
# mk/upstream-headers.mk, which is included after this file, so the stamp names
# are defined here the way mk/libxt.mk does it. Without them the order-only
# prerequisite below expands to nothing and a parallel clean build can compile
# these objects before the headers exist.
UPSTREAM_HEADERS_DIR ?= $(OUT)/upstream/include
UPSTREAM_HEADERS_STAMP ?= $(UPSTREAM_HEADERS_DIR)/.upstream-stamp

ifeq ($(XCB),1)
XCB_COMPAT_TARGET := $(OUT)/libxcb-compat.so
XCB_COMPAT_OBJS := $(OUT)/xcb-compat.o $(OUT)/xcb-requests.o
XCB_DEFINED := $(OUT)/libxcb-compat.defined-syms
XCB_EXPORT_LIST := $(OUT)/libxcb-compat.$(if $(filter Darwin,$(UNAME_S)),exports,map)
XCB_EXPORT_FORMAT := $(if $(filter Darwin,$(UNAME_S)),macho,elf)
X11_XCB_COMPAT_TARGET := $(OUT)/libX11-xcb-compat.so
X11_XCB_COMPAT_OBJ := $(OUT)/xlib-xcb-compat.o
X11_XCB_DEFINED := $(OUT)/libX11-xcb-compat.defined-syms
X11_XCB_EXPORT_LIST := $(OUT)/libX11-xcb-compat.$(if $(filter Darwin,$(UNAME_S)),exports,map)
XCB_RPATH_FLAGS := $(call shared_lib_rpath_ldflags,$(notdir $(XCB_COMPAT_TARGET)))

$(OUT)/xcb-compat.o: compat/xcb-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	$(cc_object)

$(OUT)/xcb-requests.o: compat/xcb-requests.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	$(cc_object)

$(XCB_DEFINED): $(XCB_COMPAT_OBJS) | $(OUT)
	@echo "  GEN     $@"
	$(Q)nm -g $(XCB_COMPAT_OBJS) 2>/dev/null \
	    | awk '$$1 ~ /^[0-9a-fA-F]+$$/ { print $$NF }' \
	    | $(if $(filter Darwin,$(UNAME_S)),sed 's/^_//',cat) \
	    | LC_ALL=C sort -u > $@
	$(Q)test -s $@

$(XCB_EXPORT_LIST): tests/xcb-symbols.txt $(XCB_DEFINED) scripts/gen-export-list.sh | $(OUT)
	@echo "  GEN     $@"
	$(Q)scripts/gen-export-list.sh "$(XCB_EXPORT_FORMAT)" 0 \
	    $(XCB_DEFINED) tests/xcb-symbols.txt tests/xcb-private-symbols.txt > $@

# -Bsymbolic for the same reason mk/library.mk applies it to libX11-compat: a
# process that also loads the host libxcb.so would otherwise let its
# xcb_get_setup and friends interpose ours, and the host implementation would
# then be handed this shim's connection object, which is a different type.
XCB_EXPORT_FLAGS := $(if $(filter Darwin,$(UNAME_S)),-Wl$(comma)-exported_symbols_list$(comma)$(XCB_EXPORT_LIST),-Wl$(comma)--version-script=$(XCB_EXPORT_LIST) -Wl$(comma)-Bsymbolic)
$(XCB_COMPAT_TARGET): $(XCB_COMPAT_OBJS) $(TARGET) $(XCB_EXPORT_LIST) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(XCB_RPATH_FLAGS) $(XCB_EXPORT_FLAGS) -shared \
	    -o $@ $(XCB_COMPAT_OBJS) -L$(OUT) -lX11-compat

.PHONY: xcb
## Build the core XCB compatibility shared library (needs XCB=1)
xcb: $(XCB_COMPAT_TARGET)
all: $(XCB_COMPAT_TARGET)

$(X11_XCB_COMPAT_OBJ): compat/xlib-xcb-compat.c $(UPSTREAM_HEADERS_STAMP) | $(OUT)
	$(cc_object)

$(X11_XCB_DEFINED): $(X11_XCB_COMPAT_OBJ) | $(OUT)
	@echo "  GEN     $@"
	$(Q)nm -g $< 2>/dev/null | awk '$$1 ~ /^[0-9a-fA-F]+$$/ { print $$NF }' \
	    | $(if $(filter Darwin,$(UNAME_S)),sed 's/^_//',cat) | LC_ALL=C sort -u > $@
	$(Q)test -s $@

$(X11_XCB_EXPORT_LIST): tests/xlib-xcb-symbols.txt $(X11_XCB_DEFINED) scripts/gen-export-list.sh | $(OUT)
	@echo "  GEN     $@"
	$(Q)scripts/gen-export-list.sh "$(XCB_EXPORT_FORMAT)" 0 \
	    $(X11_XCB_DEFINED) tests/xlib-xcb-symbols.txt > $@

$(X11_XCB_COMPAT_TARGET): $(X11_XCB_COMPAT_OBJ) $(XCB_COMPAT_TARGET) $(TARGET) $(X11_XCB_EXPORT_LIST) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(call shared_lib_rpath_ldflags,$(notdir $@)) \
	    $(if $(filter Darwin,$(UNAME_S)),-Wl$(comma)-exported_symbols_list$(comma)$(X11_XCB_EXPORT_LIST),-Wl$(comma)--version-script=$(X11_XCB_EXPORT_LIST) -Wl$(comma)-Bsymbolic) \
	    -shared -o $@ $(X11_XCB_COMPAT_OBJ) -L$(OUT) -lxcb-compat -lX11-compat

all: $(X11_XCB_COMPAT_TARGET)
endif
