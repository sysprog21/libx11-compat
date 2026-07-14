#!/usr/bin/env python3
"""Gate for the private Tcl/TkX11 build.

Two checks:
  1. wish reports the x11 windowing system and can map a Tk toplevel through
     libx11-compat headless (SDL_VIDEODRIVER=dummy).
  2. wish and libtk do not link a host X11 library (Aqua Tk, XQuartz, /opt/X11).
     Our own soname farm records the compat dylib via its @rpath install name
     (libX11-compat.so), so only a genuine host X11 reference is a failure.
"""

import argparse
import os
import platform
import re
import subprocess
import sys

# A dependency path that contains one of these means a real host X11 / Aqua
# stack crept in regardless of the library basename.
HOST_PATH_MARKERS = ("/opt/X11", "/usr/X11", "XQuartz", "Tk.framework", "Tcl.framework")
# A dependency whose basename starts with one of these X client libraries is a
# host X11 link. Our own shims end in -compat.so and are excluded before this
# test, so libX11-compat.so never matches. The \b keeps libXt from swallowing
# an unrelated libXtstuff while still catching libX11.6.dylib / libX11.so.6.
HOST_LIB_RE = re.compile(
    r"^lib(X11|Xext|Xrender|Xft|Xt|Xmu|Xpm|Xi|Xcursor|Xfixes|"
    r"Xinerama|Xss|Xdamage|Xcomposite|xcb)\b"
)


def dep_paths(path):
    """Return recorded dynamic-dependency paths for a binary. Fails closed:
    raises if the inspection tool errors so the audit never passes blind."""
    if platform.system() == "Darwin":
        proc = subprocess.run(["otool", "-L", path], capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"otool -L {path} failed: {proc.stderr.strip()}")
        deps = []
        # First line is the binary's own path/id; skip it. Each dependency line
        # is "<path> (compatibility version X, current version Y)".
        for line in proc.stdout.splitlines()[1:]:
            line = line.strip()
            if line:
                deps.append(line.split(" (", 1)[0].strip())
        return deps
    proc = subprocess.run(["ldd", path], capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"ldd {path} failed: {proc.stderr.strip()}")
    deps = []
    # ldd lines are "soname => /resolved/path (0x...)" or a bare loader path.
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        if "=>" in line:
            rhs = line.split("=>", 1)[1].strip()
            resolved = rhs.split(" (", 1)[0].strip()
            # "soname => not found" leaves no path; keep the left-hand soname so
            # a missing host X11 link still trips HOST_LIB_RE instead of the
            # unmatchable string "not found" silently passing the gate.
            if not resolved or resolved == "not found":
                resolved = line.split("=>", 1)[0].strip() or line.split()[0]
            deps.append(resolved)
        else:
            deps.append(line.split()[0])
    return deps


def is_private_dep(dep, out_dir):
    if os.path.basename(dep).endswith("-compat.so"):
        return True
    if out_dir and os.path.isabs(dep):
        # Only the Tcl/Tk alias farm is exempt as a directory. Exempting the
        # whole --out tree would let a host X11 library dropped under it bypass
        # HOST_PATH_MARKERS / HOST_LIB_RE; the compat libraries living in --out
        # are already covered by the -compat.so basename check above.
        #
        # Canonicalize the containing DIRECTORY, not the dependency file: the
        # alias-farm entries are symlinks into build/ (libX11.so ->
        # libX11-compat.so), so realpath on the file itself resolves out of the
        # farm and would miss the exemption, failing the gate on its own build.
        aliases = os.path.realpath(os.path.join(out_dir, "tcltk-build", "lib-aliases"))
        dep_dir = os.path.realpath(os.path.dirname(dep))
        return dep_dir == aliases
    return False


def is_host_x11(dep, out_dir=None):
    if is_private_dep(dep, out_dir):
        return False
    if any(m in dep for m in HOST_PATH_MARKERS):
        return True
    return bool(HOST_LIB_RE.match(os.path.basename(dep)))


def audit_no_host_x11(paths, out_dir=None):
    bad = []
    for p in paths:
        if not os.path.exists(p):
            continue
        for dep in dep_paths(p):
            if is_host_x11(dep, out_dir):
                bad.append(f"{os.path.basename(p)}: {dep}")
    return bad


def run_wish(wish, out_dir):
    """Drive wish headless; return (windowingsystem, mapped_ok)."""
    script = r"""
package require Tk
set ws [tk windowingsystem]
wm geometry . 320x240+0+0
button .b -text hi
pack .b
update idletasks
update
# winfo ismapped is the real proof the window reached the server layer.
set mapped [winfo ismapped .]
puts "WINDOWINGSYSTEM=$ws"
puts "MAPPED=$mapped"
puts "GEOMETRY=[winfo width .]x[winfo height .]"
exit 0
"""
    env = dict(os.environ)
    libpath = out_dir
    for var in ("DYLD_LIBRARY_PATH", "LD_LIBRARY_PATH"):
        env[var] = libpath + (":" + env[var] if env.get(var) else "")
    env["SDL_VIDEODRIVER"] = "dummy"
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    # Tk reads $DISPLAY itself before ever calling into libx11-compat, so a
    # value must be present in the process env; the layer maps it onto SDL.
    env.setdefault("DISPLAY", ":0")
    proc = subprocess.run(
        [wish], input=script, capture_output=True, text=True, env=env, timeout=60
    )
    ws = mapped = geom = None
    for line in proc.stdout.splitlines():
        if line.startswith("WINDOWINGSYSTEM="):
            ws = line.split("=", 1)[1].strip()
        elif line.startswith("MAPPED="):
            mapped = line.split("=", 1)[1].strip()
        elif line.startswith("GEOMETRY="):
            geom = line.split("=", 1)[1].strip()
    return ws, mapped, geom, proc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wish", required=True)
    ap.add_argument("--out", required=True, help="dir holding libX11-compat.so")
    args = ap.parse_args()

    if not os.path.exists(args.wish):
        sys.exit(f"FAIL: wish not found at {args.wish}")

    ws, mapped, geom, proc = run_wish(args.wish, args.out)
    # Fail closed: a nonzero exit means wish crashed or aborted even if it
    # managed to print the probe lines first, so ignore the parsed markers.
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        sys.exit(f"FAIL: wish exited {proc.returncode}")
    if ws != "x11":
        sys.stderr.write(proc.stdout + proc.stderr)
        sys.exit(f"FAIL: tk windowingsystem is {ws!r}, expected 'x11'")
    if mapped != "1":
        sys.stderr.write(proc.stdout + proc.stderr)
        sys.exit(f"FAIL: Tk toplevel did not map (ismapped={mapped!r})")
    print(f"OK: wish windowingsystem=x11, toplevel mapped ({geom})")

    # libtk lives next to wish under ../lib.
    tk_libdir = os.path.join(os.path.dirname(os.path.dirname(args.wish)), "lib")
    audit_targets = [args.wish]
    if os.path.isdir(tk_libdir):
        for f in os.listdir(tk_libdir):
            if f.startswith("libtk") and (f.endswith(".so") or f.endswith(".dylib")):
                audit_targets.append(os.path.join(tk_libdir, f))
    bad = audit_no_host_x11(audit_targets, args.out)
    if bad:
        print("FAIL: host X11 / Aqua Tk linked into the stack:")
        for b in bad:
            print("  " + b)
        sys.exit(1)
    print("OK: no host X11 / Aqua Tk links in wish or libtk")


if __name__ == "__main__":
    main()
