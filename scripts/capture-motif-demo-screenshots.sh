#!/bin/sh
set -eu

build_dir=${1:?usage: capture-motif-demo-screenshots.sh BUILD_DIR OUT_DIR}
out_dir=${2:?usage: capture-motif-demo-screenshots.sh BUILD_DIR OUT_DIR}
run_seconds=${MOTIF_DEMO_SCREENSHOT_SECONDS:-3}
screenshot_dir=${MOTIF_DEMO_SCREENSHOT_DIR:-"$out_dir/motif-demo-screenshots"}
log_dir=${MOTIF_DEMO_SCREENSHOT_LOG_DIR:-"$out_dir/motif-demo-screenshot-logs"}
result_file=${MOTIF_DEMO_SCREENSHOT_RESULT_FILE:-"$log_dir/results.tsv"}
demo_filter=${MOTIF_DEMO_SCREENSHOT_FILTER:-}

macos_window_id_for_pid() {
    pid=$1

    if ! command -v swift >/dev/null 2>&1; then
        return 1
    fi

    swift "$window_id_helper" "$pid"
}

capture_screen() {
    pid=$1
    shot=$2

    case "${MOTIF_SCREENSHOT_COMMAND:-auto}" in
        import)
            import -window root "$shot"
            return $?
            ;;
        gnome-screenshot)
            gnome-screenshot -f "$shot"
            return $?
            ;;
        screencapture)
            if window_id=$(macos_window_id_for_pid "$pid"); then
                screencapture -x -o -l"$window_id" "$shot"
                return $?
            fi
            return 1
            ;;
        auto) ;;
        *)
            echo "Unknown MOTIF_SCREENSHOT_COMMAND=$MOTIF_SCREENSHOT_COMMAND" >&2
            return 1
            ;;
    esac

    if command -v screencapture >/dev/null 2>&1; then
        if window_id=$(macos_window_id_for_pid "$pid"); then
            screencapture -x -o -l"$window_id" "$shot"
        else
            echo "No on-screen macOS window found for process $pid" >&2
            return 1
        fi
    elif command -v gnome-screenshot >/dev/null 2>&1; then
        gnome-screenshot -f "$shot"
    elif command -v import >/dev/null 2>&1; then
        import -window root "$shot"
    else
        echo "No screenshot command found; install screencapture, gnome-screenshot, or ImageMagick import" >&2
        return 1
    fi
}

image_has_visible_content() {
    shot=$1

    if command -v magick >/dev/null 2>&1; then
        magick "$shot" -format '%[fx:maxima.r + maxima.g + maxima.b]\n' info: \
            | awk '{ exit ($1 > 0.0) ? 0 : 1 }'
        return
    fi

    if command -v identify >/dev/null 2>&1; then
        identify -format '%[fx:maxima.r + maxima.g + maxima.b]\n' "$shot" \
            | awk '{ exit ($1 > 0.0) ? 0 : 1 }'
        return
    fi

    return 0
}

motif_demo_file() {
    rel=$1

    for base in "$motif_src_dir" "$abs_build_dir"; do
        if [ -n "$base" ] && [ -f "$base/demos/$rel" ]; then
            printf '%s\n' "$base/demos/$rel"
            return 0
        fi
    done
    return 1
}

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

terminate_process() {
    pid=$1

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
}

record_result() {
    status=$1
    rel=$2
    shot=$3
    detail=$4

    printf '%s\t%s\t%s\t%s\n' "$status" "$rel" "$shot" "$detail" >>"$result_file"
}

rm -rf "$screenshot_dir" "$log_dir"
mkdir -p "$screenshot_dir" "$log_dir"
screenshot_dir=$(cd "$screenshot_dir" && pwd)
log_dir=$(cd "$log_dir" && pwd)
# Honor MOTIF_DEMO_SCREENSHOT_RESULT_FILE if the caller set it; otherwise
# default to the now-absolute log_dir. Don't unconditionally clobber.
result_file=${MOTIF_DEMO_SCREENSHOT_RESULT_FILE:-"$log_dir/results.tsv"}
mkdir -p "$(dirname "$result_file")"
printf 'status\trelative_path\tscreenshot\tdetail\n' >"$result_file"

abs_build_dir=$(cd "$build_dir" && pwd)
abs_out_dir=$(cd "$out_dir" && pwd)
tmp_list="$log_dir/.executables"
window_id_helper="$log_dir/window-id-for-pid.swift"
motif_src_dir=
if [ -n "${MOTIF_DEMO_SOURCE_DIR:-}" ]; then
    if [ ! -d "$MOTIF_DEMO_SOURCE_DIR/demos" ]; then
        echo "MOTIF_DEMO_SOURCE_DIR does not contain demos: $MOTIF_DEMO_SOURCE_DIR" >&2
        exit 1
    fi
    motif_src_dir=$(cd "$MOTIF_DEMO_SOURCE_DIR" && pwd)
else
    for candidate in "$abs_build_dir/../upstream/motif" "$abs_build_dir/../motif-src"; do
        if [ -d "$candidate/demos" ]; then
            motif_src_dir=$(cd "$candidate" && pwd)
            break
        fi
    done
fi

cat >"$window_id_helper" <<'EOF'
import CoreGraphics
import Foundation

guard CommandLine.arguments.count == 2,
      let targetPID = Int(CommandLine.arguments[1]) else {
    exit(1)
}

let options = CGWindowListOption(arrayLiteral: .optionAll,
                                 .excludeDesktopElements)
let windows = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as?
    [[String: Any]] ?? []
var bestWindowID: Int?
var bestArea = 0
var bestNamedWindowID: Int?
var bestNamedArea = 0

func intValue(_ value: Any?) -> Int {
    if let intValue = value as? Int {
        return intValue
    }
    if let doubleValue = value as? Double {
        return Int(doubleValue)
    }
    if let stringValue = value as? String, let intValue = Int(stringValue) {
        return intValue
    }
    return 0
}

for window in windows {
    let ownerPID = window[kCGWindowOwnerPID as String] as? Int ?? -1
    let layer = window[kCGWindowLayer as String] as? Int ?? -1
    guard ownerPID == targetPID && layer == 0 else {
        continue
    }
    guard let bounds = window[kCGWindowBounds as String] as? [String: Any] else {
        continue
    }
    let width = intValue(bounds["Width"])
    let height = intValue(bounds["Height"])
    guard width > 0 && height > 0 else {
        continue
    }
    if width > 1000 && height < 100 {
        continue
    }
    let area = width * height
    let name = window[kCGWindowName as String] as? String ?? ""
    if !name.isEmpty && area > bestNamedArea {
        bestNamedArea = area
        bestNamedWindowID = intValue(window[kCGWindowNumber as String])
        continue
    }
    if area > bestArea {
        bestArea = area
        bestWindowID = intValue(window[kCGWindowNumber as String])
    }
}

if let windowID = bestNamedWindowID, bestNamedArea > 0 {
    print(windowID)
    exit(0)
}
if let windowID = bestWindowID, bestArea > 0 {
    print(windowID)
    exit(0)
}
exit(1)
EOF

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
captured=0
failed=0

while IFS= read -r exe; do
    rel=${exe#"$abs_build_dir/demos/"}
    if [ -n "$demo_filter" ] && ! printf '%s\n' "$rel" | grep -Eq "$demo_filter"; then
        continue
    fi
    name=$(printf '%s' "$rel" | tr '/ ' '__')
    log="$log_dir/$name.log"
    shot="$screenshot_dir/$name.png"
    work_dir=$(dirname "$exe")
    run_exe="$exe"
    real_exe="$work_dir/.libs/$(basename "$exe")"
    if [ -x "$real_exe" ]; then
        run_exe="$real_exe"
    fi
    lib_path="$abs_build_dir/lib/Xm/.libs:$abs_build_dir/lib/Mrm/.libs:$abs_build_dir/clients/uil/.libs:$abs_out_dir"
    input_file=
    home_dir=
    extra_lang=
    app_res_dir=$(motif_app_resource_dir "$rel")
    xappresdir="$app_res_dir"
    xfile_search_path=$(motif_xfile_search_path "$app_res_dir")
    set -- "$run_exe"
    count=$((count + 1))
    printf 'SHOT %s\n' "$rel"

    case "$rel" in
        doc/programGuide/ch17/simple_drop/simple_drop)
            if ! bitmap=$(motif_demo_file "programs/IconB/small.bm"); then
                echo "FAIL $rel missing input bitmap programs/IconB/small.bm" >&2
                record_result "missing-input" "$rel" "" "programs/IconB/small.bm"
                failed=$((failed + 1))
                continue
            fi
            set -- "$run_exe" \
                "$bitmap"
            ;;
        unsupported/uilsymdump/uilsymdump)
            if ! uil_file=$(motif_demo_file "programs/hellomotif/hellomotif.uil"); then
                echo "FAIL $rel missing input UIL programs/hellomotif/hellomotif.uil" >&2
                record_result "missing-input" "$rel" "" "programs/hellomotif/hellomotif.uil"
                failed=$((failed + 1))
                continue
            fi
            input_dir="$log_dir/uilsymdump-input"
            mkdir -p "$input_dir"
            cp "$uil_file" "$input_dir/hellomotif.uil"
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
saveAsShell_WSM*allWorkspaces:True
occupyShell_WSM*allWorkspaces:True
EOF
            ;;
        programs/earth/earth)
            set -- "$run_exe" -speed 0
            ;;
        programs/todo/todo)
            if ! todo_file=$(motif_demo_file "programs/todo/example.todo"); then
                echo "FAIL $rel missing input todo file programs/todo/example.todo" >&2
                record_result "missing-input" "$rel" "" "programs/todo/example.todo"
                failed=$((failed + 1))
                continue
            fi
            app_res_dir=$(dirname "$todo_file")
            set -- "$run_exe" \
                -todoFile "$todo_file"
            ;;
        programs/hellomotifi18n/helloint)
            # Mirror scripts/validate-motif-demos.sh: Mrm's XtResolvePathname
            # expands %L from LANG to pick a uid/ locale subdir. The demo ships
            # C/english/french/hebrew/japan/japanese/swedish; a host LANG like
            # en_US.UTF-8 has no match and the lookup falls through to
            # MrmNOT_FOUND. Pin LANG=C so the resolver hits C/uid/l_strings.uid.
            xappresdir=.
            extra_lang=C
            ;;
    esac

    set +e
    if [ -n "$input_file" ]; then
        (
            cd "$work_dir"
            exec env -u SDL_VIDEODRIVER \
                ${extra_lang:+LANG="$extra_lang" LC_ALL="$extra_lang"} \
                DYLD_LIBRARY_PATH="$lib_path${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
                LD_LIBRARY_PATH="$lib_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                XFILESEARCHPATH="$xfile_search_path${XFILESEARCHPATH:+:$XFILESEARCHPATH}" \
                XAPPLRESDIR="$xappresdir" \
                HOME="${home_dir:-$HOME}" \
                "$@" <"$input_file"
        ) >"$log" 2>&1 &
    else
        (
            cd "$work_dir"
            exec env -u SDL_VIDEODRIVER \
                ${extra_lang:+LANG="$extra_lang" LC_ALL="$extra_lang"} \
                DYLD_LIBRARY_PATH="$lib_path${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}" \
                LD_LIBRARY_PATH="$lib_path${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
                XFILESEARCHPATH="$xfile_search_path${XFILESEARCHPATH:+:$XFILESEARCHPATH}" \
                XAPPLRESDIR="$xappresdir" \
                HOME="${home_dir:-$HOME}" \
                "$@"
        ) >"$log" 2>&1 &
    fi
    pid=$!
    sleep "$run_seconds"

    if kill -0 "$pid" >/dev/null 2>&1; then
        capture_screen "$pid" "$shot"
        shot_status=$?
        if [ "$shot_status" -ne 0 ] && kill -0 "$pid" >/dev/null 2>&1; then
            sleep 1
            capture_screen "$pid" "$shot"
            shot_status=$?
        fi
        if [ "$shot_status" -eq 0 ] && [ -s "$shot" ] \
            && ! image_has_visible_content "$shot" \
            && kill -0 "$pid" >/dev/null 2>&1; then
            sleep 2
            capture_screen "$pid" "$shot"
            shot_status=$?
        fi
        if [ "$shot_status" -ne 0 ] \
            && ! kill -0 "$pid" >/dev/null 2>&1; then
            rm -f "$shot"
            shot_status=0
        fi
        terminate_process "$pid"
        wait "$pid" >/dev/null 2>&1
        proc_status=$?
    else
        wait "$pid" >/dev/null 2>&1
        proc_status=$?
        shot_status=0
    fi
    set -e

    if [ "$proc_status" -ne 0 ] && [ "$proc_status" -ne 124 ] && [ "$proc_status" -ne 137 ] && [ "$proc_status" -ne 143 ]; then
        echo "FAIL $rel exited with status $proc_status; see $log" >&2
        record_result "process-failed" "$rel" "" "$proc_status"
        failed=$((failed + 1))
        continue
    fi

    if [ "${shot_status:-0}" -ne 0 ]; then
        echo "FAIL $rel did not produce a usable screenshot; see $log" >&2
        record_result "screenshot-failed" "$rel" "" "$shot_status"
        failed=$((failed + 1))
        continue
    fi

    if grep -Eiq '(^|[^A-Za-z])(abort|segmentation fault|bus error|trace/bpt trap|xt error|x error|cannot open|can.t open|failed|fatal)' "$log"; then
        echo "FAIL $rel emitted fatal diagnostics; see $log" >&2
        record_result "fatal-diagnostic" "$rel" "" "$log"
        failed=$((failed + 1))
        continue
    fi

    if [ -f "$shot" ]; then
        if [ "$shot_status" -ne 0 ] || [ ! -s "$shot" ]; then
            echo "FAIL $rel produced an empty screenshot; see $shot" >&2
            record_result "empty-screenshot" "$rel" "$shot" "$shot_status"
            failed=$((failed + 1))
            continue
        fi
        if ! image_has_visible_content "$shot"; then
            echo "FAIL $rel produced an all-black screenshot; see $shot" >&2
            record_result "all-black" "$rel" "$shot" "$shot"
            failed=$((failed + 1))
            continue
        fi
        captured=$((captured + 1))
        record_result "captured" "$rel" "$shot" ""
        printf 'OK   %s -> %s\n' "$rel" "$shot"
    else
        record_result "exited-before-capture" "$rel" "" ""
        printf 'OK   %s exited before screenshot capture\n' "$rel"
    fi
done <"$tmp_list"

if [ "$failed" -ne 0 ]; then
    echo "$failed of $count Motif demo screenshot checks failed" >&2
    exit 1
fi

echo "$count Motif demos screenshot-checked; $captured screenshots saved in $screenshot_dir"
