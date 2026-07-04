#!/bin/sh
# Fetch a pristine desktop-GL header used only to compile the mesa xdemos.
#
# The lib itself does not use these; only the demo build does. glext.h and
# glxext.h are pristine Khronos, generated from the OpenGL XML registry
# (KhronosGroup/OpenGL-Registry); gl.h is Mesa's (not in the Khronos registry).
# They are cached raw under third_party/ (git-ignored, survives make clean, so
# only the first demo build needs the network) rather than vendored. The commit
# hash content-addresses each file, so the pin is the integrity guarantee.
#
# glx.h is deliberately NOT fetched: that one is our own hand-trimmed API contract
# (include/GL/glx.h), coupled to src/glx.c and tests/api-symbols.txt.
#
# Usage: fetch-gl-headers.sh <destdir> <gl.h|glext.h|glxext.h>
# Writes <destdir>/GL/<name>.
set -eu

dest=${1:?usage: fetch-gl-headers.sh <destdir> <name>}
name=${2:?usage: fetch-gl-headers.sh <destdir> <name>}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# tr strips a trailing newline and any stray CR so a pin cannot leak whitespace
# into the URL.
reg_pin=$(tr -d '\r\n' <"$root/scripts/opengl-registry-pin.txt" 2>/dev/null || true)
mesa_pin=$(tr -d '\r\n' <"$root/scripts/mesa-pin.txt" 2>/dev/null || true)

case "$name" in
    glext.h | glxext.h)
        [ -n "$reg_pin" ] || {
            echo "fetch-gl-headers.sh: scripts/opengl-registry-pin.txt missing" >&2
            exit 1
        }
        url="https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/$reg_pin/api/GL/$name"
        ;;
    gl.h)
        [ -n "$mesa_pin" ] || {
            echo "fetch-gl-headers.sh: scripts/mesa-pin.txt missing" >&2
            exit 1
        }
        url="https://gitlab.freedesktop.org/mesa/mesa/-/raw/$mesa_pin/include/GL/$name"
        ;;
    *)
        echo "fetch-gl-headers.sh: unknown header $name" >&2
        exit 1
        ;;
esac

out="$dest/GL/$name"
mkdir -p "$dest/GL"
# Fetch to a same-directory temp then rename: an atomic replace, and a failed or
# partial fetch never leaves a half-written header a later build treats as good.
tmp=$(mktemp "$dest/GL/.gl.XXXXXX")
trap 'rm -f "$tmp"' EXIT INT TERM
if ! curl -fsS --retry 2 --max-time 60 "$url" -o "$tmp"; then
    echo "fetch-gl-headers.sh: could not fetch $name from $url" >&2
    echo "  the first demo build needs the network; it is cached under third_party/ after" >&2
    exit 1
fi
mv "$tmp" "$out"
