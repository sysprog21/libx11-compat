# Shared library target.

.PHONY: all

# The wasm leg has no shared object: it links a static archive (and the clock
# browser artifact) from mk/wasm-deps.mk, which owns TARGET and the default
# goal there. Everything below is the native .so build.
ifneq ($(WASM),1)

## Build the SDL2-backed X11 compatibility shared library
all: $(TARGET)

# On Linux SDL2 transitively loads /lib/.../libX11.so.6. The staged
# upstream locking.c, Context.c, etc. define globals (_XLockDisplay,
# _XInitDisplayLock, _XInitDisplayLock_fn, _Xglobal_lock, ...) under
# the same names as the system libX11. Both the function symbols and
# the data symbols (function-pointer variables like
# _XInitDisplayLock_fn) participate; the dynamic linker otherwise
# rebinds our intra-library references to libX11.so.6's copies, our
# XOpenDisplay ends up running libX11.so.6's _XInitDisplayLock against
# our Display layout, and the next display->lock_fns->lock_display(...)
# segfaults at offset 0x50. -Bsymbolic (not just -Bsymbolic-functions)
# binds *both* function and data references to the local library at
# link time. Mach-O ld on macOS does not accept the flag, so gate it
# on Linux only. The version script below now hides those same lock
# globals outright (they are in no manifest, so local: keeps them out
# of the dynamic table entirely), which is strictly stronger than
# -Bsymbolic; the flag stays as belt-and-suspenders for the exported
# symbols' own intra-library references.

# Pin the exported surface to the intended public API. The library defaults to
# exporting every non-static symbol, which leaks ~400 internal helpers into the
# dynamic table; restricting the export set to the enforced manifests also lets
# the link garbage-collect code no exported symbol reaches (e.g. the Xft/Fc copy
# that only libXft-compat consumes). See scripts/gen-export-list.sh.
X11_EXPORT_MANIFESTS := tests/api-symbols.txt tests/shim-symbols.txt \
                        tests/private-symbols.txt tests/whitebox-symbols.txt

# GLX (and the export FORMAT) toggle the exported surface, so a GLX=0->1 flip
# without make clean must not reuse a map that hid glX* as local. That identity
# is tracked once by X11_LINK_CONFIG below: the defined-syms, the export list,
# and the linked libraries all take it as a prerequisite, so a change to GLX or
# FORMAT regenerates the map and relinks. The artifact names stay unversioned.
# The core link splits in two: LDFLAGS_LIB_COMMON is the binding every core .so
# needs (notably -Bsymbolic on Linux), LDFLAGS_LIB_PIN is the export-surface
# restriction only the shipped library wants. The whitebox test twin below
# reuses COMMON but drops PIN, so it keeps -Bsymbolic (without which the system
# libX11.so.6 that SDL2 loads on Linux interposes our lock/event globals) while
# exporting every internal the tests reach. The @loader_path rpath and the
# @rpath install_name / $ORIGIN rpath come from shared_lib_rpath_ldflags, keyed
# on $@ so each library carries its own soname.
LDFLAGS_LIB_COMMON :=
X11_RPATH_FLAGS = $(call shared_lib_rpath_ldflags,$(notdir $@))
ifeq ($(UNAME_S),Linux)
  X11_EXPORT_LIST := $(OUT)/libX11-compat.map
  X11_EXPORT_FORMAT := elf
  LDFLAGS_LIB_COMMON += -Wl,-Bsymbolic
  LDFLAGS_LIB_PIN := -Wl,--version-script=$(X11_EXPORT_LIST) -Wl,--gc-sections
endif
ifeq ($(UNAME_S),Darwin)
  X11_EXPORT_LIST := $(OUT)/libX11-compat.exports
  X11_EXPORT_FORMAT := macho
  LDFLAGS_LIB_PIN := -Wl,-exported_symbols_list,$(X11_EXPORT_LIST) -Wl,-dead_strip
endif

X11_LINK_CONFIG := $(OUT)/libX11-compat.link-config
.PHONY: FORCE
$(X11_LINK_CONFIG): FORCE | $(OUT)
	$(Q){ printf 'GLX=%s\n' '$(GLX)'; \
	      printf 'FORMAT=%s\n' '$(X11_EXPORT_FORMAT)'; } > $@.tmp
	$(Q)if test -r $@ && cmp -s $@.tmp $@; then \
	    rm -f $@.tmp; \
	else \
	    mv $@.tmp $@; \
	fi

# The symbols the core objects actually define, so the export list can be
# intersected against them (see scripts/gen-export-list.sh for why). Darwin nm
# spells C symbols with a leading underscore; strip it so names match the
# manifests. Undefined entries (type U) are dropped.
X11_EXPORT_DEFINED := $(OUT)/libX11-compat.defined-syms
$(X11_EXPORT_DEFINED): $(OBJS) $(X11_LINK_CONFIG) | $(OUT)
	@echo "  GEN     $@"
	$(Q)nm -g $(OBJS) 2>/dev/null \
	    | awk '$$1 ~ /^[0-9a-fA-F]+$$/ { print $$NF }' \
	    | $(if $(filter Darwin,$(UNAME_S)),sed 's/^_//',cat) \
	    | LC_ALL=C sort -u > $@
	$(Q)test -s $@ || { echo "  ERROR   $@ empty (nm found no defined symbols)" >&2; exit 1; }

# X11_EXPORT_FORMAT and GLX are quoted because a make GLX= override leaves GLX
# empty; unquoted it would vanish from the argv and shift every later positional,
# so the script would read the defined-syms path as the glx flag. Matching only
# lines whose first nm field is a hex address above keeps undefined-weak (type
# w/v) references out of the defined set, not just type U.
$(X11_EXPORT_LIST): $(X11_EXPORT_MANIFESTS) $(X11_EXPORT_DEFINED) \
    scripts/gen-export-list.sh $(X11_LINK_CONFIG) | $(OUT)
	@echo "  GEN     $@"
	$(Q)scripts/gen-export-list.sh "$(X11_EXPORT_FORMAT)" "$(GLX)" \
	    $(X11_EXPORT_DEFINED) $(X11_EXPORT_MANIFESTS) > $@

$(TARGET): $(OBJS) $(SDL_WRAPPER_TARGETS) $(X11_EXPORT_LIST) \
    $(X11_LINK_CONFIG) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(LDFLAGS_LIB_COMMON) $(X11_RPATH_FLAGS) \
	    $(LDFLAGS_LIB_PIN) -shared -o $@ $(OBJS) $(LDLIBS)

# Fat, unpinned twin of the core library for the whitebox tests (see mk/tests.mk
# for why they cannot link the pinned .so or the bare objects). Same objects and
# the same COMMON binding as $(TARGET), so -Bsymbolic still shields our globals
# from the system libX11.so.6, but with no export list every internal stays
# reachable. Built only as a prerequisite of the whitebox test binaries, never
# by all and never installed.
X11_TEST_LIB := $(OUT)/libX11-compat-test.so
$(X11_TEST_LIB): $(OBJS) $(SDL_WRAPPER_TARGETS) $(X11_LINK_CONFIG) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(LDFLAGS_LIB_COMMON) $(X11_RPATH_FLAGS) \
	    -shared -o $@ $(OBJS) $(LDLIBS)

endif # native .so build
