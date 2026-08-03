# Build libXpm-compat.so from the pinned libXpm source staged under
# $(OUT)/upstream/src-libXpm/ by scripts/sync-upstream-headers.py.

LIBXPM_SRC_DIR := $(OUT)/upstream/src-libXpm
# Static archive with a separate object dir under wasm; native .so otherwise.
ifeq ($(WASM),1)
LIBXPM_OBJ_DIR := $(OUT)/libxpm-wasm
LIBXPM_TARGET  := $(OUT)/libXpm-compat.a
else
LIBXPM_OBJ_DIR := $(OUT)/libxpm
LIBXPM_TARGET  := $(OUT)/libXpm-compat.so
endif

LIBXPM_SRC_BASES := \
    Attrib.c CrBufFrI.c CrBufFrP.c CrDatFrI.c CrDatFrP.c CrIFrBuf.c \
    CrIFrDat.c CrIFrP.c CrPFrBuf.c CrPFrDat.c CrPFrI.c Image.c Info.c \
    RdFToBuf.c RdFToDat.c RdFToI.c RdFToP.c WrFFrBuf.c WrFFrDat.c \
    WrFFrI.c WrFFrP.c create.c data.c hashtab.c misc.c parse.c rgb.c scan.c
LIBXPM_SRCS := $(addprefix $(LIBXPM_SRC_DIR)/,$(LIBXPM_SRC_BASES))
LIBXPM_OBJS := $(patsubst $(LIBXPM_SRC_DIR)/%.c,$(LIBXPM_OBJ_DIR)/%.o,$(LIBXPM_SRCS))

LIBXPM_CPPFLAGS := \
    -DHAVE_CONFIG_H \
    -Iinclude/libxpm-build \
    -I$(OUT)/upstream/include/X11 \
    -I$(OUT)/upstream/include \
    -Iinclude \
    -iquote $(OUT)/upstream/src-libXpm \
    $(CPPFLAGS)

LIBXPM_CFLAGS := -std=c99 -Wall -fPIC \
    -Wno-unused-parameter -Wno-sign-compare -Wno-deprecated-declarations
LIBXPM_LDFLAGS := $(call shared_lib_rpath_ldflags,$(notdir $(LIBXPM_TARGET)))

$(LIBXPM_OBJ_DIR):
	@mkdir -p $@

$(LIBXPM_SRCS): $(UPSTREAM_HEADERS_STAMP)

$(LIBXPM_OBJ_DIR)/%.o: $(LIBXPM_SRC_DIR)/%.c $(UPSTREAM_HEADERS_STAMP) \
    $(SDL_BACKEND_STAMP) | $(LIBXPM_OBJ_DIR)
	@echo "  CC      $<"
	$(Q)$(CC) $(LIBXPM_CPPFLAGS) $(LEGACY_FP_CFLAGS) $(LIBXPM_CFLAGS) $(CFLAGS_EXTRA) \
	    $(DEPFLAGS) -c $< -o $@

ifeq ($(WASM),1)
$(LIBXPM_TARGET): $(LIBXPM_OBJS) | $(OUT)
	@echo "  AR      $@"
	$(Q)rm -f $@
	$(Q)$(AR) rcs $@ $(LIBXPM_OBJS)
else
$(LIBXPM_TARGET): $(LIBXPM_OBJS) $(TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(LIBXPM_LDFLAGS) -shared -o $@ $(LIBXPM_OBJS) -L$(OUT) -lX11-compat $(LDLIBS)
endif

.PHONY: libxpm
## Build the libXpm compatibility library (shared native, static wasm)
libxpm: $(LIBXPM_TARGET)

ifneq ($(WASM),1)
all: $(LIBXPM_TARGET)
endif

# Header-dependency files are -included by mk/deps.mk via $(ALL_DEPS).
