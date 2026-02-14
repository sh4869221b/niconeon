import QtQuick

Rectangle {
    id: root
    required property string commentId
    required property string userId
    required property string text
    required property real posX
    required property real posY
    required property real alpha
    required property int lane
    required property bool dragging
    required property int widthEstimate
    required property bool ngDropHovered
    required property bool active
    property var controller
    property Item overlay
    property bool localDragging: false
    property real localX: 0
    property real localY: 0
    function finishDrag(canceled) {
        if (!controller || !localDragging) {
            return
        }
        localDragging = false

        if (canceled) {
            controller.cancelDrag(commentId)
        } else {
            controller.dropDrag(commentId, false)
        }
    }

    x: localDragging ? localX : posX
    y: localDragging ? localY : posY
    visible: active
    enabled: active
    width: widthEstimate
    height: 42
    radius: 8
    color: "#88000000"
    border.color: ngDropHovered ? "#FFFF4466" : "#AAFFFFFF"
    border.width: ngDropHovered ? 2 : 1
    opacity: alpha

    Text {
        anchors.centerIn: parent
        color: "white"
        font.pixelSize: 24
        text: root.text
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
                localX = posX
                localY = posY
                controller.beginDrag(commentId)
            }
        }

        onPositionChanged: function(mouse) {
            if (!controller || !pressed || !overlay) {
                return
            }
            const p = root.mapToItem(overlay, mouse.x, mouse.y)
            localX = p.x - root.width / 2
            localY = p.y - root.height / 2
            controller.moveDrag(commentId, localX, localY)
        }

        onReleased: {
            finishDrag(false)
        }

        onCanceled: {
            finishDrag(true)
        }

        onPressedChanged: {
            if (!pressed && localDragging) {
                // Fallback only when release/cancel is truly missing.
                Qt.callLater(function() {
                    if (localDragging) {
                        finishDrag(false)
                    }
                })
            }
        }
    }
}
