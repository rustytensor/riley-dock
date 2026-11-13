# The QML model

`riley-dock-qt` keeps all of the dock's logic in C++: the core in `src/core/` and its front end in `src/app/`. QML
only draws. Each dock window gets one `DockModel` with the scene of the last frame, the same data the Rust dock handed
its GTK view (riley `dock/src/view.rs:17-29`, `struct Scene`). Every rectangle, scale, gradient stop, colour and timing
is already worked out there. **QML adds no position, size, offset or duration of its own.** Where the Rust view used a
fixed number, the model has a constant for it.

All coordinates are in logical pixels, local to the dock surface: (0, 0) is its top-left corner and y grows downward.
The surface has a fixed size for a given icon size and slot count. In auto-hide mode the painted dock slides down
inside that surface, and the slide is already part of every rectangle below.

The module is `RileyDock` (`import RileyDock`). Its QML files sit at the module root. The C++ types are registered
with `QML_ELEMENT` and cannot be created from QML.

## Files

| File | Root type | Shown by | Status |
|---|---|---|---|
| `qml/Dock.qml` | `Item` | the dock surface (`QQuickView`, layer TOP, namespace `riley-dock`) | minimal: bar, shelf, icons, launcher, dots, focused line, lip dots |
| `qml/Icon.qml` | `Item`, delegate of `dock.items` | Dock.qml | stub: the icon image at `drawRect`; no hover shadow and no symbolic recolouring yet |
| `qml/Reflection.qml` | `Item`, delegate of `dock.items` | Dock.qml | stub: draws nothing |
| `qml/HoverName.qml` | any `Item` | the name surface (`QQuickView`, layer OVERLAY, namespace `riley-dock-name`) | stub: plain label |
| `qml/Launcher.qml` | `Item`, delegate of `dock.items` | Dock.qml | stub: the nine dots |
| `qml/Menu.qml` | `Window` (`Qt.Popup`) | Dock.qml | stub: window lines and Pin/Unpin; no per-window submenu yet |
| `qml/Minimized.qml` | `Window` (`Qt.Popup`) | Dock.qml | stub: plain rows |

The root of `Dock.qml` and of `HoverName.qml` must declare `required property DockModel dock`. C++ sets it as an
initial property, and each delegate passes it on (`delegate: Icon { dock: root.dock }`). Every delegate declares the
roles it uses as `required property` with the role's name.

Three rules come from the C++ side and must hold:

- **HoverName.qml**: the root's `implicitWidth` × `implicitHeight` is the label's full outer size (text plus padding
  plus border), as GTK measured it (`ui.rs:739`). The C++ code sizes the name surface to it and places it from the core's
  rectangles (`ui.rs:740-746`), so nothing in the file may move the label inside its surface.
- **Images**: `Image` elements that use `image://dockicon/...` stay synchronous (`asynchronous: false`, the
  default). The provider reads the icon theme, and that is safe only on the GUI thread.
- **Menus**: every time a menu closes, exactly one of its ending calls runs (see `DockMenu`). Closing the popup
  window without a choice (a click outside, Escape) calls `dismiss()`.

## DockModel

One per dock window. All scene properties change together and share one signal, `sceneChanged`, which fires at most
once per frame and only when something changed.

### Scene (signal `sceneChanged`)

| Property | Type | Meaning | Source |
|---|---|---|---|
| `surfaceWidth`, `surfaceHeight` | int | The surface size; it is also the window size. | `autohide::dockLayout` |
| `bar` | rect | The bar's glass in this frame. It is moved by the slide and drawn in every pose; while hidden only its top edge is on screen. | `Layout.bar` after `autohide::translate` (view.rs:63) |
| `hidden` | bool | Auto-hide has settled hidden. Draw only `bar` and `lipDots` (view.rs:91-99). | `Controller::settledHidden` |
| `offset` | real | The auto-hide slide in pixels (0 when shown). It is already part of every rectangle; for information only. | `Controller::offset` |
| `lipDots` | list of point | Centres of the running-app dots on the hidden lip. Empty unless `hidden`. Radius `lipDotRadius`, colour `style.accent`. | `autohide::lipDots` |
| `shelfVisible` | bool | Draw the shelf line: reflections on and not hidden. | view.rs:66 |
| `shelf` | rect | The 1 px shelf the reflections stand on, colour `style.shelf`. | `motion::shelf` |
| `indicatorY` | real | The y of the running-dot centres. | ui.rs:505 |
| `activeLineVisible` | bool | Draw the line under the focused app. | view.rs:70-71 |
| `activeLine` | rect | That line's box, 20 × 3 px, centred under the focused icon. Paint `style.activeLineWidth` px at its bottom in `style.activeLine` (the CSS `border-bottom`). | view.rs:72-76 |
| `reflections` | bool | Reflections are on (config `REFLECTIONS`, `--no-reflections`). | `rules::reflectionsEnabled` |
| `swell` | bool | Swell is on (no `--no-swell`). | main.rs:37 |
| `restWidth` | real | The icon size at scale 1. | `LayoutConfig.restWidth` |
| `iconLookupSize` | int | The pixel size to request icon images at (`sourceSize`). It covers the full 1.6× swell. | `rules::iconLookupSize` (ui.rs:316-317) |

### Hover name (signal `hoverChanged`)

| Property | Type | Meaning | Source |
|---|---|---|---|
| `hoverText` | string | The app's name, then one line per window (`<title>  · <workspace>`), joined by `\n`. "Applications" for the launcher. | `rules::hoverText` (ui.rs:735-737) |
| `hoverVisible` | bool | The name surface is shown. | ui.rs:747 |

### Constants (fixed numbers from the Rust view)

| Property | Value | Meaning | Source |
|---|---|---|---|
| `dotRadius` | 2 | Running-dot radius. | view.rs:149 |
| `hollowLineWidth` | 1.2 | Stroke of the hollow dot (every window minimized). | view.rs:150 |
| `lipDotRadius` | 1 | Lip-dot radius. | `autohide::DOT_RADIUS` |
| `shadowColor` | rgba(0, 0, 0, 0.35) | The hover shadow. | view.rs:131-132 |
| `shadowOffsetX`, `shadowOffsetY` | 0, 6 | Shadow offset. | view.rs:132 |
| `shadowRadius` | 10 | Shadow blur radius (GSK `Shadow` radius). | view.rs:132 |

### Objects (constant)

| Property | Type | Meaning |
|---|---|---|
| `items` | DockItems | The dock's slots, in row order. |
| `menu` | DockMenu | This dock's menu. |
| `style` | DockStyle | The colours, fonts and sizes from `style.json`, shared by all docks. |
| `output` | string | The screen's name, for example `DP-2`. |

## DockItems (list model of `dock.items`)

One row per slot, in dock order (launcher, pins, running apps, then placed by `LAUNCHER`). A change of slots resets
the model. Otherwise a frame emits `dataChanged` only for the rows that changed. Property `count` gives the number of
rows.

| Role | Type | Meaning | Source |
|---|---|---|---|
| `itemId` | string | `launcher`, a pin id or a window class. | `DockItem.id` |
| `name` | string | "Applications" for the launcher, else the desktop entry's Name, else `itemId`. | ui.rs:735-736 |
| `iconSource` | string | `image://dockicon/...`, or empty for the launcher. | ui.rs:322-329 |
| `symbolic` | bool | The icon is a `-symbolic` one: paint it in `style.barForeground`. | view.rs:173-174 |
| `launcher` | bool | The app-grid button: drawn by Launcher.qml, never by Icon.qml, and never reflected. | view.rs:103-104 |
| `running` | bool | The item has windows. Minimized windows count. | `autohide::running` |
| `focused` | bool | One of its windows is the active window. | ui.rs:331-332 |
| `minimized` | bool | Every window is minimized (indicator Hollow). | `rules::indicator` |
| `indicator` | int | 0 None, 1 One, 2 Two, 3 Hollow. | `rules::Indicator` |
| `dots` | list of real | The running dots' x offsets from the icon centre: `[]`, `[0]` or `[-3, 3]`. Filled in `style.accent`, or a ring of width `hollowLineWidth` when `minimized`. Centre y is `dock.indicatorY`. | view.rs:142-153 |
| `rect` | rect | The icon rectangle after swell, click squash and the slide, without the launch bounce. Running dots are centred on it. | `grid::layout`, `autohide::translate` |
| `drawRect` | rect | Where the icon is painted: `rect` moved up by `bounce`. | view.rs:134 |
| `bounce` | real | The launch bounce, in pixels upward. | ui.rs:449-456 |
| `scale` | real | `rect.width / restWidth`: the swell times the click squash. Already in `rect`; for information only. | ui.rs:440-448 |
| `hovered` | bool | Under the pointer. | ui.rs:710 |
| `shadow` | bool | Paint the hover shadow (`dock.shadow*`) under the icon. | ui.rs:493 |
| `reflected` | bool | Paint this item's reflection: reflections on, not hidden, not the launcher. | view.rs:101-104 |
| `reflectionImage` | rect | Where the mirrored icon goes (`motion::Reflection.image`). | view.rs:105 |
| `reflectionClip` | rect | The visible part (`motion::Reflection.visible`), which is both the clip and the gradient's extent. | view.rs:106-107 |
| `reflectionFlipY` | real | The mirror line: translate to (`reflectionImage.x`, `reflectionFlipY`), then scale (1, −1), then paint the icon at `reflectionImage.width` × `reflectionImage.height`. | view.rs:118-121 |
| `reflectionStops` | list of point | The alpha mask: each point is (position 0..1, alpha), running from the top of `reflectionClip` (position 0) to its bottom (position 1). Today (0, 0.55) and (1, 0). | `motion::reflection` stops, view.rs:108-116 |
| `gridDots` | list of point | Launcher only: the nine dot centres in the icon's own coordinates (origin at the top-left of `rect`). | `grid::dots` (view.rs:162) |
| `gridDotRadius` | real | Launcher only: the dot radius. | `grid::dots` |
| `gridAccent` | real | Launcher only: 0 gives `style.barForeground`, 1 gives `style.accent`; mix each channel, alpha included, linearly by this value. | view.rs:164-166 |
| `gridSpacing` | real | Launcher only: `grid::Look.spacing`, already applied to `gridDots`. For information only. | `grid::Animation` |

## DockMenu (`dock.menu`)

The right-click menu (riley `dock/src/ui.rs:808-851`) and the left-click window list for an app with several windows
(`ui.rs:792-806`). A dock has at most one open. While any menu is open, every dock stays shown, hover names are
hidden, and window-list updates wait (`ui.rs:179`, `:729`, `:758`).

| Property | Type | Meaning |
|---|---|---|
| `open` | bool | Show the menu. |
| `kind` | string | `"context"` (Menu.qml) or `"windows"` (Minimized.qml). |
| `anchor` | rect (integers) | The icon's rectangle rounded outward, in dock-surface coordinates: the popup points at it (`ui.rs:760-762`, GTK `Popover` at the top). |
| `itemId` | string | The item the menu is for. |
| `windows` | list of map | One entry per window: `line` (`rules::windowLine`: `<title>  · <workspace>`, with "minimized" as the workspace of a minimized window), `address`, `minimized` (bool). |
| `pinned` | bool | Context menu: the item is pinned now. |
| `pinLabel` | string | Context menu: "Unpin" or "Pin". |
| `actions` | list of string | Context menu: the per-window actions in order: "Close", "Float/tile", "Full screen", "Move to workspace 1" … "Move to workspace 9" (`ui.rs:816-818`). |

All properties share one signal, `changed`.

Ending calls. Each one closes the menu first, then acts:

| Call | Menu | Effect |
|---|---|---|
| `pickWindow(index)` | windows | Restore that window when it is minimized, else focus it (`rules::menuPick`). |
| `windowAction(window, action)` | context | Run `actions[action]` on `windows[window]` through the compositor. |
| `togglePin()` | context | Pin or unpin the item (the pins file is re-read first, then saved). |
| `dismiss()` | either | Close without a choice. It does nothing when the menu is not open, so it is safe to call twice. |

A call that does not fit the open menu, or has an index out of range, acts as `dismiss()`.

Each popup is a `Window` with `flags: Qt.Popup` declared inside Dock.qml, so its transient parent is the dock's window.
On Wayland that makes it an `xdg_popup` of the layer surface (layer-shell-qt `attachPopup`), and it may extend beyond
the dock surface as the GTK popover did. When the compositor dismisses it, `visible` turns false and the file calls
`dismiss()`.

## DockStyle (`dock.style`)

The keys of `style.json` (docs/files.md, section 3.2), with their defaults when a key is missing or bad. All of them
share one signal, `changed`, and are re-read when the file changes.

| Property | Type | Key |
|---|---|---|
| `barBackground`, `barForeground`, `barBorder` | color | `bar_background`, `bar_foreground`, `bar_border` |
| `barBorderWidth`, `barRadius` | int | `bar_border_width`, `bar_radius` |
| `accent`, `shelf`, `activeLine` | color | `accent`, `shelf`, `active_line` |
| `activeLineWidth` | int | `active_line_width` |
| `nameBackground`, `nameForeground`, `nameBorder` | color | `name_background`, `name_foreground`, `name_border` |
| `nameBorderWidth`, `nameRadius`, `namePaddingX`, `namePaddingY` | int | `name_border_width`, `name_radius`, `name_padding_x`, `name_padding_y` |
| `nameFontFamily` | string | `name_font_family` |
| `nameFontSize` | int (pixels, `font.pixelSize`) | `name_font_size` |
| `menuPadding` | int | `menu_padding` |

## What stays in C++

Input never goes through QML. The dock window receives the pointer's enter, motion, leave, press and release events
in C++ (`DockWindow::event`) and feeds them to the core: hover, swell, dwell and reveal, clicks, the launcher press,
and menus. The surface's input region, size, exclusive zone, layer and anchors are set in C++ from the core as well.
QML items in Dock.qml need no `MouseArea` or `HoverHandler`, and must not grab the mouse. Only the popup windows take
their own clicks.
