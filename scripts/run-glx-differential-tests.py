#!/usr/bin/env python3
"""GLX differential on a Linux SSH host (node11): render fixed GLES2 cases
through our GLX layer and through direct EGL on the same system Mesa driver, and
assert the readbacks are identical. Any difference is our GLX->EGL translation.

Unlike the Motif/Xfig differentials this needs no screenshot capture: the compare
runs on the remote and this script just exits nonzero if the two dumps diverge.

Env/flags: GLX_DIFF_REMOTE (default node11), GLX_DIFF_REMOTE_ROOT, GLX_DIFF_CC
(default gcc), GLX_DIFF_JOBS (default 1; the from-scratch lib build can race the
upstream-header staging under -j, so 1 is the safe default).
"""

import argparse
import sys

from differential import (
    execute,
    parse_env_default,
    q,
    sync_repo,
)


def remote_script(remote_repo, cc, jobs):
    # xvfb gives Mesa's EGL X11 platform a display; the glx run pins the provider
    # to system libEGL, the egl run links it directly. Same driver both sides, so
    # the compare demands exact equality (0 0).
    return f"""set -e
need() {{ command -v "$1" >/dev/null 2>&1 || {{ echo "FAIL: missing $1" >&2; exit 1; }}; }}
need {q(cc)}
need make
need xvfb-run
need python3
pkg-config --exists glesv2 2>/dev/null || {{ echo "FAIL: missing glesv2.pc" >&2; exit 1; }}

cd {q(remote_repo)}
# Ask the compiler where libEGL.so lives (covers /usr/lib64, multiarch, etc.);
# it prints the bare name if not found, so fall back to a directory glob.
MESA=$({q(cc)} -print-file-name=libEGL.so)
case "$MESA" in
  /*) ;;
  *) MESA=$(ls /usr/lib/*/libEGL.so /usr/lib64/*/libEGL.so /usr/lib/libEGL.so \\
              /usr/lib64/libEGL.so 2>/dev/null | head -1) ;;
esac
if [ -z "$MESA" ] || [ ! -e "$MESA" ]; then
  echo "FAIL: no system libEGL.so found" >&2; exit 1; fi
export MESA
echo "provider: $MESA"
make -j{q(jobs)} GLX=1 CC={q(cc)} build/libX11-compat.so
mkdir -p build/tests
{q(cc)} -Iinclude -Ibuild/upstream/include tests/glx-egl-diff.c \\
    build/libX11-compat.so -lEGL -lGLESv2 -o build/tests/glx-egl-diff
# MESA is exported, so the inner shell reads it directly (no host-side splicing,
# so a path with spaces or shell metacharacters stays intact).
xvfb-run -a sh -c '
  set -e
  LD_LIBRARY_PATH="$PWD/build" LIBX11_COMPAT_EGL="$MESA" \\
      ./build/tests/glx-egl-diff glx > build/glx-diff-glx.rgba
  LD_LIBRARY_PATH="$PWD/build" ./build/tests/glx-egl-diff egl > build/glx-diff-egl.rgba
  python3 scripts/compare-rgba.py build/glx-diff-glx.rgba build/glx-diff-egl.rgba 0 0
'
"""


def main():
    parser = argparse.ArgumentParser(
        description="Run the GLX-vs-direct-EGL differential on a Linux SSH host."
    )
    parser.add_argument(
        "--remote", default=parse_env_default("GLX_DIFF_REMOTE", "node11")
    )
    parser.add_argument(
        "--remote-root",
        default=parse_env_default(
            "GLX_DIFF_REMOTE_ROOT", "/tmp/libx11-compat-glx-differential"
        ),
    )
    parser.add_argument("--cc", default=parse_env_default("GLX_DIFF_CC", "gcc"))
    parser.add_argument("--jobs", default=parse_env_default("GLX_DIFF_JOBS", "1"))
    parser.add_argument(
        "--local",
        action="store_true",
        help="run the remote script on this host instead of over SSH",
    )
    args = parser.parse_args()

    remote_repo = sync_repo(args)
    execute(args, remote_script(remote_repo, args.cc, args.jobs))
    print("GLX differential passed: our GLX layer matches direct EGL on Mesa")
    return 0


if __name__ == "__main__":
    sys.exit(main())
