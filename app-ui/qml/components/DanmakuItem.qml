import QtQuick

Rectangle {
    id: root
    property var itemData
    property var controller
    property Item overlay
    property Item ngZone
    property bool localDragging: false
    property real localX: 0
    property real localY: 0
    property real dragVisualOffsetX: {
        if (localDragging || !controller) {
            return 0
        }
        const elapsedMs = Number(controller.dragVisualElapsedMs || 0)
        const speedPxPerSec = Number(itemData.speedPxPerSec || 0)
        return elapsedMs * speedPxPerSec / 1000.0
    }

    function computeNgHit(mouse) {
        if (!overlay || !ngZone) {
            return false
        }

        const left = localDragging ? localX : itemData.x
        const top = localDragging ? localY : itemData.y
        const right = left + root.width
        const bottom = top + root.height

        const zoneLeft = ngZone.x
        const zoneTop = ngZone.y
        const zoneRight = zoneLeft + ngZone.width
        const zoneBottom = zoneTop + ngZone.height

        const overlap = !(right < zoneLeft || zoneRight < left || bottom < zoneTop || zoneBottom < top)

        let pointerInZone = false
        if (mouse && overlay) {
            const p = root.mapToItem(overlay, mouse.x, mouse.y)
            pointerInZone = p.x >= zoneLeft && p.x <= zoneRight && p.y >= zoneTop && p.y <= zoneBottom
        }

        return overlap || pointerInZone
    }

    function finishDrag(mouse, canceled) {
        if (!controller || !localDragging) {
            return
        }

        const inNgZone = !canceled && computeNgHit(mouse)
        localDragging = false

        if (canceled) {
            controller.cancelDrag(itemData.commentId)
        } else {
            controller.dropDrag(itemData.commentId, inNgZone)
        }
    }

    x: (localDragging ? localX : itemData.x) - dragVisualOffsetX
    y: localDragging ? localY : itemData.y
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
                localDragging = true
                localX = itemData.x
                localY = itemData.y
                controller.beginDrag(itemData.commentId)
            }
        }

        onPositionChanged: function(mouse) {
            if (!controller || !pressed || !overlay) {
                return
            }
            const p = root.mapToItem(overlay, mouse.x, mouse.y)
            localX = p.x - root.width / 2
            localY = p.y - root.height / 2
            controller.moveDrag(itemData.commentId, localX, localY)
        }

        onReleased: function(mouse) {
            finishDrag(mouse, false)
        }

        onCanceled: {
            finishDrag(null, true)
        }

        onPressedChanged: {
            if (!pressed && localDragging) {
                // Fallback for cases where release is not delivered.
                finishDrag(null, false)
            }
        }
    }
}
