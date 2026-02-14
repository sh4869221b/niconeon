import QtQuick
import QtQuick.Controls

Button {
    id: control

    implicitHeight: 36
    leftPadding: 14
    rightPadding: 14

    contentItem: Text {
        text: control.text
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
        color: control.enabled ? "#F3F6FF" : "#9AA6BF"
    }

    background: Rectangle {
        radius: 8
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? "#9CB9FF" : "#5A6A8A"
        color: !control.enabled
            ? "#2A2F3A"
            : (control.down
                ? "#3B4A67"
                : (control.hovered ? "#34415C" : "#2E3950"))
    }
}
