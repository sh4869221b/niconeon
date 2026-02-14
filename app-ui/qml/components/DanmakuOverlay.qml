import QtQuick
import QtQuick.Controls
import Niconeon

Item {
    id: root
    property var controller
    property bool sceneDragging: false
    clip: true

    function syncNgZoneRect() {
        if (!root.controller) {
            return
        }
        root.controller.setNgDropZoneRect(ngZone.x, ngZone.y, ngZone.width, ngZone.height)
    }

    function finishSceneDrag(canceled) {
        if (!root.controller || !sceneDragging) {
            return
        }
        sceneDragging = false
        if (canceled) {
            root.controller.cancelActiveDrag()
        } else {
            root.controller.dropActiveDrag(false)
        }
    }

    onWidthChanged: syncNgZoneRect()
    onHeightChanged: syncNgZoneRect()
    onControllerChanged: syncNgZoneRect()
    Component.onCompleted: syncNgZoneRect()

    Text {
        id: glyphWarmupText
        visible: root.controller ? root.controller.glyphWarmupEnabled : false
        text: root.controller ? root.controller.glyphWarmupText : ""
        anchors.left: parent.left
        anchors.top: parent.top
        width: 1
        height: 1
        clip: true
        color: "white"
        opacity: 0.01
        font.pixelSize: 24
        z: -1
    }

    DanmakuRenderNodeItem {
        anchors.fill: parent
        controller: root.controller
    }

    MouseArea {
        anchors.fill: parent
        enabled: !!root.controller
        hoverEnabled: true
        preventStealing: true

        onPressed: function(mouse) {
            if (!root.controller) {
                return
            }
            sceneDragging = root.controller.beginDragAt(mouse.x, mouse.y)
            if (sceneDragging) {
                root.controller.moveActiveDrag(mouse.x, mouse.y)
            }
        }

        onPositionChanged: function(mouse) {
            if (!root.controller || !pressed || !sceneDragging) {
                return
            }
            root.controller.moveActiveDrag(mouse.x, mouse.y)
        }

        onReleased: {
            finishSceneDrag(false)
        }

        onCanceled: {
            finishSceneDrag(true)
        }

        onPressedChanged: {
            if (!pressed && sceneDragging) {
                Qt.callLater(function() {
                    if (sceneDragging) {
                        finishSceneDrag(false)
                    }
                })
            }
        }
    }

    NgDropZone {
        id: ngZone
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        visible: root.controller ? root.controller.ngDropZoneVisible : false
        onXChanged: root.syncNgZoneRect()
        onYChanged: root.syncNgZoneRect()
        onWidthChanged: root.syncNgZoneRect()
        onHeightChanged: root.syncNgZoneRect()
    }
}
