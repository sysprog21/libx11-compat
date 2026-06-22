# SDL detection and derived flags, isolated from config.mk so the rest of the
# build consumes one stable interface and a future SDL3 path can live here
# alongside SDL2 rather than being scattered across config / toolchain / tests.
#
# Consumers use:
#   SDL_CPPFLAGS        include flags to fold into CPPFLAGS
#   SDL_COMPAT_LIBS     link flags for the in-tree SDL wrapper shims
#   SDL_RUNTIME_LIBDIR  loader path dir tests need at runtime (may be empty)
#   SDL2_PREFIX         install prefix, also read by mk/sdl-wrapper.mk and
#                       mk/libxt.mk for the dlopen override and include path
#
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
