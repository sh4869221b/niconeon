import QtQuick
import QtQuick.Controls
import Niconeon

TabButton {
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
        color: control.checked ? AppTheme.tabCheckedText : AppTheme.tabText
    }

    background: Rectangle {
        radius: 8
        color: control.checked
            ? AppTheme.tabCheckedBackground
            : (control.hovered ? AppTheme.tabHoverBackground : AppTheme.tabBackground)
        border.width: 1
        border.color: control.checked ? AppTheme.tabCheckedBorder : AppTheme.tabBorder
    }
}
