# x11perf import

The X.Org `x11perf` benchmark, imported from upstream and wired into the
`libX11-compat` build so the SDL2-backed Xlib paths have a self-contained
performance harness.

## Upstream

- Repository: https://gitlab.freedesktop.org/xorg/test/x11perf
- Imported commit: `0c3597b6ecd43008092db8472779ab2d7aa111fe`

The commit is upstream `master` at import time. It is newer than the most
recent release tag (`x11perf-1.7.0`), which lacks fixes we wanted.

License is unchanged from upstream; see `COPYING`. Bug reports, patches,
and protocol questions belong upstream:

- xorg mailing list: https://lists.x.org/mailman/listinfo/xorg
- Submission guide: https://www.x.org/wiki/Development/Documentation/SubmittingPatches

## Local changes

The upstream `.c` and `.h` files are unmodified except for the
trailing-blank-line drop noted below. Build glue is local:

- `config.h` replaces the autotools-generated header. It defines
  `PACKAGE_STRING`, `HAVE_GETTIMEOFDAY`, and `X_GETTIMEOFDAY`.
- `mk/examples.mk` builds `examples/x11perf/*.c` against
  `libX11-compat.so` with `-DHAVE_CONFIG_H=1` and suppresses three
  upstream-style warnings (`missing-field-initializers`,
  `unused-but-set-variable`, `sign-compare`).
- `x11perf.c` `ProcessTest()` no longer prints the blank line after
  every test. The full `x11perf -all` log is one row per result.

## Build and run

```sh
make examples              # builds build/examples/x11perf
```

The short regression loop used during performance work:

```sh
SDL_VIDEODRIVER=dummy build/examples/x11perf -all -repeat 1 -reps 1
```

`x11perf -all` without those flags is intentionally long. Each test
auto-calibrates against its default timing window, so a full pass takes
several minutes and is not a quick check.
