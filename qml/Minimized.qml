// SPDX-License-Identifier: GPL-3.0-or-later
//
// The window list of an app with several windows, opened by a left click (riley dock/src/ui.rs:792-806, built on
// popover() at :751-790). One button per window, its line from rules::windowLine ("<title>  · <workspace>", with
// "minimized" as the workspace of a minimized window); picking a row restores a minimized window and focuses any
// other (rules::menuPick). A popup surface of the dock, shown while dock.menu.open and dock.menu.kind is "windows".
// Every close ends in exactly one of dock.menu.pickWindow(index) or dock.menu.dismiss().
//
// The box and the rows are the right-click menu's (Menu.qml), so both menus look alike, as in the Rust dock.
pragma ComponentBehavior: Bound
import QtQuick
import RileyDock

Window {
    id: popup
    required property DockModel dock
    readonly property DockMenu menu: dock.menu

    // Shown once the rows for this menu exist (they and `open` change on the same signal, in no fixed order).
    readonly property bool wanted: menu.open && menu.kind === "windows"
    onWantedChanged: {
        if (wanted)
            Qt.callLater(popup.openNow);
        else
            visible = false;
    }
    function openNow() {
        if (wanted && !visible)
            visible = true;
    }

    flags: Qt.Popup
    color: "transparent"
    width: Math.max(1, box.implicitWidth)
    height: Math.max(1, box.implicitHeight)
    // Above the icon, centred on it (gtk::Popover at PositionType::Top pointing at the icon, ui.rs:753, :760-762).
    // The compositor keeps it on the screen.
    x: (transientParent ? transientParent.x : 0) + menu.anchor.x + menu.anchor.width / 2 - width / 2
    y: (transientParent ? transientParent.y : 0) + menu.anchor.y - height
    onVisibleChanged: if (!visible && menu.open && menu.kind === "windows") menu.dismiss()

    Menu.MenuBox {
        id: box
        style: popup.dock.style
        focus: true
        Keys.onEscapePressed: popup.close()

        Repeater {
            model: popup.menu.windows
            delegate: Menu.MenuRow {
                required property int index
                required property var modelData
                style: popup.dock.style
                text: modelData.line
                onActivated: popup.menu.pickWindow(index)
            }
        }
    }
}
