#!/usr/bin/env bash

# Verify clang-format-20 conformance for tracked C/H files under
# include/, src/, and tests/. Examples are excluded because
# examples/x11perf/ ships verbatim from xorg and intentionally keeps
# upstream style.
#
# clang-format-20 is required: older releases produce different output
# for the .clang-format style used in this tree.

set -e -u -o pipefail

if [ -z "${CLANG_FORMAT:-}" ]; then
    if command -v clang-format-20 >/dev/null 2>&1; then
        CLANG_FORMAT="clang-format-20"
    else
        echo "Error: clang-format-20 is required (older versions differ in style)" >&2
        exit 1
    fi
fi

# Reject anything that isn't clang-format 20.x.
version=$("$CLANG_FORMAT" --version 2>/dev/null | grep -oE 'version [0-9]+' | awk '{print $2}')
if [ "$version" != "20" ]; then
    echo "Error: \$CLANG_FORMAT ($CLANG_FORMAT) reports version '$version', expected 20" >&2
    exit 1
fi

ret=0
while IFS= read -r -d '' file; do
    expected=$(mktemp)
    "$CLANG_FORMAT" "$file" >"$expected"
    if ! diff -u -p --label="$file" --label="expected coding style" "$file" "$expected"; then
        ret=1
    fi
    rm -f "$expected"
done < <(git ls-files -z -- \
    'include/*.h' 'include/**/*.h' \
    'src/*.c' 'src/*.h' \
    'tests/*.c' 'tests/*.h')

exit $ret
