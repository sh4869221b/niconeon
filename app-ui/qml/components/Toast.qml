import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: root
    property string message: ""
    property string actionText: ""
    signal actionTriggered()

    width: Math.min(parent ? parent.width - 40 : 600, 600)
    height: actionText === "" ? 52 : 60
    radius: 10
    color: "#E0212430"
    border.color: "#66FFFFFF"

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: root.message
            color: "white"
            wrapMode: Text.Wrap
        }

        Button {
            visible: root.actionText !== ""
            text: root.actionText
            onClicked: root.actionTriggered()
        }
    }
}
