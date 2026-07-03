#!/bin/sh
# Generate the .set alias assembly that mints the public desktop gl* symbols from
# gl4es's gl4es_gl* internals.
#
# gl4es names every entry point gl4es_glFoo internally; the public glFoo is an
# alias onto it. clang/Mach-O has no __attribute__((alias)), so we emit a tiny
# assembly file of ".globl glFoo / .set glFoo, gl4es_glFoo" directives, one per
# internal entry point. The gl4es build (mk/gl4es.mk) assembles this into
# gl-aliases.o and bakes it into libgl4es.a, so a static link of the archive
# exposes the public gl* directly, with no dynamic gl4es library.
#
# glX* are excluded: those stay with libx11-compat, not gl4es.
#
# Works for both toolchains: Mach-O nm prefixes symbols with a leading underscore
# (_gl4es_glFoo, the Darwin/ANGLE path), GNU/ELF nm does not (gl4es_glFoo, the
# Linux/Mesa path). The match tolerates an optional underscore and carries
# whichever spelling nm used straight into the .set, so the alias name matches the
# object's symbol on either. Finding no gl4es_gl* at all is still a hard error,
# not an empty, silently-useless alias file.
#
# Reads the gl4es object files given as arguments and writes the assembly to
# stdout (the makefile redirects it to the target).
#
# Usage: gen-gl4es-aliases.sh <object-file>...
set -eu

[ $# -gt 0 ] || {
    echo "usage: gen-gl4es-aliases.sh <object>..." >&2
    exit 1
}

# Capture nm first so set -e catches its failure (a pipeline would mask it: sh
# has no pipefail and the trailing sort exits 0 on empty input).
syms=$(nm "$@")

# nm lists defined text symbols as " T name" (name is _gl4es_glFoo on Mach-O,
# gl4es_glFoo on ELF). Keep the gl4es_gl* ones, derive the public name by dropping
# only the gl4es_ (so the leading underscore, if any, is preserved), skip glX*,
# and emit the alias. LC_ALL=C so the sort order (hence the baked-in bytes) is
# stable across locales; sort -u because a symbol can appear in more than one
# object.
aliases=$(printf '%s\n' "$syms" | awk '
    / T _?gl4es_gl[A-Za-z0-9]+$/ {
        pub = $3
        sub(/gl4es_/, "", pub)
        if (pub !~ /^_?glX/)
            print ".globl " pub "\n.set " pub ", " $3
    }
' | LC_ALL=C sort -u)

[ -n "$aliases" ] || {
    echo "gen-gl4es-aliases.sh: no gl4es_gl* symbols found (empty build?)" >&2
    exit 1
}

printf '%s\n' "$aliases"
