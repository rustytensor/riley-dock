// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dock surface (riley dock/src/view.rs snapshot, :84-155). It draws the scene of `dock` (docs/qml-model.md);
// every position, size, scale and colour comes from the model, so this file holds no geometry of its own.
//
// Draw order, as view.rs: the bar (:90); when settled hidden only the lip dots after it (:91-99); else the
// reflections (:101-125), the shelf (:126), the icons in slot order with the app-grid button among them (:128-138),
// the running dots (:139-153) and the focused-app line (:154).
//
// Keep the Menu and Minimized instances at the end: they are the menus' own files.
pragma ComponentBehavior: Bound

import QtQuick
import QtQml.Models
import RileyDock

Item {
    id: root
    required property DockModel dock

    // The bar's glass (view.rs:63, :90), the CSS box of .dock-bar: the background fills the whole border box under
    // the border (background-clip border-box), and the border is painted over it. Drawn in every pose: while
    // hidden only its top edge is on screen.
    Rectangle {
        x: root.dock.bar.x
        y: root.dock.bar.y
        width: root.dock.bar.width
        height: root.dock.bar.height
        radius: root.dock.style.barRadius
        color: root.dock.style.barBackground
    }
    Rectangle {
        x: root.dock.bar.x
        y: root.dock.bar.y
        width: root.dock.bar.width
        height: root.dock.bar.height
        radius: root.dock.style.barRadius
        color: "transparent"
        border.color: root.dock.style.barBorder
        border.width: root.dock.style.barBorderWidth
    }

    // Settled hidden: only the bar's top edge and the running-app dots of the lip (view.rs:91-99).
    Repeater {
        model: root.dock.hidden ? root.dock.lipDots : []
        delegate: Rectangle {
            required property point modelData
            x: modelData.x - root.dock.lipDotRadius
            y: modelData.y - root.dock.lipDotRadius
            width: 2 * root.dock.lipDotRadius
            height: width
            radius: root.dock.lipDotRadius
            color: root.dock.style.accent
        }
    }

    Item {
        anchors.fill: parent
        visible: !root.dock.hidden

        // Reflections, then the shelf they stand on (view.rs:101-127).
        Repeater {
            model: root.dock.items
            delegate: Reflection { dock: root.dock }
        }
        Rectangle {
            visible: root.dock.shelfVisible
            x: root.dock.shelf.x
            y: root.dock.shelf.y
            width: root.dock.shelf.width
            height: root.dock.shelf.height
            color: root.dock.style.shelf
            // Fractional edges, as cairo fills them: at a fractional scale an unsmoothed 1 px line covers two
            // whole device rows (measured on DP-2 at 1.25: GTK 1.2 rows, Qt without this 2).
            antialiasing: true
        }

        // The icons and the app-grid button, in slot order as view.rs:128-138 paints them, so a hover shadow
        // falls over its neighbours the same way.
        Repeater {
            model: root.dock.items
            delegate: DelegateChooser {
                role: "launcher"
                DelegateChoice {
                    roleValue: true
                    Launcher { dock: root.dock }
                }
                DelegateChoice {
                    Icon { dock: root.dock }
                }
            }
        }

        // Running dots (view.rs:139-153): filled discs of radius dotRadius, or, when every window is minimized, that
        // circle stroked hollowLineWidth wide. Cairo strokes on the path, half the width on each side, so the ring
        // runs from dotRadius - w/2 to dotRadius + w/2. A pinned app's dots are style.accent, those of an open app
        // that is not pinned style.unpinnedDot (Marc, 2026-11-13).
        Repeater {
            model: root.dock.items
            delegate: Item {
                id: slot
                required property rect rect
                required property var dots
                required property bool minimized
                required property bool pinned
                readonly property color dotColor: pinned ? root.dock.style.accent : root.dock.style.unpinnedDot
                readonly property real outer: minimized ? root.dock.dotRadius + root.dock.hollowLineWidth / 2
                                                        : root.dock.dotRadius
                Repeater {
                    model: slot.dots
                    delegate: Rectangle {
                        required property real modelData
                        x: slot.rect.x + slot.rect.width / 2 + modelData - slot.outer
                        y: root.dock.indicatorY - slot.outer
                        width: 2 * slot.outer
                        height: width
                        radius: slot.outer
                        color: slot.minimized ? "transparent" : slot.dotColor
                        border.color: slot.dotColor
                        border.width: slot.minimized ? root.dock.hollowLineWidth : 0
                    }
                }
            }
        }

        // The line under the focused app (view.rs:68-77, :154): the CSS border-bottom of its box.
        Rectangle {
            visible: root.dock.activeLineVisible
            x: root.dock.activeLine.x
            y: root.dock.activeLine.y + root.dock.activeLine.height - root.dock.style.activeLineWidth
            width: root.dock.activeLine.width
            height: root.dock.style.activeLineWidth
            color: root.dock.style.activeLine
            antialiasing: true // fractional edges, as the shelf
        }
    }

    // The popups (Menu.qml, Minimized.qml).
    Menu { dock: root.dock }
    Minimized { dock: root.dock }
}
