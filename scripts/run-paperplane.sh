#!/bin/sh
# Run unmodified Motif paperplane (GLwDrawingArea + immediate-mode desktop GL) on
# libx11-compat + GLX. GLwDrawingArea is a child widget with no native surface, so
# the GLX layer renders it to an offscreen pbuffer, reads it back, and composites
# it into the Motif window.
#
# paperplane renders desktop gl* through a Mesa provider (surfaceless EGL), with
# the Motif runtime libs on the loader path. The platform-specific
# driver/replay/watchdog live in the shared runners. macOS uses Homebrew Mesa's
# libEGL plus the in-tree gl-only shim (paperplane's GLw lib was built against the
# shim); Linux uses the system Mesa libEGL, located via the compiler, with Mesa
# already on the standard loader path.
#
# Usage:
#   run-paperplane.sh              open a live interactive window (default)
#   run-paperplane.sh --snapshot [out.png]   headless one-shot screenshot
set -eu

out=${OUT:-build}
demo="$out/motif-demos/demos/GLw/.libs/paperplane"
motif_libs="$PWD/$out/motif-demos/lib/Xm/.libs:$PWD/$out/motif-demos/lib/Mrm/.libs:$PWD/$out/motif-demos/lib/GLw/.libs"

preload=""
case "$(uname -s)" in
    Darwin)
        mesa=$(brew --prefix mesa 2>/dev/null || echo /opt/homebrew/opt/mesa)
        egl=${LIBX11_COMPAT_EGL:-$mesa/lib/libEGL.dylib}
        extra_libs="$PWD/$out/glshim:$mesa/lib:$motif_libs"
        ;;
    *)
        # -print-file-name is a query, not a compile, so drop any launcher prefix
        # (e.g. "ccache clang" -> "clang", as the motif CI job sets CC) and run the
        # real compiler to locate the system libEGL.
        cc=${GLX_DIFF_CC:-${CC:-cc}}
        realcc=${cc##* }
        egl=${LIBX11_COMPAT_EGL:-$("$realcc" -print-file-name=libEGL.so)}
        extra_libs="$motif_libs"
        # On Linux the Motif GLw widget links system libGL, which also exports
        # glX*; those would shadow our layer's, so paperplane would call the real
        # glXQueryExtension (no X server GLX) and bail with "GLX extension not
        # available". Preload our library so our glX* interpose. gl* still resolve
        # to system Mesa (we export none), so desktop GL keeps rendering through
        # Mesa while GLX comes from us.
        preload="$PWD/$out/libX11-compat.so"
        ;;
esac

[ -x "$demo" ] || {
    echo "missing $demo; run: make GLX=1 motif-demos" >&2
    exit 1
}
[ -f "$egl" ] || {
    echo "missing libEGL at $egl" >&2
    exit 1
}

# Mesa provider: surfaceless EGL, provider-specific extra loader dirs + Motif libs.
export LIBX11_COMPAT_EGL="$egl"
export EGL_PLATFORM=surfaceless
export GLX_EXTRA_LIBS="$extra_libs"
[ -n "$preload" ] && export LD_PRELOAD="$preload${LD_PRELOAD:+:$LD_PRELOAD}"

if [ "${1:-}" = "--snapshot" ]; then
    exec env GLX_SNAPSHOT_SECONDS="${PAPERPLANE_SECONDS:-14}" \
        scripts/glx-snapshot.sh "$demo" "${2:-/tmp/paperplane.png}"
fi

exec scripts/run-glx-window.sh "$demo"
