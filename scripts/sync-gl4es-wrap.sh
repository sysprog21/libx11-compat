#!/bin/sh
# Fetch the gl4es GLES wrapper sources from a pinned upstream commit into a local
# cache. These four files are upstream artifacts, not our source: they are cached
# under third_party/ (git-ignored, survives make clean, so only the first build
# needs the network) and flattened into build/ at compile time by mk/gl4es.mk.
# They are deliberately NOT vendored under src/GL, so a tree-wide reformat never
# touches them and the drift check below stays a plain byte compare to upstream.
#
#   upstream src/gl/wrap/   ->   third_party/gl4es-wrap/ (raw, upstream names)
#     gl4es.h                      gl4es.h       (hand-written upstream)
#     gl4eswraps.c                 gl4eswraps.c  (hand-written upstream)
#     gles.h                       gles.h        (generated upstream by spec/gen.py)
#     gles.c                       gles.c        (generated upstream by spec/gen.py)
#
# The include flatten (parent path strip and the wrap-*.h collision rename that
# keeps these from clashing with the core src/GL/gl4es.h and gles.h once on one
# include path) is applied at build time, not here; see mk/gl4es.mk. We do not
# re-run gen.py: upstream's live template no longer reproduces its own checked-in
# gles.h/gles.c (dropped APIENTRY, the near/far -> Near/Far rename, some const),
# and our hand-maintained gl4es .c sources are coupled to those checked-in names.
#
# The pin lives in scripts/gl4es-pin.txt. The commit hash content-addresses each
# file (GitHub serves the exact blob at that commit), so the pin is the integrity
# guarantee; no separate checksum is needed.
#
# Usage:
#   sync-gl4es-wrap.sh [destdir]          fetch the four raw files into destdir
#   sync-gl4es-wrap.sh --check [destdir]  verify destdir matches the pin; write
#                                         nothing (exit 1 on drift)
# destdir defaults to third_party/gl4es-wrap.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
# tr strips a trailing newline and any stray CR (CRLF checkout) so the pin cannot
# leak whitespace into the URL.
pin=$(tr -d '\r\n' <"$root/scripts/gl4es-pin.txt" 2>/dev/null || true)
[ -n "$pin" ] || {
    echo "sync-gl4es-wrap.sh: scripts/gl4es-pin.txt is missing or empty" >&2
    exit 1
}
base="https://raw.githubusercontent.com/ptitSeb/gl4es/$pin/src/gl/wrap"

check=0
if [ "${1:-}" = "--check" ]; then
    check=1
    shift
fi
dest=${1:-$root/third_party/gl4es-wrap}
[ $# -gt 0 ] && shift
# Any remaining args name specific files to (re)fetch; default to all four. The
# build fetches one at a time so deleting a single cached file refetches just it.
files="$*"
[ -n "$files" ] || files="gl4es.h gl4eswraps.c gles.h gles.c"
status=0

work=$(mktemp -d)
trap 'rm -rf "$work"; rm -f "$dest"/.sync.* 2>/dev/null' EXIT INT TERM

# Fetch $1 to $work/raw. Fetch to a file, not through a pipe: curl -f fails on any
# HTTP/network error, and only a non-piped command lets set -e catch it before we
# overwrite a cached source with an empty file. In --check mode a genuinely
# unreachable host (curl 6 could-not-resolve, 7 could-not-connect) is not drift,
# so skip the whole check rather than fail an offline/sandboxed build; every other
# failure (bad pin, 404, transient) still fails.
fetch() {
    # Capture curl's exit directly. Reading $? after "if curl; then ...; fi"
    # would see the if-statement's status (0 when curl failed and no else ran),
    # silently treating a failed fetch as success.
    curl -fsS --retry 2 --max-time 30 "$base/$1" -o "$work/raw" && return 0
    rc=$?
    if [ "$check" = 1 ] && { [ "$rc" -eq 6 ] || [ "$rc" -eq 7 ]; }; then
        echo "  skip  gl4es upstream unreachable (curl $rc); drift not checked"
        exit 0
    fi
    echo "sync-gl4es-wrap: fetch failed for $1 (curl exit $rc)" >&2
    exit "$rc"
}

sync_one() {
    name=$1
    target="$dest/$name"
    fetch "$name"
    if [ "$check" = 1 ]; then
        if diff -q "$work/raw" "$target" >/dev/null 2>&1; then
            echo "  ok    $name"
        else
            echo "  DRIFT $name differs from gl4es@$pin"
            status=1
        fi
    else
        # Write into the target's own directory then rename: an atomic replace so
        # a concurrent reader never sees a partial file.
        mkdir -p "$dest"
        tmp=$(mktemp "$dest/.sync.XXXXXX")
        cp "$work/raw" "$tmp"
        mv "$tmp" "$target"
        echo "  cached $name"
    fi
}

echo "gl4es wrap sync @ $pin"
for f in $files; do
    sync_one "$f"
done
exit $status
