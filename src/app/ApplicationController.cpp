#include "app/ApplicationController.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace niconeon {
ApplicationController::ApplicationController(ServiceOptions options, QObject *parent)
    : QObject(parent), m_service(std::move(options)), m_danmaku(this) {
    QVector<double> values;
    const auto savedPresets =
        QJsonDocument::fromJson(m_settings.value("playback/ratePresetsJson", "[1.0,1.5,2.0]").toByteArray());
    for (const auto value : savedPresets.array())
        if (value.isDouble())
            values.push_back(value.toDouble());
    m_presets = normalizedPresets(values);
    m_commentsVisible = m_settings.value("ui/commentsVisible", true).toBool();
    m_perfLog = m_settings.value("ui/perfLogEnabled", false).toBool();
    m_fontLevel = std::clamp(m_settings.value("ui/fontSizeLevel", 1).toInt(), 0, 2);
    auto profile = parseRuntimeProfile(m_settings.value("ui/perfProfile", "low_spec").toString());
    m_selectedProfile = profile ? *profile : RuntimeProfile::LowSpec;
    const auto defaults = makeRuntimeProfile(m_selectedProfile);
    RuntimeProfileOverrides overrides;
    overrides.targetFps = m_settings.value("ui/targetFps", defaults.targetFps).toInt();
    overrides.maxEmitPerTick = m_settings.value("ui/maxEmitPerTick", static_cast<int>(defaults.maxEmitPerTick)).toInt();
    overrides.coalesceSameContent = m_settings.value("ui/coalesceSameContent", defaults.coalesceSameContent).toBool();
    m_profile = makeRuntimeProfile(m_selectedProfile, overrides);
    m_danmaku.setLaneMetrics(36, 6);
    m_danmaku.setTargetFps(m_profile.targetFps);
    m_danmaku.setPerfLogEnabled(m_perfLog);
    m_danmaku.setGlyphWarmupEnabled(true);
    connect(&m_service, &CommentService::ready, this, [this] { m_service.setRuntimeProfile(m_profile); });
    connect(&m_service, &CommentService::opened, this, [this](quint64, CommentSource source, qsizetype count) {
        m_totalComments = count;
        m_danmaku.resetGlyphSession();
        emit changed();
        if (!m_selectedPath.isEmpty())
            toast(QStringLiteral("コメント取得: %1 / %2件").arg(commentSourceName(source)).arg(count));
        if (m_player && m_commentsVisible)
            m_service.requestTick(m_player->positionMs(), m_player->paused(), true);
    });
    connect(&m_service, &CommentService::commentsReady, this, [this](const PlaybackBatchResult &result) {
        m_results += static_cast<int>(result.processedTicks);
        m_dropped += result.droppedComments;
        m_coalesced += result.coalescedComments;
        if (result.emitOverBudget)
            ++m_overBudget;
        if (!m_commentsVisible || result.emitComments.isEmpty())
            return;
        if (m_renderQueue.size() >= 2) {
            m_dropped += result.emitComments.size();
            ++m_overBudget;
            return;
        }
        m_renderQueue.enqueue({result.emitComments, 0, result.lastPositionMs});
        drainRenderBatch();
    });
    connect(&m_service, &CommentService::filtersChanged, this, [this](const FilterSnapshot &snapshot) {
        m_ngUsers = snapshot.ngUsers;
        m_regexFilters.clear();
        for (const auto &filter : snapshot.regexFilters)
            m_regexFilters.push_back(QVariantMap{{"filter_id", filter.filterId}, {"pattern", filter.pattern}});
        emit changed();
    });
    connect(&m_danmaku, &DanmakuController::ngDropRequested, this, [this](const QString &id) {
        if (!m_service.addNgUser(id)) {
            m_danmaku.rollbackPendingNgUserFade(id);
            queueFailure(false);
        }
    });
    connect(&m_service, &CommentService::ngAdded, this, [this](const AddNgUserResult &result) {
        m_danmaku.applyNgUserFade(result.hiddenUserId);
        if (result.applied) {
            m_undoToken = result.undoToken;
            toast(QStringLiteral("NGユーザーを登録しました"), QStringLiteral("Undo"));
        } else
            toast(QStringLiteral("このユーザーは既にNG登録済みです"));
    });
    connect(&m_service, &CommentService::ngRemoved, this, [this](const RemoveNgUserResult &result) {
        toast(result.removed ? QStringLiteral("NGユーザーを削除しました")
                             : QStringLiteral("指定ユーザーはNG登録されていません"));
    });
    connect(&m_service, &CommentService::ngUndone, this, [this](const UndoNgResult &result) {
        if (result.restored)
            m_undoToken.clear();
        toast(result.restored ? QStringLiteral("NG登録を取り消しました")
                              : QStringLiteral("Undo可能なNG登録がありません"));
    });
    connect(&m_service, &CommentService::regexAdded, this,
            [this](qint64) { toast(QStringLiteral("正規表現フィルタを追加しました")); });
    connect(&m_service, &CommentService::operationFailed, this,
            [this](const QString &operation, const QString &detail, const QString &id) {
                if (operation == "add_ng")
                    m_danmaku.rollbackPendingNgUserFade(id);
                toast(detail);
            });
    connect(&m_service, &CommentService::warning, this, [this](const QString &message) {
        qWarning().noquote() << message;
        toast(message);
    });
    connect(&m_service, &CommentService::stopped, this, &ApplicationController::readyToQuit);
    m_tickTimer.setInterval(50);
    connect(&m_tickTimer, &QTimer::timeout, this, &ApplicationController::playbackTick);
    m_tickTimer.start();
    m_renderTimer.setInterval(16);
    connect(&m_renderTimer, &QTimer::timeout, this, &ApplicationController::drainRenderBatch);
    m_renderTimer.start();
    m_perfTimer.setInterval(2000);
    connect(&m_perfTimer, &QTimer::timeout, this, &ApplicationController::performanceWindow);
    m_perfTimer.start();
}
ApplicationController::~ApplicationController() {
    shutdown();
}
QVariantList ApplicationController::speedPresets() const {
    QVariantList result;
    for (const auto rate : m_presets)
        result.push_back(rate);
    return result;
}
QVector<double> ApplicationController::normalizedPresets(const QVector<double> &values) {
    QVector<double> result;
    for (double value : values) {
        if (!std::isfinite(value))
            continue;
        value = std::round(std::clamp(value, 0.5, 3.0) * 100) / 100;
        if (std::none_of(result.begin(), result.end(),
                         [value](double other) { return std::abs(other - value) < 0.001; }))
            result.push_back(value);
    }
    if (result.isEmpty())
        return {1.0, 1.5, 2.0};
    std::sort(result.begin(), result.end());
    return result;
}
QString ApplicationController::localPath(const QString &value) {
    const QString trimmed = value.trimmed();
    const QUrl url(trimmed);
    return url.isLocalFile() ? url.toLocalFile() : trimmed;
}
double ApplicationController::nearestPreset(double value) const {
    if (!std::isfinite(value))
        value = 1.0;
    return *std::min_element(m_presets.begin(), m_presets.end(),
                             [value](double a, double b) { return std::abs(a - value) < std::abs(b - value); });
}
void ApplicationController::attachPlayer(MpvItem *player) {
    if (!player || m_player == player)
        return;
    if (m_player)
        disconnect(m_player, nullptr, this, nullptr);
    m_player = player;
    connect(player, &MpvItem::pausedChanged, this, [this] { m_danmaku.setPlaybackPaused(m_player->paused()); });
    connect(player, &MpvItem::speedChanged, this, [this] {
        m_danmaku.setPlaybackRate(m_player->speed());
        m_settings.setValue("playback/rate", nearestPreset(m_player->speed()));
    });
    connect(player, &MpvItem::errorOccurred, this, [this](const QString &message) { toast(message); });
    setPlaybackRate(m_settings.value("playback/rate", 1.0).toDouble());
    m_danmaku.setPlaybackPaused(player->paused());
    const auto autoPerf = qEnvironmentVariable("NICONEON_AUTO_PERF_LOG");
    if (autoPerf == "1" || autoPerf.compare("true", Qt::CaseInsensitive) == 0)
        setPerfLogEnabled(true);
    const auto autoPath = qEnvironmentVariable("NICONEON_AUTO_VIDEO_PATH");
    if (!autoPath.isEmpty())
        openVideo(autoPath);
}
void ApplicationController::resetComments() {
    m_renderQueue.clear();
    m_totalComments = 0;
    m_waitingSeek = false;
    m_danmaku.resetForSeek();
    emit changed();
}
void ApplicationController::openVideo(const QString &path) {
    if (!m_player || m_closing)
        return;
    const auto candidate = localPath(path);
    if (candidate.isEmpty()) {
        toast(QStringLiteral("動画ファイルを選択してください"));
        return;
    }
    if (!m_player->openFile(candidate)) {
        toast(QStringLiteral("動画を開けませんでした"));
        return;
    }
    m_selectedPath = candidate;
    resetComments();
    m_danmaku.resetGlyphSession();
    setPlaybackRate(m_settings.value("playback/rate", 1.0).toDouble());
    const auto videoId = extractVideoId(candidate);
    m_service.openVideo(candidate, videoId.value_or(QString{}));
    if (!videoId)
        toast(QStringLiteral("動画IDが見つからないためコメント取得をスキップしました"));
    emit changed();
}
void ApplicationController::togglePause() {
    if (m_player)
        m_player->togglePause();
}
void ApplicationController::seek(qint64 positionMs) {
    if (!m_player)
        return;
    m_seekTarget = std::clamp(positionMs, qint64(0), std::max(m_player->durationMs(), qint64(0)));
    m_waitingSeek = true;
    m_renderQueue.clear();
    m_danmaku.resetForSeek();
    m_player->seek(m_seekTarget);
    if (m_commentsVisible) {
        m_service.requestTick(m_seekTarget, m_player->paused(), true);
        ++m_sent;
    }
}
void ApplicationController::setVolume(double value) {
    if (m_player && std::isfinite(value))
        m_player->setVolume(std::clamp(value, 0.0, 100.0));
}
void ApplicationController::setPlaybackRate(double rate) {
    const double applied = nearestPreset(rate);
    if (m_player)
        m_player->setSpeed(applied);
    m_danmaku.setPlaybackRate(applied);
    m_settings.setValue("playback/rate", applied);
}
void ApplicationController::cyclePlaybackSpeed() {
    const auto current = nearestPreset(m_player ? m_player->speed() : 1.0);
    const auto it = std::find_if(m_presets.begin(), m_presets.end(),
                                 [current](double value) { return std::abs(value - current) < 0.001; });
    const auto next = m_presets[(std::distance(m_presets.begin(), it) + 1) % m_presets.size()];
    setPlaybackRate(next);
    toast(QStringLiteral("再生速度: %1x").arg(next));
}
void ApplicationController::persistPresets() {
    QJsonArray array;
    for (const auto value : m_presets)
        array.push_back(value);
    m_settings.setValue("playback/ratePresetsJson",
                        QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
    emit changed();
}
void ApplicationController::addSpeedPreset(const QString &value) {
    bool ok = false;
    const double parsed = value.toDouble(&ok);
    if (!ok || !std::isfinite(parsed)) {
        toast(QStringLiteral("0.5〜3.0 の範囲で速度を入力してください"));
        return;
    }
    auto next = m_presets;
    next.push_back(parsed);
    next = normalizedPresets(next);
    if (next.size() == m_presets.size()) {
        toast(QStringLiteral("同じ速度プリセットは追加できません"));
        return;
    }
    if (next.size() > 100) {
        toast(QStringLiteral("速度プリセットは100件までです"));
        return;
    }
    m_presets = next;
    persistPresets();
    toast(QStringLiteral("速度プリセットを追加しました"));
}
void ApplicationController::removeSpeedPreset(double rate) {
    if (m_presets.size() <= 1) {
        toast(QStringLiteral("最低1つの速度プリセットが必要です"));
        return;
    }
    m_presets.removeIf([rate](double value) { return std::abs(value - rate) < 0.001; });
    persistPresets();
    setPlaybackRate(m_player ? m_player->speed() : 1.0);
    toast(QStringLiteral("速度プリセットを削除しました"));
}
void ApplicationController::setCommentsVisible(bool visible) {
    if (m_commentsVisible == visible)
        return;
    m_commentsVisible = visible;
    m_settings.setValue("ui/commentsVisible", visible);
    m_renderQueue.clear();
    m_danmaku.resetForSeek();
    if (visible && m_player) {
        m_service.requestTick(m_player->positionMs(), m_player->paused(), true);
        ++m_sent;
    }
    emit changed();
    toast(visible ? QStringLiteral("コメント表示を有効化しました") : QStringLiteral("コメント表示を無効化しました"));
}
void ApplicationController::setPerfLogEnabled(bool enabled) {
    m_perfLog = enabled;
    m_settings.setValue("ui/perfLogEnabled", enabled);
    m_danmaku.setPerfLogEnabled(enabled);
    m_sent = m_results = m_overBudget = m_overloadStreak = m_stableStreak = 0;
    m_dropped = m_coalesced = 0;
    emit changed();
}
void ApplicationController::applyProfile(RuntimeProfileConfig profile, bool persist) {
    m_profile = profile;
    if (persist) {
        m_selectedProfile = profile.profile;
        m_settings.setValue("ui/perfProfile", profileName(profile.profile));
        m_settings.setValue("ui/targetFps", profile.targetFps);
        m_settings.setValue("ui/maxEmitPerTick", static_cast<int>(profile.maxEmitPerTick));
        m_settings.setValue("ui/coalesceSameContent", profile.coalesceSameContent);
    }
    m_danmaku.setTargetFps(profile.targetFps);
    queueFailure(m_service.setRuntimeProfile(profile));
    emit changed();
}
void ApplicationController::cycleRuntimeProfile() {
    const auto next = m_profile.profile == RuntimeProfile::High       ? RuntimeProfile::Balanced
                      : m_profile.profile == RuntimeProfile::Balanced ? RuntimeProfile::LowSpec
                                                                      : RuntimeProfile::High;
    applyProfile(makeRuntimeProfile(next), true);
    toast(QStringLiteral("Runtime profile: ") + profileName(next));
}
void ApplicationController::setFontSizeLevel(int level) {
    m_fontLevel = std::clamp(level, 0, 2);
    m_settings.setValue("ui/fontSizeLevel", m_fontLevel);
    emit changed();
}
void ApplicationController::queueFailure(bool accepted) {
    if (!accepted)
        toast(QStringLiteral("処理待ちがいっぱいです。少し待って再試行してください"));
}
void ApplicationController::addRegexFilter(const QString &pattern) {
    if (!pattern.trimmed().isEmpty())
        queueFailure(m_service.addRegexFilter(pattern));
}
void ApplicationController::removeRegexFilter(qint64 id) {
    queueFailure(m_service.removeRegexFilter(id));
}
void ApplicationController::removeNgUser(const QString &id) {
    queueFailure(m_service.removeNgUser(id));
}
void ApplicationController::refreshFilters() {
    queueFailure(m_service.listFilters());
}
void ApplicationController::undoNg() {
    if (!m_undoToken.isEmpty())
        queueFailure(m_service.undoLastNg(m_undoToken));
}
void ApplicationController::toast(const QString &text, const QString &action) {
    if (!m_closing)
        emit toastRequested(text, action);
}
void ApplicationController::playbackTick() {
    if (!m_player || !m_commentsVisible || m_closing)
        return;
    if (m_waitingSeek) {
        if (std::abs(m_player->positionMs() - m_seekTarget) >= 250)
            return;
        m_waitingSeek = false;
    }
    m_service.requestTick(m_player->positionMs(), m_player->paused(), false);
    ++m_sent;
}
void ApplicationController::drainRenderBatch() {
    if (!m_commentsVisible || m_renderQueue.isEmpty())
        return;
    auto &batch = m_renderQueue.head();
    QVariantList comments;
    const qsizetype end = std::min(batch.offset + 64, batch.comments.size());
    for (; batch.offset < end; ++batch.offset) {
        const auto &comment = batch.comments.at(batch.offset);
        comments.push_back(QVariantMap{{"comment_id", comment.commentId},
                                       {"user_id", comment.userId},
                                       {"at_ms", comment.atMs},
                                       {"text", comment.text}});
    }
    m_danmaku.appendComments(comments, m_player ? m_player->positionMs() : batch.positionMs);
    if (batch.offset == batch.comments.size())
        m_renderQueue.dequeue();
}
bool ApplicationController::degradeQos() {
    auto next = m_profile;
    if (next.profile == RuntimeProfile::High)
        next = makeRuntimeProfile(RuntimeProfile::Balanced);
    else if (next.maxEmitPerTick == 0)
        next.maxEmitPerTick = makeRuntimeProfile(next.profile).maxEmitPerTick;
    else if (next.maxEmitPerTick > 16)
        next.maxEmitPerTick = std::max(qsizetype(16), next.maxEmitPerTick - 8);
    else if (!next.coalesceSameContent)
        next.coalesceSameContent = true;
    else if (next.targetFps > 45)
        next.targetFps = std::max(45, next.targetFps - 15);
    else
        return false;
    applyProfile(next, false);
    return true;
}
bool ApplicationController::recoverQos() {
    auto next = m_profile;
    const auto defaults = makeRuntimeProfile(next.profile);
    if (next.targetFps < defaults.targetFps)
        next.targetFps = defaults.targetFps;
    else if (next.coalesceSameContent && !defaults.coalesceSameContent)
        next.coalesceSameContent = false;
    else if (defaults.maxEmitPerTick > 0 && next.maxEmitPerTick < defaults.maxEmitPerTick)
        next.maxEmitPerTick = std::min(defaults.maxEmitPerTick, next.maxEmitPerTick + 4);
    else if (next.profile != m_selectedProfile)
        next = makeRuntimeProfile(m_selectedProfile);
    else
        return false;
    applyProfile(next, false);
    return true;
}
void ApplicationController::performanceWindow() {
    const auto backlog = m_service.queueDepth();
    if (m_perfLog) {
        qInfo().noquote()
            << QStringLiteral("[perf-ui] window_ms=2000 tick_sent=%1 tick_result=%2 tick_backlog=%3 "
                              "dropped_comments=%4 coalesced_comments=%5 emit_over_budget=%6 profile=%7 target_fps=%8 "
                              "queue_high_water=%9 stale_results=%10 coalesced_ticks=%11 render_batches=%12")
                   .arg(m_sent)
                   .arg(m_results)
                   .arg(backlog)
                   .arg(m_dropped)
                   .arg(m_coalesced)
                   .arg(m_overBudget)
                   .arg(perfProfile())
                   .arg(m_profile.targetFps)
                   .arg(m_service.highWaterMark())
                   .arg(m_service.staleResults())
                   .arg(m_service.coalescedTicks())
                   .arg(m_renderQueue.size());
    }
    if (m_commentsVisible && m_player && !m_player->paused()) {
        const double fps = m_danmaku.commentRenderFps();
        const bool overloaded =
            m_overBudget > 0 || backlog >= 3 ||
            (m_danmaku.activeCommentCountMetric() > 0 && fps > 0 && fps < m_profile.targetFps * 0.8);
        if (overloaded) {
            ++m_overloadStreak;
            m_stableStreak = 0;
        } else {
            ++m_stableStreak;
            m_overloadStreak = std::max(0, m_overloadStreak - 1);
        }
        if (m_overloadStreak >= 2) {
            degradeQos();
            m_overloadStreak = 0;
        } else if (m_stableStreak >= 4) {
            recoverQos();
            m_stableStreak = 0;
        }
    } else {
        m_overloadStreak = m_stableStreak = 0;
    }
    m_sent = m_results = m_overBudget = 0;
    m_dropped = m_coalesced = 0;
}
void ApplicationController::shutdown() {
    if (m_closing)
        return;
    m_closing = true;
    m_tickTimer.stop();
    m_renderTimer.stop();
    m_perfTimer.stop();
    m_renderQueue.clear();
    m_service.shutdown();
    if (m_service.isStopped())
        emit readyToQuit();
}
} // namespace niconeon
