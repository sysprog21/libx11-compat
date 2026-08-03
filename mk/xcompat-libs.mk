# Xmu archives statically under wasm (the showcase apps need it). Xext, Xft, and
# the other compat libs stay native-only: their symbols are all already in the
# core archive (Xext's XShape family via src/xshape.c, Xft via src/xft.c), so a
# wasm app resolves them from libX11-compat.a and a separate archive would only
# collide at static-link time.
XEXT_COMPAT_TARGET := $(OUT)/libXext-compat.so
ifeq ($(WASM),1)
XMU_COMPAT_TARGET  := $(OUT)/libXmu-compat.a
else
XMU_COMPAT_TARGET  := $(OUT)/libXmu-compat.so
endif
XINERAMA_COMPAT_TARGET := $(OUT)/libXinerama-compat.so
ICE_COMPAT_TARGET := $(OUT)/libICE-compat.so
SM_COMPAT_TARGET := $(OUT)/libSM-compat.so
XFT_COMPAT_TARGET := $(OUT)/libXft-compat.so
XEXT_COMPAT_LDFLAGS := $(call shared_lib_rpath_ldflags,$(notdir $(XEXT_COMPAT_TARGET)))
XMU_COMPAT_LDFLAGS  := $(call shared_lib_rpath_ldflags,$(notdir $(XMU_COMPAT_TARGET)))
XINERAMA_COMPAT_LDFLAGS := $(call shared_lib_rpath_ldflags,$(notdir $(XINERAMA_COMPAT_TARGET)))
ICE_COMPAT_LDFLAGS := $(call shared_lib_rpath_ldflags,$(notdir $(ICE_COMPAT_TARGET)))
SM_COMPAT_LDFLAGS := $(call shared_lib_rpath_ldflags,$(notdir $(SM_COMPAT_TARGET)))
XFT_COMPAT_LDFLAGS := $(call shared_lib_rpath_ldflags,$(notdir $(XFT_COMPAT_TARGET)))
XMU_SRC_DIR := $(OUT)/upstream/src-libXmu
ifeq ($(WASM),1)
XMU_OBJ_DIR := $(OUT)/xmu-wasm
# The xmu-compat shim object lives beside the upstream objects under wasm so it
# is not shared with the native $(OUT)/xmu-compat.o across alternating builds.
XMU_COMPAT_OBJ := $(XMU_OBJ_DIR)/xmu-compat.o
else
XMU_OBJ_DIR := $(OUT)/xmu
XMU_COMPAT_OBJ := $(OUT)/xmu-compat.o
endif
XMU_UPSTREAM_SRC_BASES := \
    Clip.c CloseHook.c CvtCache.c DisplayQue.c Distinct.c DrRndRect.c EditresCom.c ExtAgent.c \
    GrayPixmap.c Initer.c LocBitmap.c RdBitF.c ShapeWidg.c StrToShap.c \
    StrToWidg.c reallocarray.c
XMU_UPSTREAM_SRCS := $(addprefix $(XMU_SRC_DIR)/,$(XMU_UPSTREAM_SRC_BASES))
XMU_UPSTREAM_OBJS := $(patsubst $(XMU_SRC_DIR)/%.c,$(XMU_OBJ_DIR)/%.o,$(XMU_UPSTREAM_SRCS))

$(OUT)/xext-compat.o: compat/xext-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	$(cc_object)

# Order-only on both $(OUT) and the object's own dir: under wasm the object
# lives in $(XMU_OBJ_DIR) (=$(OUT)/xmu-wasm), so declare that dir the way the
# upstream-object rule below does rather than leaning on cc_object's mkdir.
$(XMU_COMPAT_OBJ): compat/xmu-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(LIBXT_STAGED_H) $(SDL_BACKEND_STAMP) | $(OUT) $(XMU_OBJ_DIR)
	$(cc_object)

$(XMU_OBJ_DIR):
	@mkdir -p $@

$(XMU_UPSTREAM_SRCS): $(UPSTREAM_HEADERS_STAMP)

$(XMU_OBJ_DIR)/%.o: $(UPSTREAM_HEADERS_STAMP) $(LIBXT_STAGED_H) \
    $(SDL_BACKEND_STAMP) | $(XMU_OBJ_DIR)
	@echo "  CC      $(XMU_SRC_DIR)/$*.c"
	$(Q)$(CC) $(LIBXT_CPPFLAGS) -iquote $(OUT)/upstream/include/X11/Xmu \
	    $(LEGACY_FP_CFLAGS) $(LIBXT_CFLAGS) $(CFLAGS_EXTRA) \
	    $(DEPFLAGS) -c $(XMU_SRC_DIR)/$*.c -o $@

$(OUT)/xinerama-compat.o: compat/xinerama-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	$(cc_object)

$(OUT)/ice-compat.o: compat/ice-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	$(cc_object)

$(OUT)/sm-compat.o: compat/sm-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	$(cc_object)

$(OUT)/xft-compat.o: src/xft.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	$(cc_object)

$(XEXT_COMPAT_TARGET): $(OUT)/xext-compat.o $(TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(XEXT_COMPAT_LDFLAGS) -shared -o $@ $< -L$(OUT) -lX11-compat

ifeq ($(WASM),1)
$(XMU_COMPAT_TARGET): $(XMU_COMPAT_OBJ) $(XMU_UPSTREAM_OBJS) | $(OUT)
	@echo "  AR      $@"
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $(XMU_COMPAT_OBJ) $(XMU_UPSTREAM_OBJS)
else
$(XMU_COMPAT_TARGET): $(XMU_COMPAT_OBJ) $(XMU_UPSTREAM_OBJS) $(TARGET) \
    $(LIBXT_TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(XMU_COMPAT_LDFLAGS) -shared -o $@ \
	    $(XMU_COMPAT_OBJ) $(XMU_UPSTREAM_OBJS) \
	    -L$(OUT) -lXt-compat -lX11-compat
endif

$(XINERAMA_COMPAT_TARGET): $(OUT)/xinerama-compat.o $(TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(XINERAMA_COMPAT_LDFLAGS) -shared -o $@ $< \
	    -L$(OUT) -lX11-compat

$(ICE_COMPAT_TARGET): $(OUT)/ice-compat.o | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(ICE_COMPAT_LDFLAGS) -shared -o $@ $<

$(SM_COMPAT_TARGET): $(OUT)/sm-compat.o $(ICE_COMPAT_TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(SM_COMPAT_LDFLAGS) -shared -o $@ $< \
	    -L$(OUT) -lICE-compat

$(XFT_COMPAT_TARGET): $(OUT)/xft-compat.o $(TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(XFT_COMPAT_LDFLAGS) -shared -o $@ $< \
	    -L$(OUT) -lX11-compat $(LDLIBS)

.PHONY: xext xmu xinerama ice sm xft
## Build the libXext compatibility shared library
xext: $(XEXT_COMPAT_TARGET)
## Build the minimal libXmu compatibility shared library
xmu: $(XMU_COMPAT_TARGET)
## Build the minimal libXinerama compatibility shared library
xinerama: $(XINERAMA_COMPAT_TARGET)
## Build the minimal libICE compatibility shared library
ice: $(ICE_COMPAT_TARGET)
## Build the minimal libSM compatibility shared library
sm: $(SM_COMPAT_TARGET)
## Build the minimal libXft compatibility shared library
xft: $(XFT_COMPAT_TARGET)

# The wasm leg builds only Xext/Xmu (on demand, as app prerequisites); the
# aggregate all stays native. The other four compat libs are native-only.
ifneq ($(WASM),1)
all: $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(XINERAMA_COMPAT_TARGET) \
    $(ICE_COMPAT_TARGET) $(SM_COMPAT_TARGET) $(XFT_COMPAT_TARGET)
endif

# Header-dependency files are -included by mk/deps.mk via $(ALL_DEPS).
