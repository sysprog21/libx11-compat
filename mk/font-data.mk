# 6x13.bdf is X.Org's classic -misc-fixed bitmap font, the metric and render
# source for the "fixed" / "6x13" core-font aliases. The repo tracks neither
# the BDF nor the generated table: the header is produced on demand from the
# upstream file, content-verified against a known digest so a moved, changed,
# or corrupted download aborts the build instead of silently embedding a
# different font.
#
# Offline or air-gapped build: pass FONT_BDF=/path/to/6x13.bdf to convert a
# local copy and skip the network fetch entirely.

# Byte-identical mirrors, tried in order. Every candidate is content-verified
# against FONT_BDF_SHA256, so failover covers both an unreachable host and a
# mirror that serves the wrong bytes; a mismatching mirror is skipped, not
# trusted. Add an internal mirror by prepending to FONT_BDF_URLS.
FONT_BDF_URLS ?= \
    https://gitlab.freedesktop.org/xorg/font/misc-misc/-/raw/c4e2af05583764fae3d0b2aa20d5c93b031cbcbf/6x13.bdf \
    https://cgit.freedesktop.org/xorg/font/misc-misc/plain/6x13.bdf?id=c4e2af05583764fae3d0b2aa20d5c93b031cbcbf
FONT_BDF_SHA256 := c68c55ca8effcdc2714fffa9c8e07df290411867b00b547eeae952f7473efc9b
FONT_BDF ?=

FONT_GEN_DIR := $(OUT)/generated
FONT_BDF_CACHE := $(FONT_GEN_DIR)/6x13.bdf
FONT_GEN_HEADER := $(FONT_GEN_DIR)/font-6x13-bitmap.h

# src/font.c includes the generated header by name.
CPPFLAGS += -I$(FONT_GEN_DIR)

$(FONT_GEN_DIR):
	@mkdir -p $@

# Fetch (or copy) the BDF and content-verify it before use. Every source must
# match the pinned digest; a local FONT_BDF or a mirror that does not is
# rejected. Fails closed: nothing is embedded unless the bytes are exactly the
# expected font.
$(FONT_BDF_CACHE): | $(FONT_GEN_DIR)
	@set -f; sha=$(FONT_BDF_SHA256); \
	digest() { (sha256sum "$$1" 2>/dev/null || shasum -a 256 "$$1") | cut -d' ' -f1; }; \
	if [ -n "$(FONT_BDF)" ]; then \
	    echo "  COPY    $(notdir $(FONT_BDF))"; \
	    cp "$(FONT_BDF)" "$@.tmp" || exit 1; \
	    got=`digest "$@.tmp"`; \
	    if [ "$$got" != "$$sha" ]; then \
	        echo "FONT_BDF sha256 mismatch: got $$got want $$sha" >&2; \
	        rm -f "$@.tmp"; exit 1; \
	    fi; \
	else \
	    ok=0; \
	    for url in $(FONT_BDF_URLS); do \
	        echo "  FETCH   $$url"; \
	        curl -fsSL --max-time 30 -o "$@.tmp" "$$url" || continue; \
	        got=`digest "$@.tmp"`; \
	        if [ "$$got" = "$$sha" ]; then ok=1; break; fi; \
	        echo "  ...digest mismatch from this mirror, trying next" >&2; \
	    done; \
	    if [ "$$ok" != 1 ]; then \
	        echo "no 6x13.bdf mirror returned the expected digest $$sha" >&2; \
	        rm -f "$@.tmp"; exit 1; \
	    fi; \
	fi
	@mv "$@.tmp" "$@"

$(FONT_GEN_HEADER): $(FONT_BDF_CACHE) scripts/embed-bdf-font.py | $(FONT_GEN_DIR)
	@echo "  GEN     $(notdir $@)"
	$(Q)$(PYTHON) scripts/embed-bdf-font.py "$<" --out "$@.tmp" && mv "$@.tmp" "$@"

# font.c #includes the table, so its object cannot compile until the header
# exists. Order-only: the content is deterministic, so regeneration must not
# force a recompile on every build.
$(OUT)/src/font.o: | $(FONT_GEN_HEADER)
