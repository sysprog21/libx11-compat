.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

# Include order matters:
#   - toolchain.mk first: CC, PYTHON, PKG_CONFIG, version checks.
#   - config.mk needs the toolchain values to probe SDL2/pixman.
#   - sources.mk + common.mk: source enumeration + Q/V verbosity and
#     reusable helpers (shared_lib_rpath_ldflags, etc.) before any
#     fragment that needs them.
#   - per-library fragments next; pkgconfig.mk consumes targets they
#     define.
#   - tests.mk and examples.mk consume LIBXT/LIBXPM/compat targets.
#   - upstream-headers.mk adds order-only deps to $(OBJS), $(CHECK_BINS),
#     $(EXAMPLE_BINS), so it must be after their definers.
#   - deps.mk aggregates *_OBJS dep-file lists, so it must be last.
include mk/toolchain.mk
include mk/config.mk
include mk/sdl.mk
include mk/sources.mk
include mk/common.mk
include mk/sdl-wrapper.mk
include mk/library.mk
include mk/libxt.mk
include mk/libxpm.mk
include mk/xcompat-libs.mk
include mk/libxaw.mk
include mk/pkgconfig.mk
include mk/motif.mk
include mk/violawww.mk
include mk/mosaic.mk
include mk/osiris.mk
include mk/xclock.mk
include mk/xfig.mk
include mk/gimp-motif.mk
include mk/xnedit.mk
include mk/xwpe.mk
include mk/xephem.mk
include mk/xcircuit.mk
include mk/tests.mk
include mk/examples.mk
include mk/upstream-headers.mk
include mk/format.mk
include mk/help.mk
include mk/deps.mk
