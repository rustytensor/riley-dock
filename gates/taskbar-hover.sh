#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Taskbar and grid-hover gate (gates/taskbar_hover.py): open apps that are not pinned get a running dot in the
# style's unpinned-dot colour and leave when their last window closes; the app-grid button's dots spread on hover,
# smoothly, inside the button. Runs in a nested Hyprland on DP-2 (workspace 97, opened silently) with its own two
# test windows (kitty), cursor and screenshots; the session's windows, cursor and dock are never touched.
#
#   gates/taskbar-hover.sh            QT_DOCK defaults to $BUILD_DIR/riley-dock-qt, BUILD_DIR to
#                                     ${XDG_CACHE_HOME:-$HOME/.cache}/riley-dock/build, HYPRLAND to /usr/bin/Hyprland
#
# Holds the DP-2 lock (dp2-lock) for the whole run. Prints one line per check, then "GATE PASS n/n" or "GATE FAIL".
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
build_dir="${BUILD_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/riley-dock/build}"
qt_dock="${QT_DOCK:-$build_dir/riley-dock-qt}"
work="${WORK_DIR:-$build_dir/taskbar-gate/$(date +%Y%m%d-%H%M%S)}"
for tool in python3 hyprctl grim kitty dp2-lock; do
    command -v "$tool" >/dev/null 2>&1 || { echo "GATE FAIL $tool is not installed"; exit 1; }
done
[[ -n "${HYPRLAND_INSTANCE_SIGNATURE:-}" ]] || { echo "GATE FAIL no Hyprland session"; exit 1; }
exec dp2-lock python3 -I "$here/taskbar_hover.py" --qt-dock "$qt_dock" --hyprland "${HYPRLAND:-/usr/bin/Hyprland}" \
    --work "$work"
