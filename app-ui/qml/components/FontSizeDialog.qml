import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

AppDialog {
    id: root
    modal: true
    title: "フォントサイズ設定"
    width: 420
    height: 260

    property int currentLevel: 1

    signal fontSizeSelected(int level)

    function optionText(level) {
        if (level === 0) {
            return "小"
        }
        if (level === 2) {
            return "大"
        }
        return "標準"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            text: "表示フォントサイズを選択"
            font.bold: true
        }

        Repeater {
            model: [0, 1, 2]
            delegate: RadioButton {
                required property int modelData
                text: root.optionText(modelData)
                checked: root.currentLevel === modelData
                onClicked: {
                    root.fontSizeSelected(modelData)
                }
            }
        }

        Item { Layout.fillHeight: true }

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
