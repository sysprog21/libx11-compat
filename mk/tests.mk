CHECK_BINS := $(OUT)/tests/check $(OUT)/tests/symbol-coverage \
              $(OUT)/tests/test-libxt-link $(OUT)/tests/test-libxt-micro \
              $(OUT)/tests/test-libxt-resources \
              $(OUT)/tests/test-xmu-link \
              $(OUT)/tests/test-ice-sm-link \
              $(OUT)/tests/test-xinerama-link \
              $(OUT)/tests/test-libxpm-link \
              $(OUT)/tests/test-xft-link \
              $(OUT)/tests/test-xtest
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
# SDL_RUNTIME_LIBDIR (mk/sdl.mk) is the prefix lib dir needed on the loader
# path so an sdl2-compat libSDL2 can resolve its transitive libSDL3; build/
# stays first so the compat sonames win. Empty when SDL is undetected.
ifeq ($(strip $(SDL_RUNTIME_LIBDIR)),)
  TEST_RUNTIME_ENV :=
else
  TEST_RUNTIME_ENV := \
    DYLD_LIBRARY_PATH=$(abspath $(OUT)):$(SDL_RUNTIME_LIBDIR)$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    LD_LIBRARY_PATH=$(abspath $(OUT)):$(SDL_RUNTIME_LIBDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}
endif

.PHONY: check check-unit check-differential check-link-xaw symbol-coverage api-symbol-coverage bench bench-paths

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
		$(TEST_RUNTIME_ENV) SDL_VIDEODRIVER=dummy $$test_bin; \
	done
	@printf "$(BLUE)RUN$(RESET) tests/check-api-symbols.py\n"
	$(Q)$(PYTHON) tests/check-api-symbols.py $(TARGET) tests/api-symbols.txt

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
	@printf "$(BLUE)RUN$(RESET) check-smoke\n"
	$(Q)$(MAKE) --no-print-directory check-smoke
	@printf "$(BLUE)RUN$(RESET) check-differential\n"
	$(Q)$(MAKE) --no-print-directory check-differential

## Run all system-libX11-vs-libx11-compat differential checks
check-differential: check-differential-motif check-differential-violawww

## Build the Athena libXaw link-test binary.
check-link-xaw: $(OUT)/tests/test-libxaw-link

## Run exported-symbol coverage checks
symbol-coverage: $(OUT)/tests/symbol-coverage api-symbol-coverage
	$(TEST_RUNTIME_ENV) SDL_VIDEODRIVER=dummy $(OUT)/tests/symbol-coverage

api-symbol-coverage: $(TARGET) tests/api-symbols.txt tests/check-api-symbols.py
	$(PYTHON) tests/check-api-symbols.py $(TARGET) tests/api-symbols.txt

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
	$(Q)$(CC) $(LIBXT_CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(LIBXT_TARGET) $(TARGET) $(LDLIBS) $(LIBXT_TEST_LDFLAGS) \
	    $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-libxpm-%: tests/test-libxpm-%.c $(LIBXPM_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(LIBXPM_TARGET) $(TARGET) $(LDLIBS) $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-xmu-link: tests/test-xmu-link.c $(XMU_COMPAT_TARGET) $(LIBXT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(LIBXT_CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(XMU_COMPAT_TARGET) $(LIBXT_TARGET) $(TARGET) $(LDLIBS) \
	    $(LIBXT_TEST_LDFLAGS) $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-xinerama-link: tests/test-xinerama-link.c $(XINERAMA_COMPAT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(XINERAMA_COMPAT_TARGET) $(TARGET) $(LDLIBS) $(TEST_LDFLAGS) \
	    -o $@

$(OUT)/tests/test-xft-link: tests/test-xft-link.c $(XFT_COMPAT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(XFT_COMPAT_TARGET) $(TARGET) $(LDLIBS) $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-libxaw-link: tests/test-libxaw-link.c $(LIBXAW_TARGET) \
    $(LIBXT_TARGET) $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(LIBXAW_CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(LIBXAW_TARGET) $(LIBXT_TARGET) $(XMU_COMPAT_TARGET) \
	    $(LIBXPM_TARGET) $(TARGET) $(LDLIBS) $(LIBXT_TEST_LDFLAGS) \
	    $(TEST_LDFLAGS) -o $@

$(OUT)/tests/test-ice-sm-link: tests/test-ice-sm-link.c \
    $(SM_COMPAT_TARGET) $(ICE_COMPAT_TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(SM_COMPAT_TARGET) $(ICE_COMPAT_TARGET) $(LDLIBS) \
	    $(TEST_LDFLAGS) -o $@

## symbol-coverage references both libX11 and libXft symbols, so it must
## link against libXft-compat.so in addition to $(TARGET). The generic
## tests/%.c rule below only adds $(TARGET), so the explicit rule comes
## first to win pattern-precedence.
$(OUT)/tests/symbol-coverage: tests/symbol-coverage.c $(TARGET) $(XFT_COMPAT_TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< \
	    $(TARGET) $(XFT_COMPAT_TARGET) $(LDLIBS) $(TEST_LDFLAGS) -o $@

$(OUT)/tests/%: tests/%.c $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< $(TARGET) \
	    $(LDLIBS) $(TEST_LDFLAGS) -o $@
