OUT ?= build
TARGET ?= $(OUT)/libX11-compat.so

# GLX ?= 1 builds the optional GLX-over-EGL layer (src/glx.c + src/egl-wrapper.c)
# and exports the glX* symbols. Set GLX=0 to compile it out entirely: the glX*
# surface and the libEGL dlopen dependency disappear, and the coverage gate plus
# the glX link test drop with it. GLX is inert without a runtime EGL provider
# either way, so GLX=0 is only for builds that want the smaller symbol surface.
GLX ?= 1

# PYTHON is set in mk/toolchain.mk; do not redefine here.
# SDL detection lives in mk/sdl.mk; this file consumes SDL_CPPFLAGS and
# SDL_COMPAT_LIBS from it.
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
            $(SDL_CPPFLAGS) $(PIXMAN_CFLAGS) \
            -DNARROWPROTO -DXTHREADS -D_GNU_SOURCE

# SDL3's SDL_system.h forward-declares `typedef union _XEvent XEvent;` (for the
# X11 event-hook API) with no opt-out, and Xlib.h defines the same typedef. The
# two are identical, so it is harmless; -std=c99 just flags the C11-legal
# repeat. Silence only that case (conflicting typedefs stay hard errors); gcc
# does not warn here and accepts the unknown -Wno- option quietly.
CFLAGS += -std=c99 -Wall -Wextra -Wno-unused-parameter \
          -Wno-typedef-redefinition -fPIC

# Opt-in strict mode: STRICT=1 turns warnings into errors so CI surfaces
# new diagnostics at PR time. STRICT_CFLAGS is applied only to first-
# party objects via mk/common.mk's compile rule; upstream-derived libXt
# and libXpm sources have their own warning footprint we do not own,
# and the libXt typecast warnings would otherwise block the build.
STRICT_CFLAGS :=
ifeq ($(STRICT),1)
  STRICT_CFLAGS += -Werror
endif
# SDL_COMPAT_LIBS comes from mk/sdl.mk. CoreFoundation on Darwin backs the
# live-resize run-loop observer in src/mac-live-resize.c.
LDLIBS += $(SDL_COMPAT_LIBS) $(PIXMAN_LIBS) -lm -pthread \
          $(if $(filter Linux,$(UNAME_S)),-ldl) \
          $(if $(filter Darwin,$(UNAME_S)),-framework CoreFoundation) \
          $(if $(filter Darwin,$(UNAME_S)),-lobjc)

GREEN  := \033[0;32m
BLUE   := \033[0;34m
YELLOW := \033[1;33m
RED    := \033[0;31m
RESET  := \033[0m
