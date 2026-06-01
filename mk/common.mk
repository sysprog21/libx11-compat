# Generic build rules.

# Verbosity: make V=1 shows full commands.
ifeq ($(V),1)
  Q :=
else
  Q := @
  MAKEFLAGS += --no-print-directory
endif

$(OUT):
	@mkdir -p $@

$(OUT)/%.o: %.c | $(OUT)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) -MMD -MP -MF $(@:.o=.d) -c $< -o $@

# Upstream sources staged under $(OUT)/upstream/src/ live next to their
# objects rather than mirroring an in-tree path, so they need their own
# pattern rule. The order-only stamp dependency from mk/upstream-headers.mk
# is what guarantees the .c file actually exists before this fires.
# -Wno-sign-compare matches what libX11's own build does; we don't own
# upstream's signedness choices and shouldn't see those warnings on every
# build. -D_XLIBINT_ matches the flag libX11's own Makefile.am passes when
# compiling these translation units and keeps the WIN32 macro rewrites in
# Xlibint.h disabled so the function-pointer storage in src/xlibint-stubs.c
# stays consistent across platforms.
$(OUT)/upstream/src/%.o: $(OUT)/upstream/src/%.c | $(OUT)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_EXTRA) -Wno-sign-compare -D_XLIBINT_ -MMD -MP -MF $(@:.o=.d) -c $< -o $@

.PHONY: clean distclean

## Remove build artifacts
clean:
	rm -rf $(OUT)

## Remove all generated local artifacts
distclean: clean
