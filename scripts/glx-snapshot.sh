#!/bin/sh
# Headless one-shot render of a GLX client through libx11-compat: drive the replay
# engine to wait, snapshot the composited window, and close.
#
# Provider-agnostic: the caller selects the GL/EGL provider by exporting
# LIBX11_COMPAT_EGL (and EGL_PLATFORM if the provider needs it, e.g. surfaceless
# Mesa) plus GLX_EXTRA_LIBS for any extra loader directories. This script only
# adds build/ to the loader path, the headless SDL driver, the replay, and a
# watchdog. On Linux it runs under Xvfb when there is no DISPLAY.
#
# Usage: glx-snapshot.sh <binary> <out.png>
# Env:   GLX_EXTRA_LIBS extra colon-separated loader dirs (e.g. ANGLE, Motif libs)
#        GLX_SNAPSHOT_SECONDS watchdog ceiling in seconds (default 14)
set -eu

demo=${1:?usage: glx-snapshot.sh <binary> <out.png>}
shot=${2:?usage: glx-snapshot.sh <binary> <out.png>}
out=${OUT:-build}
seconds=${GLX_SNAPSHOT_SECONDS:-14}
extra=${GLX_EXTRA_LIBS:-}

[ -x "$demo" ] || {
    echo "missing $demo" >&2
    exit 1
}

runner=""
libpath="$PWD/$out${extra:+:$extra}"
case "$(uname -s)" in
    Darwin)
        export DYLD_LIBRARY_PATH="$libpath${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
        export SDL_VIDEODRIVER=dummy
        ;;
    Linux)
        export LD_LIBRARY_PATH="$libpath${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        if [ -z "${DISPLAY:-}" ]; then
            command -v xvfb-run >/dev/null 2>&1 || {
                echo "no DISPLAY and no xvfb-run" >&2
                exit 1
            }
            runner="xvfb-run -a"
        fi
        ;;
    *)
        echo "unsupported OS: $(uname -s)" >&2
        exit 1
        ;;
esac

replay=$(mktemp /tmp/glx-snapshot-XXXX.replay)
trap 'rm -f "$replay"' EXIT
cat >"$replay" <<EOF
delay 1800
snapshot $shot
delay 200
close latest
EOF
export LIBX11_COMPAT_REPLAY="$replay"

rm -f "$shot"
$runner "$demo" >/tmp/glx-snapshot-run.log 2>&1 &
pid=$!

i=0
while [ "$i" -lt "$seconds" ]; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 1
    i=$((i + 1))
done
kill -9 "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

[ -f "$shot" ] || {
    echo "FAIL: no snapshot from $demo; see /tmp/glx-snapshot-run.log" >&2
    exit 1
}
echo "snapshot: $shot"
