#!/usr/bin/env python3
"""Post-run gates for Magic built against the private Tcl/TkX11 stack.

Two modes, both driven off the magic-startup replay smoke that CI already
runs (the smoke proves Magic starts, maps, and paints a real x11 layout
window, so this script never relaunches Magic just to re-check that):

  --scan-log  Fail if the run log records an unexpected missing-Xlib call.
  --audit     Fail if magicexec, magicdnull, or tclmagic, or the private
              wish / libtk / libtcl, link a host X11 / Aqua Tk library.

The host-link audit lives in host-link-audit.py; it is imported rather than
copied so this gate and any other caller cannot drift.
"""

import argparse
import importlib.util
import os
import re
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load_audit():
    spec = importlib.util.spec_from_file_location(
        "host_link_audit", os.path.join(_HERE, "host-link-audit.py")
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# Emitted by WARN_UNIMPLEMENTED when LIBX11_COMPAT_WARN_UNIMPLEMENTED is set.
STUB_RE = re.compile(r"Hit unimplemented function (\w+)")
# Benign Tk input-method setup probes Magic never depends on; everything else
# reaching a stub during the run is an unexpected missing-Xlib call.
DEFAULT_STUB_ALLOWLIST = ("XSetIMValues", "XRegisterIMInstantiateCallback")


def scan_stub_log(log_path, allow):
    """Fail if the run log records any unimplemented Xlib call outside the
    allowlist, or if the allowlisted probes are absent. The run must have set
    LIBX11_COMPAT_WARN_UNIMPLEMENTED. Requiring the known probes to appear stops
    an inert logging path (env flag removed, or WARN_UNIMPLEMENTED regressed to a
    no-op) from making the gate pass trivially on an empty or stale log."""
    if not os.path.exists(log_path):
        sys.exit(f"FAIL: scan log not found: {log_path}")
    allow = set(allow)
    with open(log_path, encoding="utf-8", errors="replace") as f:
        hits = set(STUB_RE.findall(f.read()))
    unexpected = sorted(hits - allow)
    if unexpected:
        print("FAIL: unexpected missing-X11 calls in the Magic run:")
        for name in unexpected:
            print("  " + name)
        sys.exit(1)
    absent = sorted(allow - hits)
    if absent:
        sys.exit(
            "FAIL: expected missing-X11 probes not logged (unimplemented "
            "warnings disabled or log stale); if a probe is now implemented "
            "drop it from the allowlist: " + ", ".join(absent)
        )
    print(
        f"OK: exactly the allowlisted missing-X11 probes seen: "
        f"{', '.join(sorted(hits))}"
    )


def first_existing(directory, names):
    """Path of the first candidate basename that exists in directory, else None.
    An artifact may carry the platform shared-object suffix (.dylib vs .so) or a
    version suffix, so callers pass the candidates in preference order."""
    for name in names:
        path = os.path.join(directory, name)
        if os.path.exists(path):
            return path
    return None


def audit_tcltk_stack(audit, tcltk_prefix, out_dir):
    """Return (host-link failures, missing artifacts) for the private
    Tcl/TkX11 binaries so one gate can prove the whole stack is private."""
    bindir = os.path.join(tcltk_prefix, "bin")
    libdir = os.path.join(tcltk_prefix, "lib")
    targets, missing = [], []
    wanted = [
        ("wish", bindir, ("wish8.6", "wish")),
        ("libtk", libdir, ("libtk8.6.dylib", "libtk8.6.so")),
        ("libtcl", libdir, ("libtcl8.6.dylib", "libtcl8.6.so")),
    ]
    for label, d, names in wanted:
        found = first_existing(d, names)
        if found:
            targets.append(found)
        else:
            missing.append(label)
    return audit.audit_no_host_x11(targets, out_dir), missing


def run_link_audit(cad_root, out_dir, tcltk_prefix):
    audit = _load_audit()
    tcl_dir = os.path.join(cad_root, "magic", "tcl")
    # Require the mandatory Tk-mode artifacts so a renamed/missing binary fails
    # the gate instead of shrinking the audit to nothing. tclmagic uses the
    # platform shared-object extension; magicexec is the wish stand-in the GUI
    # path runs. magicdnull is audited when present but is not required.
    # Columns: missing-message label, candidate basenames, required.
    wanted = [
        ("tclmagic.{dylib,so}", ("tclmagic.dylib", "tclmagic.so"), True),
        ("magicexec", ("magicexec",), True),
        ("magicdnull", ("magicdnull",), False),
    ]
    targets, missing = [], []
    for label, names, required in wanted:
        found = first_existing(tcl_dir, names)
        if found:
            targets.append(found)
        elif required:
            missing.append(label)

    if missing:
        sys.exit(
            "FAIL: expected Magic artifacts missing for link audit: "
            + ", ".join(missing)
        )
    bad = audit.audit_no_host_x11(targets, out_dir)
    # Name only what was audited: magicdnull is optional, so the label is built
    # from the collected targets rather than a fixed string that would claim an
    # absent artifact was checked.
    stack = " / ".join(os.path.basename(t) for t in targets)

    # Extend the audit across the private Tcl/TkX11 binaries so one gate proves
    # the whole stack, not just the Magic-side artifacts, is private. main()
    # requires --tcltk-prefix for --audit, so it is always present here.
    tcltk_bad, tcltk_missing = audit_tcltk_stack(audit, tcltk_prefix, out_dir)
    if tcltk_missing:
        sys.exit(
            "FAIL: expected Tcl/Tk artifacts missing for link audit: "
            + ", ".join(tcltk_missing)
        )
    bad += tcltk_bad
    stack += " / wish / libtk / libtcl"

    if bad:
        print("FAIL: host X11 / Aqua Tk linked into the Magic stack:")
        for b in bad:
            print("  " + b)
        sys.exit(1)
    print(f"OK: no host X11 / Aqua Tk links in {stack}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--scan-log", help="scan a run log for unexpected missing-X11 calls"
    )
    ap.add_argument(
        "--allow",
        action="append",
        default=[],
        help="allowlisted unimplemented-call name (repeatable)",
    )
    ap.add_argument(
        "--audit",
        action="store_true",
        help="audit the Magic + private Tcl/Tk stack for host X11 / Aqua Tk links",
    )
    ap.add_argument("--cad-root", help="Magic lib root (holds magic/tcl); for --audit")
    ap.add_argument("--out", help="dir holding libX11-compat.so; for --audit")
    ap.add_argument(
        "--tcltk-prefix", help="private Tcl/Tk prefix (bin/wish, lib/libtk, lib/libtcl)"
    )
    args = ap.parse_args()

    if not args.scan_log and not args.audit:
        ap.error("need --scan-log and/or --audit")

    if args.scan_log:
        scan_stub_log(args.scan_log, args.allow or DEFAULT_STUB_ALLOWLIST)

    if args.audit:
        for name in ("cad_root", "out", "tcltk_prefix"):
            if not getattr(args, name):
                sys.exit(f"FAIL: --{name.replace('_', '-')} is required for --audit")
        run_link_audit(args.cad_root, args.out, args.tcltk_prefix)


if __name__ == "__main__":
    main()
