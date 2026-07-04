#!/bin/sh
# Flatten a raw gl4es wrapper file's includes for our single-directory build.
# Reads the raw upstream file on stdin, writes the flattened form on stdout.
#
# Upstream keeps the wrapper under src/gl/wrap/, so its files include the core
# headers as "../gl4es.h" and their siblings as "gles.h"/"gl4es.h". We compile
# every gl4es source from one include path, where a bare "gles.h"/"gl4es.h" would
# resolve to the CORE header, not the wrapper. Two rules, applied in this order so
# they never overlap (the renames match only the bare wrapper spellings; the ../
# strip handles every parent path):
#   - same-directory wrapper include keeps a collision rename:
#       "gles.h"  -> "wrap-gles.h",  "gl4es.h" -> "wrap-gl4es.h"
#   - parent include loses the ../ and lands on the flat core name:
#       "../X.h"  -> "X.h"
set -eu

sed -e 's#include "gles\.h"#include "wrap-gles.h"#' \
    -e 's#include "gl4es\.h"#include "wrap-gl4es.h"#' \
    -e 's#include "\.\./\([^"]*\)"#include "\1"#'
