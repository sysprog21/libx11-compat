EXAMPLE_NAMES := 2048 paint life clock mandel processing clipboard
EXAMPLE_BINS := $(addprefix $(OUT)/examples/,$(EXAMPLE_NAMES))
X11PERF_DIR := examples/x11perf
X11PERF_SRCS := \
    $(X11PERF_DIR)/bitmaps.c \
    $(X11PERF_DIR)/do_arcs.c \
    $(X11PERF_DIR)/do_blt.c \
    $(X11PERF_DIR)/do_complex.c \
    $(X11PERF_DIR)/do_dots.c \
    $(X11PERF_DIR)/do_lines.c \
    $(X11PERF_DIR)/do_movewin.c \
    $(X11PERF_DIR)/do_rects.c \
    $(X11PERF_DIR)/do_segs.c \
    $(X11PERF_DIR)/do_simple.c \
    $(X11PERF_DIR)/do_tests.c \
    $(X11PERF_DIR)/do_text.c \
    $(X11PERF_DIR)/do_traps.c \
    $(X11PERF_DIR)/do_tris.c \
    $(X11PERF_DIR)/do_valgc.c \
    $(X11PERF_DIR)/do_windows.c \
    $(X11PERF_DIR)/x11perf.c
X11PERF_BIN := $(OUT)/examples/x11perf
EXAMPLE_LDFLAGS :=
ifeq ($(UNAME_S),Linux)
  EXAMPLE_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  EXAMPLE_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
endif

.PHONY: examples

## Build the bundled Xlib client examples (linked against libX11-compat.so)
examples: $(EXAMPLE_BINS) $(X11PERF_BIN)

$(OUT)/examples/%: examples/%.c $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< $(TARGET) \
	    $(LDLIBS) $(EXAMPLE_LDFLAGS) -o $@

$(X11PERF_BIN): $(X11PERF_SRCS) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      examples/x11perf"
	$(Q)$(CC) $(CPPFLAGS) -I$(X11PERF_DIR) $(CFLAGS) $(CFLAGS_EXTRA) \
	    -Wno-missing-field-initializers -Wno-unused-but-set-variable \
	    -Wno-sign-compare -DHAVE_CONFIG_H=1 \
	    $(X11PERF_SRCS) $(TARGET) $(LDLIBS) $(EXAMPLE_LDFLAGS) -o $@
