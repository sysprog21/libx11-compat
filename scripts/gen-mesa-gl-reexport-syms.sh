#!/bin/sh
# Generate the reexport symbol list for the macOS gl-only Mesa shim.
#
# Homebrew Mesa ships a monolithic libGL that exports both gl* and glX*. Linking
# it directly would bind a Motif GLw client's glX* to Mesa's real-X11 GLX stack
# (which crashes with no X server). We rebuild a gl-only shim dylib that reexports
# Mesa's gl* but NOT its glX*, so glX* resolve to libx11-compat instead. This
# emits the allowlist of gl* symbols (glX* excluded) that the shim reexports.
#
# Reads the Mesa libGL given as the argument and writes the symbol names to
# stdout (the makefile redirects it to gl.syms, then feeds it to
# -reexported_symbols_list).
#
# Usage: gen-mesa-gl-reexport-syms.sh <path/to/libGL.dylib>
set -eu

lib=${1:?usage: gen-mesa-gl-reexport-syms.sh <libGL.dylib>}

# Capture nm first so set -e catches its failure (a pipeline would mask it: sh
# has no pipefail and the trailing sort exits 0 on empty input).
syms=$(nm -gU "$lib")

# nm -gU lists external defined symbols. Keep the gl* entry points (any of the
# defined-symbol types T/D/S/B/I), drop glX*, and de-duplicate. LC_ALL=C keeps
# the order stable across locales.
out=$(printf '%s\n' "$syms" | awk '$2 ~ /^[TDSBI]$/ && $3 ~ /^_gl/ && $3 !~ /^_glX/ { print $3 }' | LC_ALL=C sort -u)

[ -n "$out" ] || {
    echo "gen-mesa-gl-reexport-syms.sh: no gl* symbols in $lib" >&2
    exit 1
}

printf '%s\n' "$out"
