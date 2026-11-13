#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The switch gate. The installed Qt dock (re)starts on the desktop's screens in place of the dock running there, with
# the Go dock (nwg-dock-hyprland) as the one-command fallback. The Rust dock left Riley on 2026-11-12; its successor,
# Hyprswell, is only the capture gate's reference.
#
#   LAUNCHER=/path/to/hypr-dock INSTALLED=/path/to/bin/riley-dock-qt BUILD_DIR=/path/to/build \
#   HYPRSWELL=/path/to/hyprswell gates/switch.sh
#
#   LAUNCHER     the desktop's dock launcher: "LAUNCHER restart" starts the Qt dock, "LAUNCHER --old" the Go dock,
#                "LAUNCHER --old --dry-run" prints "start: setsid -f <binary> ..." and starts and stops nothing
#   INSTALLED    the installed riley-dock-qt
#   BUILD_DIR    the build it was installed from (BUILD_DIR/riley-dock-qt, BUILD_DIR/riley-dock-pointer); the core gate
#                builds afresh in BUILD_DIR/switch-core, logs go to BUILD_DIR/switch-gate
#   HYPRSWELL    the Hyprswell binary for the capture gate's reference runs
#   SCREENS      the screens the desktop's dock is on (default "DP-1 DP-3")
#   CAPTURE_DIR  where the capture gate puts its screenshots (default: its own)
#
# Checks, in order:
#   a. INSTALLED is an executable file with the same sha256 as BUILD_DIR/riley-dock-qt.
#   b. tests/run-core.sh passes (GATE PASS 30/30) in a fresh build directory.
#   c. gates/qt-capture.sh passes against INSTALLED (on DP-2; it moves the cursor there, see that script).
#   d. Before the switch, read only: each of SCREENS has one riley-dock layer (the Qt dock, possibly an older
#      build); its position and size, the screen's reserved area, the owning process and the open windows are
#      recorded.
#   e. The switch, "LAUNCHER restart". Within 5 s each of SCREENS has exactly one riley-dock layer owned by INSTALLED,
#      with the reserved area and the surface size recorded in d; the open windows are the same as in d; the dock
#      still runs 5 s later; the dock log got no WARNING, CRITICAL or panicked line; no dock is on any other screen
#      and no Rust, Hyprswell or Go dock process is left.
#   f. The fallback: "LAUNCHER --old --dry-run" names a binary that exists, is not INSTALLED, is the Go dock
#      (nwg-dock-hyprland), and answers --help with exit 0; the dry run stopped and started no dock.
#   g. On any failure in e, or when the gate is stopped after e began, a trap runs "LAUNCHER --old", waits until each
#      of SCREENS has one Go dock layer (nwg-dock) and prints "RESTORED: Go dock back"; the gate then fails.
#
# The steps before e must all pass, or the switch is not made. Hyprland is only queried (layers, monitors, clients);
# nothing is dispatched. Do not use the dock while e runs: a swell under the pointer changes the surface size.
# Prints "PASS ..." or "FAIL ..." per check, then "GATE PASS n/n" (exit 0) or "GATE FAIL k/n ..." (exit 1).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
source_dir="$(cd "$here/.." && pwd -P)"

# ---------------------------------------------------------------------------------------------------------------
# Fixed values and their sources. Every other expected value is measured in step d (the dock before the switch)
# or is the sha256 of the build.
# ---------------------------------------------------------------------------------------------------------------
CORE_PASS='GATE PASS 30/30'   # tests/run-core.sh: the 29 Rust tests (riley dock/tests/*.rs, src/ipc.rs) and the grid-swell check
NAMESPACE='riley-dock'        # the dock's layer namespace (riley dock/src/ui.rs; gates/qt_capture.py NAMESPACE)
GO_NAMESPACE='nwg-dock'       # the Go dock's layer namespace (the desktop's blur rule)
GO_DOCK='nwg-dock-hyprland'   # the fallback's binary name (riley ea5f243 bin/hypr-dock --old; Marc, 2026-11-12)
DOCK_NAMESPACES='riley-dock nwg-dock'   # every dock's layer namespace (the desktop's blur rule names both)
APPEAR_MS=5000                # the switch must be complete within 5 s (milestone 3 plan)
ALIVE_MS=5000                 # and the dock must still run 5 s after that (milestone 3 plan)
RESTORE_MS=15000              # time the fallback gets to put its layers back (3 s stop + start + screen choice)
DIAGNOSTICS='WARNING|CRITICAL|panicked'   # riley dock/gates/reflections_capture.py:125; docs/files.md section 4
OLD_DOCKS='riley-dock hyprswell nwg-dock-hyprla'   # process names of the old Rust dock, Hyprswell and the Go dock

passed=0
total=0
failed=()
pass() { passed=$((passed + 1)); total=$((total + 1)); echo "PASS $1: $2"; }
fail() { total=$((total + 1)); failed+=("$1"); echo "FAIL $1: $2"; }
fail_n() { total=$((total + $1)); failed+=("$2"); echo "FAIL $2 ($1 checks): $3"; }
check() { if [[ "$1" == 1 ]]; then pass "$2" "$3"; else fail "$2" "$4"; fi; }
now_ms() { date +%s%3N; }

finish() {
    if (( passed == total )); then
        echo "GATE PASS $passed/$total"
        exit 0
    fi
    echo "GATE FAIL $passed/$total ${failed[*]}${1:+ ($1)}"
    exit 1
}

missing=()
for name in LAUNCHER INSTALLED BUILD_DIR HYPRSWELL; do
    [[ -n "${!name:-}" ]] || missing+=("$name")
done
if (( ${#missing[@]} )); then
    echo "GATE FAIL not set: ${missing[*]}"
    exit 2
fi
for tool in hyprctl python3 sha256sum readlink pgrep timeout; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "GATE FAIL $tool is not installed"
        exit 2
    fi
done
if [[ -z "${HYPRLAND_INSTANCE_SIGNATURE:-}" || -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "GATE FAIL no Hyprland session (HYPRLAND_INSTANCE_SIGNATURE and WAYLAND_DISPLAY must be set)"
    exit 2
fi

launcher="$LAUNCHER"
installed="$INSTALLED"
build_dir="$(cd "$BUILD_DIR" && pwd -P)"
hyprswell="$HYPRSWELL"
read -r -a screens <<< "${SCREENS:-DP-1 DP-3}"
dock_log="${XDG_STATE_HOME:-$HOME/.local/state}/riley/dock.log"   # the launcher's log (riley dock/src/main.rs:16-22)
work="$build_dir/switch-gate"
rm -rf -- "$work"
mkdir -p "$work"
installed_real="$(readlink -f -- "$installed" || true)"

# The launcher runs from the home folder, as the desktop starts it, and without this gate's variables, so the dock it
# starts keeps nothing of the gate.
run_launcher() {
    (cd "$HOME" && env -u LAUNCHER -u INSTALLED -u BUILD_DIR -u HYPRSWELL -u SCREENS -u CAPTURE_DIR \
        "$launcher" "$@" </dev/null)
}

# --- Hyprland, read only ----------------------------------------------------------------------------------------
hq() {
    case "$1" in
        layers|monitors|clients) hyprctl "$1" -j ;;
        *) echo "hyprctl $1 is not a read-only query this gate may make" >&2; return 1 ;;
    esac
}

snapshot_py=$(cat <<'PY'
import json, sys
layers, monitors = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
screens, docks = sys.argv[3].split(), set(sys.argv[4].split())
for s in screens:
    rows = [r for level in layers.get(s, {}).get('levels', {}).values() for r in level if r.get('namespace') == sys.argv[5]]
    if rows:
        r = rows[0]
        print('layer', s, len(rows), r['x'], r['y'], r['w'], r['h'], r['pid'])
    else:
        print('layer', s, 0, '-', '-', '-', '-', '-')
    m = next((m for m in monitors if m['name'] == s), None)
    print('reserved', s, ','.join(map(str, m['reserved'])) if m else 'missing')
for s, entry in sorted(layers.items()):
    if s not in screens:
        for level in entry.get('levels', {}).values():
            for r in level:
                if r.get('namespace') in docks:
                    print('other', s, r['namespace'], r.get('pid'))
PY
)

clients_py=$(cat <<'PY'
import json, sys
for c in sorted((c.get('address'), c.get('class'), (c.get('workspace') or {}).get('id')) for c in json.load(sys.stdin)):
    print(*c)
PY
)

declare -A L_COUNT=() L_POS=() L_SIZE=() L_PID=() RES=()
OTHER=()
snapshot() {   # one read of layers and monitors into L_COUNT, L_POS, L_SIZE, L_PID, RES and OTHER; $1: the namespace
    L_COUNT=() L_POS=() L_SIZE=() L_PID=() RES=() OTHER=()
    hq layers > "$work/layers.json" || return 1
    hq monitors > "$work/monitors.json" || return 1
    local kind s a b c d e f
    while read -r kind s a b c d e f; do
        case "$kind" in
            layer) L_COUNT[$s]=$a; L_POS[$s]="$b,$c"; L_SIZE[$s]="${d}x${e}"; L_PID[$s]=$f ;;
            reserved) RES[$s]=$a ;;
            other) OTHER+=("$s:$a:pid $b") ;;
        esac
    done < <(python3 -I -c "$snapshot_py" "$work/layers.json" "$work/monitors.json" "${screens[*]}" \
                 "$DOCK_NAMESPACES" "${1:-$NAMESPACE}")
}
exe_of() { readlink -f -- "/proc/$1/exe" 2>/dev/null || true; }
clients() { hq clients | python3 -I -c "$clients_py"; }
dock_pids() { local n; for n in riley-dock-qt $OLD_DOCKS; do pgrep -x "$n" || true; done | sort -n | tr '\n' ' '; }

# --- a. the installed binary ------------------------------------------------------------------------------------
# Source: the build's own binary; install copies it unchanged (no RPATH to rewrite).
built="$build_dir/riley-dock-qt"
ok_a=0
if [[ -f "$installed" && -x "$installed" ]]; then
    pass a1 "installed binary = $installed"
    want_sum="$(sha256sum 2>/dev/null < "$built" | cut -d' ' -f1 || true)"
    have_sum="$(sha256sum < "$installed" | cut -d' ' -f1)"
    if [[ -n "$want_sum" && "$have_sum" == "$want_sum" ]]; then
        pass a2 "installed sha256 = $have_sum (= $built)"
        ok_a=1
    else
        fail a2 "installed sha256 = $have_sum, expected ${want_sum:-unreadable} (sha256 of $built)"
    fi
else
    fail a1 "installed binary $installed is not an executable file"
    fail a2 "installed sha256 not compared (no installed binary)"
fi

# --- b. the core gate, in a fresh build directory -----------------------------------------------------------------
core_dir="$build_dir/switch-core"
rm -rf -- "$core_dir"
core_status=0
BUILD_DIR="$core_dir" "$source_dir/tests/run-core.sh" > "$work/core.log" 2>&1 || core_status=$?
core_line="$(tail -n 1 "$work/core.log")"
ok_b=0
if (( core_status == 0 )) && [[ "$core_line" == "$CORE_PASS" ]]; then
    pass b "core gate in $core_dir: $core_line"
    ok_b=1
else
    fail b "core gate: exit $core_status, '$core_line', expected '$CORE_PASS' (tests/run-core.sh; log $work/core.log)"
fi

# --- c. the capture gate against the installed build ----------------------------------------------------------------
ok_c=0
if (( ok_a )); then
    capture_status=0
    capture_env=(BUILD_DIR="$build_dir" QT_DOCK="$installed" HYPRSWELL="$hyprswell")
    [[ -n "${CAPTURE_DIR:-}" ]] && capture_env+=(CAPTURE_DIR="$CAPTURE_DIR")
    env "${capture_env[@]}" "$source_dir/gates/qt-capture.sh" > "$work/capture.log" 2>&1 || capture_status=$?
    capture_line="$(grep -E '^GATE (PASS|FAIL)' "$work/capture.log" | tail -n 1 || true)"
    if (( capture_status == 0 )) && [[ "$capture_line" == "GATE PASS "* ]]; then
        pass c "capture gate on the installed build: $capture_line"
        ok_c=1
    else
        fail c "capture gate: exit $capture_status, '${capture_line:-no GATE line}', expected GATE PASS (gates/qt-capture.sh; log $work/capture.log)"
    fi
else
    fail c "capture gate not run: the installed binary failed step a"
fi

# --- d. before the switch: the running dock, measured read only -----------------------------------------------------
# These measurements are the expected values of step e.
declare -A D_POS=() D_SIZE=() D_RES=() D_PID=() D_EXE=()
ok_d=1
snapshot || true
for s in "${screens[@]}"; do
    if [[ "${L_COUNT[$s]:-0}" == 1 && "${RES[$s]:-missing}" != missing ]]; then
        D_POS[$s]=${L_POS[$s]}; D_SIZE[$s]=${L_SIZE[$s]}; D_RES[$s]=${RES[$s]}; D_PID[$s]=${L_PID[$s]}
        D_EXE[$s]="$(exe_of "${L_PID[$s]}")"
        pass "d1:$s" "before: one $NAMESPACE layer at ${D_POS[$s]} size ${D_SIZE[$s]}, reserved ${D_RES[$s]}, pid ${D_PID[$s]}"
        if [[ -n "${D_EXE[$s]}" ]]; then
            pass "d2:$s" "before: the dock is ${D_EXE[$s]}"
        else
            fail "d2:$s" "before: the dock's binary is unknown (pid ${D_PID[$s]})"
            ok_d=0
        fi
    else
        fail "d1:$s" "before: ${L_COUNT[$s]:-0} $NAMESPACE layers, reserved ${RES[$s]:-missing}; expected one layer on a connected screen (the desktop's dock)"
        fail "d2:$s" "before: no dock to identify"
        ok_d=0
    fi
done
clients > "$work/clients-before.txt" || ok_d=0
d_pids="$(printf '%s\n' "${D_PID[@]:-}" | sort -un | grep -c . || true)"

# --- e. the switch, and g. the way back -------------------------------------------------------------------------------
switch_state=none   # none, running (the trap restores), switched, restoring, restored
restore() {
    switch_state=restoring
    echo "restoring: $launcher --old"
    run_launcher --old >/dev/null 2>&1 || true
    local deadline=$(( $(now_ms) + RESTORE_MS )) s back
    while :; do
        snapshot "$GO_NAMESPACE" || true
        back=1
        for s in "${screens[@]}"; do
            [[ "${L_COUNT[$s]:-0}" == 1 && "$(basename -- "$(exe_of "${L_PID[$s]:-0}")")" == "$GO_DOCK" ]] || back=0
        done
        if (( back )); then
            echo "RESTORED: Go dock back"
            switch_state=restored
            return 0
        fi
        if (( $(now_ms) > deadline )); then
            echo "NOT RESTORED: the Go dock's layers are not on ${screens[*]} after $((RESTORE_MS / 1000)) s; run: $launcher --old"
            switch_state=restored
            return 1
        fi
        sleep 0.2
    done
}
on_exit() {
    local status=$?
    if [[ "$switch_state" == running ]]; then
        fail e "the gate stopped during the switch (exit $status)"
        restore || true
        echo "GATE FAIL $passed/$total ${failed[*]} (stopped during the switch)"
        exit 1
    fi
}
trap on_exit EXIT
trap 'exit 130' INT TERM HUP

e_checks=$(( 7 + 4 * ${#screens[@]} ))
if (( ok_a && ok_b && ok_c && ok_d )); then
    switch_state=running
    e_failed=0
    efail() { fail "$@"; e_failed=1; }
    log_offset="$(stat -c %s -- "$dock_log" 2>/dev/null || echo 0)"
    launcher_status=0
    run_launcher restart > "$work/launcher.out" 2>&1 || launcher_status=$?
    if (( launcher_status == 0 )); then
        pass e0 "$launcher restart exit 0"
    else
        efail e0 "$launcher restart exit $launcher_status, expected 0 ($work/launcher.out)"
    fi

    # Wait until every screen shows the installed dock with the recorded sizes, at most APPEAR_MS.
    deadline=$(( $(now_ms) + APPEAR_MS ))
    while :; do
        snapshot || true
        done_all=1
        for s in "${screens[@]}"; do
            [[ "${L_COUNT[$s]:-0}" == 1 && "$(exe_of "${L_PID[$s]:-0}")" == "$installed_real" \
               && "${RES[$s]:-}" == "${D_RES[$s]}" && "${L_SIZE[$s]:-}" == "${D_SIZE[$s]}" ]] || done_all=0
        done
        (( done_all )) && break
        (( $(now_ms) > deadline )) && break
        sleep 0.1
    done
    declare -A E_PID=()
    for s in "${screens[@]}"; do
        E_PID[$s]=${L_PID[$s]:-}
        if [[ "${L_COUNT[$s]:-0}" == 1 ]]; then
            pass "e1:$s" "one $NAMESPACE layer at ${L_POS[$s]}"
        else
            efail "e1:$s" "${L_COUNT[$s]:-0} $NAMESPACE layers after $((APPEAR_MS / 1000)) s, expected exactly 1"
        fi
        exe="$(exe_of "${L_PID[$s]:-0}")"
        if [[ "$exe" == "$installed_real" ]]; then
            pass "e2:$s" "owned by pid ${L_PID[$s]} = $installed_real"
        else
            efail "e2:$s" "owned by pid ${L_PID[$s]:-none} = '${exe:-unknown}', expected $installed_real (INSTALLED)"
        fi
        if [[ "${RES[$s]:-}" == "${D_RES[$s]}" ]]; then
            pass "e3:$s" "reserved ${RES[$s]} (as the dock before)"
        else
            efail "e3:$s" "reserved ${RES[$s]:-missing}, expected ${D_RES[$s]} (the dock measured in d)"
        fi
        if [[ "${L_SIZE[$s]:-}" == "${D_SIZE[$s]}" ]]; then
            pass "e4:$s" "surface ${L_SIZE[$s]} (as the dock before)"
        else
            efail "e4:$s" "surface ${L_SIZE[$s]:-none}, expected ${D_SIZE[$s]} (the dock measured in d)"
        fi
    done

    # Same pins, same apps: the sizes of d are only an oracle while the windows are the same.
    clients > "$work/clients-after.txt" || true
    if cmp -s "$work/clients-before.txt" "$work/clients-after.txt"; then
        pass e5 "open windows unchanged ($(grep -c . "$work/clients-before.txt" || true))"
    else
        efail e5 "app set changed (open windows differ: $work/clients-before.txt, $work/clients-after.txt)"
    fi
    e_pids="$(printf '%s\n' "${E_PID[@]:-}" | grep -E '^[0-9]+$' | sort -un | grep -c . || true)"
    if [[ "$e_pids" == "$d_pids" ]]; then
        pass e6 "dock processes for ${screens[*]}: $e_pids (as before)"
    else
        efail e6 "dock processes for ${screens[*]}: $e_pids, expected $d_pids (the dock in d)"
    fi

    sleep "$((ALIVE_MS / 1000))"
    alive=1
    for s in "${screens[@]}"; do
        pid=${E_PID[$s]:-}
        [[ -n "$pid" && "$pid" != - ]] && kill -0 "$pid" 2>/dev/null && [[ "$(exe_of "$pid")" == "$installed_real" ]] || alive=0
    done
    if (( alive )); then
        pass e7 "the Qt dock still runs $((ALIVE_MS / 1000)) s later"
    else
        efail e7 "the Qt dock is gone $((ALIVE_MS / 1000)) s later (pids ${E_PID[*]:-none})"
    fi

    size_now="$(stat -c %s -- "$dock_log" 2>/dev/null || echo 0)"
    if (( size_now >= log_offset )); then
        tail -c +"$((log_offset + 1))" -- "$dock_log" > "$work/dock-log-since-switch.txt" 2>/dev/null || true
    else
        cp -- "$dock_log" "$work/dock-log-since-switch.txt"   # the log was replaced: all of it is new
    fi
    bad_lines="$(grep -cE "$DIAGNOSTICS" "$work/dock-log-since-switch.txt" || true)"
    if [[ "$bad_lines" == 0 ]]; then
        pass e8 "dock log: no WARNING, CRITICAL or panicked line since the switch"
    else
        efail e8 "dock log: $bad_lines WARNING, CRITICAL or panicked lines since the switch, expected 0: $(grep -m 1 -E "$DIAGNOSTICS" "$work/dock-log-since-switch.txt" | head -c 300)"
    fi

    snapshot || true
    if (( ${#OTHER[@]} == 0 )); then
        pass e9 "no dock layer on any screen but ${screens[*]}"
    else
        efail e9 "dock layers on other screens: ${OTHER[*]}, expected none"
    fi
    left=""
    for n in $OLD_DOCKS; do
        for pid in $(pgrep -x "$n" || true); do left+="$n:$pid "; done
    done
    if [[ -z "$left" ]]; then
        pass e10 "no Rust, Hyprswell or Go dock process left"
    else
        efail e10 "old dock processes still run: $left, expected none"
    fi

    if (( e_failed )); then
        restore || true
    else
        switch_state=switched
    fi
else
    reason="not run: a step before the switch failed (a=$ok_a b=$ok_b c=$ok_c d=$ok_d)"
    fail_n "$e_checks" e "$reason"
fi

# --- f. the fallback ------------------------------------------------------------------------------------------------
pids_before="$(dock_pids)"
dry_status=0
dry_out="$(run_launcher --old --dry-run 2>&1)" || dry_status=$?
pids_after="$(dock_pids)"
fallback="$(sed -n 's/^start: setsid -f \([^ ]*\).*/\1/p' <<< "$dry_out" | head -n 1)"
if (( dry_status == 0 )) && [[ -n "$fallback" ]]; then
    pass f1 "$launcher --old --dry-run starts $fallback"
else
    fail f1 "$launcher --old --dry-run: exit $dry_status, no 'start: setsid -f <binary>' line: $(head -c 300 <<< "$dry_out")"
fi
if [[ "$fallback" == /* && -f "$fallback" && -x "$fallback" ]]; then
    pass f2 "the fallback binary exists"
else
    fail f2 "the fallback binary '${fallback:-none}' is not an executable file with an absolute path"
fi
fallback_real="$(readlink -f -- "$fallback" 2>/dev/null || true)"
if [[ -n "$fallback_real" && "$fallback_real" != "$installed_real" \
      && "$(sha256sum < "$fallback_real" | cut -d' ' -f1)" != "$(sha256sum 2>/dev/null < "$installed" | cut -d' ' -f1)" ]]; then
    pass f3 "the fallback is not the installed Qt dock"
else
    fail f3 "the fallback '${fallback_real:-none}' is the installed Qt dock or unreadable, expected the Go dock"
fi
help_status=0
if [[ -n "$fallback_real" ]]; then
    # Go's flag package: -help prints the usage and exits 0 before the dock starts.
    (cd "$work" && timeout 10 "$fallback_real" --help </dev/null > "$work/fallback-help.txt" 2>&1) || help_status=$?
else
    help_status=127
fi
check "$(( help_status == 0 ))" f4 "$fallback --help exit 0" "$fallback --help exit $help_status, expected 0 (Go flag package: -help exits 0)"
check "$( [[ -n "$fallback_real" && "$(basename -- "$fallback_real")" == "$GO_DOCK" ]] && echo 1 || echo 0)" f5 \
    "the fallback is the Go dock ($GO_DOCK)" \
    "the fallback '${fallback_real:-none}' is not the Go dock ($GO_DOCK; riley ea5f243 bin/hypr-dock --old)"
check "$( [[ "$pids_before" == "$pids_after" ]] && echo 1 || echo 0)" f6 "the dry run stopped and started no dock (pids $pids_after)" \
    "dock pids changed during the dry run: '$pids_before' -> '$pids_after', expected unchanged (the launcher's --dry-run)"

if [[ "$switch_state" == restored ]]; then
    finish "the Go dock was restored"
fi
finish
