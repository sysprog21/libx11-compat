# Stage upstream Xorg headers (and a whitelisted slice of libX11 src/) under
# $(OUT) so the local include/X11/ and src/X11/ trees only need to carry the
# files that diverge from upstream.

UPSTREAM_HEADERS_DIR := $(OUT)/upstream/include
UPSTREAM_SRC_DIR := $(OUT)/upstream/src
UPSTREAM_HEADERS_STAMP := $(UPSTREAM_HEADERS_DIR)/.upstream-stamp
UPSTREAM_SYNC := scripts/sync-upstream-headers.py

$(UPSTREAM_HEADERS_STAMP): $(UPSTREAM_SYNC) | $(OUT)
	@echo "  SYNC    upstream tree -> $(OUT)/upstream"
	$(Q)$(PYTHON) $(UPSTREAM_SYNC) fetch $(UPSTREAM_HEADERS_DIR)

# The staged .c files are created as a side effect of the stamp recipe.
# Declaring them here lets dependent .o rules use them as prerequisites
# without Make running the recipe N times.
$(UPSTREAM_SRCS): $(UPSTREAM_HEADERS_STAMP)

# Order-only: stamp must exist before anything that compiles C against
# <X11/...>. Header content changes propagate through -MMD per-source
# dependency files. Loaded after tests/examples so their target lists are
# already defined. The matching -I$(OUT)/upstream/include is added by
# mk/config.mk so the path order relative to system include dirs is
# explicit there.
$(OBJS): | $(UPSTREAM_HEADERS_STAMP)
$(CHECK_BINS): | $(UPSTREAM_HEADERS_STAMP)
$(EXAMPLE_BINS): | $(UPSTREAM_HEADERS_STAMP)

.PHONY: upstream-sync upstream-diff upstream-prune upstream-info

## Re-download and re-extract upstream headers
upstream-sync: | $(OUT)
	$(Q)$(PYTHON) $(UPSTREAM_SYNC) fetch --force $(UPSTREAM_HEADERS_DIR)

## Compare local include/X11/ against the pinned upstream snapshot
upstream-diff:
	$(Q)$(PYTHON) $(UPSTREAM_SYNC) diff

## Delete tracked include/X11/ headers identical to upstream
upstream-prune:
	$(Q)$(PYTHON) $(UPSTREAM_SYNC) prune

## Print pinned upstream versions and source URLs
upstream-info:
	$(Q)$(PYTHON) $(UPSTREAM_SYNC) info
