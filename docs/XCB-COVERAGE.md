# XCB Compatibility Coverage

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
