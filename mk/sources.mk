SRCS := $(wildcard src/*.c) $(wildcard src/path/*.c)

# GLX (src/glx.c + src/egl-wrapper.c) is optional; drop it when GLX=0. See the
# GLX toggle note in mk/config.mk.
ifneq ($(GLX),1)
SRCS := $(filter-out src/glx.c src/egl-wrapper.c,$(SRCS))
endif

# Upstream libX11 translation units staged by mk/upstream-headers.mk via
# scripts/sync-upstream-headers.py. The Makefile compiles them in place so
# the upstream tree is the single source of truth for the libX11 internals
# the library reuses. The basenames here must match SRC_WHITELIST in
# scripts/sync-upstream-headers.py; the list is hardcoded because
# $(wildcard) evaluates at parse time, before the first fetch has created
# the files.
UPSTREAM_SRC_BASES := \
    Context.c \
    ParseGeom.c \
    Quarks.c \
    SetWMProto.c \
    locking.c \
    reallocarray.c
UPSTREAM_SRCS := $(addprefix $(OUT)/upstream/src/,$(UPSTREAM_SRC_BASES))

# Core objects go under OBJROOT, which is $(OUT) natively but $(OUT)/wasm under
# WASM=1, so alternating native and wasm builds do not clobber each other's core
# objects in one tree (matching the build/<lib>-wasm dirs the toolkit fragments
# already use). Native is byte-identical: OBJROOT collapses to $(OUT). Only the
# compiled objects diverge; the staged upstream sources and generated headers
# stay shared under $(OUT).
OBJROOT := $(OUT)$(if $(filter 1,$(WASM)),/wasm)
UPSTREAM_OBJS := $(patsubst $(OUT)/upstream/src/%.c,$(OBJROOT)/upstream/src/%.o,$(UPSTREAM_SRCS))

OBJS := $(patsubst %.c,$(OBJROOT)/%.o,$(SRCS)) $(UPSTREAM_OBJS)
