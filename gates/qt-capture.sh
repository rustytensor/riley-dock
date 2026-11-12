#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Milestone 2 gate: the dock window on DP-2. Runs the Rust dock's capture gates (screens, reflections, launcher,
# auto-hide) against the Qt dock, one test dock at a time with an isolated config and a private control socket, and
# compares the layer surface, DP-2's reserved area and the auto-hide reservation with fixed expected values.
#
#   gates/qt-capture.sh               QT_DOCK defaults to $BUILD_DIR/riley-dock-qt,
#                                     BUILD_DIR to ${XDG_CACHE_HOME:-$HOME/.cache}/riley-dock/build
#   RUST_DOCK=/path/to/riley-dock gates/qt-capture.sh
#                                     also runs the Rust dock first, as a reference only (REF lines, side-by-side PNGs)
#   CAPTURE_DIR=/some/dir             where the screenshots go (default $BUILD_DIR/captures/<time>)
#   gates/qt-capture.sh --hover       adds the swell-under-hover captures; these MOVE THE POINTER with hyprctl
#                                     dispatch (synthetic events) and are never part of the default gate
#
# The gate does not build the dock; it measures the binary it is given. It needs a running Hyprland session with a
# DP-2 output that has no dock on it. Prints one line per check, then "GATE PASS n/n" (exit 0) or "GATE FAIL ..."
# (exit 1).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
build_dir="${BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/riley-dock/build}"
qt_dock="${QT_DOCK:-$build_dir/riley-dock-qt}"
capture_dir="${CAPTURE_DIR:-$build_dir/captures/$(date +%Y%m%d-%H%M%S)}"

args=(--qt-dock "$qt_dock" --capture-dir "$capture_dir")
if [[ -n "${RUST_DOCK:-}" ]]; then
    args+=(--rust-dock "$RUST_DOCK")
fi
for arg in "$@"; do
    case "$arg" in
        --hover) args+=(--hover) ;;
        *) echo "GATE FAIL unknown argument: $arg"; exit 2 ;;
    esac
done

for tool in python3 hyprctl grim; do
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
