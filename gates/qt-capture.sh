#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Capture gate: the dock window on DP-2. Runs the Rust dock's capture gates (screens, reflections, launcher,
# auto-hide, each with its hover) against the Qt dock, one test dock at a time with an isolated config and a private
# control socket, and compares the layer surface, DP-2's reserved area and the auto-hide reservation with fixed
# expected values. The surface sizes are compared with Hyprswell (the Rust dock's successor) measured in the same run.
#
#   HYPRSWELL=/path/to/hyprswell gates/qt-capture.sh
#                                     QT_DOCK defaults to $BUILD_DIR/riley-dock-qt, POINTER_TOOL to
#                                     $BUILD_DIR/riley-dock-pointer, BUILD_DIR to
#                                     ${XDG_CACHE_HOME:-$HOME/.cache}/riley-dock/build. Hyprswell runs each
#                                     scenario first (REF lines, side-by-side PNGs); without HYPRSWELL the surface
#                                     size checks fail.
#   CAPTURE_DIR=/some/dir             where the screenshots go (default $BUILD_DIR/captures/<time>)
#   gates/qt-capture.sh --no-hover    leaves the pointer alone (no swell or held-open captures)
#
# By default the gate MOVES THE CURSOR on DP-2 for a few seconds per hover scenario, with riley-dock-pointer (the wlr
# virtual pointer protocol, motion only), after one notification. A red ring shows the test's pointer; the cursor is
# put back where it was afterwards, and the hover part stops at once if someone else moves the pointer.
#
# The gate does not build the dock; it measures the binaries it is given. It needs a running Hyprland session with a
# DP-2 output that has no dock on it. Prints one line per check, then "GATE PASS n/n" (exit 0) or "GATE FAIL ..."
# (exit 1).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
build_dir="${BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/riley-dock/build}"
qt_dock="${QT_DOCK:-$build_dir/riley-dock-qt}"
pointer_tool="${POINTER_TOOL:-$build_dir/riley-dock-pointer}"
capture_dir="${CAPTURE_DIR:-$build_dir/captures/$(date +%Y%m%d-%H%M%S)}"

args=(--qt-dock "$qt_dock" --capture-dir "$capture_dir")
if [[ -n "${HYPRSWELL:-}" ]]; then
    args+=(--hyprswell "$HYPRSWELL")
fi
hover=1
for arg in "$@"; do
    case "$arg" in
        --no-hover) hover=0; args+=(--no-hover) ;;
        *) echo "GATE FAIL unknown argument: $arg"; exit 2 ;;
    esac
done
tools=(python3 hyprctl grim)
if (( hover )); then
    args+=(--pointer-tool "$pointer_tool")
    tools+=(notify-send)
fi

for tool in "${tools[@]}"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "GATE FAIL $tool is not installed"
        exit 1
    fi
done
if [[ -z "${HYPRLAND_INSTANCE_SIGNATURE:-}" || -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "GATE FAIL no Hyprland session (HYPRLAND_INSTANCE_SIGNATURE and WAYLAND_DISPLAY must be set)"
    exit 1
fi

exec python3 -I "$here/qt_capture.py" "${args[@]}"
