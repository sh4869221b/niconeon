import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    modal: true
    title: "フィルタ設定"
    standardButtons: Dialog.Close
    width: 560
    height: 460

    property var regexFilters: []
    property var ngUsers: []

    signal addRequested(string pattern)
    signal removeRequested(int filterId)
    signal removeNgUserRequested(string userId)

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        Label { text: "NGユーザー" }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: 120
            model: root.ngUsers
            clip: true

            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: modelData
                    color: "#333"
                    elide: Text.ElideRight
                }

                Button {
                    text: "削除"
                    onClicked: root.removeNgUserRequested(modelData)
                }
            }
        }

        Label { text: "正規表現フィルタ" }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TextField {
                id: regexInput
                Layout.fillWidth: true
                placeholderText: "例: (草|www)+"
            }

            Button {
                text: "追加"
                onClicked: {
                    if (regexInput.text.trim() === "") {
                        return
                    }
                    root.addRequested(regexInput.text)
                    regexInput.text = ""
                }
            }
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.regexFilters

            delegate: RowLayout {
                required property var modelData
                width: ListView.view.width
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: modelData.pattern
                    elide: Text.ElideRight
                }

                Button {
                    text: "削除"
                    onClicked: root.removeRequested(modelData.filter_id)
                }
            }
        }
    }
}
