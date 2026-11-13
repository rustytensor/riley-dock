// SPDX-License-Identifier: GPL-3.0-or-later
//
// One application icon (riley dock/src/view.rs:128-138, paint_icon :172-176), a delegate of DockModel.items. It is
// also the icon that Reflection.qml paints mirrored (view.rs:121), with drawRect at its own origin and no shadow.
//
// - The image fills drawRect keeping its aspect ratio, centred, as GtkIconPaintable draws itself.
// - A symbolic icon is painted in style.barForeground: GTK's snapshot_symbolic with all four colours set to the
//   bar's colour (view.rs:173-174) is that colour, alpha included, times the icon's own alpha; here a rectangle of
//   that colour under the icon's alpha mask.
// - The hover shadow is GSK's push_shadow (view.rs:130-133, :136): the shadow of the painted icon's alpha, offset and
//   blurred, drawn beneath the icon.
pragma ComponentBehavior: Bound

import QtQuick
import Qt5Compat.GraphicalEffects
import RileyDock

Item {
    id: icon
    required property DockModel dock
    required property bool launcher
    required property string iconSource
    required property bool symbolic
    required property rect drawRect
    required property bool shadow

    // GSK takes the shadow's blur radius as CSS does, a Gaussian of deviation radius / 2. DropShadow takes a
    // radius it turns into the deviation (radius + 1) / 3.3333 (Qt5Compat GaussianBlur), and wants
    // samples = 1 + 2 * radius. Both are unit conversions of dock.shadowRadius, not numbers of their own.
    readonly property real shadowDeviation: dock.shadowRadius / 2
    readonly property real shadowBlur: shadowDeviation * 3.3333 - 1

    visible: !launcher
    x: drawRect.x
    y: drawRect.y
    width: drawRect.width
    height: drawRect.height

    // paint_icon (view.rs:172-176). Hidden while the shadow effect paints it instead.
    Item {
        id: art
        anchors.fill: parent
        visible: !icon.shadow

        Item {
            id: picture
            anchors.fill: parent
            // A symbolic icon's image is only the mask of its colour.
            visible: !icon.symbolic

            Image {
                anchors.fill: parent
                source: icon.launcher ? "" : icon.iconSource
                sourceSize: Qt.size(icon.dock.iconLookupSize, icon.dock.iconLookupSize)
                fillMode: Image.PreserveAspectFit
                asynchronous: false
                smooth: true
                // The image is looked up at the full swell size, so it is never shrunk below half: GSK draws it
                // with plain linear filtering there.
                mipmap: false
            }
        }

        Loader {
            anchors.fill: parent
            active: icon.symbolic && !icon.launcher
            sourceComponent: Item {
                Rectangle {
                    id: tint
                    anchors.fill: parent
                    visible: false
                    color: icon.dock.style.barForeground
                }
                OpacityMask {
                    anchors.fill: parent
                    source: tint
                    maskSource: picture
                }
            }
        }
    }

    Loader {
        anchors.fill: parent
        active: icon.shadow && !icon.launcher
        sourceComponent: DropShadow {
            source: art
            color: icon.dock.shadowColor
            horizontalOffset: icon.dock.shadowOffsetX
            verticalOffset: icon.dock.shadowOffsetY
            radius: icon.shadowBlur
            samples: 2 * Math.ceil(icon.shadowBlur) + 1
            spread: 0
            transparentBorder: true
        }
    }
}
