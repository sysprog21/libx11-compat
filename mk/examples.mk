EXAMPLE_NAMES := 2048 paint life clock mandel processing clipboard catclock \
    moire
EXAMPLE_BINS := $(addprefix $(OUT)/examples/,$(EXAMPLE_NAMES))
ifeq ($(XCB),1)
  XCB_SHOWCASE_NAMES := xcb-kaleidoscope xcb-mandelbrot
  XCB_SHOWCASE_BINS := $(addprefix $(OUT)/examples/,$(XCB_SHOWCASE_NAMES))
  EXAMPLE_BINS += $(XCB_SHOWCASE_BINS)
endif
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
X11PERF_BENCH_ARGS ?= -all -repeat 1 -reps 1
EXAMPLE_LDFLAGS :=
ifeq ($(UNAME_S),Linux)
  EXAMPLE_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  EXAMPLE_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
endif

.PHONY: examples check-xcb-showcases bench-x11perf

## Build the bundled example clients (add XCB=1 for the native XCB ones)
examples: $(EXAMPLE_BINS) $(X11PERF_BIN)

## Run the XCB showcases headless under SDL's dummy driver (needs XCB=1)
check-xcb-showcases: $(XCB_SHOWCASE_BINS)
	@test "$(XCB)" = 1 || \
	    { echo "Error: check-xcb-showcases needs XCB=1" >&2; exit 1; }
	@set -e; for showcase in $(XCB_SHOWCASE_BINS); do \
	    echo "  RUN     $$showcase --smoke"; \
	    SDL_VIDEODRIVER=dummy $$showcase --smoke; \
	done

## Run the bundled x11perf benchmark in short regression mode
bench-x11perf: $(X11PERF_BIN)
	SDL_VIDEODRIVER=dummy $(X11PERF_BIN) $(X11PERF_BENCH_ARGS)

$(OUT)/examples/%: examples/%.c $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< $(TARGET) \
	    $(LDLIBS) $(EXAMPLE_LDFLAGS) -o $@

ifeq ($(XCB),1)
$(XCB_SHOWCASE_BINS): $(OUT)/examples/%: examples/%.c $(XCB_COMPAT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(XCB_COMPAT_TARGET) $(TARGET) $(LDLIBS) $(EXAMPLE_LDFLAGS) -o $@
endif

$(X11PERF_BIN): $(X11PERF_SRCS) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      examples/x11perf"
	$(Q)$(CC) $(CPPFLAGS) -I$(X11PERF_DIR) $(FP_CFLAGS) $(CFLAGS_EXTRA) \
	    -Wno-missing-field-initializers -Wno-unused-but-set-variable \
	    -Wno-sign-compare -DHAVE_CONFIG_H=1 \
	    $(X11PERF_SRCS) $(TARGET) $(LDLIBS) $(EXAMPLE_LDFLAGS) -o $@
