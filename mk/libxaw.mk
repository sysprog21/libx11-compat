# Build libXaw-compat.so from the pinned libXaw-1.0.16 source tree staged
# under $(OUT)/upstream/src-libXaw/ by scripts/sync-upstream-headers.py.

LIBXAW_SRC_DIR := $(OUT)/upstream/src-libXaw
LIBXAW_OBJ_DIR := $(OUT)/libxaw
LIBXAW_TARGET := $(OUT)/libXaw-compat.so

UPSTREAM_HEADERS_DIR ?= $(OUT)/upstream/include
UPSTREAM_HEADERS_STAMP ?= $(UPSTREAM_HEADERS_DIR)/.upstream-stamp

LIBXAW_SRC_BASES := \
    Actions.c AllWidgets.c AsciiSink.c AsciiSrc.c AsciiText.c Box.c \
    Command.c Converters.c Dialog.c DisplayList.c Form.c Grip.c Label.c \
    List.c MenuButton.c MultiSink.c MultiSrc.c OS.c Paned.c Panner.c \
    Pixmap.c Porthole.c Repeater.c Scrollbar.c Simple.c SimpleMenu.c \
    Sme.c SmeBSB.c SmeLine.c StripChart.c Text.c TextAction.c TextPop.c \
    TextSink.c TextSrc.c TextTr.c Tip.c Toggle.c Tree.c Vendor.c Viewport.c \
    XawI18n.c XawIm.c XawInit.c
LIBXAW_SRCS := $(addprefix $(LIBXAW_SRC_DIR)/,$(LIBXAW_SRC_BASES))
LIBXAW_OBJS := $(patsubst $(LIBXAW_SRC_DIR)/%.c,$(LIBXAW_OBJ_DIR)/%.o,$(LIBXAW_SRCS))

LIBXAW_CPPFLAGS := \
    -DHAVE_CONFIG_H \
    -Iinclude/libxaw-build \
    -I$(OUT)/upstream/include \
    -Iinclude \
    -iquote $(OUT)/upstream/include/X11 \
    -iquote include/X11 \
    $(LIBXT_CPPFLAGS)

LIBXAW_CFLAGS := -std=c99 -Wall -fPIC \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function \
    -Wno-missing-braces -Wno-sign-compare -Wno-deprecated-declarations \
    -Wno-incompatible-pointer-types-discards-qualifiers \
    -Wno-incompatible-pointer-types
# MultiSrc.c, TextAction.c, and XawI18n.c call iswalnum / iswspace /
# iswctype without #include <wctype.h>. Apple clang transitively
# pulls the header through <ctype.h>, but Ubuntu's LLVM 20 clang flags
# the implicit declaration and refuses to compile under -std=c99.
# Force-include wctype.h so the build is portable without patching
# the upstream sources.
LIBXAW_CFLAGS += -include wctype.h

$(LIBXAW_OBJ_DIR):
	@mkdir -p $@

$(LIBXAW_SRCS): $(UPSTREAM_HEADERS_STAMP)

$(LIBXAW_OBJ_DIR)/%.o: $(UPSTREAM_HEADERS_STAMP) $(LIBXT_STAGED_H) \
    $(SDL_BACKEND_STAMP) | $(LIBXAW_OBJ_DIR)
	@echo "  CC      $(LIBXAW_SRC_DIR)/$*.c"
	$(Q)$(CC) $(LIBXAW_CPPFLAGS) $(LEGACY_FP_CFLAGS) $(LIBXAW_CFLAGS) $(CFLAGS_EXTRA) \
	    $(DEPFLAGS) \
	    -c $(LIBXAW_SRC_DIR)/$*.c -o $@

LIBXAW_LDFLAGS := $(call shared_lib_rpath_ldflags,$(notdir $(LIBXAW_TARGET)))

$(LIBXAW_TARGET): $(LIBXAW_OBJS) $(LIBXT_TARGET) $(TARGET) \
    $(LIBXPM_TARGET) $(XMU_COMPAT_TARGET) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(LIBXAW_LDFLAGS) -shared -o $@ $(LIBXAW_OBJS) \
	    -L$(OUT) -lXt-compat -lX11-compat -lXpm-compat -lXmu-compat \
	    $(LDLIBS)

.PHONY: libxaw
## Build the libXaw compatibility shared library
libxaw: $(LIBXAW_TARGET)

all: $(LIBXAW_TARGET)
