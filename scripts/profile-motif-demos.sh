#!/bin/sh
set -eu

build_dir=${1:?usage: profile-motif-demos.sh BUILD_DIR OUT_DIR}
out_dir=${2:?usage: profile-motif-demos.sh BUILD_DIR OUT_DIR}
run_seconds=${MOTIF_DEMO_PROFILE_SECONDS:-5}
profile_dir=${MOTIF_DEMO_PROFILE_DIR:-"$out_dir/motif-demo-profiles"}

abs_build_dir=$(cd "$build_dir" && pwd)
abs_out_dir=$(cd "$out_dir" && pwd)
motif_src_dir=
for candidate in "$abs_build_dir/../upstream/motif" "$abs_build_dir/../motif-src"; do
    if [ -d "$candidate/demos" ]; then
        motif_src_dir=$(cd "$candidate" && pwd)
        break
    fi
done

motif_app_resource_dir() {
    rel=$1
    if [ -n "$motif_src_dir" ] && [ -d "$motif_src_dir/demos/$(dirname "$rel")" ]; then
        printf '%s\n' "$motif_src_dir/demos/$(dirname "$rel")"
        return
    fi
    printf '.\n'
}

motif_xfile_search_path() {
    dir=$1
    printf '%s/%%N.ad:%s/%%N\n' "$dir" "$dir"
}

rm -rf "$profile_dir"
mkdir -p "$profile_dir"
profile_dir=$(cd "$profile_dir" && pwd)

lib_path="$abs_build_dir/lib/Xm/.libs:$abs_build_dir/lib/Mrm/.libs:$abs_build_dir/clients/uil/.libs:$abs_out_dir"
summary="$profile_dir/summary.tsv"

printf 'demo\tstatus\telapsed_seconds\tprofile\n' >"$summary"

run_profile() {
    rel=$1
    shift

    exe="$abs_build_dir/demos/$rel"
    if [ ! -x "$exe" ]; then
        echo "Missing Motif demo executable: $rel" >&2
        return 1
    fi

    work_dir=$(dirname "$exe")
    real_exe="$work_dir/.libs/$(basename "$exe")"
    if [ -x "$real_exe" ]; then
        exe="$real_exe"
    fi

    name=$(printf '%s' "$rel" | tr '/ ' '__')
    log="$profile_dir/$name.log"
    sample_out="$profile_dir/$name.sample.txt"
    app_res_dir=$(motif_app_resource_dir "$rel")
    xappresdir="$app_res_dir"
    xfile_search_path=$(motif_xfile_search_path "$app_res_dir")
    case "$rel" in
        programs/hellomotifi18n/helloint)
            xappresdir=.
            ;;
    esac

    printf 'PROFILE %s\n' "$rel"
    start=$(date +%s)

    (
        cd "$work_dir"
        exec env \
            DYLD_LIBRARY_PATH="$lib_path${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
            LD_LIBRARY_PATH="$lib_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}" \
            XFILESEARCHPATH="$xfile_search_path${XFILESEARCHPATH:+:$XFILESEARCHPATH}" \
            XAPPLRESDIR="$xappresdir" \
            "$exe" "$@"
    ) >"$log" 2>&1 &
    pid=$!

    sleep 1
    if kill -0 "$pid" >/dev/null 2>&1 && command -v sample >/dev/null 2>&1; then
        sample "$pid" "$run_seconds" -file "$sample_out" >/dev/null 2>&1 || true
    else
        sleep "$run_seconds"
    fi

    status=0
    if kill -0 "$pid" >/dev/null 2>&1; then
        if command -v pkill >/dev/null 2>&1; then
            pkill -TERM -P "$pid" >/dev/null 2>&1 || true
        fi
        kill "$pid" >/dev/null 2>&1 || true
        sleep 1
        if kill -0 "$pid" >/dev/null 2>&1; then
            if command -v pkill >/dev/null 2>&1; then
                pkill -KILL -P "$pid" >/dev/null 2>&1 || true
            fi
            kill -KILL "$pid" >/dev/null 2>&1 || true
        fi
        wait "$pid" >/dev/null 2>&1 || status=$?
    else
        wait "$pid" >/dev/null 2>&1 || status=$?
    fi

    elapsed=$(($(date +%s) - start))
    profile_path=
    if [ -s "$sample_out" ]; then
        profile_path="$sample_out"
    fi
    printf '%s\t%s\t%s\t%s\n' "$rel" "$status" "$elapsed" "$profile_path" >>"$summary"

    # 124/137/143 are the expected outcomes when our own kill/timeout
    # logic above terminates the demo; treat anything else non-zero as
    # a real failure rather than reporting OK based only on grep.
    if [ "$status" -ne 0 ] && [ "$status" -ne 124 ] && [ "$status" -ne 137 ] && [ "$status" -ne 143 ]; then
        echo "FAIL $rel exited with status $status; see $log" >&2
        return 1
    fi

    if grep -Eiq '(^|[^A-Za-z])(abort|segmentation fault|bus error|trace/bpt trap|xt error|x error|cannot open|can.t open|failed|fatal)' "$log"; then
        echo "FAIL $rel emitted fatal diagnostics; see $log" >&2
        return 1
    fi

    printf 'OK      %s -> %s\n' "$rel" "${profile_path:-$log}"
}

failed=0

run_profile programs/draw/draw || failed=$((failed + 1))
run_profile programs/earth/earth || failed=$((failed + 1))
run_profile programs/panner/panner || failed=$((failed + 1))
run_profile programs/filemanager/filemanager || failed=$((failed + 1))
run_profile unsupported/xmfonts/xmfonts || failed=$((failed + 1))

if [ "$failed" -ne 0 ]; then
    echo "$failed Motif demo profile runs failed" >&2
    exit 1
fi

echo "Motif demo profile summary written to $summary"
