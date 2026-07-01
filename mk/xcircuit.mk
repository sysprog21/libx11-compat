XCIRCUIT_URL := https://github.com/RTimothyEdwards/XCircuit
XCIRCUIT_REVISION := 30032514f600e5c2901479cf455fcb2952f0278f
XCIRCUIT_CLONE_FLAGS ?= --filter=blob:none --no-checkout
XCIRCUIT_SRC_DIR := $(OUT)/upstream/xcircuit-src
XCIRCUIT_SOURCE_STAMP := $(XCIRCUIT_SRC_DIR)/.source-stamp
XCIRCUIT_PATCHES := $(sort $(wildcard compat/xcircuit-patches/*.patch))

XCIRCUIT_BUILD_DIR := $(OUT)/xcircuit
XCIRCUIT_WORK_DIR := $(XCIRCUIT_BUILD_DIR)/source
XCIRCUIT_BIN := $(XCIRCUIT_WORK_DIR)/xcircuit
XCIRCUIT_LOG := $(abspath $(XCIRCUIT_BUILD_DIR))/build.log
XCIRCUIT_LIB_ALIASES := $(XCIRCUIT_BUILD_DIR)/lib-aliases
XCIRCUIT_LIB_ALIASES_STAMP := $(XCIRCUIT_LIB_ALIASES)/.stamp

XCIRCUIT_RPATH_FLAGS :=
ifeq ($(UNAME_S),Linux)
  XCIRCUIT_RPATH_FLAGS += -Wl,-rpath,$(abspath $(OUT)) \
      -Wl,-rpath,$(abspath $(XCIRCUIT_LIB_ALIASES)) \
      -Wl,-rpath-link,$(abspath $(OUT))
endif
ifeq ($(UNAME_S),Darwin)
  XCIRCUIT_RPATH_FLAGS += -Wl,-rpath,$(abspath $(OUT)) \
      -Wl,-rpath,$(abspath $(XCIRCUIT_LIB_ALIASES))
endif

XCIRCUIT_CPPFLAGS := \
    -I$(abspath include) \
    -I$(abspath $(OUT)/upstream/include) \
    -I$(abspath include/libxt-build)
# resources.h / Button.c use caddr_t, which glibc gates behind
# _DEFAULT_SOURCE. Apple's libc exposes it unconditionally via
# <sys/types.h>, so local macOS builds pass without the macro; Ubuntu's
# gcc fails Button.c with "caddr_t undeclared" unless it is set.
XCIRCUIT_CPPFLAGS += -D_DEFAULT_SOURCE
# The XCircuit sources predate C99 and trip modern default-error
# diagnostics. clang and gcc spell the pointer-mismatch and
# missing-prototype warnings differently, so gate the compiler-specific
# spellings by platform. Passing clang's names (incompatible-function-
# pointer-types, deprecated-non-prototype) to gcc makes -Wno-error=
# reject an unknown warning, and every compile including configure's own
# link test fails with "C compiler cannot create executables".
XCIRCUIT_CFLAGS := $(CFLAGS) $(CFLAGS_EXTRA) \
    -Wno-error=implicit-int \
    -Wno-error=implicit-function-declaration
ifeq ($(UNAME_S),Darwin)
  XCIRCUIT_CFLAGS += -Wno-error=incompatible-function-pointer-types \
      -Wno-deprecated-non-prototype
endif
ifeq ($(UNAME_S),Linux)
  XCIRCUIT_CFLAGS += -Wno-error=incompatible-pointer-types
endif
XCIRCUIT_LDFLAGS := -L$(abspath $(XCIRCUIT_LIB_ALIASES)) \
    -L$(abspath $(OUT)) $(XCIRCUIT_RPATH_FLAGS)

$(XCIRCUIT_SOURCE_STAMP): mk/xcircuit.mk
	@echo "  GIT     $(XCIRCUIT_URL)"
	$(Q)mkdir -p $(dir $(XCIRCUIT_SRC_DIR))
	$(Q)test -d $(XCIRCUIT_SRC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(XCIRCUIT_CLONE_FLAGS) $(XCIRCUIT_URL) $(XCIRCUIT_SRC_DIR)
	$(Q)cd $(XCIRCUIT_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin $(XCIRCUIT_REVISION) || \
	    (cd $(XCIRCUIT_SRC_DIR) && git fetch $(MOTIF_GIT_Q) origin)
	$(Q)cd $(XCIRCUIT_SRC_DIR) && \
	    git checkout $(MOTIF_GIT_Q) --detach $(XCIRCUIT_REVISION) && \
	    git reset --hard $(MOTIF_GIT_Q) $(XCIRCUIT_REVISION) >/dev/null && \
	    git clean $(MOTIF_GIT_Q) -fdx >/dev/null
	$(Q)printf '%s\n' '$(XCIRCUIT_REVISION)' > $@

$(XCIRCUIT_LIB_ALIASES_STAMP): $(LIBXT_TARGET) $(LIBXPM_TARGET) \
    $(ICE_COMPAT_TARGET) $(SM_COMPAT_TARGET) $(TARGET) mk/xcircuit.mk
	@mkdir -p $(XCIRCUIT_LIB_ALIASES)
	$(Q)rm -f $(XCIRCUIT_LIB_ALIASES)/libX*.so $(XCIRCUIT_LIB_ALIASES)/libX*.dylib \
	    $(XCIRCUIT_LIB_ALIASES)/libICE*.so $(XCIRCUIT_LIB_ALIASES)/libICE*.dylib \
	    $(XCIRCUIT_LIB_ALIASES)/libSM*.so $(XCIRCUIT_LIB_ALIASES)/libSM*.dylib
	$(Q)set -e; for pair in \
	    libX11.so:libX11-compat.so \
	    libXt.so:libXt-compat.so \
	    libXpm.so:libXpm-compat.so \
	    libICE.so:libICE-compat.so \
	    libSM.so:libSM-compat.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(XCIRCUIT_LIB_ALIASES)/$$alias"; \
	done
ifeq ($(UNAME_S),Darwin)
	$(Q)set -e; for pair in \
	    libX11.dylib:libX11-compat.so \
	    libXt.dylib:libXt-compat.so \
	    libXpm.dylib:libXpm-compat.so \
	    libICE.dylib:libICE-compat.so \
	    libSM.dylib:libSM-compat.so; do \
	    alias="$${pair%%:*}"; target="$${pair##*:}"; \
	    ln -sf "$(abspath $(OUT))/$$target" "$(XCIRCUIT_LIB_ALIASES)/$$alias"; \
	done
endif
	$(Q)touch $@

.PHONY: xcircuit check-differential-xcircuit xcircuit-clean
## Build XCircuit against the libx11-compat + libXt compatibility stack
xcircuit: $(XCIRCUIT_BIN)

$(XCIRCUIT_BIN): $(XCIRCUIT_SOURCE_STAMP) $(XCIRCUIT_PATCHES) $(PKGCONFIG_FILES) \
    $(XCIRCUIT_LIB_ALIASES_STAMP)
	@mkdir -p $(XCIRCUIT_BUILD_DIR)
	$(Q)rm -f $(XCIRCUIT_LOG)
	$(Q)rm -rf $(XCIRCUIT_WORK_DIR)
	$(Q)mkdir -p $(XCIRCUIT_WORK_DIR)
	# Stage the source copy through a temp tarball rather than a tar | tar
	# pipe: a pipe reports only the reader's exit status, so a failed
	# source-side tar (partial copy) would go undetected. Two separate
	# recipe lines let make catch either failure.
	$(Q)tar --exclude .git -cf $(XCIRCUIT_BUILD_DIR)/.src.tar -C $(XCIRCUIT_SRC_DIR) .
	$(Q)tar -xf $(XCIRCUIT_BUILD_DIR)/.src.tar -C $(XCIRCUIT_WORK_DIR)
	$(Q)rm -f $(XCIRCUIT_BUILD_DIR)/.src.tar
	$(Q)set -e; for patch in $(XCIRCUIT_PATCHES); do \
	    patch -d $(XCIRCUIT_WORK_DIR) -p1 < "$$patch"; \
	done
	@echo "  CONF    xcircuit"
	$(Q)cd $(XCIRCUIT_WORK_DIR) && \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
	    CC='$(CC)' \
	    CPPFLAGS='$(XCIRCUIT_CPPFLAGS)' \
	    CFLAGS='$(XCIRCUIT_CFLAGS)' \
	    LDFLAGS='$(XCIRCUIT_LDFLAGS)' \
	    ./configure --prefix=$(abspath $(XCIRCUIT_BUILD_DIR))/install \
	        --without-tcl \
	        --without-cairo \
	        --without-python \
	        --with-xpm=$(abspath $(OUT)) \
	        --x-includes=$(abspath include) \
	        --x-libraries=$(abspath $(XCIRCUIT_LIB_ALIASES)) \
	        >> $(XCIRCUIT_LOG) 2>&1 || { \
	            echo "  FAIL    see $(XCIRCUIT_LOG)" >&2; \
	            tail -60 $(XCIRCUIT_LOG) >&2; \
	            exit 1; \
	        }
	@echo "  MAKE    xcircuit"
	$(Q)$(MAKE) -C $(XCIRCUIT_WORK_DIR) \
	    librarydir=$(abspath $(XCIRCUIT_WORK_DIR))/lib \
	    scriptsdir=$(abspath $(XCIRCUIT_WORK_DIR))/lib \
	    >> $(XCIRCUIT_LOG) 2>&1 || { \
	    echo "  FAIL    see $(XCIRCUIT_LOG)" >&2; \
	    tail -60 $(XCIRCUIT_LOG) >&2; \
	    exit 1; \
	}

XCIRCUIT_DIFF_REMOTE ?= node11
XCIRCUIT_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-xcircuit-differential
XCIRCUIT_DIFF_DISPLAY ?= 131
XCIRCUIT_DIFF_JOBS ?= 1
XCIRCUIT_DIFF_INSTALL_DEPS ?= 0
XCIRCUIT_DIFF_LOCAL ?= 0
XCIRCUIT_DIFF_MAE_THRESHOLD ?= 0.20
XCIRCUIT_DIFF_CHANGED_THRESHOLD ?= 0.50
XCIRCUIT_DIFF_GEOMETRY ?= 1280x1024x24
XCIRCUIT_DIFF_TOP ?= 12
XCIRCUIT_DIFF_COMPARE_LOCATION ?= $(if $(filter 1 yes true,$(XCIRCUIT_DIFF_LOCAL)),local,remote)
XCIRCUIT_DIFF_OUT_ROOT ?= $(OUT)/xcircuit-differential
XCIRCUIT_DIFF_SCREENSHOT_REGION ?= 0,0,1024,768

xcircuit_diff_env = \
    XCIRCUIT_DIFF_REMOTE='$(XCIRCUIT_DIFF_REMOTE)' \
    XCIRCUIT_DIFF_REMOTE_ROOT='$(XCIRCUIT_DIFF_REMOTE_ROOT)' \
    XCIRCUIT_DIFF_DISPLAY='$(XCIRCUIT_DIFF_DISPLAY)' \
    XCIRCUIT_DIFF_JOBS='$(XCIRCUIT_DIFF_JOBS)' \
    XCIRCUIT_DIFF_MAE_THRESHOLD='$(XCIRCUIT_DIFF_MAE_THRESHOLD)' \
    XCIRCUIT_DIFF_CHANGED_THRESHOLD='$(XCIRCUIT_DIFF_CHANGED_THRESHOLD)' \
    XCIRCUIT_DIFF_GEOMETRY='$(XCIRCUIT_DIFF_GEOMETRY)' \
    XCIRCUIT_DIFF_TOP='$(XCIRCUIT_DIFF_TOP)' \
    XCIRCUIT_DIFF_COMPARE_LOCATION='$(XCIRCUIT_DIFF_COMPARE_LOCATION)' \
    XCIRCUIT_DIFF_OUT_ROOT='$(abspath $(XCIRCUIT_DIFF_OUT_ROOT))' \
    XCIRCUIT_DIFF_SCREENSHOT_REGION='$(XCIRCUIT_DIFF_SCREENSHOT_REGION)'

## Compare XCircuit screenshots for system X11 vs libx11-compat.
check-differential-xcircuit:
	$(Q)$(xcircuit_diff_env) $(PYTHON) scripts/run-xcircuit-differential-tests.py \
	    $(if $(filter 1 yes true,$(XCIRCUIT_DIFF_INSTALL_DEPS)),--install-deps) \
	    $(if $(filter 1 yes true,$(XCIRCUIT_DIFF_LOCAL)),--local)

xcircuit-clean:
	@echo "  CLEAN   xcircuit"
	$(Q)rm -rf $(XCIRCUIT_BUILD_DIR)
