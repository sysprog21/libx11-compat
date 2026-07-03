#!/bin/sh
# Run a built Mesa demo (glxgears, glxdemo, glxgears_fbconfig, ...) through
# libx11-compat + GLX: on-screen by default, or --snapshot for a headless PNG.
# The platform-specific runtime setup lives in the shared runners this delegates
# to, so it works on both macOS and Linux (see scripts/run-glx-window.sh and
# scripts/glx-snapshot.sh). Build the demos first with: make GLX=1 mesa-demos
#
# Usage:
#   run-mesa-demo.sh <demo>                    open a live interactive window
#   run-mesa-demo.sh <demo> --snapshot [out.png]   headless one-shot screenshot
set -eu

out=${OUT:-build}

# No demo named: list the built ones so the caller can pick.
if [ $# -eq 0 ]; then
    echo "usage: run-mesa-demo.sh <demo> [--snapshot [out.png]]"
    if [ -d "$out/mesa-demos" ] && [ -n "$(ls "$out/mesa-demos" 2>/dev/null)" ]; then
        echo "available demos:"
        for d in "$out"/mesa-demos/*; do
            [ -x "$d" ] && echo "  $(basename "$d")"
        done
    else
        echo "no demos built yet; run: make GLX=1 mesa-demos"
    fi
    exit 0
fi

name=$1
demo="$out/mesa-demos/$name"

[ -x "$demo" ] || {
    echo "missing $demo; run: make GLX=1 mesa-demos" >&2
    echo "available: $(ls "$out/mesa-demos" 2>/dev/null | tr '\n' ' ')" >&2
    exit 1
}

# The demo statically links the in-tree gl4es loader (src/GL), so desktop GL runs
# through gl4es -> ANGLE -> Metal with no Mesa. The provider is ANGLE's libEGL,
# with libGLESv2 on the loader path.
angle="$PWD/$out/angle"
export LIBX11_COMPAT_EGL="${LIBX11_COMPAT_EGL:-$angle/libEGL.dylib}"
export GLX_EXTRA_LIBS="$angle"

if [ "${2:-}" = "--snapshot" ]; then
    exec scripts/glx-snapshot.sh "$demo" "${3:-/tmp/mesa-demo-$name.png}"
fi

exec scripts/run-glx-window.sh "$demo"
