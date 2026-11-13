// SPDX-License-Identifier: GPL-3.0-or-later
//
// One icon's reflection on the shelf (riley dock/src/view.rs:101-125), a delegate of DockModel.items. As GTK does:
// clip to reflectionClip (view.rs:106, :110), paint the icon mirrored by translating to (reflectionImage.x,
// reflectionFlipY) and scaling (1, -1) at reflectionImage's size (view.rs:118-121), and multiply it by the alpha of
// a vertical gradient that runs over the clip from its top (reflectionStops[0]) to its bottom (reflectionStops[1])
// (view.rs:108-117, MaskMode::Alpha). The launcher is never reflected (view.rs:103-104): `reflected` is false.
pragma ComponentBehavior: Bound

import QtQuick
import Qt5Compat.GraphicalEffects
import RileyDock

Item {
    id: reflection
    required property DockModel dock
    required property bool reflected
    required property string iconSource
    required property bool symbolic
    required property rect reflectionImage
    required property rect reflectionClip
    required property real reflectionFlipY
    required property var reflectionStops

    visible: reflected
    x: reflectionClip.x
    y: reflectionClip.y
    width: reflectionClip.width
    height: reflectionClip.height

    Loader {
        anchors.fill: parent
        active: reflection.reflected && reflection.reflectionStops.length === 2
        sourceComponent: Item {
            // The mirrored icon, in clip coordinates. Its own bounds are the clip: the mask samples only them.
            Item {
                id: mirror
                anchors.fill: parent
                visible: false

                Item {
                    x: reflection.reflectionImage.x - reflection.reflectionClip.x
                    y: reflection.reflectionFlipY - reflection.reflectionClip.y
                    transform: Scale { yScale: -1 }

                    Icon {
                        dock: reflection.dock
                        launcher: false
                        iconSource: reflection.iconSource
                        symbolic: reflection.symbolic
                        drawRect: Qt.rect(0, 0, reflection.reflectionImage.width, reflection.reflectionImage.height)
                        shadow: false
                    }
                }
            }

            // The alpha mask: only its alpha counts (GTK paints it white).
            Rectangle {
                id: fade
                anchors.fill: parent
                visible: false
                gradient: Gradient {
                    GradientStop {
                        position: reflection.reflectionStops[0].x
                        color: Qt.rgba(1, 1, 1, reflection.reflectionStops[0].y)
                    }
                    GradientStop {
                        position: reflection.reflectionStops[1].x
                        color: Qt.rgba(1, 1, 1, reflection.reflectionStops[1].y)
                    }
                }
            }

            OpacityMask {
                anchors.fill: parent
                source: mirror
                maskSource: fade
            }
        }
    }
}
