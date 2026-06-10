#!/bin/sh
set -eu

build_dir=${1:?usage: validate-osiris-demos.sh BUILD_DIR SOURCE_DIR OUT_DIR}
source_dir=${2:?usage: validate-osiris-demos.sh BUILD_DIR SOURCE_DIR OUT_DIR}
out_dir=${3:?usage: validate-osiris-demos.sh BUILD_DIR SOURCE_DIR OUT_DIR}

timeout_bin=${TIMEOUT_BIN:-}
run_seconds=${OSIRIS_DEMO_SECONDS:-2}
log_dir=${OSIRIS_DEMO_LOG_DIR:-"$out_dir/osiris-demo-logs"}
demo_skip=${OSIRIS_DEMO_SKIP:-}

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
abs_source_dir=$(cd "$source_dir" && pwd)
abs_out_dir=$(cd "$out_dir" && pwd)
tmp_list="$log_dir/.executables"

find "$abs_build_dir/examples" "$abs_build_dir/tutorial" -type f | while IFS= read -r file; do
    case "$file" in
        *.o | *.a | *.dylib | *.so | *.symbols | */*.p/*)
            continue
            ;;
    esac
    if [ -x "$file" ]; then
        printf '%s\n' "$file"
    fi
done | sort >"$tmp_list"

if [ ! -s "$tmp_list" ]; then
    echo "No Osiris demo executables found under $abs_build_dir" >&2
    exit 1
fi

count=0
failed=0
skipped=0

while IFS= read -r exe; do
    rel=${exe#"$abs_build_dir/"}
    if is_skipped "$rel"; then
        printf 'SKIP %s\n' "$rel"
        skipped=$((skipped + 1))
        continue
    fi

    name=$(printf '%s' "$rel" | tr '/ ' '__')
    log="$log_dir/$name.log"
    src_work_dir="$abs_source_dir/$(dirname "$rel")"
    if [ ! -d "$src_work_dir" ]; then
        src_work_dir=$(dirname "$exe")
    fi

    count=$((count + 1))
    printf 'RUN %s\n' "$rel"

    set +e
    (
        cd "$src_work_dir"
        DYLD_LIBRARY_PATH="$abs_out_dir${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
            LD_LIBRARY_PATH="$abs_out_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            LIBX11_COMPAT_FONT_DIR="$abs_out_dir/../fonts" \
            SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-dummy}" \
            "$timeout_bin" --kill-after=1s "$run_seconds" "$exe"
    ) >"$log" 2>&1
    status=$?
    set -e

    if [ "$status" -ne 0 ] && [ "$status" -ne 124 ] && [ "$status" -ne 137 ]; then
        echo "FAIL $rel exited with status $status; see $log" >&2
        failed=$((failed + 1))
        continue
    fi

    filtered_log="$log_dir/$name.filtered.log"
    grep -Ev 'Fatal IO error: client killed|QDir::readDirEntries: Cannot read the directory:' \
        "$log" >"$filtered_log" || true
    if grep -Eiq '(^|[^A-Za-z])(abort|segmentation fault|bus error|trace/bpt trap|x error|cannot open|can.t open|failed|fatal)' "$filtered_log"; then
        echo "FAIL $rel emitted fatal diagnostics; see $log" >&2
        failed=$((failed + 1))
        continue
    fi

    printf 'OK  %s\n' "$rel"
done <"$tmp_list"

if [ "$skipped" -ne 0 ]; then
    echo "$skipped Osiris demos skipped via OSIRIS_DEMO_SKIP" >&2
fi

if [ "$failed" -ne 0 ]; then
    echo "$failed of $count Osiris demos failed validation" >&2
    exit 1
fi

echo "$count Osiris demos validated"
