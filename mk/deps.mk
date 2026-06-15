# Generated header dependencies. Each library fragment compiles with
# -MMD -MP and emits a .d file alongside its .o; this file aggregates
# them into one -include so downstream fragments stay focused on their
# own build rules. The empty rule on $(ALL_DEPS) prevents make from
# trying to "build" a .d when its .c was removed.
ALL_DEPS := $(OBJS:.o=.d) \
            $(OUT)/xext-compat.d \
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

$(ALL_DEPS):
	@:

-include $(ALL_DEPS)
