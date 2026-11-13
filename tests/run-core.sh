#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Core gate: build the core, its Qt Test ports of the 29 Rust tests and one hand-worked check (the grid button
# stops the swell), run them, and pass only when exactly those 30 test functions ran and all passed.
#
#   tests/run-core.sh                 build in ${XDG_CACHE_HOME:-$HOME/.cache}/riley-dock/core-build
#   BUILD_DIR=/some/dir tests/run-core.sh
#
# Prints one line per test function, then "GATE PASS 30/30" (exit 0) or "GATE FAIL ..." (exit 1). Build and test
# output goes to logs in the build directory.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source_dir="$(cd "$here/.." && pwd -P)"
build_dir="${BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/riley-dock/core-build}"

# The 29 Rust tests in Rust file order, plus grid_button_stops_the_swell (not a Rust port):
# <test executable>::<test name>.
expected=(
    tst_autohide::a_hidden_lip_is_exactly_six_pixels
    tst_autohide::geometry_dots_and_zones
    tst_autohide::input_regions
    tst_autohide::motion_oracle
    tst_autohide::timer_timeline
    tst_autohide::show_enable_and_config
    tst_autohide::urgency_records
    tst_autohide::control_socket_ownership
    tst_launcher::grid_positions_and_fixed_frame
    tst_launcher::grid_dots_and_animation
    tst_launcher::grid_preferences
    tst_launcher::grid_button_stops_the_swell
    tst_motion::cosine_at_hand_worked_distances
    tst_motion::packed_icons_and_exact_click_through_region
    tst_motion::leave_spring_and_idle_deadline
    tst_motion::click_multiplier_and_return
    tst_parity::pins_replace_file_and_preserve_order
    tst_parity::pinned_then_workspace_and_class_order
    tst_parity::clicks_restore_before_considering_focus
    tst_parity::indicators_and_hover_lines
    tst_parity::icon_configuration_and_integer_shrink
    tst_parity::icon_lookup_covers_swell
    tst_parity::screens_config_and_fallback
    tst_parity::screens_parsing
    tst_parity::desktop_matching_isolated
    tst_reflections::reflection_maths_rectangles_and_gradients
    tst_reflections::reflection_maths_lane_and_input
    tst_reflections::reflection_preferences
    tst_reflections::startup_keeps_pending_fallback_at_expiry
    tst_ipc::lua_actions
)
executables=(tst_autohide tst_launcher tst_motion tst_parity tst_reflections tst_ipc)
total=${#expected[@]}

mkdir -p "$build_dir"
build_dir="$(cd "$build_dir" && pwd -P)"
case "$build_dir/" in
    "$source_dir"/*)
        echo "GATE FAIL build directory $build_dir is inside the source tree $source_dir"
        exit 1
        ;;
esac
log_dir="$build_dir/gate-logs"
rm -rf "$log_dir"
mkdir -p "$log_dir"
build_log="$log_dir/build.log"

jobs="$(nproc 2>/dev/null || echo 1)"
(( jobs > 8 )) && jobs=8
generator=()
if [[ ! -f "$build_dir/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then
    generator=(-G Ninja)
fi

if ! { cmake -S "$source_dir" -B "$build_dir" "${generator[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        && cmake --build "$build_dir" --parallel "$jobs"; } >"$build_log" 2>&1; then
    echo "GATE FAIL build $build_log"
    exit 1
fi

# Tests get a private runtime directory and no compositor instance: nothing can reach a real socket.
runtime_dir="$build_dir/gate-runtime"
rm -rf "$runtime_dir"
mkdir -p "$runtime_dir"
chmod 700 "$runtime_dir"

declare -A result=()  # <exe>::<function> -> PASS | FAIL
declare -A failed_exe=()
for exe in "${executables[@]}"; do
    binary="$build_dir/$exe"
    log="$log_dir/$exe.txt"
    status=0
    if [[ -x "$binary" ]]; then
        (cd "$runtime_dir" && env -u HYPRLAND_INSTANCE_SIGNATURE XDG_RUNTIME_DIR="$runtime_dir" \
            timeout --kill-after=10 300 "$binary" -o "$log,txt") >"$log_dir/$exe.stdout" 2>&1 || status=$?
    else
        status=127
    fi
    (( status != 0 )) && failed_exe[$exe]=$status
    [[ -f "$log" ]] || continue
    while read -r verdict class function; do
        [[ "$class" == "$exe" ]] || { result["$exe::?$class::$function"]=FAIL; continue; }
        case "$function" in initTestCase|cleanupTestCase|init|cleanup) continue ;; esac
        key="$exe::$function"
        if [[ "$verdict" == PASS ]]; then
            [[ "${result[$key]:-}" == FAIL ]] || result[$key]=PASS
        else
            result[$key]=FAIL
        fi
    done < <(sed -nE 's/^(PASS|FAIL!|XPASS|SKIP|BPASS|BFAIL|BXPASS|BXFAIL) *: ([A-Za-z0-9_]+)::([A-Za-z0-9_]+)\(.*/\1 \2 \3/p' "$log")
done

passed=0
declare -A wanted=()
for key in "${expected[@]}"; do
    wanted[$key]=1
    if [[ "${result[$key]:-}" == PASS ]]; then
        echo "PASS $key"
        passed=$((passed + 1))
    elif [[ -n "${result[$key]:-}" ]]; then
        echo "FAIL $key"
    else
        echo "FAIL $key (did not run)"
    fi
done
ran=0
extra=()
for key in "${!result[@]}"; do
    if [[ -n "${wanted[$key]:-}" ]]; then
        ran=$((ran + 1))
    else
        extra+=("$key")
        echo "FAIL $key (not one of the $total expected tests)"
    fi
done

reasons=()
(( ran == total )) || reasons+=("ran $ran of $total")
(( ${#extra[@]} == 0 )) || reasons+=("${#extra[@]} unexpected test functions")
for exe in "${executables[@]}"; do
    [[ -z "${failed_exe[$exe]:-}" ]] || reasons+=("$exe exit ${failed_exe[$exe]}")
done
if (( passed == total && ran == total && ${#extra[@]} == 0 && ${#failed_exe[@]} == 0 )); then
    echo "GATE PASS $passed/$total"
    exit 0
fi
echo "GATE FAIL $passed/$total ${reasons[*]:-} (logs: $log_dir)"
exit 1
