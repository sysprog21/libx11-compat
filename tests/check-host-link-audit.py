#!/usr/bin/env python3
"""Regression test for the host-link audit classifier.

The audit in scripts/host-link-audit.py is the gate that proves no host X11 or
Aqua Tk library links into the private Magic + Tcl/Tk stack, and it now runs
live in CI (check-magic.py --audit). This locks its classification so a change
to HOST_LIB_RE or the alias-farm exemption cannot silently weaken the gate to
pass on everything. Run: python3 tests/check-host-link-audit.py
"""

import importlib.util
import os
import platform
import tempfile

_HERE = os.path.dirname(os.path.abspath(__file__))
_MODULE = os.path.join(_HERE, "..", "scripts", "host-link-audit.py")


def load_audit():
    spec = importlib.util.spec_from_file_location("host_link_audit", _MODULE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_classification(audit):
    # (dependency string, is it a host X11 / Aqua link?)
    cases = [
        ("/opt/X11/lib/libX11.6.dylib", True),  # host path marker
        ("/usr/lib/libSystem.B.dylib", False),  # ordinary system lib
        ("libX11.so.6", True),  # Linux soname form
        ("libX11-compat.so", False),  # our own shim
        ("/build/out/libX11-compat.so", False),  # shim by absolute path
        ("libXext.so.6", True),
        ("libXft.so.2", True),
        ("libxcb.so.1", True),  # xcb is a host X11 transport
        ("libtk8.6.so", False),  # private Tk, not a host X client lib
        ("libtcl8.6.so", False),
        ("/System/Library/Frameworks/Tk.framework/Tk", True),  # Aqua Tk marker
        # \b boundary in HOST_LIB_RE: libXt is a host lib, but libXtstuff is an
        # unrelated basename that must not be swallowed by the Xt alternative.
        ("libXt.so.6", True),
        ("libXtstuff.so", False),
    ]
    for dep, want in cases:
        got = audit.is_host_x11(dep)
        assert got == want, f"is_host_x11({dep!r}) = {got}, want {want}"


def test_alias_farm_exemption(audit):
    # is_private_dep exempts exactly the Tcl/Tk alias farm directory
    # (<out>/tcltk-build/lib-aliases), where host-spelled sonames like libX11.so
    # are symlinks onto our compat shim. This is the most delicate branch: it
    # must exempt the farm (canonicalizing the directory, since the entries are
    # symlinks) yet must NOT exempt a host lib sitting elsewhere under --out.
    out = tempfile.mkdtemp()
    farm = os.path.join(out, "tcltk-build", "lib-aliases")
    os.makedirs(farm)
    compat = os.path.join(out, "libX11-compat.so")
    open(compat, "w").close()
    # A host-spelled soname that is really a symlink into the farm -> private.
    alias = os.path.join(farm, "libX11.so")
    os.symlink(compat, alias)
    assert audit.is_host_x11(alias, out) is False, "alias-farm entry must be private"
    # Same host soname sitting elsewhere under --out (not the farm) -> host.
    stray = os.path.join(out, "libX11.so.6")
    open(stray, "w").close()
    assert audit.is_host_x11(stray, out) is True, "a stray host lib under --out is not exempt"
    # The exemption must not depend on out_dir being passed: without it, the
    # farm path is classified purely on its basename -> host.
    assert audit.is_host_x11(alias) is True, "no out_dir: farm path falls back to basename"


def test_uninspectable_is_reported_not_raised(audit):
    # An artifact ldd/otool cannot read must surface as a clean audit failure,
    # never propagate as a traceback that aborts the whole gate.
    original = audit.dep_paths

    def boom(_path):
        raise RuntimeError("ldd failed: not a dynamic executable")

    audit.dep_paths = boom
    try:
        bad = audit.audit_no_host_x11([__file__])  # a path that exists
    finally:
        audit.dep_paths = original
    assert bad, "an uninspectable artifact must count as an audit failure"
    assert "not inspectable" in bad[0], f"expected a clean message, got {bad!r}"


def test_otool_fails_closed_on_non_object(audit):
    # macOS-only: otool exits 0 on a non-object file (unlike ldd, which exits
    # nonzero), so dep_paths must detect the empty listing and raise, or the
    # audit would pass a binary it never actually inspected. No-op on Linux,
    # where the ldd branch already fails closed on its own nonzero exit.
    if platform.system() != "Darwin":
        return
    fd, junk = tempfile.mkstemp(suffix=".txt")
    try:
        os.write(fd, b"not a mach-o binary\n")
        os.close(fd)
        raised = False
        try:
            audit.dep_paths(junk)
        except RuntimeError:
            raised = True
        assert raised, "dep_paths must raise on a non-object file (fail-closed)"
    finally:
        os.unlink(junk)


if __name__ == "__main__":
    audit = load_audit()
    test_classification(audit)
    test_alias_farm_exemption(audit)
    test_uninspectable_is_reported_not_raised(audit)
    test_otool_fails_closed_on_non_object(audit)
    print("OK: host-link audit classifier tests passed")
