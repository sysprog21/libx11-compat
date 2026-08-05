# Packaging recipes

Distribution packaging recipes for libx11-compat. Each recipe builds the
compat shared libraries with standard-name aliases plus the public X11
API headers via `make install` (see `mk/install.mk`), into the private
prefix `/usr/lib/libx11-compat` (`%{_libdir}/libx11-compat` on RPM
distros): the shipped headers (`X11/`, `GL/`, ...) and `libX11.so`
aliases would clash with the real X11 dev packages under `/usr`.
Downstreams point their build at this tree, e.g.
`./configure --with-libx11-compat=/usr/lib/libx11-compat`.

Note for all recipes: the build stages pinned upstream Xorg headers via
`scripts/sync-upstream-headers.py`, which downloads sha256-pinned
tarballs. For offline builds (sbuild, mock/koji, or a clean chroot
without network), the script seeds those tarballs from the directory
named by `LIBX11_COMPAT_TARBALL_DIR` instead of the network, verifying
the pinned sha256 either way. The RPM spec declares the tarballs as
`Source1:`..`Source6:` and points the variable at `%{_sourcedir}`; the
PKGBUILD lists them in `source=()` (checksummed by makepkg) and points
the variable at `$srcdir`; for Debian, place the tarballs in a directory
outside the source tree (the clean target wipes the in-tree cache) and
export the variable before `dpkg-buildpackage`.

## Debian / Ubuntu / Pardus (`../debian/`)

debhelper-13 packaging at the repository root. Build with:

```sh
dpkg-buildpackage -us -uc -b
```

The changelog targets `unstable`; retarget the distribution field when
uploading to a specific distro.

## RPM (Fedora / RHEL / openSUSE) (`rpm/libx11-compat.spec`)

```sh
rpmbuild -ba packaging/rpm/libx11-compat.spec
```

## Arch Linux (AUR) (`aur/PKGBUILD`)

A `-git` package tracking the main branch:

```sh
cd packaging/aur && makepkg -si
```
