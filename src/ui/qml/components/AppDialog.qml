import QtQuick
import QtQuick.Controls
import Niconeon

Dialog {
    id: root

    palette {
        window: AppTheme.window
        windowText: AppTheme.windowText
        base: AppTheme.base
        text: AppTheme.text
        button: AppTheme.button
        buttonText: AppTheme.buttonText
        placeholderText: AppTheme.placeholderText
        highlight: AppTheme.highlight
        highlightedText: AppTheme.highlightedText
    }

    background: Rectangle {
        radius: 10
        color: AppTheme.window
        border.color: AppTheme.dialogBorder
        border.width: 1
    }
}
