import QtQuick
import QtQuick.Controls
import Niconeon

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
        color: control.enabled ? AppTheme.buttonText : AppTheme.buttonTextDisabled
    }

    background: Rectangle {
        radius: 8
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? AppTheme.buttonFocusBorder : AppTheme.buttonBorder
        color: !control.enabled
            ? AppTheme.buttonDisabledBackground
            : (control.down
                ? AppTheme.buttonPressedBackground
                : (control.hovered ? AppTheme.buttonHoverBackground : AppTheme.buttonDefaultBackground))
    }
}
