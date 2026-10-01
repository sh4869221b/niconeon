pragma Singleton

import QtQuick

QtObject {
    id: theme

    readonly property var systemPalette: SystemPalette {
        colorGroup: SystemPalette.Active
    }

    function channelToLinear(value) {
        const normalized = value / 255.0
        if (normalized <= 0.03928) {
            return normalized / 12.92
        }
        return Math.pow((normalized + 0.055) / 1.055, 2.4)
    }

    function luminance(color) {
        const r = channelToLinear(color.r * 255.0)
        const g = channelToLinear(color.g * 255.0)
        const b = channelToLinear(color.b * 255.0)
        return 0.2126 * r + 0.7152 * g + 0.0722 * b
    }

    readonly property bool darkMode: luminance(systemPalette.window) < 0.5

    readonly property color window: darkMode ? "#1F2430" : "#F5F7FB"
    readonly property color windowText: darkMode ? "#F3F6FF" : "#1D2638"
    readonly property color base: darkMode ? "#141B28" : "#FFFFFF"
    readonly property color text: darkMode ? "#F3F6FF" : "#1D2638"
    readonly property color button: darkMode ? "#2E3950" : "#E3E9F5"
    readonly property color buttonText: darkMode ? "#F3F6FF" : "#1D2638"
    readonly property color placeholderText: darkMode ? "#9AA6BF" : "#677792"
    readonly property color highlight: darkMode ? "#5A7FCF" : "#2F6FEB"
    readonly property color highlightedText: "#FFFFFF"

    readonly property color dialogBorder: darkMode ? "#3F4D67" : "#B9C4D9"
    readonly property color textInputBackground: darkMode ? "#141B28" : "#FFFFFF"
    readonly property color textInputBorder: darkMode ? "#5A6A8A" : "#AAB7CD"
    readonly property color textInputFocusBorder: darkMode ? "#9CB9FF" : "#2F6FEB"

    readonly property color tabBackground: darkMode ? "#2A3348" : "#E9EEF8"
    readonly property color tabHoverBackground: darkMode ? "#32405C" : "#DFE7F5"
    readonly property color tabCheckedBackground: darkMode ? "#425678" : "#C9D7EF"
    readonly property color tabBorder: darkMode ? "#5A6A8A" : "#AAB7CD"
    readonly property color tabCheckedBorder: darkMode ? "#9CB9FF" : "#2F6FEB"
    readonly property color tabText: darkMode ? "#E5EBFA" : "#233148"
    readonly property color tabCheckedText: darkMode ? "#FFFFFF" : "#1D2638"

    readonly property color buttonTextDisabled: darkMode ? "#9AA6BF" : "#8B97AE"
    readonly property color buttonBorder: darkMode ? "#5A6A8A" : "#9BA9C2"
    readonly property color buttonFocusBorder: darkMode ? "#9CB9FF" : "#2F6FEB"
    readonly property color buttonDisabledBackground: darkMode ? "#2A2F3A" : "#E4E9F2"
    readonly property color buttonPressedBackground: darkMode ? "#3B4A67" : "#CCD8EC"
    readonly property color buttonHoverBackground: darkMode ? "#34415C" : "#D8E1F0"
    readonly property color buttonDefaultBackground: darkMode ? "#2E3950" : "#E3E9F5"

    readonly property color toastBackground: darkMode ? "#E0212430" : "#F2FFFFFF"
    readonly property color toastBorder: darkMode ? "#66FFFFFF" : "#668190A9"
    readonly property color toastText: darkMode ? "#FFFFFF" : "#1D2638"

    readonly property color ngDropBackground: darkMode ? "#CC7A0012" : "#FCE8EC"
    readonly property color ngDropBorder: darkMode ? "#FFFF4466" : "#D63858"
    readonly property color ngDropText: darkMode ? "#FFFFFF" : "#6B1222"
}
