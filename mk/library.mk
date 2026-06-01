# Shared library target.

.PHONY: all

## Build the SDL2-backed X11 compatibility shared library
all: $(TARGET)

$(TARGET): $(OBJS) | $(OUT)
	@echo "  LD      $@"
	$(Q)$(CC) $(LDFLAGS) -shared -o $@ $(OBJS) $(LDLIBS)
