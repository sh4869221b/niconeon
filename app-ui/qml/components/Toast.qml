import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Niconeon

Rectangle {
    id: root
    property string message: ""
    property string actionText: ""
    signal actionTriggered()

    width: Math.min(parent ? parent.width - 40 : 600, 600)
    height: actionText === "" ? 52 : 60
    radius: 10
    color: AppTheme.toastBackground
    border.color: AppTheme.toastBorder

    RowLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: root.message
            color: AppTheme.toastText
            wrapMode: Text.Wrap
        }

        AppButton {
            visible: root.actionText !== ""
            text: root.actionText
            onClicked: root.actionTriggered()
        }
    }
}
