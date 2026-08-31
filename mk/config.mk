OUT ?= build
TARGET ?= $(OUT)/libX11-compat.so

# GLX ?= 1 builds the optional GLX-over-EGL layer (src/glx.c + src/egl-wrapper.c)
# and exports the glX* symbols. Set GLX=0 to compile it out entirely: the glX*
# surface and the libEGL dlopen dependency disappear, and the coverage gate plus
# the glX link test drop with it. GLX is inert without a runtime EGL provider
# either way, so GLX=0 is only for builds that want the smaller symbol surface.
GLX ?= 1

# XCB ?= 0 builds the core XCB shim (libxcb-compat.so) and the Xlib/XCB bridge
# (libX11-xcb-compat.so) alongside the Xlib library, together with their tests,
# example clients, pkg-config files and install entries. It is off by default
# because no in-tree workload needs it yet: an XCB client has to opt in with
# XCB=1, and a build that does not stays free of the extra libraries and of the
# xcb/x11-xcb .pc files a downstream configure might otherwise pick up.
XCB ?= 0

# PYTHON is set in mk/toolchain.mk; do not redefine here.
# SDL detection lives in mk/sdl.mk; this file consumes SDL_CPPFLAGS and
# SDL_COMPAT_LIBS from it.
ifeq ($(WASM),1)
# Pixman is a static archive staged under WASM_SYSROOT (set in mk/wasm.mk) for
# the wasm build. The paths are deterministic, so pinning them directly avoids
# a parse-time pkg-config against a .pc that the sysroot rule has not built yet,
# and guarantees no host pixman can leak in.
PIXMAN_CFLAGS := -I$(WASM_SYSROOT)/include/pixman-1
PIXMAN_LIBS := -L$(WASM_SYSROOT)/lib -lpixman-1
else
PIXMAN_CFLAGS := $(shell $(PKG_CONFIG) --cflags pixman-1 2>/dev/null)
PIXMAN_LIBS := $(shell $(PKG_CONFIG) --libs pixman-1 2>/dev/null)
endif

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

# Per-function/-data sections so the shared-library link can --gc-sections away
# code no exported symbol reaches once mk/library.mk pins the export surface.
# ELF-only leverage; clang on Mach-O ignores these and dead-strips by symbol.
CFLAGS += -ffunction-sections -fdata-sections

# Release optimization default for first-party objects: libX11-compat core, the
# staged upstream libX11 sources, the compat toolkit libraries (libXt/Xpm/Xaw/
# Xmu/Xext/Xinerama/ICE/SM/Xft-compat), tests, and bundled examples. Debug CI
# passes OPTFLAGS=-O0 explicitly.
OPTFLAGS ?= -O2

# FP_CFLAGS / LEGACY_FP_CFLAGS are the single source of truth for the first-
# party optimization + base flag set. Every first-party compile rule uses one of
# them (via cc_object or by hand) rather than threading $(OPTFLAGS) itself, so
# the opt level is defined here once and cannot silently drift out of an
# individual rule.
#
# OPTFLAGS comes BEFORE $(CFLAGS): $(OPTFLAGS) is the default, and a value the
# user or a job supplies later wins because the last -O on the command line
# takes effect. So `make CFLAGS="-O0 -g"` still lowers the optimization (Make's
# usual convention), a sanitizer job's -O1 in $(CFLAGS_EXTRA) still wins, and
# OPTFLAGS=-O0 sets it directly. Recursive (=) so command-line overrides of
# OPTFLAGS/CFLAGS are honored at recipe time.
FP_CFLAGS = $(OPTFLAGS) $(CFLAGS)

# Legacy/upstream X11 sources (staged libX11 plus the libXt/Xaw/Xpm/Xmu compat
# libs) predate strict aliasing and contain type-punning that is benign at -O0
# but a TBAA hazard at the -O2 default. Keep the speed but disable the aliasing
# assumption so -O2 cannot reorder or elide those punned loads/stores. Trailing
# so it is not overridden by a -fstrict-aliasing that leaks in via $(CFLAGS).
LEGACY_FP_CFLAGS = $(FP_CFLAGS) -fno-strict-aliasing

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
#
# The native-only link bits (-pthread, -ldl, the Darwin frameworks) are gated
# on the WASM target rather than the build host: the wasm leg is single-
# threaded (ASYNCIFY, no pthreads), has no dlopen, and pulls SDL/libm from
# Emscripten ports, so it links only the pixman archive and the SDL port flags.
LDLIBS += $(SDL_COMPAT_LIBS) $(PIXMAN_LIBS) -lm
ifneq ($(WASM),1)
LDLIBS += -pthread \
          $(if $(filter Linux,$(UNAME_S)),-ldl) \
          $(if $(filter Darwin,$(UNAME_S)),-framework CoreFoundation) \
          $(if $(filter Darwin,$(UNAME_S)),-lobjc)
endif

GREEN  := \033[0;32m
BLUE   := \033[0;34m
YELLOW := \033[1;33m
RED    := \033[0;31m
RESET  := \033[0m
