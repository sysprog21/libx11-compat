# SDL detection and derived flags, isolated from config.mk so the rest of the
# build consumes one stable interface across both SDL major versions rather
# than scattering the choice across config / toolchain / tests.
#
# Consumers use:
#   SDL_CPPFLAGS        include flags to fold into CPPFLAGS
#   SDL_COMPAT_LIBS     link flags (wrapper shims for sdl2, real libSDL3 for sdl3)
#   SDL_RUNTIME_LIBDIR  loader path dir tests need at runtime (may be empty)
#   SDL_USE_WRAPPER     1 when the in-tree dlopen wrapper shims are built
#   SDL2_PREFIX         install prefix, also read by mk/sdl-wrapper.mk and
#                       mk/libxt.mk for the dlopen override and include path
#
# SDL_BACKEND selects the SDL major version.
#   sdl2: source compiles against the SDL2 API and links the in-tree dlopen
#         wrapper shims (the runtime SDL2 may itself be sdl2-compat over SDL3).
#   sdl3: source compiles against the native SDL3 API through src/sdl-compat.h
#         and links libSDL3 / libSDL3_ttf directly, skipping the wrapper
#         (extending the ~300-symbol wrapper to SDL3's renamed surface buys
#         nothing).
# When SDL_BACKEND is not set on the command line or environment, auto-detect:
# prefer SDL3 (both sdl3 and sdl3-ttf present via pkg-config), else fall back to
# SDL2. An explicit SDL_BACKEND=... always wins.
#
# WASM=1 is a third leg: the SDL2 API compiled against Emscripten's bundled
# SDL2/SDL_ttf/freetype ports. It shares the SDL2-spelled source path but links
# no wrapper and probes no host SDL: the port flags below are the whole
# interface, needed identically at compile and link time.
ifeq ($(WASM),1)

# override: the wasm leg is SDL2-port only, so a stray command-line
# SDL_BACKEND=sdl3 must not leave a mislabeled stamp or reach the sdl3 flags.
override SDL_BACKEND := sdl2
SDL_USE_WRAPPER := 0
SDL_PORT_FLAGS := -sUSE_SDL=2 -sUSE_SDL_TTF=2 -sUSE_FREETYPE=1
SDL_CPPFLAGS := $(SDL_PORT_FLAGS)
SDL_COMPAT_LIBS := $(SDL_PORT_FLAGS)
SDL_RUNTIME_LIBDIR :=

else # native (host SDL2 or SDL3)

ifeq ($(origin SDL_BACKEND),undefined)
SDL_BACKEND := $(shell $(PKG_CONFIG) --exists sdl3 sdl3-ttf 2>/dev/null && echo sdl3 || echo sdl2)
endif

# Reject anything but the two known backends so a typo (e.g. SDL_BACKEND=SDL3)
# fails loudly instead of silently selecting the sdl2 branch below.
ifeq ($(filter $(SDL_BACKEND),sdl2 sdl3),)
$(error Invalid SDL_BACKEND '$(SDL_BACKEND)'; expected 'sdl2' or 'sdl3')
endif

# Detection prefers pkg-config (the interface sdl2-compat standardizes on) and
# falls back to sdl2-config for a classic SDL2 install. sdl2-compat ships both
# today, but a distro that packages it with only the .pc file must still
# configure cleanly, so the sdl2 -> sdl2-compat swap stays a no-op.
SDL2_CONFIG ?= sdl2-config

SDL2_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2 2>/dev/null || $(SDL2_CONFIG) --cflags 2>/dev/null)
SDL2_PREFIX := $(shell $(PKG_CONFIG) --variable=prefix sdl2 2>/dev/null || $(SDL2_CONFIG) --prefix 2>/dev/null)
SDL2_LIBS := $(shell $(PKG_CONFIG) --libs sdl2 2>/dev/null || $(SDL2_CONFIG) --libs 2>/dev/null)

SDL2_LIBDIR := $(shell $(PKG_CONFIG) --variable=libdir sdl2 2>/dev/null)
SDL2_TTF_PREFIX := $(shell $(PKG_CONFIG) --variable=prefix SDL2_ttf 2>/dev/null || brew --prefix sdl2_ttf 2>/dev/null)
SDL2_TTF_CFLAGS := $(shell $(PKG_CONFIG) --cflags SDL2_ttf 2>/dev/null)
SDL2_TTF_LIBS := $(shell $(PKG_CONFIG) --libs SDL2_ttf 2>/dev/null)

ifeq ($(SDL_BACKEND),sdl3)

# Native SDL3 leg. The pkg-config modules are sdl3 and sdl3-ttf.
SDL3_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3 2>/dev/null)
SDL3_LIBS := $(shell $(PKG_CONFIG) --libs sdl3 2>/dev/null)
SDL3_TTF_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl3-ttf 2>/dev/null)
SDL3_TTF_LIBS := $(shell $(PKG_CONFIG) --libs sdl3-ttf 2>/dev/null)
SDL3_LIBDIR := $(shell $(PKG_CONFIG) --variable=libdir sdl3 2>/dev/null)

SDL_CPPFLAGS := $(SDL3_CFLAGS) $(SDL3_TTF_CFLAGS) -DLIBX11_COMPAT_SDL3
SDL_COMPAT_LIBS := $(SDL3_LIBS) $(SDL3_TTF_LIBS)
SDL_RUNTIME_LIBDIR := $(SDL3_LIBDIR)
SDL_USE_WRAPPER := 0

else

SDL_USE_WRAPPER := 1

SDL_CPPFLAGS := $(if $(SDL2_PREFIX),-I$(SDL2_PREFIX)/include) \
                $(if $(SDL2_TTF_PREFIX),-I$(SDL2_TTF_PREFIX)/include) \
                $(SDL2_CFLAGS) $(SDL2_TTF_CFLAGS)

# The compat stack links the in-tree SDL wrapper shims, never the real SDL
# directly; the wrapper dlopens the host SDL at runtime.
SDL_COMPAT_LIBS := -L$(abspath $(OUT)) -lSDL2-x11compat -lSDL2_ttf-x11compat

# sdl2-compat's libSDL2 is a thin shim over SDL3, so binaries that dlopen SDL2
# need the SDL lib dir on the loader path to resolve the transitive libSDL3.
# Prefer pkg-config's libdir (handles lib64 / multiarch / non-prefix/lib
# installs); fall back to prefix/lib for the sdl2-config-only case. Empty when
# SDL is undetected.
SDL_RUNTIME_LIBDIR := $(if $(SDL2_LIBDIR),$(SDL2_LIBDIR),$(if $(SDL2_PREFIX),$(SDL2_PREFIX)/lib))

endif

endif # WASM vs native

# Shared by every backend: a stamp recording the resolved SDL config so a
# backend switch forces the affected objects to rebuild.
SDL_BACKEND_STAMP := $(OUT)/.sdl-backend

.PHONY: sdl-backend-force

$(SDL_BACKEND_STAMP): sdl-backend-force | $(OUT)
	$(Q){ \
	    printf '%s\n' 'SDL_BACKEND=$(SDL_BACKEND)'; \
	    printf '%s\n' 'SDL_CPPFLAGS=$(SDL_CPPFLAGS)'; \
	    printf '%s\n' 'SDL_COMPAT_LIBS=$(SDL_COMPAT_LIBS)'; \
	} > $@.tmp
	$(Q)if test -r $@ && cmp -s $@.tmp $@; then \
	    rm -f $@.tmp; \
	else \
	    echo "  SDL     $(SDL_BACKEND)"; \
	    mv $@.tmp $@; \
	    sleep 1; \
	    touch $@; \
	fi
