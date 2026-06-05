#!/bin/sh
set -eu

build_dir=${1:?usage: validate-motif-demos.sh BUILD_DIR OUT_DIR}
out_dir=${2:?usage: validate-motif-demos.sh BUILD_DIR OUT_DIR}
timeout_bin=${TIMEOUT_BIN:-}
run_seconds=${MOTIF_DEMO_SECONDS:-2}
log_dir=${MOTIF_DEMO_LOG_DIR:-"$out_dir/motif-demo-logs"}
# Space-separated list of demo paths (relative to demos/) to skip while
# their crashes are being triaged. Each entry is an exact match against
# the path printed in RUN lines, e.g. "programs/Tree/tree".
demo_skip=${MOTIF_DEMO_SKIP:-}

is_skipped() {
    candidate=$1
    for entry in $demo_skip; do
        if [ "$entry" = "$candidate" ]; then
            return 0
        fi
    done
    return 1
}

if [ -z "$timeout_bin" ]; then
    if command -v timeout >/dev/null 2>&1; then
        timeout_bin=timeout
    elif command -v gtimeout >/dev/null 2>&1; then
        timeout_bin=gtimeout
    else
        echo "No timeout command found; install coreutils or set TIMEOUT_BIN" >&2
        exit 1
    fi
fi

rm -rf "$log_dir"
mkdir -p "$log_dir"
log_dir=$(cd "$log_dir" && pwd)

abs_build_dir=$(cd "$build_dir" && pwd)
abs_out_dir=$(cd "$out_dir" && pwd)
tmp_list="$log_dir/.executables"
motif_src_dir=
if [ -n "${MOTIF_DEMO_SOURCE_DIR:-}" ]; then
    if [ ! -d "$MOTIF_DEMO_SOURCE_DIR/demos" ]; then
        echo "MOTIF_DEMO_SOURCE_DIR does not contain demos: $MOTIF_DEMO_SOURCE_DIR" >&2
        exit 1
    fi
    motif_src_dir=$(cd "$MOTIF_DEMO_SOURCE_DIR" && pwd)
else
    for candidate in "$abs_build_dir/../upstream/motif-src" "$abs_build_dir/../motif-src"; do
        if [ -d "$candidate/demos" ]; then
            motif_src_dir=$(cd "$candidate" && pwd)
            break
        fi
    done
fi

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

find "$abs_build_dir/demos" -type f | while IFS= read -r file; do
    case "$file" in
        */.libs/* | *.o | *.lo | *.la | *.a | *.dylib | *.so | *.uid | *.uil | *.bm | *.xpm | *.ad | *.dat | *.txt | *.html | *.c | *.h | *.in | *.am | *.Po | *.Plo)
            continue
            ;;
    esac
    if [ -x "$file" ]; then
        printf '%s\n' "$file"
    fi
done | sort >"$tmp_list"

if [ ! -s "$tmp_list" ]; then
    echo "No Motif demo executables found under $abs_build_dir/demos" >&2
    exit 1
fi

count=0
failed=0
skipped=0

while IFS= read -r exe; do
    rel=${exe#"$abs_build_dir/demos/"}
    if is_skipped "$rel"; then
        printf 'SKIP %s\n' "$rel"
        skipped=$((skipped + 1))
        continue
    fi
    name=$(printf '%s' "$rel" | tr '/ ' '__')
    log="$log_dir/$name.log"
    work_dir=$(dirname "$exe")
    run_exe="$exe"
    real_exe="$work_dir/.libs/$(basename "$exe")"
    if [ -x "$real_exe" ]; then
        run_exe="$real_exe"
    fi
    lib_path="$abs_build_dir/lib/Xm/.libs:$abs_build_dir/lib/Mrm/.libs:$abs_build_dir/clients/uil/.libs:$abs_out_dir"
    input_file=
    home_dir=
    app_res_dir=$(motif_app_resource_dir "$rel")
    xappresdir="$app_res_dir"
    xfile_search_path=$(motif_xfile_search_path "$app_res_dir")
    set -- "$run_exe"
    count=$((count + 1))
    printf 'RUN %s\n' "$rel"

    case "$rel" in
        doc/programGuide/ch17/simple_drop/simple_drop)
            set -- "$exe" \
                "$abs_build_dir/../upstream/motif-src/demos/programs/IconB/small.bm"
            ;;
        unsupported/uilsymdump/uilsymdump)
            input_dir="$log_dir/uilsymdump-input"
            mkdir -p "$input_dir"
            cp "$abs_build_dir/../upstream/motif-src/demos/programs/hellomotif/hellomotif.uil" \
                "$input_dir/hellomotif.uil"
            input_file="$input_dir/uilsymdump.stdin"
            printf '%s/hellomotif\n' "$input_dir" >"$input_file"
            ;;
        programs/workspace/wsm)
            home_dir="$log_dir/wsm-home"
            mkdir -p "$home_dir"
            cat >"$home_dir/.wsmdb" <<'EOF'
wsm_WSM.WSM.0.linked:True
wsm_WSM.WSM.0.allWorkspaces:True
wsm_WSM.WSM.0.linkedRoom.hidden:0
saveAsShell_WSM*allWorkspaces:True
configureShell_WSM*allWorkspaces:True
nameShell_WSM*allWorkspaces: True
backgroundShell_WSM*allWorkspaces:True
deleteShell_WSM*allWorkspaces:True
saveAsShell_WSM*allWorksapces:True
occupyShell_WSM*allWorkspaces:True
EOF
            ;;
        programs/hellomotifi18n/helloint)
            xappresdir=.
            ;;
    esac

    set +e
    (
        cd "$work_dir"
        if [ -n "$input_file" ]; then
            DYLD_LIBRARY_PATH="$lib_path${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
                LD_LIBRARY_PATH="$lib_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}" \
                XFILESEARCHPATH="$xfile_search_path${XFILESEARCHPATH:+:$XFILESEARCHPATH}" \
                XAPPLRESDIR="$xappresdir" \
                HOME="${home_dir:-$HOME}" \
                "$timeout_bin" --kill-after=1s "$run_seconds" "$@" <"$input_file"
        else
            DYLD_LIBRARY_PATH="$lib_path${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
                LD_LIBRARY_PATH="$lib_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}" \
                XFILESEARCHPATH="$xfile_search_path${XFILESEARCHPATH:+:$XFILESEARCHPATH}" \
                XAPPLRESDIR="$xappresdir" \
                HOME="${home_dir:-$HOME}" \
                "$timeout_bin" --kill-after=1s "$run_seconds" "$@"
        fi
    ) >"$log" 2>&1
    status=$?
    set -e

    if [ "$status" -ne 0 ] && [ "$status" -ne 124 ] && [ "$status" -ne 137 ]; then
        echo "FAIL $rel exited with status $status; see $log" >&2
        failed=$((failed + 1))
        continue
    fi

    if grep -Eiq '(^|[^A-Za-z])(abort|segmentation fault|bus error|trace/bpt trap|xt error|x error|cannot open|can.t open|failed|fatal)' "$log"; then
        echo "FAIL $rel emitted fatal diagnostics; see $log" >&2
        failed=$((failed + 1))
        continue
    fi

    printf 'OK  %s\n' "$rel"
done <"$tmp_list"

if [ "$skipped" -ne 0 ]; then
    echo "$skipped Motif demos skipped via MOTIF_DEMO_SKIP" >&2
fi

if [ "$failed" -ne 0 ]; then
    echo "$failed of $count Motif demos failed validation" >&2
    exit 1
fi

echo "$count Motif demos validated"
