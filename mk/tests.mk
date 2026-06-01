CHECK_BINS := $(OUT)/tests/check $(OUT)/tests/symbol-coverage
BENCH_BINS := $(OUT)/tests/bench-paths

.PHONY: check symbol-coverage api-symbol-coverage bench-paths

## Build and run the regression test suite
check: $(CHECK_BINS)
	@set -e; for test_bin in $(CHECK_BINS); do \
		printf "$(BLUE)RUN$(RESET) %s\n" "$$test_bin"; \
		SDL_VIDEODRIVER=dummy $$test_bin; \
	done
	@printf "$(BLUE)RUN$(RESET) tests/check-api-symbols.py\n"
	$(Q)$(PYTHON) tests/check-api-symbols.py $(TARGET) tests/api-symbols.txt

## Run exported-symbol coverage checks
symbol-coverage: $(OUT)/tests/symbol-coverage api-symbol-coverage
	SDL_VIDEODRIVER=dummy $(OUT)/tests/symbol-coverage

api-symbol-coverage: $(TARGET) tests/api-symbols.txt tests/check-api-symbols.py
	$(PYTHON) tests/check-api-symbols.py $(TARGET) tests/api-symbols.txt

bench-paths: $(BENCH_BINS)
	SDL_VIDEODRIVER=dummy $(OUT)/tests/bench-paths

$(OUT)/tests/%: tests/%.c $(TARGET)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) $< $(TARGET) $(LDLIBS) -o $@
