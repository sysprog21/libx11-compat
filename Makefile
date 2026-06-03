.DEFAULT_GOAL := all
.DELETE_ON_ERROR:

include mk/toolchain.mk
include mk/config.mk
include mk/sources.mk
include mk/common.mk
include mk/library.mk
include mk/libxt.mk
include mk/tests.mk
include mk/examples.mk
include mk/upstream-headers.mk
include mk/format.mk
include mk/help.mk
include mk/deps.mk
