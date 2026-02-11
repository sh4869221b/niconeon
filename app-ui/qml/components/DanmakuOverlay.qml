import QtQuick
import QtQuick.Controls

Item {
    id: root
    property var controller

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
    }
}
