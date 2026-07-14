#!/usr/bin/env bash
#
# Build the user-space GL provider libraries for the Magic OpenGL stack on macOS.
#
#   build/libGLU.1.dylib  vendored Mesa GLU tessellator (third_party/glu).
#                         Magic's grtogl uses gluTess* for polygon rendering.
#   build/libGL.1.dylib   the gl4es desktop-GL -> GLES translator, exposed with a
#                         libGL soname and re-exporting libX11-compat's glX* so an
#                         unmodified GLX client binds every gl*/glX* symbol here.
#
# These were one-off clang invocations; keeping them in a script makes the OpenGL
# stack reproducible after a clean (build/ is untracked and gets wiped).
#
# Prerequisites (build these first):
#   make gl4es            -> build/libgl4es.a
#   make (default)        -> build/libX11-compat.so
#   make build-angle      -> build/angle/libGLESv2.dylib (runtime GLES provider)
#
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=${OUT:-build}
CC=${CC:-clang}

test -f "$OUT/libgl4es.a" || {
    echo "missing $OUT/libgl4es.a; run: make gl4es" >&2
    exit 1
}
test -f "$OUT/libX11-compat.so" || {
    echo "missing $OUT/libX11-compat.so; run: make" >&2
    exit 1
}

echo "  GLU     $OUT/libGLU.1.dylib"
# priorityq-heap.c is #included by priorityq.c, so it is not compiled directly.
"$CC" -dynamiclib -O2 -fPIC -fcommon -include limits.h \
    -Ithird_party/glu/include -Ithird_party/glu/src/include \
    -Ithird_party/gl-headers -Iinclude \
    -install_name @rpath/libGLU.1.dylib -undefined dynamic_lookup \
    third_party/glu/src/libtess/dict.c \
    third_party/glu/src/libtess/geom.c \
    third_party/glu/src/libtess/memalloc.c \
    third_party/glu/src/libtess/mesh.c \
    third_party/glu/src/libtess/normal.c \
    third_party/glu/src/libtess/priorityq.c \
    third_party/glu/src/libtess/render.c \
    third_party/glu/src/libtess/sweep.c \
    third_party/glu/src/libtess/tess.c \
    third_party/glu/src/libtess/tessmono.c \
    -o "$OUT/libGLU.1.dylib"
ln -sf libGLU.1.dylib "$OUT/libGLU.dylib"

echo "  GL      $OUT/libGL.1.dylib"
# -force_load pulls every gl4es object (all desktop gl*); -reexport_library folds
# libX11-compat's glX* into this library so clients see one GL provider.
"$CC" -dynamiclib -install_name @rpath/libGL.1.dylib \
    -Wl,-force_load,"$OUT/libgl4es.a" \
    -Wl,-reexport_library,"$OUT/libX11-compat.so" \
    -undefined dynamic_lookup -lobjc -framework Foundation \
    -o "$OUT/libGL.1.dylib"
ln -sf libGL.1.dylib "$OUT/libGL.dylib"

echo "  DONE    GLU + GL provider libs"
ls -la "$OUT"/libGL.1.dylib "$OUT"/libGLU.1.dylib
