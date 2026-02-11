import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Niconeon

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    visible: true
    title: "Niconeon"

    property string selectedVideoPath: ""
    property string sessionId: ""
    property string undoToken: ""
    property var regexFilters: []
    property var ngUsers: []

    function extractVideoId(path) {
        const match = path.toLowerCase().match(/(sm|nm|so)\d+/)
        return match ? match[0] : ""
    }

    function showToast(message, actionText) {
        toast.message = message
        toast.actionText = actionText || ""
        toast.visible = true
        toastTimer.restart()
    }

    CoreClient {
        id: coreClient
    }

    DanmakuController {
        id: danmakuController
        onNgDropRequested: function(userId) {
            coreClient.addNgUser(userId)
        }
    }

    Timer {
        id: playbackTickTimer
        interval: 33
        repeat: true
        running: true
        onTriggered: {
            if (root.sessionId !== "") {
                coreClient.playbackTick(root.sessionId, mpv.positionMs, mpv.paused, false)
            }
        }
    }

    Timer {
        id: toastTimer
        interval: 3500
        repeat: false
        onTriggered: toast.visible = false
    }

    FileDialog {
        id: fileDialog
        title: "動画ファイルを選択"
        onAccepted: {
            const path = selectedFile.toString().replace("file://", "")
            root.selectedVideoPath = decodeURIComponent(path)
            pathInput.text = root.selectedVideoPath
        }
    }

    FilterDialog {
        id: filterDialog
        regexFilters: root.regexFilters
        ngUsers: root.ngUsers
        onAddRequested: function(pattern) {
            coreClient.addRegexFilter(pattern)
        }
        onRemoveRequested: function(filterId) {
            coreClient.removeRegexFilter(filterId)
        }
    }

    Toast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        visible: false
        onActionTriggered: {
            if (root.undoToken !== "") {
                coreClient.undoLastNg(root.undoToken)
                root.undoToken = ""
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            spacing: 8

            Button {
                text: "動画を開く"
                onClicked: fileDialog.open()
            }

            TextField {
                id: pathInput
                Layout.fillWidth: true
                placeholderText: "ファイル名に sm/nm/so ID を含めてください"
                text: root.selectedVideoPath
            }

            Button {
                text: "再生開始"
                onClicked: {
                    root.selectedVideoPath = pathInput.text
                    const videoId = root.extractVideoId(root.selectedVideoPath)
                    if (videoId === "") {
                        mpv.openFile(root.selectedVideoPath)
                        root.sessionId = ""
                        showToast("動画IDが見つからないためコメント取得をスキップしました")
                        return
                    }

                    if (!mpv.openFile(root.selectedVideoPath)) {
                        showToast("動画を開けませんでした")
                        return
                    }

                    coreClient.openVideo(root.selectedVideoPath, videoId)
                }
            }

            Button {
                text: mpv.paused ? "再生" : "一時停止"
                onClicked: mpv.togglePause()
            }

            Button {
                text: "フィルタ"
                onClicked: {
                    coreClient.listFilters()
                    filterDialog.open()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            spacing: 8

            Slider {
                id: seekSlider
                Layout.fillWidth: true
                from: 0
                to: Math.max(1, mpv.durationMs)
                value: pressed ? value : mpv.positionMs
                onMoved: {
                    mpv.seek(value)
                    if (root.sessionId !== "") {
                        coreClient.playbackTick(root.sessionId, value, mpv.paused, true)
                    }
                }
            }

            Label {
                text: Math.floor(mpv.positionMs / 1000) + " / " + Math.floor(mpv.durationMs / 1000) + " sec"
            }

            Label { text: "Vol" }

            Slider {
                id: volumeSlider
                from: 0
                to: 100
                value: mpv.volume
                onValueChanged: mpv.volume = value
                Layout.preferredWidth: 160
            }
        }

        Item {
            id: playerArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.bottomMargin: 8

            Rectangle {
                anchors.fill: parent
                color: "black"
                radius: 8
            }

            MpvItem {
                id: mpv
                anchors.fill: parent
            }

            DanmakuOverlay {
                id: overlay
                anchors.fill: parent
                controller: danmakuController
            }

            onWidthChanged: danmakuController.setViewportSize(width, height)
            onHeightChanged: danmakuController.setViewportSize(width, height)
        }
    }

    Connections {
        target: coreClient

        function onResponseReceived(method, result, error) {
            if (error && error !== "") {
                if (method === "add_regex_filter") {
                    showToast("正規表現が不正です")
                } else if (method === "open_video") {
                    showToast("コメント取得に失敗しました（再生は継続）")
                } else {
                    showToast("Core error: " + error)
                }
                return
            }

            if (method === "open_video") {
                root.sessionId = result.session_id
                showToast("コメント取得: " + result.comment_source + " / " + result.total_comments + "件")
            } else if (method === "playback_tick") {
                danmakuController.appendFromCore(result.emit_comments || [])
            } else if (method === "add_ng_user") {
                danmakuController.applyNgUserFade(result.hidden_user_id)
                if (result.applied) {
                    root.undoToken = result.undo_token
                    showToast("NGユーザーを登録しました", "Undo")
                } else {
                    showToast("このユーザーは既にNG登録済みです")
                }
                coreClient.listFilters()
            } else if (method === "undo_last_ng") {
                if (result.restored) {
                    showToast("NG登録を取り消しました")
                } else {
                    showToast("Undo可能なNG登録がありません")
                }
                coreClient.listFilters()
            } else if (method === "add_regex_filter") {
                showToast("正規表現フィルタを追加しました")
                coreClient.listFilters()
            } else if (method === "remove_regex_filter") {
                coreClient.listFilters()
            } else if (method === "list_filters") {
                root.ngUsers = result.ng_users || []
                root.regexFilters = result.regex_filters || []
            }
        }

        function onCoreCrashed(reason) {
            showToast(reason)
        }
    }

    Component.onCompleted: {
        coreClient.startDefault()
        danmakuController.setViewportSize(playerArea.width, playerArea.height)
        danmakuController.setLaneMetrics(36, 6)
    }
}
