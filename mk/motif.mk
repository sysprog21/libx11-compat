MOTIF_COMMIT := 11cd50b42991e8f11a13522fd820d736cc2f7fcf
MOTIF_URL := https://github.com/thentenaar/motif
MOTIF_SRC_DIR := $(OUT)/upstream/motif-src
MOTIF_SRC_STAMP := $(MOTIF_SRC_DIR)/.source-stamp
MOTIF_PATCHES := $(wildcard compat/motif-patches/*.patch)
MOTIF_AUTOGEN_STAMP := $(MOTIF_SRC_DIR)/.autogen-stamp
MOTIF_AUTOGEN_LOG := $(abspath $(MOTIF_SRC_DIR))/autoreconf.log
MOTIF_BUILD_DIR := $(OUT)/motif
MOTIF_CONFIG_STAMP := $(MOTIF_BUILD_DIR)/.configure-stamp
MOTIF_CONFIG_LOG := $(abspath $(MOTIF_BUILD_DIR))/configure.log
MOTIF_BUILD_STAMP := $(MOTIF_BUILD_DIR)/.build-stamp
MOTIF_DEMOS_BUILD_DIR := $(OUT)/motif-demos
MOTIF_DEMOS_CONFIG_STAMP := $(MOTIF_DEMOS_BUILD_DIR)/.configure-stamp
MOTIF_DEMOS_CONFIG_LOG := $(abspath $(MOTIF_DEMOS_BUILD_DIR))/configure.log
MOTIF_DEMOS_BUILD_STAMP := $(MOTIF_DEMOS_BUILD_DIR)/.build-stamp
MOTIF_LIBXM := $(OUT)/libXm.so
MOTIF_LIBMRM := $(OUT)/libMrm.so
MOTIF_YACC ?= $(shell if [ -x /opt/homebrew/opt/bison/bin/bison ]; then printf '%s\n' '/opt/homebrew/opt/bison/bin/bison -y'; else printf '%s\n' yacc; fi)
MOTIF_CFLAGS ?= -g -O0
MOTIF_LIBS ?= -lm
MOTIF_CONFIGURE_LDFLAGS := -L$(abspath $(OUT))
ifeq ($(UNAME_S),Darwin)
  MOTIF_CONFIGURE_LDFLAGS += -Wl,-rpath,$(abspath $(OUT))
  MOTIF_LIBS += -liconv
endif
motif_runtime_env = \
    DYLD_LIBRARY_PATH=$(abspath $(OUT))$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH} \
    LD_LIBRARY_PATH=$(abspath $(OUT))$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}

# Verbosity. Default builds hide upstream noise: -s on the recursive
# make silences per-file gcc lines, --quiet on git skips clone/checkout
# progress, and autoreconf / configure stdout+stderr land in per-step
# log files so a failed quiet build still leaves a forensic trail. V=1
# disables all of that.
ifeq ($(V),1)
  MOTIF_SUBMAKE := $(MAKE)
  MOTIF_GIT_Q :=
  motif_log_redirect =
else
  MOTIF_SUBMAKE := $(MAKE) -s
  MOTIF_GIT_Q := --quiet
  motif_log_redirect = >> $(1) 2>&1 || { echo "  FAIL    see $(1)" >&2; tail -40 $(1) >&2; exit 1; }
endif

MOTIF_COMMON_CONFIGURE_FLAGS := \
    --prefix=$(abspath $(OUT)/motif-install) \
    --disable-glw \
    --disable-tests \
    --without-xft \
    --with-jpeg=no \
    --with-png=no \
    --with-xrandr=no \
    --with-xrender=no \
    --with-xcursor=no \
    --with-xinerama=no

MOTIF_CONFIGURE_FLAGS := \
    $(MOTIF_COMMON_CONFIGURE_FLAGS) \
    --disable-demos

MOTIF_DEMOS_CONFIGURE_FLAGS := \
    $(MOTIF_COMMON_CONFIGURE_FLAGS) \
    --enable-demos

$(MOTIF_SRC_STAMP):
	@echo "  GIT     $(MOTIF_URL)"
	$(Q)mkdir -p $(dir $(MOTIF_SRC_DIR))
	$(Q)test -d $(MOTIF_SRC_DIR)/.git || \
	    git clone $(MOTIF_GIT_Q) $(MOTIF_URL) $(MOTIF_SRC_DIR)
	$(Q)cd $(MOTIF_SRC_DIR) && git checkout $(MOTIF_GIT_Q) --detach $(MOTIF_COMMIT)
	$(Q)for patch in $(abspath $(MOTIF_PATCHES)); do \
	    cd $(MOTIF_SRC_DIR) && \
	    if git apply --check "$$patch"; then \
	        git apply "$$patch"; \
	    elif git apply --reverse --check "$$patch"; then \
	        :; \
	    else \
	        echo "  PATCH   failed $$patch" >&2; exit 1; \
	    fi; \
	done
	$(Q)touch $@

$(MOTIF_AUTOGEN_STAMP): $(MOTIF_SRC_STAMP)
	@echo "  AUTOGEN motif"
	$(Q)cd $(MOTIF_SRC_DIR) && autoreconf -fi \
	    $(call motif_log_redirect,$(MOTIF_AUTOGEN_LOG))
	$(Q)touch $@

$(MOTIF_BUILD_DIR):
	@mkdir -p $@

$(MOTIF_DEMOS_BUILD_DIR):
	@mkdir -p $@

$(MOTIF_CONFIG_STAMP): $(MOTIF_AUTOGEN_STAMP) $(PKGCONFIG_FILES) \
    $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(XINERAMA_COMPAT_TARGET) | $(MOTIF_BUILD_DIR)
	@echo "  CONFIG  motif"
	$(Q)cd $(MOTIF_BUILD_DIR) && \
	    $(motif_runtime_env) \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
	    CPP="$(CC) -E" \
	    CPPFLAGS="-include stdlib.h" \
	    CFLAGS="$(MOTIF_CFLAGS)" \
	    LDFLAGS="$(MOTIF_CONFIGURE_LDFLAGS)" \
	    YACC="$(MOTIF_YACC)" \
	    $(abspath $(MOTIF_SRC_DIR))/configure $(MOTIF_CONFIGURE_FLAGS) \
	    $(call motif_log_redirect,$(MOTIF_CONFIG_LOG))
	$(Q)touch $@

$(MOTIF_BUILD_STAMP): $(MOTIF_CONFIG_STAMP)
	@echo "  MAKE    motif config"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_BUILD_DIR)/config LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_BUILD_DIR))/build.log)
	@echo "  MAKE    motif lib/Xm"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_BUILD_DIR)/lib/Xm CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_BUILD_DIR))/build.log)
	@echo "  MAKE    motif lib/Mrm"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_BUILD_DIR)/lib/Mrm CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_BUILD_DIR))/build.log)
	$(Q)touch $@

$(MOTIF_DEMOS_CONFIG_STAMP): $(MOTIF_AUTOGEN_STAMP) $(PKGCONFIG_FILES) \
    $(TARGET) $(LIBXT_TARGET) $(LIBXPM_TARGET) $(XEXT_COMPAT_TARGET) $(XMU_COMPAT_TARGET) $(XINERAMA_COMPAT_TARGET) | $(MOTIF_DEMOS_BUILD_DIR)
	@echo "  CONFIG  motif demos"
	$(Q)cd $(MOTIF_DEMOS_BUILD_DIR) && \
	    $(motif_runtime_env) \
	    PKG_CONFIG_PATH=$(abspath $(PKGCONFIG_DIR)) \
	    CPP="$(CC) -E" \
	    CPPFLAGS="-include stdlib.h" \
	    CFLAGS="$(MOTIF_CFLAGS)" \
	    LDFLAGS="$(MOTIF_CONFIGURE_LDFLAGS)" \
	    YACC="$(MOTIF_YACC)" \
	    $(abspath $(MOTIF_SRC_DIR))/configure $(MOTIF_DEMOS_CONFIGURE_FLAGS) \
	    $(call motif_log_redirect,$(MOTIF_DEMOS_CONFIG_LOG))
	$(Q)touch $@

$(MOTIF_DEMOS_BUILD_STAMP): $(MOTIF_DEMOS_CONFIG_STAMP)
	@echo "  MAKE    motif demos config"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/config LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	@echo "  MAKE    motif demos lib/Xm"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/lib/Xm CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	@echo "  MAKE    motif demos lib/Mrm"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/lib/Mrm CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	@echo "  MAKE    motif uil"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/tools/wml CPP="$(CC) -E" CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/clients/uil CPP="$(CC) -E" CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	@echo "  MAKE    motif demos"
	$(Q)$(motif_runtime_env) $(MOTIF_SUBMAKE) -C $(MOTIF_DEMOS_BUILD_DIR)/demos CPP="$(CC) -E" CFLAGS="$(MOTIF_CFLAGS)" LIBS="$(MOTIF_LIBS)" \
	    $(call motif_log_redirect,$(abspath $(MOTIF_DEMOS_BUILD_DIR))/build.log)
	$(Q)touch $@

$(MOTIF_LIBXM) $(MOTIF_LIBMRM): $(MOTIF_BUILD_STAMP)
	@echo "  STAGE   motif libraries"
	$(Q)cp -f $(MOTIF_BUILD_DIR)/lib/Xm/.libs/libXm*.dylib $(OUT)/ 2>/dev/null || true
	$(Q)cp -f $(MOTIF_BUILD_DIR)/lib/Xm/.libs/libXm.so* $(OUT)/ 2>/dev/null || true
	$(Q)cp -f $(MOTIF_BUILD_DIR)/lib/Mrm/.libs/libMrm*.dylib $(OUT)/ 2>/dev/null || true
	$(Q)cp -f $(MOTIF_BUILD_DIR)/lib/Mrm/.libs/libMrm.so* $(OUT)/ 2>/dev/null || true
ifeq ($(UNAME_S),Darwin)
	$(Q)test ! -f $(OUT)/libXm.5.dylib || \
	    install_name_tool -id @rpath/libXm.5.dylib $(OUT)/libXm.5.dylib
	$(Q)test ! -f $(OUT)/libMrm.5.dylib || \
	    install_name_tool -id @rpath/libMrm.5.dylib \
	        -change $(abspath $(OUT))/motif-install/lib/libXm.5.dylib \
	        @rpath/libXm.5.dylib $(OUT)/libMrm.5.dylib
endif
	$(Q)rm -f $(MOTIF_LIBXM) $(MOTIF_LIBMRM)
	$(Q)if [ -e $(OUT)/libXm.5.dylib ]; then \
	    ln -sf libXm.5.dylib $(MOTIF_LIBXM); \
	elif [ -e $(OUT)/libXm.so.5 ]; then \
	    ln -sf libXm.so.5 $(MOTIF_LIBXM); \
	else \
	    echo "  STAGE   no libXm.5.dylib or libXm.so.5 found" >&2; exit 1; \
	fi
	$(Q)if [ -e $(OUT)/libMrm.5.dylib ]; then \
	    ln -sf libMrm.5.dylib $(MOTIF_LIBMRM); \
	elif [ -e $(OUT)/libMrm.so.5 ]; then \
	    ln -sf libMrm.so.5 $(MOTIF_LIBMRM); \
	else \
	    echo "  STAGE   no libMrm.5.dylib or libMrm.so.5 found" >&2; exit 1; \
	fi

.PHONY: motif motif-demos
## Build thentenaar/motif libXm and libMrm against the compatibility stack
motif: $(MOTIF_LIBXM) $(MOTIF_LIBMRM)

## Build thentenaar/motif demos for differential tests against host libXm/libMrm
motif-demos: $(MOTIF_DEMOS_BUILD_STAMP)

MOTIF_DIFF_REMOTE ?= node11
MOTIF_DIFF_REMOTE_ROOT ?= /tmp/libx11-compat-motif-differential
MOTIF_DIFF_DISPLAY ?= 119
MOTIF_DIFF_JOBS ?= 1
MOTIF_DIFF_FILTER ?=
MOTIF_DIFF_INSTALL_DEPS ?= 0
MOTIF_DIFF_MAE_THRESHOLD ?= 0.08
MOTIF_DIFF_CHANGED_THRESHOLD ?= 0.35
MOTIF_DIFF_SECONDS ?= 3
MOTIF_DIFF_GEOMETRY ?= 1280x1024x24
MOTIF_DIFF_TOP ?= 12
MOTIF_DIFF_COMPARE_LOCATION ?= remote
MOTIF_DIFF_OUT_ROOT ?= $(OUT)/motif-differential
MOTIF_DIFF_REPRESENTATIVE_FILTER ?=

motif_diff_env = \
    MOTIF_DIFF_REMOTE='$(MOTIF_DIFF_REMOTE)' \
    MOTIF_DIFF_REMOTE_ROOT='$(MOTIF_DIFF_REMOTE_ROOT)' \
    MOTIF_DIFF_DISPLAY='$(MOTIF_DIFF_DISPLAY)' \
    MOTIF_DIFF_JOBS='$(MOTIF_DIFF_JOBS)' \
    MOTIF_DIFF_FILTER='$(MOTIF_DIFF_FILTER)' \
    MOTIF_DIFF_MAE_THRESHOLD='$(MOTIF_DIFF_MAE_THRESHOLD)' \
    MOTIF_DIFF_CHANGED_THRESHOLD='$(MOTIF_DIFF_CHANGED_THRESHOLD)' \
    MOTIF_DIFF_SECONDS='$(MOTIF_DIFF_SECONDS)' \
    MOTIF_DIFF_GEOMETRY='$(MOTIF_DIFF_GEOMETRY)' \
    MOTIF_DIFF_TOP='$(MOTIF_DIFF_TOP)' \
    MOTIF_DIFF_COMPARE_LOCATION='$(MOTIF_DIFF_COMPARE_LOCATION)' \
    MOTIF_DIFF_OUT_ROOT='$(abspath $(MOTIF_DIFF_OUT_ROOT))' \
    MOTIF_DIFF_REPRESENTATIVE_FILTER='$(MOTIF_DIFF_REPRESENTATIVE_FILTER)'

.PHONY: motif-differential motif-differential-full motif-demos-check motif-demos-screenshots
## Compare representative Motif demo screenshots for system libX11 vs libx11-compat on node11
motif-differential:
	$(Q)$(motif_diff_env) $(PYTHON) scripts/run-motif-differential-tests.py \
	    --mode representative \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_INSTALL_DEPS)),--install-deps)

## Compare all Motif demo screenshots for system libX11 vs libx11-compat on node11
motif-differential-full:
	$(Q)$(motif_diff_env) $(PYTHON) scripts/run-motif-differential-tests.py \
	    --mode full \
	    $(if $(filter 1 yes true,$(MOTIF_DIFF_INSTALL_DEPS)),--install-deps)

## Run Motif demo process smoke checks
motif-demos-check: $(MOTIF_DEMOS_BUILD_STAMP)
	$(Q)scripts/validate-motif-demos.sh $(MOTIF_DEMOS_BUILD_DIR) $(OUT)

## Capture screenshots for Motif demos built against libx11-compat
motif-demos-screenshots: $(MOTIF_DEMOS_BUILD_STAMP)
	$(Q)scripts/capture-motif-demo-screenshots.sh $(MOTIF_DEMOS_BUILD_DIR) $(OUT)
