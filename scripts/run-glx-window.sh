#!/bin/sh
# Open a GLX client in a live, interactive on-screen window through libx11-compat.
# The client renders to an offscreen surface the layer composites into a real
# window SDL presents each frame. Runs until the client exits (close the window,
# or its quit key such as Esc in glxgears).
#
# Provider-agnostic: the caller selects the GL/EGL provider by exporting
# LIBX11_COMPAT_EGL (+ EGL_PLATFORM if needed) and GLX_EXTRA_LIBS for extra loader
# dirs. On Linux a real DISPLAY is required (a window cannot come from Xvfb).
#
# Usage: run-glx-window.sh <binary>
set -eu

demo=${1:?usage: run-glx-window.sh <binary>}
out=${OUT:-build}
extra=${GLX_EXTRA_LIBS:-}

[ -x "$demo" ] || {
    echo "missing $demo" >&2
    exit 1
}

libpath="$PWD/$out${extra:+:$extra}"
case "$(uname -s)" in
    Darwin)
        export DYLD_LIBRARY_PATH="$libpath${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
        ;;
    Linux)
        [ -n "${DISPLAY:-}" ] || {
            echo "on-screen window needs a running X display (set DISPLAY)" >&2
            exit 1
        }
        export LD_LIBRARY_PATH="$libpath${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        ;;
    *)
        echo "unsupported OS: $(uname -s)" >&2
        exit 1
        ;;
esac

echo "opening $(basename "$demo") window (close it or press its quit key to exit)..."
exec "$demo"
