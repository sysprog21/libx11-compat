# Desktop GL to GLES translation (gl4es)

This directory is the GL 1.x/2.x to GLES2 translation core, vendored from
[gl4es](https://github.com/ptitSeb/gl4es) (MIT, see LICENSE) and refined for this
project. It lets unmodified desktop-GL clients run on GLES, so a GLX app drawing
through libx11-compat renders on ANGLE/Metal (macOS) or Mesa (Linux) with no
desktop-GL driver present.

gl4es's own GLX/EGL/X11/AGL platform glue was NOT taken: libx11-compat supplies
glX* (src/glx.c) and ANGLE/Mesa supplies EGL/GLES.

## Origin

Copied from upstream commit `17f0894e19d1553e4176276c759915dab44c08e2`
(pinned in scripts/gl4es-pin.txt).

The layout is flat: every translation unit and internal header sits directly in
src/GL (upstream's src/gl/, src/gl/math/, src/gl/wrap/, and src/glx/hardext.* were
collapsed into one directory). The only Khronos headers kept in the shared
include/ tree are the two gl4es actually opens (GLES/glplatform.h and
KHR/khrplatform.h) plus GL/glx.h, which is our own hand-trimmed API contract
(coupled to src/glx.c and tests/api-symbols.txt), not an upstream copy. gl4es
declares its own GLES entry-point pointers and resolves them at runtime through
the provider, so it needs only the platform typedef headers, not the full GLES
prototype set; the EGL and unused GLES headers were dropped.

The pristine desktop-GL headers the demos compile against (GL/gl.h, glext.h,
glxext.h) are not vendored: mk/mesa-demos.mk fetches glext.h/glxext.h from the
Khronos OpenGL-Registry and gl.h from Mesa, each pinned, into third_party/ on the
first demo build (see scripts/fetch-gl-headers.sh).

## Build

Built by mk/gl4es.mk into build/libgl4es.a, a self-contained static desktop-GL
loader. The public gl* symbols are minted from gl4es's gl4es_gl* internals by a
link-time `.set` alias object baked into the archive, so a client statically links
one archive (plus the GLES provider) and gets the desktop gl* directly, with no
dynamic gl4es library. glX* stay with libx11-compat.

The build fixes the config to GLES2, no-EGL-glue, no-X11, no-GBM, static: it
always defines `-DNOEGL -DNOX11 -DNO_GBM -DSTATICLIB -DDEFAULT_ES=2`. Warnings are
silenced (-w) because this is upstream code; only the local changes below are ours.

## Wrapper files are generated, not vendored

The four gl4es/gles wrapper files upstream keeps under src/gl/wrap/ (gl4es.h,
gl4eswraps.c, gles.h, gles.c) are upstream artifacts, so they are deliberately NOT
in this directory. mk/gl4es.mk caches them raw under third_party/gl4es-wrap/
(git-ignored, survives make clean, so only the first build needs the network) and
flattens their includes into build/gl4es/gen at build time. Keeping them out of
src/GL means a tree-wide reformat never touches them and the drift check stays an
exact byte compare to upstream.

- `make sync-gl4es-wrap` refreshes the cache from the pin.
- `make check-gl4es-wrap` fails if the cache drifts from the pinned upstream.

## Local changes

Kept minimal and mechanical so re-diffing against upstream stays tractable:

- attributes.h: an `__APPLE__` branch that spells `AliasDecl` with the Mach-O
  `.set` assembler directive (clang/Mach-O has no `__attribute__((alias))`). This
  is the only hand change needed to build on macOS, and it is load-bearing.
- Flatten renames resolving basename collisions from the directory collapse
  (e.g. math/eval.h to matheval.h), with quoted include paths rewritten to bare
  filenames.
- Dead code removed: gl4es's own GetProcAddress dispatcher (gl_lookup.*,
  glesfuncs.inc) is unused here because the public gl* come from the alias object
  and internal GLES calls route through the provider's resolver; a few no-op stub
  TUs and disabled-backend headers (Raspberry Pi, GBM) went with it. All `#if 0`
  blocks were folded out.
- Dead preprocessor branches stripped with unifdef, resolving only the macros
  whose value is fixed for this build (the three the build always defines, plus
  platforms/features this tree never targets). Macros that vary by OS, arch, or
  compiler were left untouched, so no cross-platform behavior changed and the
  Linux build sees identical preprocessed source. This removes no object code
  (dead branches never compiled) -- it is a readability cut, not a size change.
- stb_dxt.h (the DXT compressor used by decompress.c and texture_compressed.c) is
  fetched from a pinned nothings/stb commit into third_party/stb on first build
  rather than vendored.

To re-derive from a newer upstream: re-copy the tree, re-apply the flatten renames
and the attributes.h patch, drop the dead units above, then run unifdef on every
*.c and *.h EXCEPT attributes.h (its `.set` macros are load-bearing) with the
build's fixed macros:

    defined:   -DNOEGL -DNOX11 -DNO_GBM
    undefined: -U_WIN32 -UWIN32 -U_WIN64 -U_MSC_VER -U__MINGW32__
               -UBUILD_WINDOWS_DLL -U_WINBASE_ -UANDROID -UUSE_ANDROID_LOG
               -UAMIGAOS4 -UPANDORA -UPYRA -UODROID -UGOA_CLONE -UBCMHOST
               -U__EMSCRIPTEN__ -UDEBUG -UTEXSTREAM -UTEXSTREAM00
               -U__BIG_ENDIAN__ -UGL4ES_COMPILE_FOR_USE_IN_SHARED_LIB

Only macros whose value is fixed for this build are resolved: the three the build
always defines, platforms this tree never targets, DEBUG and IMG texture streaming
(never built), big-endian (both targets are little endian), and the shared-lib
export path (we build STATICLIB). Macros that vary by OS, arch, or compiler, and
any macro defined inside a gl4es header, are left untouched, so no cross-platform
or feature behavior changes. A handful of headers (enum_info.h, fpe_cache.h,
glcase.h, oldprogram.h, samplers.h, shader_hacks.h) are left as-is because unifdef
misparses a construct in each; they compile fine. The full gl* API surface is kept
intact (no profile coverage trimmed).

## Integration

gl4es translates desktop gl* to GLES2; the provider (ANGLE on Metal, or Mesa) runs
the GLES2. libx11-compat's glXMakeCurrent points gl4es at the provider's GLES
resolver via `set_getprocaddress` and calls `initialize_gl4es` once a context is
current. Both are soft-resolved by dlsym, so the whole translation layer is
optional: a plain GLES/ANGLE client that never links it is unaffected.
