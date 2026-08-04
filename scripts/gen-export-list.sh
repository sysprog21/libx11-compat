#!/bin/sh

# Emit the linker export list that pins libX11-compat's exported surface to its
# intended API. Everything else (internal helpers, the duplicated Xft/Fc copy)
# is hidden, which also lets the linker dead-strip code no exported symbol
# reaches. The symbol set is the union of the manifests already enforced by
# tests/check-api-symbols.py plus tests/private-symbols.txt (the core->libXft
# private contract) and tests/whitebox-symbols.txt (the whitebox test surface),
# so the manifests stay the single source of truth.
#
# The union is intersected with <defined-syms>, the symbols the core objects
# actually define, so the list never names a symbol absent from this build. That
# matters because a manifest carries symbols that only exist on one platform or
# feature build (the libx11Compat* shims are macOS-only; glX* drop when GLX=0):
# a stray name is a hard error under ld64 and under lld's --no-undefined-version
# default, and a silent no-op only under GNU ld. Pinning to the defined set
# keeps every linker happy without per-platform manifests.
#
# Usage: gen-export-list.sh <elf|macho> <glx:0|1> <defined-syms> <manifest>...
set -eu

# comm below needs both inputs collated identically to the sort that produced
# them; pin C collation everywhere so the intersection is deterministic
# regardless of the caller's LC_* (a UTF-8 locale orders _/case differently).
export LC_ALL=C

format=$1
glx=$2
defined=$3
shift 3

# Read every manifest in one checked step: a missing or unreadable file must
# abort the build, not silently yield an empty (API-omitting) map. A sed failure
# propagates through this assignment under set -e; the later grep -v / sort only
# exit non-zero on the harmless all-blank case.
manifest_lines=$(sed -e 's/#.*//' -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' "$@")
syms=$(printf '%s\n' "$manifest_lines" | grep -v '^$' | sort -u)

# grep here reads a pipe, so it can only exit non-zero by matching nothing (a
# legal empty filter, e.g. GLX=0 against a glX-only set): tolerate that.
if [ "$glx" != "1" ]; then
    syms=$(printf '%s\n' "$syms" | grep -v '^glX' || true)
fi

# Keep only symbols this build actually defines. comm reads $defined, so a read
# error there is real and must fail the build; comm already returns 0 for an
# empty intersection, so no explicit fallback is needed for the legitimate empty
# case.
syms=$(printf '%s\n' "$syms" | comm -12 - "$defined")

# Always export the libX11 underscore ABI the core defines (_Xdebug,
# _XrmInternalStringToQuark, ...). Real libX11 exports these _X* globals, and
# legacy clients link against them directly (violawww's libIMG references
# _Xdebug); the manifests only enumerate the X[A-Z]* public names, so without
# this the underscore ABI would be hidden and those clients fail to link. Drawn
# from $defined, so every entry is guaranteed present in this build.
abi=$(grep -E '^_X' "$defined" || true)
syms=$(printf '%s\n%s\n' "$syms" "$abi" | grep -v '^$' | sort -u)

case "$format" in
    macho)

        # Mach-O -exported_symbols_list: one C symbol per line, leading
        # underscore.
        printf '%s\n' "$syms" | grep -v '^$' | sed 's/^/_/'
        ;;
    elf)
        # ELF version script: named globals, everything else local (hidden).
        printf '{\n  global:\n'
        printf '%s\n' "$syms" | grep -v '^$' | sed 's/^/    /; s/$/;/'
        printf '  local:\n    *;\n};\n'
        ;;
    *)
        echo "gen-export-list.sh: unknown format '$format'" >&2
        exit 2
        ;;
esac
