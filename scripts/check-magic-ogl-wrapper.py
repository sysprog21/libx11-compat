#!/usr/bin/env python3
"""Smoke-test Magic's default TkCon wrapper with the OpenGL display."""

import argparse
import os
import subprocess
import sys


def prepend_path(env, name, paths):
    existing = env.get(name)
    value = os.pathsep.join(str(p) for p in paths if p)
    env[name] = value if not existing else value + os.pathsep + existing


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--magic", required=True)
    parser.add_argument("--cad-root", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--tcltk-libdir", required=True)
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()

    env = os.environ.copy()
    env["CAD_ROOT"] = args.cad_root
    env.setdefault("DISPLAY", ":0")
    # Force the headless dummy backend even when SDL_VIDEODRIVER is already
    # exported, so this gate stays hermetic and matches the other Magic/Tk
    # checks instead of picking up a real backend from the environment.
    env["SDL_VIDEODRIVER"] = "dummy"
    prepend_path(env, "DYLD_LIBRARY_PATH", [args.out, args.tcltk_libdir])
    prepend_path(env, "LD_LIBRARY_PATH", [args.out, args.tcltk_libdir])

    # -norcfile so a working-directory or $HOME .magicrc cannot quit Magic,
    # change the OGL setup, or hang startup before the readiness assertion; the
    # default TkCon wrapper is then exercised in isolation.
    cmd = [args.magic, "-d", "OGL", "-norcfile"]
    # Printed by the Tcl wrapper once the interpreter is up and Magic reaches
    # its startup identity banner. A timeout without this phrase means Magic
    # hung or deadlocked before initializing, which must fail, not pass.
    ready_signal = "Starting magic under Tcl interpreter"
    try:
        proc = subprocess.run(
            cmd,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=args.timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        output = exc.output or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        if "graphics display couldn't be correctly initialized" in output:
            print(output, end="")
            print(
                "check-magic-ogl-wrapper: Magic reported OGL init failure",
                file=sys.stderr,
            )
            return 1
        # A live wrapper is only proven by the readiness banner. Timing out with
        # no such signal means Magic hung before initializing, so reject it.
        if ready_signal not in output:
            print(output, end="")
            print(
                "check-magic-ogl-wrapper: Magic timed out before initializing "
                f"(no {ready_signal!r} banner)",
                file=sys.stderr,
            )
            return 1
        print("check-magic-ogl-wrapper: ok")
        return 0

    output = proc.stdout or ""
    if output:
        print(output, end="")
    print(
        f"check-magic-ogl-wrapper: Magic exited before timeout "
        f"(status {proc.returncode})",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
