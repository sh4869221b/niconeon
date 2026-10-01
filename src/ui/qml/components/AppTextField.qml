import QtQuick
import QtQuick.Controls
import Niconeon

TextField {
    id: control

    implicitHeight: 36
    color: AppTheme.text
    selectionColor: AppTheme.highlight
    selectedTextColor: AppTheme.highlightedText
    placeholderTextColor: AppTheme.placeholderText

    background: Rectangle {
        radius: 8
        color: AppTheme.textInputBackground
        border.width: 1
        border.color: control.activeFocus ? AppTheme.textInputFocusBorder : AppTheme.textInputBorder
    }
}
