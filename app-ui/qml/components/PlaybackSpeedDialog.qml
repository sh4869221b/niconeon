import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    modal: true
    title: "再生速度設定"
    standardButtons: Dialog.Close
    width: 440
    height: 440

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
            color: "#555"
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

                Button {
                    text: "適用"
                    onClicked: root.setSpeedRequested(Number(modelData))
                }

                Button {
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

            Button {
                text: "追加"
                onClicked: {
                    root.addPresetRequested(addPresetInput.text)
                    addPresetInput.text = ""
                }
            }
        }
    }
}
