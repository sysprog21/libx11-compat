OUT ?= build
TARGET ?= $(OUT)/libX11-compat.so
PYTHON ?= python3

SDL2_CFLAGS := $(shell $(SDL2_CONFIG) --cflags 2>/dev/null)
SDL2_PREFIX := $(shell $(SDL2_CONFIG) --prefix 2>/dev/null)
SDL2_LIBS := $(shell $(SDL2_CONFIG) --libs 2>/dev/null)
SDL2_TTF_PREFIX := $(shell $(PKG_CONFIG) --variable=prefix SDL2_ttf 2>/dev/null || brew --prefix sdl2_ttf 2>/dev/null)
SDL2_TTF_CFLAGS := $(shell $(PKG_CONFIG) --cflags SDL2_ttf 2>/dev/null)
SDL2_TTF_LIBS := $(shell $(PKG_CONFIG) --libs SDL2_ttf 2>/dev/null)
PIXMAN_CFLAGS := $(shell $(PKG_CONFIG) --cflags pixman-1 2>/dev/null)
PIXMAN_LIBS := $(shell $(PKG_CONFIG) --libs pixman-1 2>/dev/null)

# Include path layering. Order matters: locally-tracked headers under
# include/X11/ win first, the upstream xorg snapshot staged by
# mk/upstream-headers.mk wins next, and only then do we fall through to
# whatever X11 headers Homebrew or other system packages happen to publish.
#
# The -iquote paths apply to `#include "..."` only and are what lets the
# staged upstream libX11 .c files resolve bare includes like "Xlibint.h"
# and "reallocarray.h" (which libX11's own build flattens via -I). The
# local include/ paths come first under -iquote so a future override under
# include/X11/ shadows the staged upstream copy even for bare includes
# coming from upstream sources.
CPPFLAGS += -Iinclude -Isrc \
            -I$(OUT)/upstream/include \
            -iquote include/X11 \
            -iquote include \
            -iquote $(OUT)/upstream/include/X11 \
            -iquote $(OUT)/upstream/src \
            $(if $(SDL2_PREFIX),-I$(SDL2_PREFIX)/include) \
            $(if $(SDL2_TTF_PREFIX),-I$(SDL2_TTF_PREFIX)/include) \
            $(SDL2_CFLAGS) $(SDL2_TTF_CFLAGS) $(PIXMAN_CFLAGS) \
            -DNARROWPROTO -DXTHREADS
CFLAGS += -std=c99 -Wall -Wextra -Wno-unused-parameter -fPIC
LDLIBS += $(SDL2_LIBS) \
          $(if $(SDL2_TTF_PREFIX),-L$(SDL2_TTF_PREFIX)/lib) \
          $(if $(SDL2_TTF_LIBS),$(filter-out -lSDL2,$(SDL2_TTF_LIBS)),-lSDL2_ttf) \
          $(PIXMAN_LIBS) -lm -pthread

GREEN  := \033[0;32m
BLUE   := \033[0;34m
YELLOW := \033[1;33m
RED    := \033[0;31m
RESET  := \033[0m
