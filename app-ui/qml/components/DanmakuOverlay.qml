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

    Repeater {
        model: root.controller ? root.controller.items : []
        delegate: DanmakuItem {
            overlay: root
            controller: root.controller
            ngZone: ngZone
            itemData: modelData
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
