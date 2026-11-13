// SPDX-License-Identifier: GPL-3.0-or-later
//
// The hover name label (riley dock/src/ui.rs:105-117, :727-749), the root of its own overlay surface. The dock sizes
// that surface to this item's implicitWidth x implicitHeight (GTK's natural size, ui.rs:739: text plus padding plus
// border) and places it from the core's rectangles, so both are the label's full outer size and nothing here moves
// the label inside its surface.
//
// The CSS box of .dock-name: the background fills the whole border box under the border (background-clip
// border-box), the border is painted over it, and the text sits inside the border and padding. The text is plain
// (GtkLabel set_text, ui.rs:738): a window title is never read as markup. Lines are left-aligned, GtkLabel's default.
pragma ComponentBehavior: Bound

import QtQuick
import RileyDock

Item {
    id: label
    required property DockModel dock

    readonly property int inset: dock.style.nameBorderWidth

    implicitWidth: text.implicitWidth + 2 * (dock.style.namePaddingX + inset)
    implicitHeight: text.implicitHeight + 2 * (dock.style.namePaddingY + inset)

    Rectangle {
        anchors.fill: parent
        radius: label.dock.style.nameRadius
        color: label.dock.style.nameBackground
    }
    Rectangle {
        anchors.fill: parent
        radius: label.dock.style.nameRadius
        color: "transparent"
        border.color: label.dock.style.nameBorder
        border.width: label.inset
    }

    Text {
        id: text
        x: label.dock.style.namePaddingX + label.inset
        y: label.dock.style.namePaddingY + label.inset
        text: label.dock.hoverText
        textFormat: Text.PlainText
        horizontalAlignment: Text.AlignLeft
        renderType: Text.NativeRendering
        color: label.dock.style.nameForeground
        font.family: label.dock.style.nameFontFamily
        font.pixelSize: label.dock.style.nameFontSize
    }
}
