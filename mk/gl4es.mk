# gl4es: desktop GL 1.x/2.x -> GLES2 translation, maintained in-tree under
# src/GL (MIT; see src/GL/README.md). Built against ANGLE's GLESv2 so
# unmodified desktop-GL clients run on Metal with no Mesa. This builds the GL
# translation core only; glX* come from libx11-compat and EGL/GLESv2 from ANGLE.
#
# Flags mirror the upstream cmake recipe for a GLES2 / no-EGL / no-X11 / static
# target. gl4es is upstream code we vendor and refine, so its own warnings are
# silenced (-w); only our deliberate local changes (README.md) are our own.
#
# The four gl4es/gles wrapper files (gl4es.h, gl4eswraps.c, gles.h, gles.c) are
# upstream artifacts, not our source. They are cached raw under third_party/
# (git-ignored, survives make clean, so only the first build needs the network)
# and flattened into $(OUT)/GL at build time as wrap-gl4es.h, gl4eswraps.c,
# wrap-gles.h and gles.c. Keeping them out of src/GL means a tree-wide reformat
# never touches them and the drift check stays a plain byte compare to upstream.
# "make sync-gl4es-wrap" refreshes the cache; "make check-gl4es-wrap" fails on
# drift from the pin (scripts/gl4es-pin.txt).

GL4ES_WRAP_CACHE := third_party/gl4es-wrap
GL4ES_WRAP_RAW := $(addprefix $(GL4ES_WRAP_CACHE)/,gl4es.h gl4eswraps.c gles.h gles.c)

# Archive tool for libgl4es.a. angle.mk sets this for the macOS/ANGLE path, but
# it is not included on Linux (no ANGLE) and the build runs with make's built-in
# variables disabled, so $(AR) would be empty there. Define it here too; ?= keeps
# any caller or environment override.
AR ?= ar

.PHONY: sync-gl4es-wrap check-gl4es-wrap
## Refresh the cached gl4es/gles wrapper sources from the pinned upstream commit
## (scripts/gl4es-pin.txt).
sync-gl4es-wrap:
	$(Q)scripts/sync-gl4es-wrap.sh $(GL4ES_WRAP_CACHE)
## Verify the cached wrapper sources still match the pinned upstream.
check-gl4es-wrap:
	$(Q)scripts/sync-gl4es-wrap.sh --check $(GL4ES_WRAP_CACHE)

# Each raw file fetches only itself, so deleting any one refetches just it. A
# shared stamp would let a deleted raw file still look up to date and break the
# flatten on a missing $<. The pin and script are the only real inputs.
$(GL4ES_WRAP_RAW): scripts/gl4es-pin.txt scripts/sync-gl4es-wrap.sh
	@mkdir -p $(GL4ES_WRAP_CACHE)
	$(Q)scripts/sync-gl4es-wrap.sh $(GL4ES_WRAP_CACHE) $(@F)

GL4ES_DIR := src/GL
# The build-time flatten output lives beside the gl4es objects. gl4eswraps.c and
# gles.c compile from here; wrap-gl4es.h and wrap-gles.h are found via -I below.
GL4ES_GENDIR := $(OUT)/gl4es/gen
GL4ES_GEN_HDRS := $(GL4ES_GENDIR)/wrap-gl4es.h $(GL4ES_GENDIR)/wrap-gles.h
GL4ES_GEN_SRCS := $(GL4ES_GENDIR)/gl4eswraps.c $(GL4ES_GENDIR)/gles.c

# raw upstream name -> flattened output name. Each rule lists the raw file first
# so $< is unambiguously the source (the script is a second prerequisite to
# retrigger on a flatten change, not the input). Recipe repeated rather than a
# canned "define" so it works on the GNU make 3.81 that ships with macOS.
$(GL4ES_GENDIR)/wrap-gl4es.h: $(GL4ES_WRAP_CACHE)/gl4es.h scripts/flatten-gl4es-wrap.sh
	@mkdir -p $(@D)
	@echo "  GEN     GL/$(@F)"
	$(Q)scripts/flatten-gl4es-wrap.sh < $< > $@
$(GL4ES_GENDIR)/wrap-gles.h: $(GL4ES_WRAP_CACHE)/gles.h scripts/flatten-gl4es-wrap.sh
	@mkdir -p $(@D)
	@echo "  GEN     GL/$(@F)"
	$(Q)scripts/flatten-gl4es-wrap.sh < $< > $@
$(GL4ES_GENDIR)/gl4eswraps.c: $(GL4ES_WRAP_CACHE)/gl4eswraps.c scripts/flatten-gl4es-wrap.sh
	@mkdir -p $(@D)
	@echo "  GEN     GL/$(@F)"
	$(Q)scripts/flatten-gl4es-wrap.sh < $< > $@
$(GL4ES_GENDIR)/gles.c: $(GL4ES_WRAP_CACHE)/gles.c scripts/flatten-gl4es-wrap.sh
	@mkdir -p $(@D)
	@echo "  GEN     GL/$(@F)"
	$(Q)scripts/flatten-gl4es-wrap.sh < $< > $@

GL4ES_OBJDIR := $(OUT)/gl4es
# src/GL is now only our hand-maintained gl4es sources; the two wrapper .c files
# come from the flattened cache (GL4ES_GEN_SRCS).
GL4ES_SRCS := $(wildcard $(GL4ES_DIR)/*.c)
GL4ES_OBJS := $(patsubst $(GL4ES_DIR)/%.c,$(GL4ES_OBJDIR)/%.o,$(GL4ES_SRCS)) \
    $(patsubst $(GL4ES_GENDIR)/%.c,$(GL4ES_OBJDIR)/%.o,$(GL4ES_GEN_SRCS))
GL4ES_LIB := $(OUT)/libgl4es.a

# stb_dxt.h (DXT texture compressor) is not vendored: it is fetched once from a
# pinned upstream commit (scripts/stb-pin.txt) into third_party/, which survives
# make clean so only the first build needs the network. decompress.c and
# texture_compressed.c include it.
STB_PIN := $(shell tr -d '\r\n' < scripts/stb-pin.txt 2>/dev/null)
STB_DXT_DIR := third_party/stb
STB_DXT_H := $(STB_DXT_DIR)/stb_dxt.h
$(STB_DXT_H): scripts/stb-pin.txt scripts/fetch-stb-dxt.sh
	@echo "  FETCH   stb_dxt.h @ $(STB_PIN)"
	$(Q)scripts/fetch-stb-dxt.sh $@

# Flat sources in src/GL; the GLES/EGL/KHR headers gl4es targets come from the
# shared include/ tree (not duplicated inside src/GL). -I third_party/stb for the
# fetched stb_dxt.h.
# -I$(GL4ES_GENDIR) first so the flattened wrap-gl4es.h/wrap-gles.h resolve there;
# the bare core gl4es.h/gles.h still come from src/GL.
GL4ES_CPPFLAGS := -DDEFAULT_ES=2 -DEGL_NO_X11 -DNOEGL -DNOX11 -DNO_GBM -DSTATICLIB \
    -I$(GL4ES_GENDIR) -I$(GL4ES_DIR) -Iinclude -I$(STB_DXT_DIR)
# No -fvisibility=hidden: the public gl* are minted from the gl4es_gl* internals
# via a link-time alias list (below), and initialize_gl4es must stay visible so
# libx11-compat's glXMakeCurrent can soft-resolve it (src/glx.c).
# Size, not the upstream -O2 -funwind-tables: gl4es CPU is not the bottleneck
# (ANGLE/Metal is), so optimize for size. -Oz (aggressive size) beats -Os once
# the unwind tables are gone. macOS emits unwind tables by default; gl4es is C
# with no exceptions and does not need them for correctness, only for crash
# backtraces through its frames, so -fno-unwind-tables drops ~120 KB. Together
# these take the archive from ~1.79 MB (upstream -O2) to ~1.47 MB. Bump back to
# -O2 (and restore unwind tables) if an immediate-mode-heavy client turns out
# CPU-bound in gl4es, or if you need gl4es frames in a backtrace.
GL4ES_CFLAGS := -std=gnu11 -Oz -fno-unwind-tables -fno-asynchronous-unwind-tables -w

# The two DXT users need the fetched header present before they compile.
$(GL4ES_OBJDIR)/decompress.o $(GL4ES_OBJDIR)/texture_compressed.o: $(STB_DXT_H)

# Every gl4es object may include wrap-gl4es.h/wrap-gles.h, so the flattened
# headers must exist before any compile (order-only: a header regen must not
# force a full recompile).
$(GL4ES_OBJS): | $(GL4ES_GEN_HDRS)

$(GL4ES_OBJDIR)/%.o: $(GL4ES_DIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      GL/$*"
	$(Q)$(CC) $(GL4ES_CPPFLAGS) $(GL4ES_CFLAGS) $(CFLAGS_EXTRA) -c $< -o $@

# The two wrapper .c files compile from the flattened cache, same flags.
$(GL4ES_OBJDIR)/%.o: $(GL4ES_GENDIR)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC      GL/$*"
	$(Q)$(CC) $(GL4ES_CPPFLAGS) $(GL4ES_CFLAGS) $(CFLAGS_EXTRA) -c $< -o $@

# Public desktop gl* are minted from the gl4es_gl* internals via a generated
# assembly object of Mach-O .set aliases (excluding glX*, which stay with
# libx11-compat). Baking that object INTO the archive makes libgl4es.a a
# self-contained static desktop-GL loader: a demo statically links it (plus
# ANGLE's GLESv2) and gets the public gl* directly, with no dynamic gl4es library.
# The alias assembly is generated by scripts/gen-gl4es-aliases.sh.
GL4ES_ALIASES_S := $(GL4ES_OBJDIR)/gl-aliases.s
GL4ES_ALIASES_OBJ := $(GL4ES_OBJDIR)/gl-aliases.o

$(GL4ES_ALIASES_S): $(GL4ES_OBJS) scripts/gen-gl4es-aliases.sh
	@mkdir -p $(dir $@)
	$(Q)scripts/gen-gl4es-aliases.sh $(GL4ES_OBJS) > $@

$(GL4ES_ALIASES_OBJ): $(GL4ES_ALIASES_S)
	$(Q)$(CC) -c $< -o $@

$(GL4ES_LIB): $(GL4ES_OBJS) $(GL4ES_ALIASES_OBJ)
	@echo "  AR      $@ (static desktop-GL loader)"
	$(Q)$(AR) rcs $@ $(GL4ES_OBJS) $(GL4ES_ALIASES_OBJ)

.PHONY: gl4es
gl4es: $(GL4ES_LIB)
