# XCB Compatibility Coverage

## Building

The layer is opt-in. `XCB=1` builds `libxcb-compat.so`, the
`libX11-xcb-compat.so` bridge and their tests; a default build has none of them.

```sh
make XCB=1
make XCB=1 check-unit
```

## Header staging

The public `xcb/` headers are not tracked and are not copied from a
distribution package. `scripts/sync-upstream-headers.py` builds them into
`build/upstream/include/xcb/` alongside the Xorg headers, from two pinned
release tarballs:

- `libxcb-1.15` ships `xcb.h` and `xcbext.h` in its `src/`.
- `xproto.h` does not exist in any release. libxcb generates it from
  xcb-proto's XML with `src/c_client.py`, so the sync script runs that same
  generator against pinned `xcb-proto-1.15.2` with the arguments libxcb's own
  `src/Makefile.am` passes.

Both tarball digests are verified before use, and the generator inputs are part
of the staging stamp, so a change to either release or to the invocation
restages. `make install` ships the built copies.

## Implemented surface

No core request is implemented yet: this is the connection, setup and event
layer only. Every core opcode is deferred and deliberately absent from the
export manifest, so a client fails at link time instead of receiving a cookie
that silently never completes.

## Deliberate limits

`xcb_get_file_descriptor()` returns -1: this backend has no transport socket, so
callers must use the wait and poll entry points rather than integrating a
connection FD into an external `poll(2)` set.

RENDER, SHM, SHAPE, XFIXES and RANDR are separate extension libraries and are
not part of this layer. DRI, Present, FD passing and authorization transport
have no in-process equivalent at all.

## Differential oracle

`tests/probe-system-xcb.c` exercises the core surface and prints a
screen-independent digest of what it observed. `make check-xcb-reference` runs
it under an isolated Xvfb and compares against `tests/data/xcb-reference.txt`.

<<<<<<< HEAD
The probe currently links system XCB on both sides, so it pins the oracle rather
than this layer. Pointing one of the two runs at `build/pkgconfig/xcb.pc` is
what would turn it into a real differential.
||||||| parent of d49ecf6 (Implement XCB pixmap, GC and drawing requests)
## Core opcode ledger

The compatibility library currently implements requests 1-4, 7-8, 10, 12,
and 14-24. Every other core opcode is deferred and deliberately absent
from the export manifest; consumers therefore fail at link time instead of
receiving a false successful cookie.

“Deferred” means no ABI is advertised. Once a checked entry point is exported,
an unsupported combination must return its protocol error (`BadWindow`,
`BadValue`, `BadMatch`, and so on) through `xcb_request_check`; it must never
return silent success.

## Header provenance

The public headers come from Ubuntu's `libxcb1-dev` 1.15-1ubuntu2 package,
published at `https://archive.ubuntu.com/ubuntu/pool/main/libx/libxcb/` from the
upstream libxcb 1.15 release. The pinned package SHA-256 is
`1bafe3432feafc9e57f858721da524dfb3ee1f6fc4ef6b0c73023e79eadb9c28`.

`scripts/sync-xcb-headers.sh` verifies two digests per header. The upstream
digest authenticates the file as unpacked from that package:

| Header | Upstream SHA-256 |
| --- | --- |
| `xcb.h` | `70218365dcfd8b2e9202b145e9bafbaba042872eae046888680bf58c38cf30e6` |
| `xcbext.h` | `b101d53f1bed75e659e7469f0b7a3eb213bf1cf228f821580c7020b8e70be647` |
| `xproto.h` | `6f45223c52dc24621e7b307b26d39e4d7c884dc08900be619361e076fcac40ec` |

The vendored digest authenticates what lands in `include/xcb/`, which is the
same text with trailing whitespace removed because this repository's whitespace
gate rejects it. Only `xcb.h` is byte-identical to its upstream form:

| Header | Vendored SHA-256 |
| --- | --- |
| `xcb.h` | `70218365dcfd8b2e9202b145e9bafbaba042872eae046888680bf58c38cf30e6` |
| `xcbext.h` | `432b474c8b74444c1ed91a529f6500a57ae826a3d7dceec1b351f09c4147fedb` |
| `xproto.h` | `3da9c1a330e53a8bd6e44b895d27627e65e0da51664c5b7d48414e84ffddd766` |

Confirm the tree against the second table with `shasum -a 256 include/xcb/*.h`.
The headers retain the upstream MIT-style license notices. Regenerate them with:

```sh
sh scripts/sync-xcb-headers.sh
```
=======
## Core opcode ledger

The compatibility library currently implements requests 1-4, 7-8, 10, 12,
14-24, 53-57, 60-68, 72-73, and 76-77. The Phase-0 probe does not issue any
input, focus, grab, pointer, GC, drawing, image, font, color, cursor,
extension-query, keyboard-control, or host control request. Every other core
opcode is deferred and deliberately absent from the export manifest; consumers
therefore fail at link time instead of receiving a false successful cookie.

The deferred opcode groups below cover every core request in the vendored X11
protocol header. A group may move to the required list only with its ABI symbols,
protocol-error behavior, and a focused differential test in the same commit.

| Opcodes | Deferred requests |
| --- | --- |
| 5-6, 9, 11, 13 | DestroySubwindows, ChangeSaveSet, MapSubwindows, UnmapSubwindows, CirculateWindow |
| 25 | SendEvent |
| 26-30 | GrabPointer, UngrabPointer, GrabButton, UngrabButton, ChangeActivePointerGrab |
| 31-37 | GrabKeyboard, UngrabKeyboard, GrabKey, UngrabKey, AllowEvents, GrabServer, UngrabServer |
| 38-44 | QueryPointer, GetMotionEvents, TranslateCoordinates, WarpPointer, SetInputFocus, GetInputFocus, QueryKeymap |
| 45-52 | OpenFont through GetFontPath |
| 58-59 | SetDashes, SetClipRectangles |
| 69-71 | FillPoly, PolyFillRectangle, PolyFillArc |
| 74-75 | PolyText8, PolyText16 |
| 78-83 | CreateColormap through ListInstalledColormaps |
| 84-92 | AllocColor through LookupColor |
| 93-97 | CreateCursor through QueryBestSize |
| 98-99 | QueryExtension, ListExtensions |
| 100-108 | ChangeKeyboardMapping through GetScreenSaver |
| 109-115 | ChangeHosts through ForceScreenSaver |
| 116-119 | SetPointerMapping through GetModifierMapping |
| 127 | NoOperation |

“Deferred” means no ABI is advertised. Once a checked entry point is exported,
an unsupported combination must return its protocol error (`BadWindow`,
`BadValue`, `BadMatch`, and so on) through `xcb_request_check`; it must never
return silent success.

## Header provenance

The public headers come from Ubuntu's `libxcb1-dev` 1.15-1ubuntu2 package,
published at `https://archive.ubuntu.com/ubuntu/pool/main/libx/libxcb/` from the
upstream libxcb 1.15 release. The pinned package SHA-256 is
`1bafe3432feafc9e57f858721da524dfb3ee1f6fc4ef6b0c73023e79eadb9c28`.

`scripts/sync-xcb-headers.sh` verifies two digests per header. The upstream
digest authenticates the file as unpacked from that package:

| Header | Upstream SHA-256 |
| --- | --- |
| `xcb.h` | `70218365dcfd8b2e9202b145e9bafbaba042872eae046888680bf58c38cf30e6` |
| `xcbext.h` | `b101d53f1bed75e659e7469f0b7a3eb213bf1cf228f821580c7020b8e70be647` |
| `xproto.h` | `6f45223c52dc24621e7b307b26d39e4d7c884dc08900be619361e076fcac40ec` |

The vendored digest authenticates what lands in `include/xcb/`, which is the
same text with trailing whitespace removed because this repository's whitespace
gate rejects it. Only `xcb.h` is byte-identical to its upstream form:

| Header | Vendored SHA-256 |
| --- | --- |
| `xcb.h` | `70218365dcfd8b2e9202b145e9bafbaba042872eae046888680bf58c38cf30e6` |
| `xcbext.h` | `432b474c8b74444c1ed91a529f6500a57ae826a3d7dceec1b351f09c4147fedb` |
| `xproto.h` | `3da9c1a330e53a8bd6e44b895d27627e65e0da51664c5b7d48414e84ffddd766` |

Confirm the tree against the second table with `shasum -a 256 include/xcb/*.h`.
The headers retain the upstream MIT-style license notices. Regenerate them with:

```sh
sh scripts/sync-xcb-headers.sh
```
>>>>>>> d49ecf6 (Implement XCB pixmap, GC and drawing requests)
