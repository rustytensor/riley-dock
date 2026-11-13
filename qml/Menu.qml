// SPDX-License-Identifier: GPL-3.0-or-later
//
// The right-click menu (riley dock/src/ui.rs:808-851, built on popover() at :751-790): one row per window that opens
// a submenu of DockMenu.actions (the gtk::MenuButton and its popover, :810-830), then a Pin or Unpin row (:831-849).
// A popup surface of the dock, shown while dock.menu.open and dock.menu.kind is "context". Every close ends in
// exactly one of dock.menu.windowAction(window, action), dock.menu.togglePin() or dock.menu.dismiss().
//
// The inline component MenuRow is the menu's button (a gtk::Button in the Rust dock); Minimized.qml uses it too.
// The Rust dock left the popover's look to the GTK theme and styled only `.dock-menu { padding }`; here the box,
// text and font come from the hover name's style keys (docs/files.md 3.2), and a row's hover and press fills are the
// style's `shelf` and `bar_border` colours, so no colour or size is made up in this file.
import QtQuick
import RileyDock

Window {
    id: popup
    required property DockModel dock
    readonly property DockMenu menu: dock.menu
    readonly property DockStyle style: dock.style
    // The window whose action submenu is open, or -1.
    property int subWindow: -1
    // That window's row, which the submenu points at.
    property Item subRow: null

    // One row of a dock menu: a button with a centred label (gtk::Button::with_label), and for a submenu row the
    // MenuButton's arrow after it. Clicked on release inside the row, as a GTK button is.
    component MenuRow: Rectangle {
        id: row
        required property DockStyle style
        property alias text: label.text
        property bool submenu: false
        property bool open: false
        signal activated()

        width: parent ? parent.width : implicitWidth
        implicitWidth: content.implicitWidth + 2 * style.namePaddingX
        implicitHeight: content.implicitHeight + 2 * style.namePaddingY
        // The rows sit inside the box's padding, so their corners follow the box's (concentric corners).
        radius: Math.max(0, style.nameRadius - style.menuPadding)
        color: area.pressed ? style.barBorder : (area.containsMouse || open) ? style.shelf : "transparent"

        Row {
            id: content
            anchors.centerIn: parent
            spacing: row.submenu ? row.style.namePaddingY : 0
            Text {
                id: label
                color: row.style.nameForeground
                font.family: row.style.nameFontFamily
                font.pixelSize: row.style.nameFontSize
            }
            Text {
                visible: row.submenu
                text: "▾"
                color: row.style.nameForeground
                font.family: row.style.nameFontFamily
                font.pixelSize: row.style.nameFontSize
            }
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: row.activated()
        }
    }

    // The box of a dock menu: the popover's content with the `.dock-menu` padding (ui.rs:765-767) and 2 px between
    // rows (gtk::Box spacing, ui.rs:765, :813). Its size is worked out from the rows' own sizes, without waiting for
    // a layout pass, so the popup is mapped once at its final size.
    component MenuBox: Rectangle {
        id: frame
        required property DockStyle style
        default property alias rows: column.data
        readonly property real rowWidth: {
            let width = 0;
            for (const child of column.children)
                if (child.visible) width = Math.max(width, child.implicitWidth);
            return width;
        }
        readonly property real rowsHeight: {
            let height = 0, count = 0;
            for (const child of column.children) {
                if (!child.visible || child.implicitHeight <= 0) continue;
                height += child.implicitHeight;
                ++count;
            }
            return height + Math.max(0, count - 1) * column.spacing;
        }
        implicitWidth: rowWidth + 2 * style.menuPadding
        implicitHeight: rowsHeight + 2 * style.menuPadding
        color: style.nameBackground
        border.color: style.nameBorder
        border.width: style.nameBorderWidth
        radius: style.nameRadius
        Column {
            id: column
            x: frame.style.menuPadding
            y: frame.style.menuPadding
            width: frame.rowWidth
            spacing: 2
        }
    }

    function closeSubmenu() {
        subWindow = -1;
        subRow = null;
    }

    // GTK's MenuButton toggles its popover; picking another window's row moves the submenu to that row.
    function toggleSubmenu(index, row) {
        if (subWindow === index) {
            closeSubmenu();
            return;
        }
        closeSubmenu();
        Qt.callLater(function () {
            if (popup.visible) {
                popup.subRow = row;
                popup.subWindow = index;
            }
        });
    }

    // Shown once the rows for this menu exist (they and `open` change on the same signal, in no fixed order).
    readonly property bool wanted: menu.open && menu.kind === "context"
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
    onVisibleChanged: {
        closeSubmenu();
        if (!visible && menu.open && menu.kind === "context")
            menu.dismiss();
    }

    MenuBox {
        id: box
        style: popup.style
        focus: true
        Keys.onEscapePressed: popup.close()

        Repeater {
            model: popup.menu.windows
            delegate: MenuRow {
                id: windowRow
                required property int index
                required property var modelData
                style: popup.style
                text: modelData.line
                submenu: true
                open: popup.subWindow === index
                onActivated: popup.toggleSubmenu(index, windowRow)
            }
        }
        MenuRow {
            style: popup.style
            text: popup.menu.pinLabel
            onActivated: popup.menu.togglePin()
        }
    }

    // The window's actions (ui.rs:811-828): Close, Float/tile, Full screen, Move to workspace 1-9. A popup of the
    // menu. GTK placed the MenuButton's popover below the row and flipped it above, since the dock sits on the
    // screen's bottom edge; it opens above the row here, centred on it. Closing it alone returns to the menu.
    Window {
        id: submenu
        flags: Qt.Popup
        color: "transparent"
        visible: popup.visible && popup.subWindow >= 0 && popup.subRow !== null
        width: Math.max(1, actionBox.implicitWidth)
        height: Math.max(1, actionBox.implicitHeight)
        readonly property point rowAt: popup.subRow ? popup.subRow.mapToItem(null, 0, 0) : Qt.point(0, 0)
        x: popup.x + rowAt.x + (popup.subRow ? popup.subRow.width : 0) / 2 - width / 2
        y: popup.y + rowAt.y - height
        onVisibleChanged: if (!visible && popup.subWindow >= 0) popup.closeSubmenu()

        MenuBox {
            id: actionBox
            style: popup.style
            focus: true
            Keys.onEscapePressed: popup.closeSubmenu()

            Repeater {
                model: popup.menu.actions
                delegate: MenuRow {
                    required property int index
                    required property string modelData
                    style: popup.style
                    text: modelData
                    onActivated: popup.menu.windowAction(popup.subWindow, index)
                }
            }
        }
    }
}
