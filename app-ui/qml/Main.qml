import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore
import Niconeon

ApplicationWindow {
    id: root
    width: 1280
    height: 800
    visible: true
    title: "Niconeon"
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

    property string selectedVideoPath: ""
    property string sessionId: ""
    property string undoToken: ""
    property var regexFilters: []
    property var ngUsers: []
    property var speedPresets: [1.0, 1.5, 2.0]
    property bool pendingSeek: false
    property int pendingSeekTargetMs: 0
    property bool commentsVisible: true
    property bool perfLogEnabled: false
    property string perfProfile: "low_spec"
    property int targetFps: 30
    property int maxEmitPerTick: 48
    property bool coalesceSameContent: true
    property int perfTickSentCount: 0
    property int perfTickResultCount: 0
    property int perfDroppedCommentsCount: 0
    property int perfCoalescedCommentsCount: 0
    property int perfEmitOverBudgetCount: 0
    property int qosOverBudgetStreak: 0
    property int qosStableStreak: 0
    property string autoVideoPath: ""
    property bool autoPerfLogStart: false
    property int autoExitMs: 0

    Settings {
        id: speedSettings
        category: "playback"
        property real rate: 1.0
        property string ratePresetsJson: "[1.0,1.5,2.0]"
    }

    Settings {
        id: uiSettings
        category: "ui"
        property bool commentsVisible: true
        property bool perfLogEnabled: false
        property string perfProfile: "low_spec"
        property int targetFps: 30
        property int maxEmitPerTick: 48
        property bool coalesceSameContent: true
    }

    function extractVideoId(path) {
        const match = path.toLowerCase().match(/(sm|nm|so)\d+/)
        return match ? match[0] : ""
    }

    function normalizeRate(value) {
        const parsed = Number(value)
        if (!isFinite(parsed)) {
            return NaN
        }
        const clamped = Math.max(0.5, Math.min(3.0, parsed))
        return Math.round(clamped * 100) / 100
    }

    function rateEquals(a, b) {
        return Math.abs(a - b) < 0.001
    }

    function normalizePresetList(values) {
        const list = Array.isArray(values) ? values : []
        const normalized = []

        for (let i = 0; i < list.length; i += 1) {
            const rate = normalizeRate(list[i])
            if (isNaN(rate)) {
                continue
            }
            let exists = false
            for (let j = 0; j < normalized.length; j += 1) {
                if (rateEquals(normalized[j], rate)) {
                    exists = true
                    break
                }
            }
            if (!exists) {
                normalized.push(rate)
            }
        }

        normalized.sort(function(a, b) { return a - b })
        if (normalized.length === 0) {
            return [1.0, 1.5, 2.0]
        }
        return normalized
    }

    function nearestPreset(rate) {
        if (!Array.isArray(root.speedPresets) || root.speedPresets.length === 0) {
            return 1.0
        }

        const target = normalizeRate(rate)
        const safeTarget = isNaN(target) ? 1.0 : target

        let best = root.speedPresets[0]
        let diff = Math.abs(best - safeTarget)
        for (let i = 1; i < root.speedPresets.length; i += 1) {
            const candidate = root.speedPresets[i]
            const candidateDiff = Math.abs(candidate - safeTarget)
            if (candidateDiff < diff) {
                best = candidate
                diff = candidateDiff
            }
        }
        return best
    }

    function formatRate(rate) {
        const value = normalizeRate(rate)
        const safe = isNaN(value) ? 1.0 : value
        if (Math.abs(safe - Math.round(safe)) < 0.001) {
            return Math.round(safe).toFixed(1)
        }
        if (Math.abs(safe * 10 - Math.round(safe * 10)) < 0.001) {
            return safe.toFixed(1)
        }
        return safe.toFixed(2)
    }

    function applyPlaybackRate(rate, persist) {
        const applied = nearestPreset(rate)
        mpv.speed = applied
        danmakuController.setPlaybackRate(applied)
        if (persist) {
            speedSettings.rate = applied
        }
    }

    function cyclePlaybackSpeed() {
        const current = nearestPreset(mpv.speed)
        let index = 0
        for (let i = 0; i < root.speedPresets.length; i += 1) {
            if (rateEquals(root.speedPresets[i], current)) {
                index = i
                break
            }
        }
        const next = root.speedPresets[(index + 1) % root.speedPresets.length]
        applyPlaybackRate(next, true)
        showToast("再生速度: " + formatRate(next) + "x")
    }

    function loadSpeedSettings() {
        let parsedPresets = [1.0, 1.5, 2.0]
        try {
            parsedPresets = JSON.parse(speedSettings.ratePresetsJson)
        } catch (e) {
            parsedPresets = [1.0, 1.5, 2.0]
        }

        root.speedPresets = normalizePresetList(parsedPresets)
        speedSettings.ratePresetsJson = JSON.stringify(root.speedPresets)

        const restored = nearestPreset(speedSettings.rate)
        speedSettings.rate = restored
        applyPlaybackRate(restored, false)
    }

    function handleAddSpeedPreset(rawValue) {
        const rate = normalizeRate(rawValue)
        if (isNaN(rate)) {
            showToast("0.5〜3.0 の範囲で速度を入力してください")
            return
        }

        const merged = normalizePresetList(root.speedPresets.concat([rate]))
        if (merged.length === root.speedPresets.length) {
            showToast("同じ速度プリセットは追加できません")
            return
        }

        root.speedPresets = merged
        speedSettings.ratePresetsJson = JSON.stringify(root.speedPresets)
        showToast("速度プリセットを追加しました")
    }

    function handleRemoveSpeedPreset(rate) {
        if (root.speedPresets.length <= 1) {
            showToast("最低1つの速度プリセットが必要です")
            return
        }

        const filtered = []
        for (let i = 0; i < root.speedPresets.length; i += 1) {
            if (!rateEquals(root.speedPresets[i], rate)) {
                filtered.push(root.speedPresets[i])
            }
        }
        root.speedPresets = normalizePresetList(filtered)
        speedSettings.ratePresetsJson = JSON.stringify(root.speedPresets)

        const adjusted = nearestPreset(mpv.speed)
        applyPlaybackRate(adjusted, true)
        showToast("速度プリセットを削除しました")
    }

    function applyCommentVisibility(visible, notify) {
        const next = !!visible
        if (root.commentsVisible === next) {
            return
        }

        root.commentsVisible = next
        uiSettings.commentsVisible = next
        danmakuController.resetForSeek()

        if (next && root.sessionId !== "") {
            root.pendingSeek = true
            root.pendingSeekTargetMs = mpv.positionMs
            coreClient.enqueuePlaybackTick(root.sessionId, mpv.positionMs, mpv.paused, true)
            root.perfTickSentCount += 1
        } else {
            root.pendingSeek = false
        }

        if (notify) {
            showToast(next ? "コメント表示を有効化しました" : "コメント表示を無効化しました")
        }
    }

    function applyPerfLogEnabled(enabled, notify) {
        const next = !!enabled
        if (root.perfLogEnabled === next) {
            return
        }

        root.perfLogEnabled = next
        uiSettings.perfLogEnabled = next
        danmakuController.setPerfLogEnabled(next)
        root.perfTickSentCount = 0
        root.perfTickResultCount = 0
        root.perfDroppedCommentsCount = 0
        root.perfCoalescedCommentsCount = 0
        root.perfEmitOverBudgetCount = 0
        root.qosOverBudgetStreak = 0
        root.qosStableStreak = 0

        if (notify) {
            showToast(next ? "計測ログを有効化しました" : "計測ログを無効化しました")
        }
    }

    function sanitizePerfProfile(value) {
        const raw = String(value || "").trim().toLowerCase()
        if (raw === "high" || raw === "balanced" || raw === "low_spec") {
            return raw
        }
        return "low_spec"
    }

    function defaultRuntimeProfileConfig(profile) {
        const normalized = sanitizePerfProfile(profile)
        if (normalized === "high") {
            return {
                targetFps: 30,
                maxEmitPerTick: 0,
                coalesceSameContent: false
            }
        }
        if (normalized === "balanced") {
            return {
                targetFps: 30,
                maxEmitPerTick: 96,
                coalesceSameContent: false
            }
        }
        return {
            targetFps: 30,
            maxEmitPerTick: 48,
            coalesceSameContent: true
        }
    }

    function applyRuntimeProfile(profile, targetFps, maxEmitPerTick, coalesceSameContent, notify) {
        const normalizedProfile = sanitizePerfProfile(profile)
        const normalizedTargetFps = Math.max(10, Math.min(120, Number(targetFps) || 30))
        const normalizedMaxEmit = Math.max(0, Math.min(2000, Number(maxEmitPerTick) || 0))
        const normalizedCoalesce = !!coalesceSameContent

        root.perfProfile = normalizedProfile
        root.targetFps = normalizedTargetFps
        root.maxEmitPerTick = normalizedMaxEmit
        root.coalesceSameContent = normalizedCoalesce
        uiSettings.perfProfile = normalizedProfile
        uiSettings.targetFps = normalizedTargetFps
        uiSettings.maxEmitPerTick = normalizedMaxEmit
        uiSettings.coalesceSameContent = normalizedCoalesce

        danmakuController.setTargetFps(normalizedTargetFps)
        coreClient.setRuntimeProfile(
            normalizedProfile,
            normalizedTargetFps,
            normalizedMaxEmit,
            normalizedCoalesce ? 1 : 0)

        if (notify) {
            showToast(
                "Runtime profile: " + normalizedProfile
                + " / target_fps=" + normalizedTargetFps
                + " / emit_cap=" + normalizedMaxEmit)
        }
    }

    function handleQosWindowFeedback(tickBacklog) {
        const backlog = Math.max(0, Number(tickBacklog) || 0)
        if (root.perfEmitOverBudgetCount > 0 || backlog >= 3) {
            root.qosOverBudgetStreak += 1
            root.qosStableStreak = 0
        } else {
            root.qosStableStreak += 1
            root.qosOverBudgetStreak = Math.max(0, root.qosOverBudgetStreak - 1)
        }

        if (root.qosOverBudgetStreak >= 2) {
            const currentCap = root.maxEmitPerTick <= 0 ? 128 : root.maxEmitPerTick
            const nextCap = Math.max(16, currentCap - 8)
            if (root.maxEmitPerTick <= 0 || nextCap < root.maxEmitPerTick) {
                applyRuntimeProfile(root.perfProfile, root.targetFps, nextCap, root.coalesceSameContent, false)
            }
            root.qosOverBudgetStreak = 0
        } else if (root.qosStableStreak >= 4) {
            const nextCap = Math.min(128, root.maxEmitPerTick + 4)
            if (nextCap > root.maxEmitPerTick) {
                applyRuntimeProfile(root.perfProfile, root.targetFps, nextCap, root.coalesceSameContent, false)
            }
            root.qosStableStreak = 0
        }
    }

    function cycleRuntimeProfile() {
        const order = ["high", "balanced", "low_spec"]
        const current = sanitizePerfProfile(root.perfProfile)
        let index = order.indexOf(current)
        if (index < 0) {
            index = 0
        }
        const next = order[(index + 1) % order.length]
        const defaults = defaultRuntimeProfileConfig(next)
        applyRuntimeProfile(next, defaults.targetFps, defaults.maxEmitPerTick, defaults.coalesceSameContent, true)
    }

    function showToast(message, actionText) {
        toast.message = message
        toast.actionText = actionText || ""
        toast.visible = true
        toastTimer.restart()
    }

    function startPlaybackFromPath(path) {
        const candidatePath = String(path || "").trim()
        if (candidatePath === "") {
            showToast("動画ファイルを選択してください")
            return
        }

        root.selectedVideoPath = candidatePath
        pathInput.text = root.selectedVideoPath

        const videoId = root.extractVideoId(root.selectedVideoPath)
        if (videoId === "") {
            if (!mpv.openFile(root.selectedVideoPath)) {
                showToast("動画を開けませんでした")
                return
            }
            root.applyPlaybackRate(speedSettings.rate, false)
            root.sessionId = ""
            showToast("動画IDが見つからないためコメント取得をスキップしました")
            return
        }

        if (!mpv.openFile(root.selectedVideoPath)) {
            showToast("動画を開けませんでした")
            return
        }

        root.applyPlaybackRate(speedSettings.rate, false)
        coreClient.openVideo(root.selectedVideoPath, videoId)
    }

    function fileUrlToLocalPath(value) {
        const raw = decodeURIComponent(String(value))
        if (!raw.startsWith("file:")) {
            return raw
        }

        let path = raw
        if (raw.startsWith("file://")) {
            path = raw.substring(7)
        } else if (raw.startsWith("file:/")) {
            path = raw.substring(5)
        }

        if (Qt.platform.os === "windows" && /^\/[A-Za-z]:\//.test(path)) {
            path = path.substring(1)
        }

        return path
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
        interval: 50
        repeat: true
        running: true
        onTriggered: {
            if (root.sessionId !== "" && root.commentsVisible) {
                let isSeekTick = false
                if (root.pendingSeek) {
                    isSeekTick = true
                    if (Math.abs(mpv.positionMs - root.pendingSeekTargetMs) < 250) {
                        root.pendingSeek = false
                    }
                }
                coreClient.enqueuePlaybackTick(root.sessionId, mpv.positionMs, mpv.paused, isSeekTick)
                root.perfTickSentCount += 1
            }
        }
    }

    Timer {
        id: perfLogTimer
        interval: 2000
        repeat: true
        running: true
        onTriggered: {
            const tickBacklog = Math.max(0, root.perfTickSentCount - root.perfTickResultCount)
            if (root.perfLogEnabled) {
                console.log(
                    "[perf-ui] window_ms=" + perfLogTimer.interval
                    + " tick_sent=" + root.perfTickSentCount
                    + " tick_result=" + root.perfTickResultCount
                    + " tick_backlog=" + tickBacklog
                    + " dropped_comments=" + root.perfDroppedCommentsCount
                    + " coalesced_comments=" + root.perfCoalescedCommentsCount
                    + " emit_over_budget=" + root.perfEmitOverBudgetCount
                    + " comments_visible=" + (root.commentsVisible ? "1" : "0")
                    + " position_ms=" + mpv.positionMs
                    + " paused=" + (mpv.paused ? "1" : "0")
                    + " speed=" + root.formatRate(mpv.speed)
                    + " profile=" + root.perfProfile
                    + " target_fps=" + root.targetFps
                    + " emit_cap=" + root.maxEmitPerTick
                )
            }
            root.handleQosWindowFeedback(tickBacklog)
            root.perfTickSentCount = 0
            root.perfTickResultCount = 0
            root.perfDroppedCommentsCount = 0
            root.perfCoalescedCommentsCount = 0
            root.perfEmitOverBudgetCount = 0
        }
    }

    Timer {
        id: toastTimer
        interval: 3500
        repeat: false
        onTriggered: toast.visible = false
    }

    Timer {
        id: autoExitTimer
        repeat: false
        onTriggered: Qt.quit()
    }

    FileDialog {
        id: fileDialog
        title: "動画ファイルを選択"
        onAccepted: {
            const path = fileUrlToLocalPath(selectedFile)
            root.startPlaybackFromPath(path)
        }
    }

    FilterDialog {
        id: filterDialog
        regexFilters: root.regexFilters
        ngUsers: root.ngUsers
        onAddRequested: function(pattern) {
            coreClient.addRegexFilter(pattern)
        }
        onRemoveNgUserRequested: function(userId) {
            coreClient.removeNgUser(userId)
        }
        onRemoveRequested: function(filterId) {
            coreClient.removeRegexFilter(filterId)
        }
    }

    PlaybackSpeedDialog {
        id: playbackSpeedDialog
        speedPresets: root.speedPresets
        currentSpeed: mpv.speed
        onSetSpeedRequested: function(rate) {
            root.applyPlaybackRate(rate, true)
            root.showToast("再生速度: " + root.formatRate(rate) + "x")
        }
        onAddPresetRequested: function(rawValue) {
            root.handleAddSpeedPreset(rawValue)
        }
        onRemovePresetRequested: function(rate) {
            root.handleRemoveSpeedPreset(rate)
        }
    }

    AboutDialog {
        id: aboutDialog
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

            AppButton {
                text: "動画を開く"
                onClicked: fileDialog.open()
            }

            AppButton {
                text: mpv.paused ? "再生" : "一時停止"
                onClicked: mpv.togglePause()
            }

            AppButton {
                text: root.formatRate(mpv.speed) + "x"
                onClicked: root.cyclePlaybackSpeed()
            }

            AppButton {
                text: "速度設定"
                onClicked: playbackSpeedDialog.open()
            }

            AppButton {
                text: root.commentsVisible ? "コメント非表示" : "コメント表示"
                onClicked: root.applyCommentVisibility(!root.commentsVisible, true)
            }

            AppButton {
                text: root.perfLogEnabled ? "計測ログ停止" : "計測ログ開始"
                onClicked: root.applyPerfLogEnabled(!root.perfLogEnabled, true)
            }

            AppButton {
                text: "Profile: " + root.perfProfile
                onClicked: root.cycleRuntimeProfile()
            }

            AppButton {
                text: "フィルタ"
                onClicked: {
                    coreClient.listFilters()
                    filterDialog.open()
                }
            }

            AppButton {
                text: "About"
                onClicked: aboutDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            spacing: 8

            Label {
                text: "ファイル"
            }

            TextField {
                id: pathInput
                Layout.fillWidth: true
                placeholderText: "ファイル名に sm/nm/so ID を含めてください"
                text: root.selectedVideoPath
                onAccepted: root.startPlaybackFromPath(text)
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
                    root.pendingSeek = true
                    root.pendingSeekTargetMs = value
                    danmakuController.resetForSeek()
                    mpv.seek(value)
                    if (root.sessionId !== "" && root.commentsVisible) {
                        coreClient.enqueuePlaybackTick(root.sessionId, value, mpv.paused, true)
                        root.perfTickSentCount += 1
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
            clip: true

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
                visible: root.commentsVisible
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
                } else if (method === "set_runtime_profile") {
                    // 起動直後にcore未起動だと失敗し得るため、トーストは抑制する。
                } else {
                    showToast("Core error: " + error)
                }
                return
            }

            if (method === "open_video") {
                root.sessionId = result.session_id
                root.pendingSeek = false
                danmakuController.resetGlyphSession()
                showToast("コメント取得: " + result.comment_source + " / " + result.total_comments + "件")
            } else if (method === "playback_tick_batch") {
                const processedTicks = Number(result.processed_ticks || 0)
                root.perfTickResultCount += processedTicks > 0 ? processedTicks : 1
                root.perfDroppedCommentsCount += Number(result.dropped_comments || 0)
                root.perfCoalescedCommentsCount += Number(result.coalesced_comments || 0)
                if (Boolean(result.emit_over_budget || false)) {
                    root.perfEmitOverBudgetCount += 1
                }
                if (root.commentsVisible) {
                    danmakuController.appendFromCore(result.emit_comments || [], mpv.positionMs)
                }
            } else if (method === "set_runtime_profile") {
                if (result && typeof result === "object") {
                    root.perfProfile = root.sanitizePerfProfile(result.profile || root.perfProfile)
                    root.targetFps = Math.max(10, Math.min(120, Number(result.target_fps || root.targetFps)))
                    root.maxEmitPerTick = Math.max(0, Math.min(2000, Number(result.max_emit_per_tick || root.maxEmitPerTick)))
                    root.coalesceSameContent = Boolean(result.coalesce_same_content)
                    uiSettings.perfProfile = root.perfProfile
                    uiSettings.targetFps = root.targetFps
                    uiSettings.maxEmitPerTick = root.maxEmitPerTick
                    uiSettings.coalesceSameContent = root.coalesceSameContent
                    danmakuController.setTargetFps(root.targetFps)
                }
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
            } else if (method === "remove_ng_user") {
                if (result.removed) {
                    showToast("NGユーザーを削除しました")
                } else {
                    showToast("指定ユーザーはNG登録されていません")
                }
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

    Connections {
        target: mpv
        function onPausedChanged() {
            danmakuController.setPlaybackPaused(mpv.paused)
        }
        function onSpeedChanged() {
            danmakuController.setPlaybackRate(mpv.speed)
            speedSettings.rate = root.nearestPreset(mpv.speed)
        }
    }

    Component.onCompleted: {
        loadSpeedSettings()
        root.commentsVisible = uiSettings.commentsVisible
        root.perfLogEnabled = uiSettings.perfLogEnabled
        root.perfProfile = root.sanitizePerfProfile(uiSettings.perfProfile)
        root.targetFps = Math.max(10, Math.min(120, Number(uiSettings.targetFps || 30)))
        root.maxEmitPerTick = Math.max(0, Math.min(2000, Number(uiSettings.maxEmitPerTick || 48)))
        root.coalesceSameContent = !!uiSettings.coalesceSameContent
        coreClient.startDefault()
        danmakuController.setViewportSize(playerArea.width, playerArea.height)
        danmakuController.setLaneMetrics(36, 6)
        danmakuController.setPlaybackPaused(mpv.paused)
        danmakuController.setPlaybackRate(mpv.speed)
        danmakuController.setTargetFps(root.targetFps)
        danmakuController.setPerfLogEnabled(root.perfLogEnabled)
        danmakuController.setGlyphWarmupEnabled(true)
        coreClient.setRuntimeProfile(
            root.perfProfile,
            root.targetFps,
            root.maxEmitPerTick,
            root.coalesceSameContent ? 1 : 0)
        if (!root.commentsVisible) {
            danmakuController.resetForSeek()
        }
        if (root.autoPerfLogStart) {
            root.applyPerfLogEnabled(true, false)
        }
        const autoPath = String(root.autoVideoPath || "").trim()
        if (autoPath !== "") {
            root.startPlaybackFromPath(autoPath)
        }
        if (root.autoExitMs > 0) {
            autoExitTimer.interval = root.autoExitMs
            autoExitTimer.start()
        }
    }
}
