import QtQuick
import QtQuick.Controls
import Niconeon

Rectangle {
    width: 220
    height: 120
    radius: 16
    color: AppTheme.ngDropBackground
    border.color: AppTheme.ngDropBorder
    border.width: 2

    Label {
        anchors.centerIn: parent
        text: "NGにドロップ"
        color: AppTheme.ngDropText
        font.bold: true
        font.pixelSize: 22
    }
}
