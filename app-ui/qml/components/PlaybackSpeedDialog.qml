import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    modal: true
    title: "再生速度設定"
    width: 440
    height: 440
    palette {
        window: "#1F2430"
        windowText: "#F3F6FF"
        base: "#141B28"
        text: "#F3F6FF"
        button: "#2E3950"
        buttonText: "#F3F6FF"
        placeholderText: "#9AA6BF"
        highlight: "#5A7FCF"
        highlightedText: "#FFFFFF"
    }
    background: Rectangle {
        radius: 10
        color: "#1F2430"
        border.color: "#3F4D67"
        border.width: 1
    }

    property var speedPresets: [1.0, 1.5, 2.0]
    property double currentSpeed: 1.0

    signal setSpeedRequested(double rate)
    signal addPresetRequested(string rawValue)
    signal removePresetRequested(double rate)

    function formatRate(rate) {
        const parsed = Number(rate)
        if (!isFinite(parsed)) {
            return "1.0"
        }
        if (Math.abs(parsed - Math.round(parsed)) < 0.001) {
            return Math.round(parsed).toFixed(1)
        }
        if (Math.abs(parsed * 10 - Math.round(parsed * 10)) < 0.001) {
            return parsed.toFixed(1)
        }
        return parsed.toFixed(2)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            text: "現在: " + root.formatRate(root.currentSpeed) + "x"
            font.bold: true
        }

        Label {
            text: "プリセットを選択して適用"
            color: root.palette.placeholderText
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 220
            clip: true
            model: root.speedPresets

            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: root.formatRate(modelData) + "x"
                }

                AppButton {
                    text: "適用"
                    onClicked: root.setSpeedRequested(Number(modelData))
                }

                AppButton {
                    text: "削除"
                    enabled: root.speedPresets.length > 1
                    onClicked: root.removePresetRequested(Number(modelData))
                }
            }
        }

        Label { text: "プリセット追加" }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: addPresetInput
                Layout.fillWidth: true
                placeholderText: "例: 1.75"
            }

            AppButton {
                text: "追加"
                onClicked: {
                    root.addPresetRequested(addPresetInput.text)
                    addPresetInput.text = ""
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignRight

            AppButton {
                text: "閉じる"
                onClicked: root.close()
            }
        }
    }
}
