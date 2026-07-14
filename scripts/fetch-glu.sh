#!/bin/sh
# Fetch the pinned Mesa GLU source tree with plain git.
#
# Magic's OpenGL display driver (grtogl) calls gluTess* for polygon rendering,
# so build-ogl-userlibs.sh compiles the GLU tessellator from
# third_party/glu/src/libtess into build/libGLU.1.dylib. The tree is fetched on
# demand rather than vendored, and make clean does not remove it, so an existing
# checkout at the pin is reused (and reset to the pin below so a stray edit
# cannot leak into the build).
#
# The pin lives in scripts/glu-pin.txt as the single source of truth. A shallow
# single-commit fetch keeps the download small.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DEST="$ROOT/third_party/glu"
URL="https://gitlab.freedesktop.org/mesa/glu.git"

# Read the pin defensively: a missing file must reach the explicit error below
# rather than abort the script on the input redirection under set -e.
PIN=""
if [ -f "$ROOT/scripts/glu-pin.txt" ]; then
    PIN="$(tr -d '\r\n' <"$ROOT/scripts/glu-pin.txt")"
fi
[ -n "$PIN" ] || {
    echo "fetch-glu.sh: scripts/glu-pin.txt missing or empty" >&2
    exit 1
}

if [ -d "$DEST/.git" ]; then
    current="$(git -C "$DEST" rev-parse HEAD 2>/dev/null || echo none)"
    if [ "$current" = "$PIN" ]; then
        # Already at the pin, but a prior build or a stray edit can leave
        # tracked files modified or deleted; reset so the OGL userlib build
        # cannot consume a dirty tree as if it were verified.
        git -C "$DEST" reset --hard -q "$PIN"
        git -C "$DEST" clean -fdxq
        echo "glu already at $PIN (reset to pin)"
        exit 0
    fi
    echo "glu at $current, expected $PIN; re-fetching" >&2
fi

mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin "$URL" 2>/dev/null \
    || git -C "$DEST" remote set-url origin "$URL"
git -C "$DEST" fetch --depth=1 origin "$PIN"
git -C "$DEST" checkout -q --detach FETCH_HEAD
git -C "$DEST" reset --hard -q FETCH_HEAD
git -C "$DEST" clean -fdxq

current="$(git -C "$DEST" rev-parse HEAD)"
if [ "$current" != "$PIN" ]; then
    echo "glu fetched $current, expected $PIN" >&2
    exit 1
fi
echo "glu fetched at $PIN"
