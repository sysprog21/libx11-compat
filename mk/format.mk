# Formatting targets.

C_FORMAT_FILES := $(shell find include src tests -type f \( -name '*.c' -o -name '*.h' \) | sort)
PYTHON_FORMAT_FILES := $(shell find scripts -type f -name '*.py' | sort)
SHELL_FORMAT_FILES := $(shell find .ci -type f -name '*.sh' 2>/dev/null | sort)

BLACK ?= black

.PHONY: indent check-format _clang_format_guard _shfmt_guard

# Fail early if mk/toolchain.mk could not locate a clang-format that
# reports the required major version. The shell guard prints the
# requested name when CLANG_FORMAT is set but not v20 (e.g. an explicit
# override on the command line) and the canonical fallback when
# detection returned empty.
_clang_format_guard:
	@if [ -z "$(CLANG_FORMAT)" ]; then \
	    echo "Error: clang-format version $(CLANG_FORMAT_REQUIRED_VERSION) not found on PATH." >&2; \
	    echo "       Install clang-format-$(CLANG_FORMAT_REQUIRED_VERSION) or set CLANG_FORMAT to a v$(CLANG_FORMAT_REQUIRED_VERSION) binary." >&2; \
	    exit 1; \
	fi
	@v=$$($(CLANG_FORMAT) --version 2>/dev/null | awk '/version/ { for (i = 1; i <= NF; i++) if ($$i ~ /^[0-9]+\./) { split($$i, a, "."); print a[1]; exit } }'); \
	if [ "$$v" != "$(CLANG_FORMAT_REQUIRED_VERSION)" ]; then \
	    echo "Error: $(CLANG_FORMAT) reports version '$$v', expected $(CLANG_FORMAT_REQUIRED_VERSION)." >&2; \
	    exit 1; \
	fi

# shfmt is required when there are shell scripts to format. The style
# is sourced from the [*.sh] section of .editorconfig so this Makefile
# does not duplicate -i / -bn / -ci flags.
_shfmt_guard:
ifneq ($(SHELL_FORMAT_FILES),)
	@if ! command -v $(SHFMT) >/dev/null 2>&1; then \
	    echo "Error: $(SHFMT) not found on PATH (install shfmt to format shell scripts)." >&2; \
	    exit 1; \
	fi
endif

## Format C sources, headers, Python scripts, and shell scripts
indent: _clang_format_guard _shfmt_guard
	@echo "  FMT     include/ src/ tests/ ($(CLANG_FORMAT))"
	$(Q)$(CLANG_FORMAT) -i $(C_FORMAT_FILES)
ifneq ($(PYTHON_FORMAT_FILES),)
	@echo "  BLACK   scripts/"
	$(Q)$(BLACK) $(PYTHON_FORMAT_FILES)
endif
ifneq ($(SHELL_FORMAT_FILES),)
	@echo "  SHFMT   .ci/ ($(SHFMT))"
	$(Q)$(SHFMT) -w $(SHELL_FORMAT_FILES)
endif

## Check C, Python, and shell formatting
check-format: _clang_format_guard _shfmt_guard
	@echo "  FMT     include/ src/ tests/ (check, $(CLANG_FORMAT))"
	$(Q)$(CLANG_FORMAT) --dry-run --Werror $(C_FORMAT_FILES)
ifneq ($(PYTHON_FORMAT_FILES),)
	@echo "  BLACK   scripts/ (check)"
	$(Q)$(BLACK) --check $(PYTHON_FORMAT_FILES)
endif
ifneq ($(SHELL_FORMAT_FILES),)
	@echo "  SHFMT   .ci/ (check, $(SHFMT))"
	$(Q)$(SHFMT) -d $(SHELL_FORMAT_FILES)
endif
