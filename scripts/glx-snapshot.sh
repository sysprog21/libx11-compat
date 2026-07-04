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

# Wait this long (replay-engine milliseconds) before snapshotting, to let the
# first frame render and composite. Configurable because a heavier client on a
# slow/loaded host (Motif GLw + gl4es first-frame shader compile under llvmpipe on
# a shared CI runner) can take longer than a bare demo to paint its first frame;
# an under-delay reads back a blank window. Default suits the light mesa demos.
delay_ms=${GLX_SNAPSHOT_DELAY_MS:-1800}

# Both are fed to shell arithmetic below, so a non-integer (a typo, or stray
# units like "4s") would abort the script with a cryptic arithmetic error or, if
# it parsed to 0, silently drop the delay and read back a blank window. Reject
# anything non-numeric back to the default with a warning instead.
case "$delay_ms" in
    '' | *[!0-9]*)
        echo "glx-snapshot: ignoring non-integer GLX_SNAPSHOT_DELAY_MS=$delay_ms" >&2
        delay_ms=1800
        ;;
esac
case "$seconds" in
    '' | *[!0-9]*)
        echo "glx-snapshot: ignoring non-integer GLX_SNAPSHOT_SECONDS=$seconds" >&2
        seconds=14
        ;;
esac

# The watchdog below (seconds) kills the demo after this many seconds. It must
# outlast the snapshot delay plus the trailing "delay 200" and some processing
# margin, or the process dies before the replay ever reaches the snapshot command
# and the run fails with an empty output for no obvious reason. Raise the watchdog
# floor from the delay so widening GLX_SNAPSHOT_DELAY_MS alone cannot foot-gun a
# premature kill; an explicit GLX_SNAPSHOT_SECONDS still wins when it is larger.
min_seconds=$(((delay_ms + 200) / 1000 + 5))
[ "$seconds" -ge "$min_seconds" ] || seconds=$min_seconds

replay=$(mktemp /tmp/glx-snapshot-XXXX.replay)
trap 'rm -f "$replay"' EXIT
cat >"$replay" <<EOF
delay $delay_ms
snapshot $shot
delay 200
close latest
EOF
export LIBX11_COMPAT_REPLAY="$replay"

# Apply any caller-supplied preload (the gl4es desktop-GL interposer for Motif
# GLw; empty for the statically-linked mesa demos) to the demo process only, so
# it never taints the shell helpers above. It goes AFTER $runner so it wraps the
# demo, not xvfb-run: xvfb-run is itself a shell script, and preloading gl4es into
# it corrupts its internals with the interposer's banner. $runner stays unquoted
# (it must split into "xvfb-run -a"); the LD_PRELOAD value and demo stay quoted so
# a build path with spaces survives.
rm -f "$shot"
if [ -n "${GLX_DEMO_PRELOAD:-}" ]; then
    $runner env "LD_PRELOAD=$GLX_DEMO_PRELOAD" "$demo" \
        >/tmp/glx-snapshot-run.log 2>&1 &
else
    $runner "$demo" >/tmp/glx-snapshot-run.log 2>&1 &
fi
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
