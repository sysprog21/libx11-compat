# Formatting targets.

C_FORMAT_FILES := $(shell find include src tests -type f \( -name '*.c' -o -name '*.h' \) | sort)
PYTHON_FORMAT_FILES := $(shell find scripts -type f -name '*.py' | sort)

BLACK ?= black

.PHONY: indent check-format

## Format C sources, headers, and Python scripts
indent:
	@echo "  FMT     include/ src/ tests/"
	$(Q)$(CLANG_FORMAT) -i $(C_FORMAT_FILES)
ifneq ($(PYTHON_FORMAT_FILES),)
	@echo "  BLACK   scripts/"
	$(Q)$(BLACK) $(PYTHON_FORMAT_FILES)
endif

## Check C source, header, and Python formatting
check-format:
	@echo "  FMT     include/ src/ tests/ (check)"
	$(Q)$(CLANG_FORMAT) --dry-run --Werror $(C_FORMAT_FILES)
ifneq ($(PYTHON_FORMAT_FILES),)
	@echo "  BLACK   scripts/ (check)"
	$(Q)$(BLACK) --check $(PYTHON_FORMAT_FILES)
endif
