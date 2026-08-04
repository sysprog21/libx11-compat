Name:           libx11-compat
Version:        0.1.0
Release:        1%{?dist}
Summary:        In-process Xlib implementation layered on SDL

License:        MIT
URL:            https://github.com/sysprog21/libx11-compat
Source0:        %{url}/archive/refs/heads/main.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig
BuildRequires:  python3
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(SDL2_ttf)
BuildRequires:  pkgconfig(pixman-1)

%description
libx11-compat is an in-process implementation of the X Window System
client library (Xlib) layered on top of SDL (SDL2 or SDL3), SDL_ttf,
and pixman. It lets existing Xlib clients keep their source unchanged
while running on platforms where a conventional X server is
unavailable or inconvenient: Wayland-only sessions, headless CI, and
similar environments.

This package ships the compat shared libraries (libX11-compat.so and
friends) with standard-name aliases plus the public X11 API headers,
so a downstream build can link against the shim as an ordinary X11.

# Note: the build stages pinned upstream Xorg headers via
# scripts/sync-upstream-headers.py, which downloads cached tarballs.
# For a fully offline (mock/koji) build, pre-populate the download
# cache first (run `make upstream-sync` with network access).

%prep
%autosetup -n %{name}-main

%build
%make_build

# Install into a private prefix: the shipped headers (X11/, GL/, ...)
# and libX11.so aliases would otherwise clash with the real X11 dev
# packages. Downstreams point their build at this tree, e.g.
# ./configure --with-libx11-compat=%{_libdir}/libx11-compat
%global compat_prefix %{_libdir}/libx11-compat

%install
%make_install PREFIX=%{compat_prefix}

%files
%license LICENSE
%doc README.md
%{compat_prefix}/

%changelog
* Tue Aug 04 2026 libx11-compat maintainers <https://github.com/sysprog21/libx11-compat/issues> - 0.1.0-1
- Initial packaging.
