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
#   - wasm.mk is first of all: it retargets CC/TARGET for WASM=1 before
#     toolchain.mk applies the native compiler default. wasm-deps.mk comes
#     after the object list is defined (it links the wasm archive/clock).
include mk/wasm.mk
include mk/toolchain.mk
include mk/config.mk
include mk/sdl.mk
include mk/sources.mk
include mk/common.mk
include mk/font-data.mk
include mk/sdl-wrapper.mk
include mk/library.mk
include mk/libxcb.mk
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
include mk/gtk1.mk
include mk/cinepaint.mk
include mk/xmms.mk
include mk/xnedit.mk
include mk/xwpe.mk
include mk/xephem.mk
include mk/grace.mk
include mk/tcltk.mk
include mk/xcircuit.mk
include mk/magic.mk
include mk/micropolis.mk
include mk/gl4es.mk
include mk/mesa-demos.mk
include mk/tests.mk
include mk/examples.mk
include mk/wasm-deps.mk
include mk/wasm-motif.mk
include mk/wasm-apps.mk
include mk/wasm-mosaic.mk
include mk/wasm-xnedit.mk
include mk/wasm-xcircuit.mk
include mk/wasm-xwpe.mk
include mk/wasm-pages.mk
include mk/upstream-headers.mk
include mk/format.mk
include mk/help.mk
include mk/angle.mk
include mk/install.mk
include mk/deps.mk
