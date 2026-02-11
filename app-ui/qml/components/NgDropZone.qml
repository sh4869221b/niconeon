import QtQuick
import QtQuick.Controls

Rectangle {
    width: 220
    height: 120
    radius: 16
    color: "#CC7A0012"
    border.color: "#FFFF4466"
    border.width: 2

    Label {
        anchors.centerIn: parent
        text: "NGにドロップ"
        color: "white"
        font.bold: true
        font.pixelSize: 22
    }
}
