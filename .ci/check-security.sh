#!/usr/bin/env bash

# Security checks for libx11-compat sources. Scope is src/ and the
# compatibility headers under include/X11/. Examples and tests are
# excluded -- examples mirror upstream Xlib clients and tests
# legitimately exercise corner-case APIs.
#
# 1. Banned functions -- unsafe libc calls with safer alternatives.
# 2. Credential / secret patterns -- catch accidental key leaks.
# 3. Dangerous preprocessor -- detect disabled security features.

set -u -o pipefail

failed=0

banned='(^|[^[:alnum:]_])(gets|sprintf|vsprintf|strcpy|stpcpy|strcat|atoi|atol|atoll|atof|mktemp|tmpnam|tempnam)[[:space:]]*\('
secrets='(password|secret|api_key|private_key|token)[[:space:]]*=[[:space:]]*"[^"]+'
# Dangerous preprocessor: catch both `#define _FORTIFY_SOURCE 0` and the
# bare `#undef _FORTIFY_SOURCE` form, plus any define/undef of __SSP__.
# An earlier version of this regex required `_FORTIFY_SOURCE` to be
# followed by `[[:space:]]+0`, which silently let `#undef _FORTIFY_SOURCE`
# pass.
dangerous_pp='#[[:space:]]*((undef|define)[[:space:]]+__SSP__|undef[[:space:]]+_FORTIFY_SOURCE|define[[:space:]]+_FORTIFY_SOURCE[[:space:]]+0)'
# Skip block-comment continuation lines (` * ...`, ` */`, blank ` *`)
# without dropping code that begins with `*` like `*p = strdup(s);`.
# The `*` must be followed by whitespace, `/`, or end of line to count
# as a comment marker.
comment_only='^[[:space:]]*(//|/\*|\*([[:space:]/]|$))'

while IFS= read -r -d '' f; do
    code=$(grep -vE "$comment_only" "$f")

    if echo "$code" | grep -qE "$banned"; then
        echo "Banned function in $f:"
        grep -nE "$banned" "$f" | grep -vE "$comment_only"
        failed=1
    fi
    if echo "$code" | grep -iqE "$secrets"; then
        echo "Possible hardcoded secret in $f:"
        grep -inE "$secrets" "$f" | grep -vE "$comment_only"
        failed=1
    fi
    if echo "$code" | grep -qE "$dangerous_pp"; then
        echo "Dangerous preprocessor directive in $f:"
        grep -nE "$dangerous_pp" "$f" | grep -vE "$comment_only"
        failed=1
    fi
    # src/GL is vendored gl4es (upstream desktop-GL-to-GLES translation, see
    # src/GL/README.md); it legitimately uses sprintf/strcpy/atoi and friends in its
    # own style, and rewriting it would break re-diffing against upstream. Exclude it
    # like examples/ and tests/, so the ban applies only to our own code.
done < <(git ls-files -z -- 'src/*.c' 'src/*.h' ':!src/GL' \
    'include/X11/*.h' 'include/X11/**/*.h')

if [ $failed -eq 0 ]; then
    echo "Security checks passed."
fi

exit $failed
