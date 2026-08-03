# Generated header dependencies. Each library fragment compiles with
# -MMD -MP and emits a .d file alongside its .o; this file aggregates
# them into one -include so downstream fragments stay focused on their
# own build rules. The empty rule on $(ALL_DEPS) prevents make from
# trying to "build" a .d when its .c was removed.
ALL_DEPS := $(OBJS:.o=.d)

ifeq ($(WASM),1)
# The wasm toolkit objects live in separate build/<lib>-wasm dirs, so their
# depfiles are distinct from the native ones and only exist once a wasm toolkit
# lib is built. Including them gives incremental header tracking for the ported
# libraries; a core-only wasm build simply finds none of them and skips. This is
# safe now that HOST_CC is native (the libXt StringDefs generator runs on the
# build machine even while CC is emcc), so a wasm toolkit .d that lists
# StringDefs.h no longer drags emcc into a host-tool build. The native-only
# compat libs (Xext/Xft/Xinerama/ICE/SM) are not built here, so their depfiles
# are omitted.
ALL_DEPS += $(LIBXT_OBJS:.o=.d) $(LIBXPM_OBJS:.o=.d) $(LIBXAW_OBJS:.o=.d) \
            $(XMU_UPSTREAM_OBJS:.o=.d) $(XMU_COMPAT_OBJ:.o=.d)
else
ALL_DEPS += $(OUT)/xext-compat.d \
            $(OUT)/xmu-compat.d \
            $(OUT)/xinerama-compat.d \
            $(OUT)/ice-compat.d \
            $(OUT)/sm-compat.d \
            $(OUT)/xft-compat.d
ifdef XMU_UPSTREAM_OBJS
  ALL_DEPS += $(XMU_UPSTREAM_OBJS:.o=.d)
endif
ifdef LIBXT_OBJS
  ALL_DEPS += $(LIBXT_OBJS:.o=.d)
endif
ifdef LIBXPM_OBJS
  ALL_DEPS += $(LIBXPM_OBJS:.o=.d)
endif
ifdef LIBXAW_OBJS
  ALL_DEPS += $(LIBXAW_OBJS:.o=.d)
endif
endif

$(ALL_DEPS):
	@:

-include $(ALL_DEPS)
