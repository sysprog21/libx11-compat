# WebAssembly unmodified-app yield plumbing.
#
# An unmodified Xt/toolkit app links the idle-yield override
# (compat/wasm-idle-yield.c) and the poll/select bridge
# (compat/wasm-poll-yield.c) with -sASYNCIFY and -Wl,--wrap=poll,--wrap=select
# so a blocking XtAppMainLoop yields to the browser instead of freezing the tab.
# libXt's _XtWaitForSomething blocks in poll() before our XNextEvent runs, so the
# wrap is what routes that wait through the yield; the idle hook covers the
# direct XNextEvent waits. The showcase apps (xnedit, xcircuit, ...) pull
# WASM_APP_YIELD_OBJS into their own link rules.

ifeq ($(WASM),1)

WASM_POLL_YIELD_OBJ := $(OBJROOT)/compat/wasm-poll-yield.o
# Both yield objects: an app pulls this set to make an unmodified blocking main
# loop browser-safe. WASM_IDLE_YIELD_OBJ is defined in mk/wasm-deps.mk, which
# the top Makefile includes before this fragment, so the := expansion below
# already sees it; it is intentionally not redefined here.
WASM_APP_YIELD_OBJS := $(WASM_IDLE_YIELD_OBJ) $(WASM_POLL_YIELD_OBJ)

endif
