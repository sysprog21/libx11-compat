#!/bin/sh
# Fetch the pinned stb_dxt.h (DXT texture compressor) to the given path.
#
# stb_dxt.h is not vendored; it is pulled once from a pinned nothings/stb commit
# (scripts/stb-pin.txt) and cached under third_party/, which survives make clean
# so only the first build needs the network. The commit hash content-addresses
# the file (GitHub serves the exact blob at that commit), so the pin itself is
# the integrity guarantee -- no separate checksum needed.
#
# Usage: fetch-stb-dxt.sh <dest-path>
set -eu

dest=${1:?usage: fetch-stb-dxt.sh <dest-path>}
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

# tr strips a trailing newline and any stray CR (CRLF checkout) so the pin cannot
# leak whitespace into the URL.
pin=$(tr -d '\r\n' <"$root/scripts/stb-pin.txt" 2>/dev/null || true)
[ -n "$pin" ] || {
    echo "fetch-stb-dxt.sh: scripts/stb-pin.txt is missing or empty" >&2
    exit 1
}

mkdir -p "$(dirname "$dest")"
# Fetch to a same-directory temp then rename: an atomic replace, and a failed or
# partial fetch never leaves a half-written header a later build treats as good.
tmp=$(mktemp "$(dirname "$dest")/.stb.XXXXXX")
trap 'rm -f "$tmp"' EXIT INT TERM

if ! curl -fsS --retry 2 --max-time 30 \
    "https://raw.githubusercontent.com/nothings/stb/$pin/stb_dxt.h" -o "$tmp"; then
    echo "fetch-stb-dxt.sh: could not fetch stb_dxt.h @ $pin" >&2
    echo "  the first build needs the network; it is cached under third_party/ after" >&2
    exit 1
fi
mv "$tmp" "$dest"
