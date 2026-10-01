import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Niconeon

ApplicationWindow {
    id: root
    objectName: "mainWindow"
    width: 1280
    height: 800
    visible: true
    title: "Niconeon"
    font.pixelSize: application.appFontPixelSize
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

    // These are presentation-only dialog and toast states. Application state lives in C++.
    property bool pendingFileDialogOpen: false
    function openFileDialog() {
        if (fileDialogLoader.item) fileDialogLoader.item.open()
        else { pendingFileDialogOpen = true; fileDialogLoader.active = true }
    }
    function formatRate(rate) { return Number(rate).toFixed(Math.abs(rate * 10 - Math.round(rate * 10)) < 0.001 ? 1 : 2) }
    onClosing: function(close) { close.accepted = false; application.shutdown() }

    Connections {
        target: application
        function onToastRequested(message, actionText) {
            toast.message = message
            toast.actionText = actionText
            toast.visible = true
            toastTimer.restart()
        }
    }
    Timer { id: toastTimer; interval: 3500; onTriggered: toast.visible = false }
    Component {
        id: fileDialogComponent
        FileDialog {
            title: "動画ファイルを選択"
            onAccepted: application.openVideo(selectedFile.toString())
        }
    }
    Loader {
        id: fileDialogLoader
        active: false
        sourceComponent: fileDialogComponent
        onLoaded: {
            if (root.pendingFileDialogOpen && item) { root.pendingFileDialogOpen = false; item.open() }
        }
    }
    FilterDialog {
        id: filterDialog
        objectName: "filterDialog"
        regexFilters: application.regexFilters
        ngUsers: application.ngUsers
        onAddRequested: function(pattern) { application.addRegexFilter(pattern) }
        onRemoveNgUserRequested: function(userId) { application.removeNgUser(userId) }
        onRemoveRequested: function(filterId) { application.removeRegexFilter(filterId) }
    }
    PlaybackSpeedDialog {
        id: playbackSpeedDialog
        speedPresets: application.speedPresets
        currentSpeed: mpv.speed
        onSetSpeedRequested: function(rate) { application.setPlaybackRate(rate) }
        onAddPresetRequested: function(rawValue) { application.addSpeedPreset(rawValue) }
        onRemovePresetRequested: function(rate) { application.removeSpeedPreset(rate) }
    }
    AboutDialog { id: aboutDialog; objectName: "aboutDialog" }
    FontSizeDialog {
        id: fontSizeDialog
        currentLevel: application.fontSizeLevel
        onFontSizeSelected: function(level) { application.setFontSizeLevel(level) }
    }
    SettingsDialog {
        id: settingsDialog
        objectName: "settingsDialog"
        fontSizeLabel: ["小", "標準", "大"][application.fontSizeLevel]
        onOpenAboutRequested: aboutDialog.open()
        onOpenSpeedPresetRequested: playbackSpeedDialog.open()
        onOpenFontSizeRequested: fontSizeDialog.open()
    }
    Toast {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        visible: false
        onActionTriggered: application.undoNg()
    }
    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 8
            spacing: 8
            AppButton { text: "動画を開く"; objectName: "openButton"; onClicked: root.openFileDialog() }
            AppButton { text: mpv.paused ? "再生" : "一時停止"; objectName: "pauseButton"; onClicked: application.togglePause() }
            AppButton { text: root.formatRate(mpv.speed) + "x"; onClicked: application.cyclePlaybackSpeed() }
            AppButton { text: application.commentsVisible ? "コメント非表示" : "コメント表示"; objectName: "commentsButton"; onClicked: application.setCommentsVisible(!application.commentsVisible) }
            AppButton { text: application.perfLogEnabled ? "計測ログ停止" : "計測ログ開始"; onClicked: application.setPerfLogEnabled(!application.perfLogEnabled) }
            AppButton { text: "Profile: " + application.perfProfile; onClicked: application.cycleRuntimeProfile() }
            AppButton { text: "フィルタ"; onClicked: { application.refreshFilters(); filterDialog.open() } }
            AppButton { text: "設定"; objectName: "settingsButton"; onClicked: settingsDialog.open() }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            spacing: 8
            Label { text: "ファイル" }
            AppTextField {
                id: pathInput
                objectName: "pathInput"
                Layout.fillWidth: true
                placeholderText: "ファイル名に sm/nm/so ID を含めてください"
                text: application.selectedVideoPath
                onAccepted: application.openVideo(text)
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            spacing: 8
            Slider {
                id: seekSlider
                objectName: "seekSlider"
                Layout.fillWidth: true
                from: 0
                to: Math.max(1, mpv.durationMs)
                value: pressed ? value : mpv.positionMs
                onMoved: application.seek(value)
            }
            Label { text: Math.floor(mpv.positionMs / 1000) + " / " + Math.floor(mpv.durationMs / 1000) + " sec" }
            Label { text: "Vol" }
            Slider { from: 0; to: 100; value: mpv.volume; onMoved: application.setVolume(value); Layout.preferredWidth: 160 }
        }
        Item {
            id: playerArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.bottomMargin: 8
            clip: true
            Rectangle { anchors.fill: parent; color: "black"; radius: 8 }
            MpvItem { id: mpv; objectName: "player"; anchors.fill: parent }
            DanmakuOverlay {
                id: overlay
                anchors.fill: parent
                controller: application.danmaku
                visible: application.commentsVisible
                totalComments: application.totalComments
                videoFps: mpv.videoFps
                commentFps: application.danmaku.commentRenderFps
                activeCommentCount: application.danmaku.activeCommentCount
            }
            onWidthChanged: application.danmaku.setViewportSize(width, height)
            onHeightChanged: application.danmaku.setViewportSize(width, height)
        }
    }
    Component.onCompleted: {
        application.danmaku.setViewportSize(playerArea.width, playerArea.height)
        application.attachPlayer(mpv)
    }
}
