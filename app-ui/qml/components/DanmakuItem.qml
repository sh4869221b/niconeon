import QtQuick

Rectangle {
    id: root
    property var itemData
    property var controller
    property Item overlay
    property Item ngZone

    x: itemData.x
    y: itemData.y
    width: itemData.widthEstimate
    height: 42
    radius: 8
    color: "#88000000"
    border.color: "#AAFFFFFF"
    opacity: itemData.alpha

    Text {
        anchors.centerIn: parent
        color: "white"
        font.pixelSize: 24
        text: itemData.text
        elide: Text.ElideRight
        width: parent.width - 16
        horizontalAlignment: Text.AlignHCenter
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true

        onPressed: {
            if (controller) {
                controller.beginDrag(itemData.commentId)
            }
        }

        onPositionChanged: function(mouse) {
            if (!controller || !pressed || !overlay) {
                return
            }
            const p = root.mapToItem(overlay, mouse.x, mouse.y)
            controller.moveDrag(itemData.commentId, p.x - root.width / 2, p.y - root.height / 2)
        }

        onReleased: {
            if (!controller || !ngZone || !overlay) {
                return
            }

            const centerX = itemData.x + root.width / 2
            const centerY = itemData.y + root.height / 2
            const inZone = centerX >= ngZone.x && centerX <= ngZone.x + ngZone.width
                && centerY >= ngZone.y && centerY <= ngZone.y + ngZone.height

            controller.dropDrag(itemData.commentId, inZone)
        }

        onCanceled: {
            if (controller) {
                controller.cancelDrag(itemData.commentId)
            }
        }
    }
}
