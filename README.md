# riley-dock

The dock of the Riley desktop for Hyprland: pinned and running apps on each configured screen, a swell under the
pointer, reflections, an app grid, minimized apps, auto-hide and attention reveal. C++ for the logic, Qt Quick for the
drawing, layer-shell-qt for its place on the screen.

Work in progress: a rewrite of Riley's first dock. Built with CMake against Qt 6.

## Install

Build outside the source tree, then install. Only the dock itself is installed: one file, `bin/riley-dock-qt`. The
QML files and the XML schemas are built into it; the tests and the capture gate's helper are never installed.

```sh
cmake -S . -B ../riley-dock-build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build ../riley-dock-build --parallel 8
cmake --install ../riley-dock-build --prefix ~/.local
```

Needs Qt 6.11 (Core, Gui, Qml, Quick, DBus, Test), layer-shell-qt 6.8, libxml2 2.14 and, for the gate helper,
wayland-client and wayland-scanner (or configure with `-DRILEY_DOCK_GATE_TOOLS=OFF`). Riley's `hypr-dock` starts the
installed `riley-dock-qt`; the files it reads are described in `docs/files.md`.

## Licence

GPL-3.0-or-later. See `LICENSE`.
