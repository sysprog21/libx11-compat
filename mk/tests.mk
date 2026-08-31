CHECK_BINS := $(OUT)/tests/check $(OUT)/tests/symbol-coverage \
              $(OUT)/tests/test-libxt-link $(OUT)/tests/test-libxt-micro \
              $(OUT)/tests/test-libxt-resources \
              $(OUT)/tests/test-xmu-link \
              $(OUT)/tests/test-ice-sm-link \
              $(OUT)/tests/test-xinerama-link \
              $(OUT)/tests/test-libxpm-link \
              $(OUT)/tests/test-xft-link \
              $(OUT)/tests/test-xlibint-link \
              $(OUT)/tests/test-xtest
ifeq ($(XCB),1)
CHECK_BINS += $(OUT)/tests/test-xcb-link
CHECK_BINS += $(OUT)/tests/test-xcb-setup
CHECK_BINS += $(OUT)/tests/test-xcb-window $(OUT)/tests/test-xcb-property
CHECK_BINS += $(OUT)/tests/test-xlib-xcb
CHECK_BINS += $(OUT)/tests/test-xcb-events
CHECK_BINS += $(OUT)/tests/test-xcb-drawing
endif
# The GLX tests only exist when the optional GLX layer is built (GLX=1).
# test-glx-link covers the no-provider degrade path; test-glx-provider drives the
# full GLX->EGL translation against the in-tree fake EGL provider.
FAKE_EGL_LIB := $(OUT)/tests/libEGL-fake.so
ifeq ($(GLX),1)
  CHECK_BINS += $(OUT)/tests/test-glx-link $(OUT)/tests/test-glx-provider \
                $(OUT)/tests/test-glx-init-fail
endif
# The libXaw link test only runs in-process on Linux. The macOS dyld loader
# hangs before main() while resolving the libXaw -> libXt -> libXmu chain
# under SDL_VIDEODRIVER=dummy (see TODO.md "Athena widget stack" Tier 1).
# Until that lockup has a fix, the test binary is still produced via
# check-link-xaw so the build path is gated, but it is not auto-run on
# Darwin.
ifeq ($(UNAME_S),Linux)
  CHECK_BINS += $(OUT)/tests/test-libxaw-link
endif
BENCH_BINS := $(OUT)/tests/bench-paths

# Every compat test goes through libX11-compat, which dlopens the host SDL.
# Test binaries already embed build/ as an rpath on Darwin; keep build/ out of
# DYLD_LIBRARY_PATH so the shared libGL.dylib does not get preloaded into SDL's
# non-GL GUI stack by accident. Linux still needs build/ on LD_LIBRARY_PATH.
ifeq ($(UNAME_S),Darwin)
  ifeq ($(strip $(SDL_RUNTIME_LIBDIR)),)
    TEST_RUNTIME_ENV :=
  else
    TEST_RUNTIME_ENV := \
      DYLD_LIBRARY_PATH=$(SDL_RUNTIME_LIBDIR)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH}
  endif
else
  ifeq ($(strip $(SDL_RUNTIME_LIBDIR)),)
    TEST_RUNTIME_ENV := \
      LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}
  else
    TEST_RUNTIME_ENV := \
      LD_LIBRARY_PATH=$(abspath $(OUT)):$(SDL_RUNTIME_LIBDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}
  endif
endif

.PHONY: check check-unit check-differential check-link-xaw symbol-coverage api-symbol-coverage bench bench-paths check-xcb-reference update-xcb-reference

XCB_REFERENCE_BIN := $(OUT)/tests/probe-system-xcb
XCB_REFERENCE_OUT := tests/data/xcb-reference.txt

$(XCB_REFERENCE_BIN): tests/probe-system-xcb.c | $(OUT)
	@mkdir -p $(dir $@)
	@echo "  CC      $< (system XCB)"
	$(Q)$(CC) $(FP_CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $$($(PKG_CONFIG) --cflags --libs xcb) -o $@

## Regenerate the deterministic system-XCB oracle under an isolated Xvfb.
update-xcb-reference: $(XCB_REFERENCE_BIN)
	@command -v xvfb-run >/dev/null || { echo "Error: xvfb-run is required" >&2; exit 1; }
	$(Q)xvfb-run -a -s '-screen 0 320x240x24 -nolisten tcp' \
	    $(XCB_REFERENCE_BIN) > $(XCB_REFERENCE_OUT).tmp; \
	    mv $(XCB_REFERENCE_OUT).tmp $(XCB_REFERENCE_OUT)

## Compare the core-XCB probe with the checked-in system-XCB oracle.
check-xcb-reference: $(XCB_REFERENCE_BIN) $(XCB_REFERENCE_OUT)
	@command -v xvfb-run >/dev/null || { echo "Error: xvfb-run is required" >&2; exit 1; }
	$(Q)tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	    xvfb-run -a -s '-screen 0 320x240x24 -nolisten tcp' \
	        $(XCB_REFERENCE_BIN) >"$$tmp/actual"; \
	    diff -u $(XCB_REFERENCE_OUT) "$$tmp/actual"

DIFFERENTIAL_TARGETS := check-differential-motif check-differential-violawww \
                        check-differential-xmms
ifeq ($(GLX),1)
  DIFFERENTIAL_TARGETS += check-differential-glx-linux
endif

## Run only the in-tree binary regression tests + api-symbol coverage.
## This is the cheap, sanitizer-friendly subset: no motif autoconf, no
## ViolaWWW build, no replay-driven UI smoke. Use this from CI sanitize
## jobs and from any caller that cannot afford a full motif build under
## CFLAGS_EXTRA flags (e.g. SAN_FLAGS), since the motif autoconf script
## probes for Xutf8TextExtents via a link test that fails when the wrong
## flag set leaks into the upstream configure.
check-unit: $(CHECK_BINS)
	@set -e; for test_bin in $(CHECK_BINS); do \
		printf "$(BLUE)RUN$(RESET) %s\n" "$$test_bin"; \
		$(TEST_RUNTIME_ENV) SDL_VIDEODRIVER=dummy LIBX11_COMPAT_STATE_SNAPSHOT_TIMEOUT_SEC=2 $$test_bin; \
	done
	@printf "$(BLUE)RUN$(RESET) tests/check-api-symbols.py\n"
	$(Q)LIBX11_COMPAT_GLX=$(GLX) $(PYTHON) tests/check-api-symbols.py $(TARGET) tests/api-symbols.txt
	@printf "$(BLUE)RUN$(RESET) tests/check-host-link-audit.py\n"
	$(Q)$(PYTHON) tests/check-host-link-audit.py $(wildcard $(OUT)/lib*-compat.so)
ifeq ($(XCB),1)
	@printf "$(BLUE)RUN$(RESET) tests/test-check-xcb-symbols.py\n"
	$(Q)$(PYTHON) tests/test-check-xcb-symbols.py
	@printf "$(BLUE)RUN$(RESET) scripts/check-xcb-symbols.py\n"
	$(Q)$(PYTHON) scripts/check-xcb-symbols.py $(XCB_COMPAT_TARGET) \
	    tests/xcb-symbols.txt
endif

## Full local regression suite: unit tests, motif link/demos gates, the
## replay smoke tier, and the SSH-backed differential screenshots. The
## sub-targets are invoked via $(MAKE) so progress markers stay ordered
## under -jN; failing prereqs surface inside the recipe rather than at
## dependency resolution.
check: check-unit
	@printf "$(BLUE)RUN$(RESET) check-link-motif\n"
	$(Q)$(MAKE) --no-print-directory check-link-motif
	@printf "$(BLUE)RUN$(RESET) check-demos-motif\n"
	$(Q)$(MAKE) --no-print-directory check-demos-motif
	@printf "$(BLUE)RUN$(RESET) check-glx\n"
	$(Q)$(MAKE) --no-print-directory check-glx
	@printf "$(BLUE)RUN$(RESET) check-smoke\n"
	$(Q)$(MAKE) --no-print-directory check-smoke
	@printf "$(BLUE)RUN$(RESET) check-differential\n"
	$(Q)$(MAKE) --no-print-directory check-differential

## GLX render tier: the desktop-GL-over-EGL/ANGLE screenshot gates (the Mesa
## xdemos and Motif paperplane). Both sub-targets self-skip when GLX=0 or the
## runtime provider (ANGLE) is absent, so this is safe in any configuration and
## is the single entry point for the GLX rendering checks.
.PHONY: check-glx
check-glx:
	@printf "$(BLUE)RUN$(RESET) check-mesa-demos\n"
	$(Q)$(MAKE) --no-print-directory check-mesa-demos
	@printf "$(BLUE)RUN$(RESET) check-paperplane-macos\n"
	$(Q)$(MAKE) --no-print-directory check-paperplane-macos
	@printf "$(BLUE)RUN$(RESET) check-paperplane-linux\n"
	$(Q)$(MAKE) --no-print-directory check-paperplane-linux
	@printf "$(BLUE)RUN$(RESET) check-gl4es-wrap\n"
	$(Q)$(MAKE) --no-print-directory check-gl4es-wrap

## Run all system-libX11-vs-libx11-compat differential checks
check-differential: $(DIFFERENTIAL_TARGETS)

## GLX differential: render fixed GLES2 cases (single-context and a two-context
## switch) through our GLX layer and through direct EGL on the same system Mesa
## driver under Xvfb, and assert the readbacks are identical (see
## scripts/run-glx-differential-tests.py). Runs over
## SSH to a Linux host by default; GLX_DIFF_LOCAL=1 runs it on this host instead
## (the CI path, where the runner already has Mesa + Xvfb).
.PHONY: check-differential-glx-linux
check-differential-glx-linux:
	$(Q)$(PYTHON) scripts/run-glx-differential-tests.py \
	    $(if $(filter 1 yes true,$(GLX_DIFF_LOCAL)),--local)

## Build the Athena libXaw link-test binary.
check-link-xaw: $(OUT)/tests/test-libxaw-link

## Run exported-symbol coverage checks
symbol-coverage: $(OUT)/tests/symbol-coverage api-symbol-coverage
	$(TEST_RUNTIME_ENV) SDL_VIDEODRIVER=dummy $(OUT)/tests/symbol-coverage

api-symbol-coverage: $(TARGET) tests/api-symbols.txt tests/check-api-symbols.py
	LIBX11_COMPAT_GLX=$(GLX) $(PYTHON) tests/check-api-symbols.py $(TARGET) tests/api-symbols.txt

## Run all local benchmark suites
bench:
	@printf "$(BLUE)RUN$(RESET) bench-paths\n"
	$(Q)$(MAKE) --no-print-directory bench-paths
	@printf "$(BLUE)RUN$(RESET) bench-x11perf\n"
	$(Q)$(MAKE) --no-print-directory bench-x11perf

## Run focused in-tree path/event microbenchmarks
bench-paths: $(BENCH_BINS)
	SDL_VIDEODRIVER=dummy $(OUT)/tests/bench-paths

# libXt-aware tests need both the upstream libXt headers (staged under
# $(OUT)/upstream/include) and a link line that pulls in libXt-compat.so
# *and* libX11-compat.so. They are compiled with the libxt-build config.h
# the same way the libXt translation units are, so XT_NO_SM stays defined
# and the public Xt headers see consistent feature flags. -rpath-link
# silences a Linux-only warning where ld walks libXt-compat.so's
# DT_NEEDED entries while linking the test and cannot find the sibling
# libX11-compat.so on a default -L path.
LIBXT_TEST_LDFLAGS :=
ifeq ($(UNAME_S),Linux)
  LIBXT_TEST_LDFLAGS += -Wl,-rpath-link,$(OUT)
endif
TEST_LDFLAGS :=
ifeq ($(UNAME_S),Linux)
  TEST_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  TEST_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
endif
$(OUT)/tests/test-libxt-%: tests/test-libxt-%.c $(LIBXT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(LIBXT_CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(LIBXT_TARGET) $(TARGET) $(LDLIBS) $(LIBXT_TEST_LDFLAGS) \
	    $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-libxpm-%: tests/test-libxpm-%.c $(LIBXPM_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(LIBXPM_TARGET) $(TARGET) $(LDLIBS) $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-xmu-link: tests/test-xmu-link.c $(XMU_COMPAT_TARGET) $(LIBXT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(LIBXT_CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(XMU_COMPAT_TARGET) $(LIBXT_TARGET) $(TARGET) $(LDLIBS) \
	    $(LIBXT_TEST_LDFLAGS) $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-xinerama-link: tests/test-xinerama-link.c $(XINERAMA_COMPAT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(XINERAMA_COMPAT_TARGET) $(TARGET) $(LDLIBS) $(TEST_LDFLAGS) \
	    -o $@

$(OUT)/tests/test-xft-link: tests/test-xft-link.c $(XFT_COMPAT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(XFT_COMPAT_TARGET) $(TARGET) $(LDLIBS) $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-libxaw-link: tests/test-libxaw-link.c $(LIBXAW_TARGET) \
    $(LIBXT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(LIBXAW_CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(LIBXAW_TARGET) $(LIBXT_TARGET) $(XMU_COMPAT_TARGET) \
	    $(LIBXPM_TARGET) $(TARGET) $(LDLIBS) $(LIBXT_TEST_LDFLAGS) \
	    $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-ice-sm-link: tests/test-ice-sm-link.c \
    $(SM_COMPAT_TARGET) $(ICE_COMPAT_TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(SM_COMPAT_TARGET) $(ICE_COMPAT_TARGET) $(LDLIBS) \
	    $(TEST_LDFLAGS) -o $@

## symbol-coverage references both libX11 and libXft symbols, so it must
## link against libXft-compat.so in addition to $(TARGET). The generic
## tests/%.c rule below only adds $(TARGET), so the explicit rule comes
## first to win pattern-precedence.
$(OUT)/tests/symbol-coverage: tests/symbol-coverage.c $(TARGET) $(XFT_COMPAT_TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(TARGET) $(XFT_COMPAT_TARGET) $(LDLIBS) $(TEST_LDFLAGS) -o $@

# The fake EGL provider is a standalone loadable object (no libX11-compat link);
# test-glx-provider dlopens it at runtime via LIBX11_COMPAT_EGL. The absolute
# path is baked in so the test resolves it regardless of the working directory.
$(FAKE_EGL_LIB): tests/fake-egl.c
	@mkdir -p $(dir $@)
	@echo "  LD      $@"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) -shared -o $@ $<

$(OUT)/tests/test-glx-provider: tests/test-glx-provider.c $(TARGET) $(FAKE_EGL_LIB)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) -DFAKE_EGL_PATH=\"$(abspath $(FAKE_EGL_LIB))\" \
	    $(FP_CFLAGS) $(CFLAGS_EXTRA) $< $(TARGET) $(LDLIBS) $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-glx-init-fail: tests/test-glx-init-fail.c $(TARGET) $(FAKE_EGL_LIB)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) -DFAKE_EGL_PATH=\"$(abspath $(FAKE_EGL_LIB))\" \
	    $(FP_CFLAGS) $(CFLAGS_EXTRA) $< $(TARGET) $(LDLIBS) $(TEST_LDFLAGS) -o $@

# check and test-xtest are whitebox tests: they call core internals directly.
# Linking them against the fat test twin (libX11-compat-test.so) instead of the
# pinned .so keeps those ~50 internals out of the shipped library's export list.
# The twin, not the bare objects, because on Linux the whitebox binary shares a
# process with the system libX11.so.6 that SDL2 loads; only a -Bsymbolic shared
# library (which an executable cannot be) keeps our lock/event globals from being
# interposed by it. Everything else links the pinned .so via the rule below.
#
# The twin only exists on the native .so build; under WASM=1 X11_TEST_LIB is
# empty, so fall back to $(TARGET), which is the full-symbol static archive
# there (no export pinning, no system libX11 to interpose) exactly as the
# generic rule linked these before this change.
X11_WHITEBOX_LIB := $(if $(X11_TEST_LIB),$(X11_TEST_LIB),$(TARGET))
$(OUT)/tests/check $(OUT)/tests/test-xtest: $(OUT)/tests/%: tests/%.c $(X11_WHITEBOX_LIB)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< $(X11_WHITEBOX_LIB) \
	    $(LDLIBS) $(TEST_LDFLAGS) -o $@

$(OUT)/tests/%: tests/%.c $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(CFLAGS_EXTRA) $< $(TARGET) \
	    $(LDLIBS) $(TEST_LDFLAGS) -o $@

ifeq ($(XCB),1)
$(OUT)/tests/test-xcb-setup: tests/test-xcb-setup.c $(XCB_COMPAT_OBJS) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(XCB_COMPAT_OBJS) $(TARGET) $(LDLIBS) $(TEST_LDFLAGS) -o $@

XCB_TESTS := test-xcb-link test-xcb-window test-xcb-property test-xcb-drawing \
             test-xlib-xcb test-xcb-events

# One recipe; each test names the libraries it needs and nothing more, so the
# link test keeps proving that libxcb-compat resolves on its own.
$(OUT)/tests/test-xcb-link: XCB_TEST_LIBS := -lxcb-compat
$(OUT)/tests/test-xcb-window $(OUT)/tests/test-xcb-property \
$(OUT)/tests/test-xcb-drawing: XCB_TEST_LIBS := -lxcb-compat -lX11-compat
$(OUT)/tests/test-xlib-xcb $(OUT)/tests/test-xcb-events: \
    XCB_TEST_LIBS := -lX11-xcb-compat -lxcb-compat -lX11-compat

$(addprefix $(OUT)/tests/,$(XCB_TESTS)): $(OUT)/tests/%: tests/%.c \
    $(X11_XCB_COMPAT_TARGET) $(XCB_COMPAT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) $< \
	    -L$(OUT) $(XCB_TEST_LIBS) $(TEST_LDFLAGS) -o $@
endif
