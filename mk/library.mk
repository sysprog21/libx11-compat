# Shared library target.

.PHONY: all

## Build the SDL2-backed X11 compatibility shared library
all: $(TARGET)

# On Linux SDL2 transitively loads /lib/.../libX11.so.6. The staged
# upstream locking.c, Context.c, etc. define globals (_XLockDisplay,
# _XInitDisplayLock, _XInitDisplayLock_fn, _Xglobal_lock, ...) under
# the same names as the system libX11. Both the function symbols and
# the data symbols (function-pointer variables like
# _XInitDisplayLock_fn) participate; the dynamic linker otherwise
# rebinds our intra-library references to libX11.so.6's copies, our
# XOpenDisplay ends up running libX11.so.6's _XInitDisplayLock against
# our Display layout, and the next display->lock_fns->lock_display(...)
# segfaults at offset 0x50. -Bsymbolic (not just -Bsymbolic-functions)
# binds *both* function and data references to the local library at
# link time. Mach-O ld on macOS does not accept the flag, so gate it
# on Linux only.
LDFLAGS_LIB :=
ifeq ($(UNAME_S),Linux)
  LDFLAGS_LIB += -Wl,-Bsymbolic
endif

$(TARGET): $(OBJS) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) $(LDFLAGS_LIB) -shared -o $@ $(OBJS) $(LDLIBS)
