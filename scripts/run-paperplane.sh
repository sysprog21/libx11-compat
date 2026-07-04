#!/bin/sh
# Run unmodified Motif paperplane (GLwDrawingArea + immediate-mode desktop GL) on
# libx11-compat + GLX. GLwDrawingArea is a child widget with no native surface, so
# the GLX layer renders it to an offscreen pbuffer, reads it back, and composites
# it into the Motif window.
#
# paperplane renders desktop gl* over a GLES provider (surfaceless EGL), with the
# Motif runtime libs on the loader path. The platform-specific driver/replay/
# watchdog live in the shared runners. macOS uses Homebrew Mesa's libEGL plus the
# in-tree gl-only shim (paperplane's GLw lib was built against the shim, so its
# desktop gl* run on Mesa directly); Linux uses the system Mesa libEGL located via
# the compiler and preloads a gl4es-backed libGL to translate the desktop gl* to
# GLES (the system GLVND libGL cannot, see the Linux branch below).
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
        # On Linux the Motif GLw widget links the system GLVND libGL, which
        # exports both glX* and gl*. Two problems, both fixed by preloading:
        #   glX*: GLVND's would shadow ours, so paperplane would call the real
        #     glXQueryExtension (no X server GLX) and bail with "GLX extension not
        #     available". Preload libX11-compat so our glX* interpose.
        #   gl*: GLVND dispatches immediate-mode gl* through its own current
        #     context, which our EGL-based glXMakeCurrent never populates, so the
        #     canvas stays black. Preload the gl4es-backed libGL ahead of GLVND so
        #     the desktop gl* interpose and route through gl4es to the GLES
        #     provider on our EGL context. Built by mk/gl4es.mk; absent (skip) if
        #     GLX was off. gl4es must come first so its gl* win over GLVND's.
        preload="$PWD/$out/libX11-compat.so"
        gl4es_so="$PWD/$out/gl4es/libGL.so"
        [ -f "$gl4es_so" ] && preload="$gl4es_so:$preload"
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
# Preload is scoped to the demo process by the runner scripts, NOT exported as a
# process-wide LD_PRELOAD: the gl4es interposer prints a banner and dlopens
# libGLESv2 in its constructor, so injecting it into the shell's own helper
# commands (uname, mktemp, sleep) both wastes work and corrupts a "$(uname -s)"
# capture with gl4es stdout. The runners apply it only when launching the demo.
[ -n "$preload" ] && export GLX_DEMO_PRELOAD="$preload${LD_PRELOAD:+:$LD_PRELOAD}"

if [ "${1:-}" = "--snapshot" ]; then
    # paperplane is a heavy first frame: SDL + Motif realize, GLw create, gl4es
    # init and its first-use shader compile, all before the first composite. On a
    # slow/loaded CI runner (surfaceless llvmpipe) that can exceed the default
    # 1800ms snapshot delay and read back a blank window, so give it a wider
    # margin. The animation is continuous, so once any frame paints later ones do
    # too; a later snapshot is strictly safer, bounded by the watchdog seconds.
    exec env GLX_SNAPSHOT_SECONDS="${PAPERPLANE_SECONDS:-20}" \
        GLX_SNAPSHOT_DELAY_MS="${PAPERPLANE_DELAY_MS:-4000}" \
        scripts/glx-snapshot.sh "$demo" "${2:-/tmp/paperplane.png}"
fi

exec scripts/run-glx-window.sh "$demo"
