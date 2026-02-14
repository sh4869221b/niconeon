import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Niconeon

Dialog {
    id: root
    modal: true
    title: "About / ライセンス"
    width: 760
    height: 560
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

    property string appLicenseText: ""
    property string distributionLicenseText: ""
    property string thirdPartyNoticesText: ""
    property bool loaded: false

    LicenseProvider {
        id: licenseProvider
    }

    function ensureLoaded() {
        if (loaded) {
            return
        }
        appLicenseText = licenseProvider.readText(
            ":/licenses/LICENSE",
            "LICENSE を読み込めませんでした。配布物に同梱された LICENSE を確認してください。"
        )
        distributionLicenseText = licenseProvider.readText(
            ":/licenses/COPYING",
            "COPYING を読み込めませんでした。配布物に同梱された COPYING を確認してください。"
        )
        thirdPartyNoticesText = licenseProvider.readText(
            ":/licenses/THIRD_PARTY_NOTICES.txt",
            "THIRD_PARTY_NOTICES.txt を読み込めませんでした。配布物に同梱されたファイルを確認してください。"
        )
        loaded = true
    }

    onOpened: ensureLoaded()

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label {
            text: "Niconeon"
            font.pixelSize: 20
            font.bold: true
        }

        Label {
            Layout.fillWidth: true
            text: "Source: MIT / Distribution: GPLv3+ / Third-Party Notices"
            color: root.palette.placeholderText
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true

            TabButton { text: "MIT License" }
            TabButton { text: "Distribution (GPLv3+)" }
            TabButton { text: "Third-Party Notices" }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: tabs.currentIndex

            ScrollView {
                clip: true

                TextArea {
                    readOnly: true
                    wrapMode: TextArea.WrapAnywhere
                    text: root.appLicenseText
                }
            }

            ScrollView {
                clip: true

                TextArea {
                    readOnly: true
                    wrapMode: TextArea.WrapAnywhere
                    text: root.distributionLicenseText
                }
            }

            ScrollView {
                clip: true

                TextArea {
                    readOnly: true
                    wrapMode: TextArea.WrapAnywhere
                    text: root.thirdPartyNoticesText
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
