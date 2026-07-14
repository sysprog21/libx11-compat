#!/usr/bin/env python3
"""Startup gate for Magic built against the private Tcl/TkX11 stack.

Runs Magic headless (SDL_VIDEODRIVER=dummy, DISPLAY=:0) through the private
wish, opens a layout window, and asserts:
  1. Tk reports the x11 windowing system.
  2. Magic maps a layout window (a .magic<N> toplevel) with nonzero geometry.
  3. magicexec, magicdnull, and tclmagic do not link a host X11 / Aqua Tk lib.

The host-link audit is shared with check-tcltk.py; it is imported rather
than copied so the two gates cannot drift.
"""

import argparse
import importlib.util
import os
import re
import subprocess
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))


def _load_audit():
    spec = importlib.util.spec_from_file_location(
        "tcltk_gate", os.path.join(_HERE, "check-tcltk.py")
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# Magic layout windows are Tk toplevels named .magic1, .magic2, ...
LAYOUT_WIN_RE = re.compile(r"^\.magic\d+$")

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


# rcfile sourced during magic::initialize: open a window synchronously (the
# -nowrapper path keeps no idle Tk event loop, so after/vwait would never fire),
# force a paint pass, report the Tk window tree, then quit.
RCFILE = r"""
puts "WS=[tk windowingsystem]"
set owrc [catch {openwindow} err]
puts "OPENWINDOW_RC=$owrc ERR=$err"
update idletasks
update
foreach w [winfo children .] {
    puts "WIN=$w MAPPED=[winfo ismapped $w] GEO=[winfo width $w]x[winfo height $w]"
}
flush stdout
quit -noprompt
"""


def run_magic(magic, cad_root, out_dir, home, tcltk_prefix):
    rc = os.path.join(home, "magic-startup.tcl")
    os.makedirs(home, exist_ok=True)
    with open(rc, "w") as f:
        f.write(RCFILE)
    env = dict(os.environ)
    env["CAD_ROOT"] = cad_root
    env["HOME"] = home
    env["SDL_VIDEODRIVER"] = "dummy"
    env.setdefault("SDL_AUDIODRIVER", "dummy")
    env.setdefault("DISPLAY", ":0")
    libpath = out_dir + ":" + os.path.join(tcltk_prefix, "lib")
    for var in ("DYLD_LIBRARY_PATH", "LD_LIBRARY_PATH"):
        env[var] = libpath + (":" + env[var] if env.get(var) else "")
    proc = subprocess.run(
        [magic, "-noconsole", "-nowrapper", "-rcfile", rc],
        capture_output=True,
        text=True,
        env=env,
        timeout=90,
    )
    return proc


def parse_windows(stdout):
    ws = None
    open_rc = None
    windows = []
    for line in stdout.splitlines():
        if line.startswith("WS="):
            ws = line.split("=", 1)[1].strip()
        elif line.startswith("OPENWINDOW_RC="):
            open_rc = line.split("=", 1)[1].split(" ", 1)[0].strip()
        elif line.startswith("WIN="):
            m = re.match(r"WIN=(\S+) MAPPED=(\d+) GEO=(\d+)x(\d+)", line.strip())
            if m:
                windows.append(
                    {
                        "name": m.group(1),
                        "mapped": m.group(2) == "1",
                        "w": int(m.group(3)),
                        "h": int(m.group(4)),
                    }
                )
    return ws, open_rc, windows


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
        found = next(
            (os.path.join(d, n) for n in names if os.path.exists(os.path.join(d, n))),
            None,
        )
        if found:
            targets.append(found)
        else:
            missing.append(label)
    return audit.audit_no_host_x11(targets, out_dir), missing


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--magic", help="path to bin/magic launcher")
    ap.add_argument("--cad-root")
    ap.add_argument("--out", help="dir holding libX11-compat.so")
    ap.add_argument("--home", help="scratch HOME for the run")
    ap.add_argument(
        "--tcltk-prefix", help="private Tcl/Tk prefix (bin/wish, lib/libtk, lib/libtcl)"
    )
    ap.add_argument(
        "--scan-log", help="scan a run log for unexpected missing-X11 calls, then exit"
    )
    ap.add_argument(
        "--allow",
        action="append",
        default=[],
        help="allowlisted unimplemented-call name (repeatable)",
    )
    args = ap.parse_args()

    if args.scan_log:
        scan_stub_log(args.scan_log, args.allow or DEFAULT_STUB_ALLOWLIST)
        return

    for name in ("magic", "cad_root", "out", "home", "tcltk_prefix"):
        if not getattr(args, name):
            sys.exit(f"FAIL: --{name.replace('_', '-')} is required for the run gate")
    if not os.path.exists(args.magic):
        sys.exit(f"FAIL: not found: {args.magic}")

    proc = run_magic(args.magic, args.cad_root, args.out, args.home, args.tcltk_prefix)
    # Fail closed: a nonzero exit means Magic crashed or aborted even if it
    # managed to print some of the probe lines first.
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        sys.exit(f"FAIL: magic exited {proc.returncode}")
    ws, open_rc, windows = parse_windows(proc.stdout)
    if ws != "x11":
        sys.stderr.write(proc.stdout + proc.stderr)
        sys.exit(f"FAIL: tk windowingsystem is {ws!r}, expected 'x11'")
    if open_rc != "0":
        sys.stderr.write(proc.stdout + proc.stderr)
        sys.exit(f"FAIL: openwindow returned {open_rc!r} (Tcl error)")
    layout = [
        w
        for w in windows
        if LAYOUT_WIN_RE.match(w["name"]) and w["mapped"] and w["w"] > 0 and w["h"] > 0
    ]
    if not layout:
        sys.stderr.write(proc.stdout + proc.stderr)
        sys.exit(f"FAIL: no mapped Magic layout window; saw {windows}")
    w = layout[0]
    print(
        f"OK: Magic maps layout window {w['name']} "
        f"({w['w']}x{w['h']}) as x11 through libx11-compat"
    )

    audit = _load_audit()
    tcl_dir = os.path.join(args.cad_root, "magic", "tcl")
    # Require the mandatory Tk-mode artifacts so a renamed/missing binary fails
    # the gate instead of shrinking the audit to nothing. tclmagic uses the
    # platform shared-object extension; magicexec is the wish stand-in the GUI
    # path runs. magicdnull is audited when present but is not required.
    targets, missing = [], []

    tclmagic = next(
        (
            p
            for ext in ("dylib", "so")
            for p in [os.path.join(tcl_dir, f"tclmagic.{ext}")]
            if os.path.exists(p)
        ),
        None,
    )
    if tclmagic:
        targets.append(tclmagic)
    else:
        missing.append("tclmagic.{dylib,so}")

    magicexec = os.path.join(tcl_dir, "magicexec")
    if os.path.exists(magicexec):
        targets.append(magicexec)
    else:
        missing.append("magicexec")

    magicdnull = os.path.join(tcl_dir, "magicdnull")
    if os.path.exists(magicdnull):
        targets.append(magicdnull)

    if missing:
        sys.exit(
            "FAIL: expected Magic artifacts missing for link audit: "
            + ", ".join(missing)
        )
    bad = audit.audit_no_host_x11(targets, args.out)
    stack = "magicexec / magicdnull / tclmagic"

    # Extend the audit across the private Tcl/TkX11 binaries so one gate proves
    # the whole stack, not just the Magic-side artifacts, is private.
    if args.tcltk_prefix:
        tcltk_bad, tcltk_missing = audit_tcltk_stack(audit, args.tcltk_prefix, args.out)
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


if __name__ == "__main__":
    main()
