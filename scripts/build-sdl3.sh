#!/bin/bash
# Build SDL3 and SDL3_ttf from source into a private prefix, with Wayland
# support. Ubuntu 24.04 (the GitHub Actions runner and the node1 differential
# host) does not package SDL3, and the override-redirect popup fix needs SDL3's
# SDL_CreatePopupWindow, so the SDL3 backend has to bring its own SDL3. The
# result is self-contained under $PREFIX and cache-friendly: point
# PKG_CONFIG_PATH and LD_LIBRARY_PATH at it to build and run SDL_BACKEND=sdl3.
#
# Pinned versions with published SHA-256 so a compromised or truncated download
# cannot slip in unnoticed. Bump the version and its checksum together.
set -euo pipefail

PREFIX="${SDL3_PREFIX:-$HOME/sdl3-prefix}"
SRC="${SDL3_SRC:-$HOME/sdl3-src}"
JOBS="${JOBS:-$(nproc)}"

SDL_VER=3.2.14
TTF_VER=3.2.2
SDL_SHA=b7e7dc05011b88c69170fe18935487b2559276955e49113f8c1b6b72c9b79c1f
TTF_SHA=63547d58d0185c833213885b635a2c0548201cc8f301e6587c0be1a67e1e045d

mkdir -p "$SRC" "$PREFIX"
cd "$SRC"

fetch() { # url dest sha256
    if [ ! -f "$2" ]; then
        curl -fsSL "$1" -o "$2.tmp"
        mv "$2.tmp" "$2"
    fi
    echo "$3  $2" | sha256sum -c - || {
        echo "checksum mismatch for $2" >&2
        rm -f "$2"
        exit 1
    }
}

echo "== fetch =="
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-$SDL_VER.tar.gz" \
    SDL3.tar.gz "$SDL_SHA"
fetch "https://github.com/libsdl-org/SDL_ttf/releases/download/release-$TTF_VER/SDL3_ttf-$TTF_VER.tar.gz" \
    SDL3_ttf.tar.gz "$TTF_SHA"
rm -rf "SDL3-$SDL_VER" "SDL3_ttf-$TTF_VER"
tar xf SDL3.tar.gz
tar xf SDL3_ttf.tar.gz

# A reused $SDL3_SRC keeps stale CMake build trees whose cache is bound to the
# previous version's source dir; wipe them so a version/checksum bump stays
# runnable without manual cleanup.
rm -rf build-sdl3 build-ttf

echo "== build SDL3 (Wayland + X11) =="
cmake -S "SDL3-$SDL_VER" -B build-sdl3 -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
    -DSDL_WAYLAND=ON -DSDL_X11=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF
ninja -j"$JOBS" -C build-sdl3
ninja -C build-sdl3 install

echo "== build SDL3_ttf (system freetype + harfbuzz) =="
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
    cmake -S "SDL3_ttf-$TTF_VER" -B build-ttf -G Ninja \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DSDLTTF_VENDORED=OFF -DSDLTTF_HARFBUZZ=ON -DSDLTTF_SAMPLES=OFF \
    -DBUILD_SHARED_LIBS=ON
ninja -j"$JOBS" -C build-ttf
ninja -C build-ttf install

echo "== installed =="
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig" \
    pkg-config --modversion sdl3 sdl3-ttf
