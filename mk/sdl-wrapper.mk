SDL_WRAPPER_TARGET := $(OUT)/libSDL2-x11compat.so
SDL_TTF_WRAPPER_TARGET := $(OUT)/libSDL2_ttf-x11compat.so
SDL_WRAPPER_TARGETS := $(SDL_WRAPPER_TARGET) $(SDL_TTF_WRAPPER_TARGET)
SDL_WRAPPER_OBJS := $(OUT)/src/wrapper/sdl-wrapper.o
SDL_TTF_WRAPPER_OBJS := $(OUT)/src/wrapper/sdl-ttf-wrapper.o
# Pick the right shared-library suffix per platform so the prefix-based
# dlopen() override path actually points at a loadable file. The "DYLIB"
# in the macro name is historical (matches the C side) — on Linux it
# resolves to the .so.<ver> SONAME.
ifeq ($(UNAME_S),Darwin)
  SDL_WRAPPER_LIB_FILE := libSDL2-2.0.0.dylib
  SDL_TTF_WRAPPER_LIB_FILE := libSDL2_ttf-2.0.0.dylib
else
  SDL_WRAPPER_LIB_FILE := libSDL2-2.0.so.0
  SDL_TTF_WRAPPER_LIB_FILE := libSDL2_ttf-2.0.so.0
endif
SDL_WRAPPER_CPPFLAGS := $(if $(SDL2_PREFIX),-DLIBX11_COMPAT_SDL2_DYLIB=\"$(SDL2_PREFIX)/lib/$(SDL_WRAPPER_LIB_FILE)\")
SDL_TTF_WRAPPER_CPPFLAGS := $(if $(SDL2_TTF_PREFIX),-DLIBX11_COMPAT_SDL2_TTF_DYLIB=\"$(SDL2_TTF_PREFIX)/lib/$(SDL_TTF_WRAPPER_LIB_FILE)\")

SDL_WRAPPER_LDLIBS :=
ifeq ($(UNAME_S),Linux)
  SDL_WRAPPER_LDLIBS += -ldl
endif

all: $(SDL_WRAPPER_TARGETS)

$(SDL_WRAPPER_OBJS): CPPFLAGS += $(SDL_WRAPPER_CPPFLAGS)
$(SDL_TTF_WRAPPER_OBJS): CPPFLAGS += $(SDL_TTF_WRAPPER_CPPFLAGS)

$(SDL_WRAPPER_TARGET): $(SDL_WRAPPER_OBJS) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(call shared_lib_rpath_ldflags,$(notdir $@)) \
	    -shared -o $@ $(SDL_WRAPPER_OBJS) $(SDL_WRAPPER_LDLIBS)

$(SDL_TTF_WRAPPER_TARGET): $(SDL_TTF_WRAPPER_OBJS) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(call shared_lib_rpath_ldflags,$(notdir $@)) \
	    -shared -o $@ $(SDL_TTF_WRAPPER_OBJS) $(SDL_WRAPPER_LDLIBS)
