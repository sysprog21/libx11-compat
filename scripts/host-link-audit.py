"""Host-link audit for the private Tcl/TkX11 stack.

Reports whether a binary links a host X11 library (Aqua Tk, XQuartz, /opt/X11)
instead of our compat shim. Our own soname farm records the compat dylib via
its @rpath install name (libX11-compat.so), so only a genuine host X11
reference is a failure.

Imported (not run) by check-magic.py, which drives the audit over the Magic
plus private Tcl/Tk artifacts as part of the Magic replay gate. That smoke
loads the shared libtcl/libtk through Magic (built -nowrapper with Tcl/Tk
embedded), so a runtime regression in the Tk libraries still surfaces; the
standalone wish binary is no longer launched, only statically link-audited.
"""

import os
import platform
import re
import subprocess

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
        # otool exits 0 even on a non-object file, printing "is not an object
        # file" and emitting no dependency listing. A valid mach-o always prints
        # at least its own path header, so empty stdout means nothing was
        # inspected. Fail closed on that, matching ldd's nonzero exit on a
        # non-ELF artifact, so a broken artifact cannot pass the audit blind.
        if (
            "is not an object file" in (proc.stdout + proc.stderr)
            or not proc.stdout.strip()
        ):
            raise RuntimeError(
                f"otool -L {path} inspected no object file: "
                f"{(proc.stderr or proc.stdout).strip()}"
            )
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
        # An artifact ldd/otool cannot inspect (a static binary, a non-ELF
        # wrapper script), or a missing/unrunnable inspection tool itself
        # (OSError, e.g. ldd not on PATH), is reported as a clean audit failure
        # rather than crashing the whole gate with a traceback. Still
        # fail-closed: an uninspectable artifact counts as bad, so the audit
        # never passes on a binary it could not actually read. Other artifacts
        # keep being audited.
        try:
            deps = dep_paths(p)
        except (OSError, RuntimeError) as error:
            bad.append(f"{os.path.basename(p)}: not inspectable ({error})")
            continue
        for dep in deps:
            if is_host_x11(dep, out_dir):
                bad.append(f"{os.path.basename(p)}: {dep}")
    return bad
