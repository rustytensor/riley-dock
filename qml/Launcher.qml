// SPDX-License-Identifier: GPL-3.0-or-later
//
// The app-grid button, a delegate of DockModel.items (riley dock/src/view.rs:128-138 with paint_icon's grid branch,
// :161-171). Nine dots at gridDots, in the icon's own coordinates, with radius gridDotRadius, filled in
// style.barForeground mixed towards style.accent by gridAccent, channel by channel, alpha included (view.rs:164-166).
//
// The hover and press animation is the core's grid::Animation (set_hover ui.rs:488, :719; release :663-671,
// :696-701; sample :489-492): its spring moves the dots' spacing and the accent mix, and the model hands over this
// frame's dots, radius and mix. Nothing is timed or eased here. The button is never swelled, squashed or bounced
// (ui.rs:420, :427, :443), never shadowed (ui.rs:493) and never reflected (view.rs:103-104).
pragma ComponentBehavior: Bound
import QtQuick
import RileyDock

Item {
    id: button
    required property DockModel dock
    required property bool launcher
    required property rect drawRect
    required property var gridDots
    required property real gridDotRadius
    required property real gridAccent

    readonly property color dotColor: {
        const a = dock.style.barForeground, b = dock.style.accent, t = gridAccent;
        return Qt.rgba(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }

    visible: launcher
    // view.rs:134: painted at (rect.x, rect.y - bounce), which is drawRect.
    x: drawRect.x
    y: drawRect.y
    width: drawRect.width
    height: drawRect.height

    Repeater {
        model: button.launcher ? button.gridDots : []
        delegate: Rectangle {
            required property point modelData
            x: modelData.x - button.gridDotRadius
            y: modelData.y - button.gridDotRadius
            width: 2 * button.gridDotRadius
            height: width
            radius: button.gridDotRadius
            color: button.dotColor
            antialiasing: true
        }
    }
}
