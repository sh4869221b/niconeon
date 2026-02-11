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
        if (!overlay || !ngZone || !ngZone.visible) {
            return false
        }

        const topLeftInZone = root.mapToItem(ngZone, 0, 0)
        const bottomRightInZone = root.mapToItem(ngZone, root.width, root.height)

        const overlap = !(
            bottomRightInZone.x < 0
            || ngZone.width < topLeftInZone.x
            || bottomRightInZone.y < 0
            || ngZone.height < topLeftInZone.y
        )

        const pointer = mouse
            ? root.mapToItem(ngZone, mouse.x, mouse.y)
            : root.mapToItem(ngZone, root.width / 2, root.height / 2)
        const pointerInZone = pointer.x >= 0
            && pointer.x <= ngZone.width
            && pointer.y >= 0
            && pointer.y <= ngZone.height

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
                // Fallback only when release/cancel is truly missing.
                Qt.callLater(function() {
                    if (localDragging) {
                        finishDrag(null, false)
                    }
                })
            }
        }
    }
}
