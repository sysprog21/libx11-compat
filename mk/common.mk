# Generic build rules.

# Remove a target whose recipe failed or was interrupted: a rule that redirects
# with > $@ (generated headers, alias assembly, flattened wrap sources) would
# otherwise leave a truncated file whose mtime looks up to date, so the next make
# compiles garbage. Applies to every rule in the build.
.DELETE_ON_ERROR:

# Verbosity: make V=1 shows full commands.
ifeq ($(V),1)
  Q :=
else
  Q := @
  MAKEFLAGS += --no-print-directory
endif

# Literal comma helper for $(call ...) macros below — make's call splits
# on bare commas, so we expand $(comma) inside the macro body to get one
# in the final shell text.
comma := ,

# LDFLAGS for compat shared libraries that live next to their DT_NEEDED
# (libX11-compat, libXt-compat, etc.). Pass the basename of the library
# being linked, e.g. $(call shared_lib_rpath_ldflags,libXt-compat.so).
#
# Linux: $$ORIGIN tells the dynamic linker to resolve transitive needs
# from the same directory; without it, autotools clients that only -lXt
# fail at runtime trying to find libX11-compat.so unless LD_LIBRARY_PATH
# is set.
#
# Darwin: @rpath install_name + @loader_path rpath give the same
# property using mach-o's two-step install-name resolution. Setting
# install_name to @rpath/<name> lets consumers re-target the search via
# -Wl,-rpath,<dir>; loader_path means "search next to the dylib that
# DT_NEEDED'd me" which is what we want for the in-tree compat stack.
define shared_lib_rpath_ldflags
$(if $(filter Linux,$(UNAME_S)),-Wl$(comma)-rpath$(comma)'$$ORIGIN') \
$(if $(filter Darwin,$(UNAME_S)),-Wl$(comma)-install_name$(comma)@rpath/$(1) -Wl$(comma)-rpath$(comma)@loader_path)
endef

# Header-dependency generation flags, shared by every compile rule in the tree.
# Recursive (=) on purpose: $@ and $< must expand per-target when the recipe
# runs, not once at parse time. Reference it as $(DEPFLAGS) in any object rule.
DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@ -MT $(@:.o=.d)

# Canned recipe: compile one first-party C translation unit to its object with
# the standard flag set and dependency tracking. The generic src/tests object
# rule below and the compat-shim object rules (mk/xcompat-libs.mk) each need
# distinct prerequisites but share this exact recipe, so it lives in one place.
# The optimization/base flags come from $(FP_CFLAGS) (defined in mk/config.mk),
# so the opt level is set there once, not per rule. Invoke it bare as
# $(cc_object) from a recipe; it relies on the automatic $@/$< of the invoking
# rule.
define cc_object
@mkdir -p $(dir $@)
@echo "  CC      $<"
$(Q)$(CC) $(CPPFLAGS) $(FP_CFLAGS) $(STRICT_CFLAGS) $(CFLAGS_EXTRA) \
    $(DEPFLAGS) -c $< -o $@
endef

$(OUT):
	@mkdir -p $@

$(OBJROOT)/%.o: %.c $(SDL_BACKEND_STAMP) | $(OUT)
	$(cc_object)

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
$(OBJROOT)/upstream/src/%.o: $(OUT)/upstream/src/%.c $(SDL_BACKEND_STAMP) | $(OUT)
	@mkdir -p $(dir $@)
	@echo "  CC      $<"
	$(Q)$(CC) $(CPPFLAGS) $(LEGACY_FP_CFLAGS) $(CFLAGS_EXTRA) -Wno-sign-compare -D_XLIBINT_ \
	    $(DEPFLAGS) -c $< -o $@
.PHONY: clean distclean

## Remove build artifacts
clean:
	rm -rf $(OUT)

## Remove all generated local artifacts
distclean: clean
