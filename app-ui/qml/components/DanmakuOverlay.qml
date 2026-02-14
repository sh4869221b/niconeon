import QtQuick
import QtQuick.Controls

Item {
    id: root
    property var controller
    function syncNgZoneRect() {
        if (!root.controller) {
            return
        }
        root.controller.setNgDropZoneRect(ngZone.x, ngZone.y, ngZone.width, ngZone.height)
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

    Repeater {
        model: root.controller ? root.controller.itemModel : null
        delegate: DanmakuItem {
            overlay: root
            controller: root.controller
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
