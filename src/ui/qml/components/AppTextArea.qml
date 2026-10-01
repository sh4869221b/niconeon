import QtQuick
import QtQuick.Controls
import Niconeon

TextArea {
    id: control

    color: AppTheme.text
    selectionColor: AppTheme.highlight
    selectedTextColor: AppTheme.highlightedText
    placeholderTextColor: AppTheme.placeholderText
    selectByMouse: true
    wrapMode: TextArea.WrapAnywhere

    background: Rectangle {
        radius: 8
        color: AppTheme.textInputBackground
        border.width: 1
        border.color: control.activeFocus ? AppTheme.textInputFocusBorder : AppTheme.textInputBorder
    }
}
