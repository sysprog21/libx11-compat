XEXT_COMPAT_TARGET := $(OUT)/libXext-compat.so
XMU_COMPAT_TARGET  := $(OUT)/libXmu-compat.so
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
XMU_OBJ_DIR := $(OUT)/xmu
XMU_UPSTREAM_SRC_BASES := \
    Clip.c CloseHook.c CvtCache.c DisplayQue.c Distinct.c DrRndRect.c EditresCom.c ExtAgent.c \
    GrayPixmap.c Initer.c LocBitmap.c RdBitF.c ShapeWidg.c StrToShap.c \
    StrToWidg.c reallocarray.c
XMU_UPSTREAM_SRCS := $(addprefix $(XMU_SRC_DIR)/,$(XMU_UPSTREAM_SRC_BASES))
XMU_UPSTREAM_OBJS := $(patsubst $(XMU_SRC_DIR)/%.c,$(XMU_OBJ_DIR)/%.o,$(XMU_UPSTREAM_SRCS))

$(OUT)/xext-compat.o: compat/xext-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d) -c $< -o $@

$(OUT)/xmu-compat.o: compat/xmu-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(LIBXT_STAGED_H) $(SDL_BACKEND_STAMP) | $(OUT)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d) -c $< -o $@

$(XMU_OBJ_DIR):
	@mkdir -p $@

$(XMU_UPSTREAM_SRCS): $(UPSTREAM_HEADERS_STAMP)

$(XMU_OBJ_DIR)/%.o: $(UPSTREAM_HEADERS_STAMP) $(LIBXT_STAGED_H) \
    $(SDL_BACKEND_STAMP) | $(XMU_OBJ_DIR)
	@echo "  CC      $(XMU_SRC_DIR)/$*.c"
	$(Q)$(CC) $(LIBXT_CPPFLAGS) -iquote $(OUT)/upstream/include/X11/Xmu \
	    $(CFLAGS) $(LIBXT_CFLAGS) $(CFLAGS_EXTRA) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d) \
	    -c $(XMU_SRC_DIR)/$*.c -o $@

$(OUT)/xinerama-compat.o: compat/xinerama-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d) -c $< -o $@

$(OUT)/ice-compat.o: compat/ice-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d) -c $< -o $@

$(OUT)/sm-compat.o: compat/sm-compat.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d) -c $< -o $@

$(OUT)/xft-compat.o: src/xft.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(OUT)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) \
	    -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d) -c $< -o $@

$(XEXT_COMPAT_TARGET): $(OUT)/xext-compat.o $(TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(XEXT_COMPAT_LDFLAGS) -shared -o $@ $< -L$(OUT) -lX11-compat

$(XMU_COMPAT_TARGET): $(OUT)/xmu-compat.o $(XMU_UPSTREAM_OBJS) $(TARGET) \
    $(LIBXT_TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(XMU_COMPAT_LDFLAGS) -shared -o $@ \
	    $(OUT)/xmu-compat.o $(XMU_UPSTREAM_OBJS) \
	    -L$(OUT) -lXt-compat -lX11-compat

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

all: $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(XINERAMA_COMPAT_TARGET) \
    $(ICE_COMPAT_TARGET) $(SM_COMPAT_TARGET) $(XFT_COMPAT_TARGET)

# Header-dependency files are -included by mk/deps.mk via $(ALL_DEPS).
