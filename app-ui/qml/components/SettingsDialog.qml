import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

AppDialog {
    id: root
    modal: true
    title: "設定"
    width: 420
    height: 260

    property string fontSizeLabel: "標準"

    signal openAboutRequested()
    signal openSpeedPresetRequested()
    signal openFontSizeRequested()

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            text: "設定メニュー"
            font.bold: true
        }

        AppButton {
            Layout.fillWidth: true
            text: "About"
            onClicked: root.openAboutRequested()
        }

        AppButton {
            Layout.fillWidth: true
            text: "速度プリセット"
            onClicked: root.openSpeedPresetRequested()
        }

        AppButton {
            Layout.fillWidth: true
            text: "フォントサイズ（現在: " + root.fontSizeLabel + "）"
            onClicked: root.openFontSizeRequested()
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
