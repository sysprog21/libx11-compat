#!/bin/bash

# Consolidated validation for the "detached menus and odd fonts" report: an
# override-redirect menu must land anchored to its parent on Wayland, and a
# FONT_IS_XFT render table must load a legible font. Runs four sub-checks and
# prints a PASS/FAIL summary:
#   1. font     FONT_IS_XFT render table measures a legible height (needs libXm
#               built --with-xft).
#   2. popup    a raw override-redirect popup is emitted as a Wayland xdg_popup
#               anchored to its parent, not a detached second xdg_toplevel
#               (needs the SDL3 backend, a Wayland compositor, and weston).
#   2b.resize   an override-redirect popup resized after mapping grows the
#               compositor's xdg_popup geometry, not just the surface buffer, so
#               a switched-to menu is not clipped to its creation size.
#   2c.slide    a popup's recorded absolute origin (ws->x/y) matches parent +
#               the compositor's actual popup offset, so pointer translation
#               tracks a compositor slide/flip (constraining compositor) and the
#               un-slid case (weston) alike.
#   3. menu     a real Motif XmPulldownMenu, posted by an injected click, maps
#               its XmMenuShell as an xdg_popup anchored to the main window
#               (the exact "drop-down menus as free-standing windows" report).
#   4. destroy  tearing a popup's parent down while the popup is live is clean
#               under AddressSanitizer, for both parent destroy and parent
#               unmap; the library itself is rebuilt with -fsanitize=address so
#               the check instruments the teardown path, not just the test binary.
#
# Wayland checks SKIP with a clear reason when no compositor / SDL3 is present,
# so the same script runs on an X11-only host for the font check. Set
# CI_REQUIRE=1 to turn any SKIP into a failure (for a required CI gate).
#
# Env overrides: ROOT, SDL3_PREFIX, WESTON, SOCK, MENUVAL_WORK (keep the work
# dir, e.g. for CI artifact upload; not deleted when set), CI_REQUIRE.
set -u
ROOT="${ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
SDL3_PREFIX="${SDL3_PREFIX:-$HOME/sdl3-prefix}"
WESTON="${WESTON:-$(command -v weston || true)}"
SOCK="${SOCK:-wayland-menuval}"
BUILD="$ROOT/build"
TDIR="$ROOT/tests/menu-rendering"
CI_REQUIRE="${CI_REQUIRE:-0}"
if [ -n "${MENUVAL_WORK:-}" ]; then
    WORK="$MENUVAL_WORK"
    mkdir -p "$WORK"
    KEEP_WORK=1
else
    WORK="$(mktemp -d /tmp/menuval.XXXXXX)"
    KEEP_WORK=0
fi
export LD_LIBRARY_PATH="$BUILD:$SDL3_PREFIX/lib:$SDL3_PREFIX/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# check_destroy rebuilds the library (make SDL_BACKEND=sdl3), which needs the
# SDL3 pkg-config files; export the path here so the harness is self-sufficient
# rather than relying on the caller's environment.
export PKG_CONFIG_PATH="$SDL3_PREFIX/lib/pkgconfig:$SDL3_PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
INC="-I$ROOT/include -I$BUILD/upstream/include"

pass=0
fail=0
skip=0
declare -a RESULTS
record() {
    RESULTS+=("$1 $2 $3")
    case "$1" in PASS) pass=$((pass + 1)) ;; FAIL) fail=$((fail + 1)) ;; SKIP) skip=$((skip + 1)) ;; esac
}

have_sdl3() { [ -f "$SDL3_PREFIX/lib/libSDL3.so" ] || [ -f "$SDL3_PREFIX/lib64/libSDL3.so" ]; }
backend_is_sdl3() { grep -q sdl3 "$BUILD/.sdl-backend" 2>/dev/null; }

# Gate the three Wayland-only checks on an active SDL3 backend and a compositor,
# recording a SKIP with the reason under the caller's check name.
#
# Returns nonzero
# so the caller can `require_wayland <name> || return`.
require_wayland() {
    if ! have_sdl3 || ! backend_is_sdl3; then
        record SKIP "$1" "SDL3 backend not active"
        return 1
    fi
    if [ -z "$WESTON" ]; then
        record SKIP "$1" "no weston / Wayland compositor"
        return 1
    fi
    return 0
}

start_weston() {
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-$(mktemp -d /tmp/xdg.XXXXXX)}"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 700 "$XDG_RUNTIME_DIR" 2>/dev/null
    printf '[core]\nidle-time=0\nrequire-input=false\n[shell]\npanel-position=none\n' >"$WORK/weston.ini"
    rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock"
    "$WESTON" --backend=headless-backend.so --width=1000 --height=700 \
        --socket="$SOCK" --config="$WORK/weston.ini" >"$WORK/weston.log" 2>&1 &
    WPID=$!
    for _ in $(seq 1 100); do
        [ -S "$XDG_RUNTIME_DIR/$SOCK" ] && return 0
        sleep 0.1
    done

    # Socket never appeared: reap the weston we spawned so a retry does not
    # leave an orphaned compositor behind.
    stop_weston
    return 1
}

# Kill only the specific weston PID, never a bare 0 (which would signal the
# whole process group), and clear it so a second stop is a no-op rather than
# killing a reused PID.
WPID=
stop_weston() {
    [ -n "$WPID" ] || return 0
    kill "$WPID" 2>/dev/null
    wait "$WPID" 2>/dev/null
    WPID=
}

# Run a client under a fresh weston up to twice (the menu post depends on a
# wall-clock replay delay, which can miss on a loaded runner) and require both
# the completion sentinel and a clean exit, so a client that emitted one
# get_popup and then hung or was killed does not count as a pass. Emits the
# per-attempt log to $2.
#
# Returns 0 only when the run completed cleanly.
run_client() {
    local bin="$1" log="$2" sentinel="$3"
    shift 3
    local attempt rc
    for attempt in 1 2; do
        start_weston || {
            echo "weston did not start" >"$log"
            return 1
        }

        # -k 5: if the client ignores the SIGTERM at 20s, SIGKILL it 5s later so
        # a wedged binary cannot hang the job past the intended bound.
        env "$@" WAYLAND_DISPLAY="$SOCK" SDL_VIDEODRIVER=wayland \
            SDL_VIDEO_WAYLAND_ALLOW_LIBDECOR=0 WAYLAND_DEBUG=client \
            timeout -k 5 20 "$bin" >"$log" 2>&1
        rc=$?
        stop_weston
        if [ $rc -eq 0 ] && grep -q "$sentinel" "$log"; then return 0; fi
        cp "$log" "$log.attempt$attempt"
    done
    return 1
}

# assert the WAYLAND_DEBUG log shows an anchored popup and no stray extra
# toplevel (the main window is the one allowed toplevel).
assert_anchored_popup() {
    local log="$1"
    local popups toplevels
    popups=$(grep -c 'get_popup' "$log")
    toplevels=$(grep -c 'get_toplevel' "$log")
    [ "$popups" -ge 1 ] && [ "$toplevels" -le 1 ]
}

# 1. font check ---------------------------------------------------------------
check_font() {
    local libxm
    libxm=$(ls "$BUILD"/libXm.so.* 2>/dev/null | head -1)
    if [ -z "$libxm" ]; then
        record SKIP font "libXm not built (run: make motif)"
        return
    fi
    cc $INC -I"$BUILD/motif/lib" -I"$ROOT/build/upstream/motif/lib" \
        "$TDIR/motif-xft-rendition.c" "$libxm" "$BUILD/libMrm.so"* "$BUILD/libXt-compat.so" \
        "$BUILD/libX11-compat.so" -lm -o "$WORK/font" 2>"$WORK/font.build" || {
        record FAIL font "compile failed (see $WORK/font.build)"
        return
    }
    local out
    out=$(SDL_VIDEODRIVER=dummy "$WORK/font" 2>/dev/null)
    if echo "$out" | grep -q XFT_FONT_OK; then
        record PASS font "$(echo "$out" | grep XmStringExtent)"
    else
        record FAIL font "${out:-no output}"
    fi
}

# 2. popup anchor check -------------------------------------------------------
check_popup() {
    require_wayland popup || return
    cc $INC "$TDIR/popup-anchor.c" "$BUILD/libX11-compat.so" \
        -Wl,-rpath,"$BUILD" -o "$WORK/popup" 2>"$WORK/popup.build" || {
        record FAIL popup "compile failed (see $WORK/popup.build)"
        return
    }
    if run_client "$WORK/popup" "$WORK/popup.log" POPUP_ANCHOR_DONE && assert_anchored_popup "$WORK/popup.log"; then
        record PASS popup "$(grep -m1 get_popup "$WORK/popup.log" | sed 's/^.*-> //')"
    else
        record FAIL popup "no anchored popup (popups=$(grep -c get_popup "$WORK/popup.log") toplevels=$(grep -c get_toplevel "$WORK/popup.log"); see $WORK/popup.log)"
    fi
}

# 2b. popup resize check ------------------------------------------------------
# An override-redirect popup resized while mapped must grow the compositor's
# xdg_popup geometry, not just the surface buffer, or a switched-to menu renders
# cropped to its creation size. Assert the WAYLAND_DEBUG log carries the resized
# dimensions after the small creation size.
check_popup_resize() {
    require_wayland resize || return
    cc $INC "$TDIR/popup-resize.c" "$BUILD/libX11-compat.so" \
        -Wl,-rpath,"$BUILD" -o "$WORK/resize" 2>"$WORK/resize.build" || {
        record FAIL resize "compile failed (see $WORK/resize.build)"
        return
    }

    # Three assertions, so a failure points at the right layer: the window is
    # actually an anchored xdg_popup (not a fallback top-level), the compositor
    # grew the popup geometry (positioner re-run), and the X backing the client
    # sees (XGetWindowAttributes) grew too, not just the surface buffer.
    if run_client "$WORK/resize" "$WORK/resize.log" POPUP_RESIZE_DONE \
        && assert_anchored_popup "$WORK/resize.log" \
        && grep -qE 'set_size\(220, 180\)|configure\([0-9-]+, [0-9-]+, 220, 180\)' \
            "$WORK/resize.log" \
        && grep -qF 'POPUP_WA wh=(220x180)' "$WORK/resize.log"; then
        record PASS resize "popup and backing grown to 220x180 (positioner re-run at new size)"
    elif ! assert_anchored_popup "$WORK/resize.log"; then
        record FAIL resize "popup not anchored (regression in popup anchoring, not resize; see $WORK/resize.log)"
    else
        record FAIL resize "popup kept its creation size; menu would render cropped (see $WORK/resize.log)"
    fi
}

# 2c. popup slide/anchor tracking --------------------------------------------
# An anchored popup's recorded absolute origin (ws->x/y, used for pointer-event
# root translation) must equal parent + the compositor's actual popup offset, so
# pointer coordinates match where the popup really is even after a compositor
# slide/flip. Compared against whatever offset the compositor reports, so it
# holds on a non-constraining compositor (weston: no slide) and a constraining
# one (wilco: slide) alike; a stale ws->x/y fails it.
check_popup_slide() {
    require_wayland slide || return
    cc $INC "$TDIR/popup-slide.c" "$BUILD/libX11-compat.so" \
        -Wl,-rpath,"$BUILD" -o "$WORK/slide" 2>"$WORK/slide.build" || {
        record FAIL slide "compile failed (see $WORK/slide.build)"
        return
    }
    if ! run_client "$WORK/slide" "$WORK/slide.log" POPUP_SLIDE_DONE \
        || ! assert_anchored_popup "$WORK/slide.log"; then
        record FAIL slide "popup not anchored (see $WORK/slide.log)"
        return
    fi
    local parent readback cfg popupid px py rx ry cx cy ex ey
    parent=$(sed -n 's/.*PARENT_ORIGIN=(\([0-9-]*\),\([0-9-]*\)).*/\1 \2/p' "$WORK/slide.log" | head -1)
    readback=$(sed -n 's/.*READBACK_ABS_ORIGIN=(\([0-9-]*\),\([0-9-]*\)).*/\1 \2/p' "$WORK/slide.log" | head -1)

    # Match the configure of the specific popup object under test, not the last
    # popup configure in the log, so an unrelated later popup cannot skew it.
    popupid=$(sed -n 's/.*get_popup(new id xdg_popup@\([0-9]*\).*/\1/p' "$WORK/slide.log" | tail -1)
    cfg=$(sed -n "s/.*xdg_popup@$popupid\.configure(\([0-9-]*\), \([0-9-]*\).*/\1 \2/p" "$WORK/slide.log" | tail -1)
    if [ -z "$parent" ] || [ -z "$readback" ] || [ -z "$cfg" ]; then
        record FAIL slide "could not parse popup geometry (see $WORK/slide.log)"
        return
    fi
    read -r px py <<<"$parent"
    read -r rx ry <<<"$readback"
    read -r cx cy <<<"$cfg"
    ex=$((px + cx))
    ey=$((py + cy))
    if [ "$rx" = "$ex" ] && [ "$ry" = "$ey" ]; then
        record PASS slide "ws origin ($rx,$ry) tracks compositor offset ($cx,$cy) from parent ($px,$py)"
    else
        record FAIL slide "ws origin ($rx,$ry) != parent+offset ($ex,$ey); pointer translation offset (see $WORK/slide.log)"
    fi
}

# 3. real Motif pulldown menu -------------------------------------------------
check_menu() {
    require_wayland menu || return
    local libxm
    libxm=$(ls "$BUILD"/libXm.so.* 2>/dev/null | head -1)
    if [ -z "$libxm" ]; then
        record SKIP menu "libXm not built (run: make motif)"
        return
    fi
    cc $INC -I"$BUILD/motif/lib" -I"$ROOT/build/upstream/motif/lib" \
        "$TDIR/motif-pulldown-menu.c" "$libxm" "$BUILD/libMrm.so"* "$BUILD/libXt-compat.so" \
        "$BUILD/libX11-compat.so" -lm -Wl,-rpath,"$BUILD" -o "$WORK/menu" 2>"$WORK/menu.build" || {
        record FAIL menu "compile failed (see $WORK/menu.build)"
        return
    }
    if run_client "$WORK/menu" "$WORK/menu.log" MOTIF_PULLDOWN_DONE \
        LIBX11_COMPAT_REPLAY="$TDIR/motif-pulldown-menu.replay" && assert_anchored_popup "$WORK/menu.log"; then
        record PASS menu "$(grep -m1 get_popup "$WORK/menu.log" | sed 's/^.*-> //')"
    else
        record FAIL menu "menu did not post as an anchored popup (popups=$(grep -c get_popup "$WORK/menu.log") toplevels=$(grep -c get_toplevel "$WORK/menu.log"); see $WORK/menu.log)"
    fi
}

# Drop every first-party object the library links so the next `make` recompiles
# them with the flags in effect. The ASan rebuild instruments both our sources
# under build/src and the staged upstream libX11 objects under
# build/upstream/src; wiping only build/src would relink the leftover
# instrumented upstream objects, leaving the "restored" library carrying
# __asan_* references that break later ordinary links. Delete the upstream
# object files but not their staged .c sources (which do not re-stage here).
wipe_first_party_objs() {
    rm -rf "$BUILD/src" "$BUILD/libX11-compat.so"
    find "$BUILD/upstream/src" -name '*.o' -delete 2>/dev/null || true
}

restore_library() {
    wipe_first_party_objs

    # Record a failure if the ordinary rebuild does not come back: otherwise a
    # broken restore is silent and leaves the workspace without libX11-compat.so
    # while the run still reports success.
    (cd "$ROOT" && make SDL_BACKEND=sdl3 -j"$(nproc)" build/libX11-compat.so >/dev/null 2>&1) \
        || record FAIL destroy-restore \
            "normal libX11-compat.so rebuild after the ASan check failed; workspace left without it"
}

# Build a teardown test against the instrumented library and run it under
# weston: require its completion sentinel and no ASan diagnostic.
#
# Returns nonzero on compile failure, a missing sentinel / dirty exit, or any
# ASan hit.
run_asan_teardown() {
    local src="$1" sentinel="$2" tag="$3"
    cc -fsanitize=address -g $INC "$TDIR/$src" "$BUILD/libX11-compat.so" \
        -Wl,-rpath,"$BUILD" -o "$WORK/$tag" 2>"$WORK/$tag.build" || return 1
    run_client "$WORK/$tag" "$WORK/$tag.log" "$sentinel" \
        ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
        && ! grep -qiE "AddressSanitizer|double-free|heap-use-after-free" "$WORK/$tag.log"
}

# 4. teardown ASan check ------------------------------------------------------
# Rebuilds only the first-party objects with the sanitizer so the library's
# teardown path is instrumented (a plain `make CFLAGS_EXTRA=...` would not
# recompile up-to-date objects, leaving the .so unsanitized). Reuses the already
# synced upstream headers, then restores the normal build. Runs last so the
# earlier checks use the ordinary library. Exercises both parent-first destroy
# and parent unmap while a popup is live, the two paths that free a popup's SDL
# surface out from under it.
check_destroy() {
    require_wayland destroy || return
    wipe_first_party_objs
    (cd "$ROOT" && make SDL_BACKEND=sdl3 -j"$(nproc)" \
        CFLAGS_EXTRA="-fsanitize=address -fno-omit-frame-pointer -g" \
        LDFLAGS_EXTRA="-fsanitize=address" build/libX11-compat.so >"$WORK/asan.build" 2>&1) || {
        record FAIL destroy "ASan lib build failed (see $WORK/asan.build)"
        restore_library
        return
    }
    if ! nm -D "$BUILD/libX11-compat.so" 2>/dev/null | grep -q asan; then
        record FAIL destroy "library not instrumented (see $WORK/asan.build)"
    elif ! run_asan_teardown popup-destroy-order.c DESTROY_ORDER_OK destroy; then
        record FAIL destroy "parent-destroy teardown unclean (see $WORK/destroy.log)"
    elif ! run_asan_teardown popup-parent-unmap.c PARENT_UNMAP_OK unmap; then
        record FAIL destroy "parent-unmap teardown unclean (see $WORK/unmap.log)"
    else
        record PASS destroy "clean under ASan: parent destroy + parent unmap (instrumented lib)"
    fi
    # restore the ordinary (non-ASan) library
    restore_library
}

echo "== menu-rendering validation =="
echo "   root=$ROOT  sdl3=$SDL3_PREFIX  weston=${WESTON:-none}  require=$CI_REQUIRE"
check_font
check_popup
check_popup_resize
check_popup_slide
check_menu
check_destroy

echo
echo "== summary =="
for r in "${RESULTS[@]}"; do
    st=${r%% *}
    rest=${r#* }
    name=${rest%% *}
    detail=${rest#* }
    printf "  %-4s  %-8s  %s\n" "$st" "$name" "$detail"
done
echo "  ($pass passed, $fail failed, $skip skipped)"
[ "$KEEP_WORK" -eq 1 ] && echo "   logs kept in $WORK"
[ "$KEEP_WORK" -eq 1 ] || rm -rf "$WORK"

# A required CI gate treats SKIP as failure: a skipped Wayland check means the
# stack was not actually exercised, which must not read as green.
if [ "$CI_REQUIRE" = 1 ] && [ "$skip" -gt 0 ]; then
    echo "  CI_REQUIRE=1: $skip skipped check(s) treated as failure"
    exit 1
fi
[ "$fail" -eq 0 ]
