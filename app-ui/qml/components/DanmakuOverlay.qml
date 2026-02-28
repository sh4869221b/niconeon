import QtQuick
import QtQuick.Controls
import Niconeon

Item {
    id: root
    property var controller
    property bool sceneDragging: false
    property double videoFps: NaN
    property double commentFps: NaN
    property int totalComments: -1
    property int activeCommentCount: -1
    clip: true

    function formatFps(value) {
        const number = Number(value)
        if (!isFinite(number) || number < 0) {
            return "--"
        }
        return number.toFixed(1)
    }

    function formatCount(value) {
        const number = Number(value)
        if (!isFinite(number) || number < 0) {
            return "--"
        }
        return Math.round(number).toString()
    }

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

    Rectangle {
        id: statsPanel
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 12
        implicitWidth: statsColumn.implicitWidth + 16
        implicitHeight: statsColumn.implicitHeight + 16
        width: implicitWidth
        height: implicitHeight
        radius: 6
        color: "#66000000"
        border.color: "#55ffffff"
        border.width: 1
        z: 10

        Column {
            id: statsColumn
            anchors.fill: parent
            anchors.margins: 8
            spacing: 4

            Label {
                color: "white"
                text: "Video FPS: %1".arg(root.formatFps(root.videoFps))
            }

            Label {
                color: "white"
                text: "Comment FPS: %1".arg(root.formatFps(root.commentFps))
            }

            Label {
                color: "white"
                text: "Comments: %1 / %2".arg(root.formatCount(root.activeCommentCount)).arg(root.formatCount(root.totalComments))
            }
        }
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
