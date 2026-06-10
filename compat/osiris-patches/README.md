Osiris compatibility patches
============================

Patches in this directory are applied to the pinned Osiris source tree under
build/upstream/osiris before Meson configures it. Keep patches small and
ordered as NNN-short-description.patch so each one can be proposed upstream
independently.

The goal is to keep local changes limited to build feature gates and other
portability fixes required for libx11-compat validation. Do not vendor the
Osiris source tree into this repository.
