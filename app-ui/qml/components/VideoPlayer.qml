import QtQuick
import Niconeon

Item {
    id: root
    property alias mpv: mpv

    MpvItem {
        id: mpv
        anchors.fill: parent
    }
}
