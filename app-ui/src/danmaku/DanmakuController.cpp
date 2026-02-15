#include "danmaku/DanmakuController.hpp"

#include "danmaku/DanmakuSimdUpdater.hpp"
#include "danmaku/DanmakuUpdateWorker.hpp"

#include <QDateTime>
#include <QMetaType>
#include <QMutexLocker>
#include <QPointF>
#include <QRectF>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace {
constexpr qreal kLaneTopMargin = 10.0;
constexpr qreal kSpawnOffset = 12.0;
constexpr qreal kItemHeight = 42.0;
constexpr qreal kItemCullThreshold = -20.0;
constexpr qint64 kMaxLagCompensationMs = 2000;
constexpr qreal kLaneSpawnGapPx = 20.0;
constexpr int kFreeRowsSoftLimit = 512;
constexpr double kCompactTriggerRatio = 0.5;
constexpr qint64 kGlyphWarmupIntervalMs = 80;
constexpr int kGlyphWarmupBatchChars = 24;
constexpr int kGlyphWarmupQueueMax = 2048;
constexpr qreal kSpatialCellWidthPx = 192.0;
constexpr qreal kDragPickSlopPx = 4.0;
constexpr qint64 kWorkerElapsedCapMs = 200;
constexpr const char *kGlyphWarmupSeed =
    "0123456789"
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "!?#$%&*+-=/:;.,_()[]{}<>|~^'\"`\\"
    "あいうえおかきくけこさしすせそたちつてとなにぬねの"
    "はひふへほまみむめもやゆよらりるれろわをん"
    "アイウエオカキクケコサシスセソタチツテトナニヌネノ"
    "ハヒフヘホマミムメモヤユヨラリルレロワヲン"
    "。、！？「」『』（）【】・ー";

bool isTrackableGlyphCodepoint(char32_t codepoint) {
    if (codepoint == U'\0') {
        return false;
    }
    if (codepoint > 0x10FFFF) {
        return false;
    }
    if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F)) {
        return false;
    }
    if (codepoint == U' ' || codepoint == 0x3000) {
        return false;
    }
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        return false;
    }
    return true;
}
}

DanmakuController::DanmakuController(QObject *parent) : QObject(parent) {
    qRegisterMetaType<DanmakuWorkerFramePtr>("DanmakuWorkerFramePtr");

    m_lastTickMs = QDateTime::currentMSecsSinceEpoch();
    m_perfLogWindowStartMs = m_lastTickMs;
    const QString workerMode = qEnvironmentVariable("NICONEON_DANMAKU_WORKER").trimmed().toLower();
    if (workerMode == QStringLiteral("off") || workerMode == QStringLiteral("0") || workerMode == QStringLiteral("false")) {
        m_workerEnabled = false;
    }
    const DanmakuSimdMode requestedSimdMode = DanmakuSimdUpdater::parseMode(qEnvironmentVariable("NICONEON_SIMD_MODE"));
    const DanmakuSimdMode resolvedSimdMode = DanmakuSimdUpdater::resolveMode(requestedSimdMode);
    m_simdModeName = DanmakuSimdUpdater::modeName(resolvedSimdMode);

    updateFrameTimerInterval();
    connect(&m_frameTimer, &QTimer::timeout, this, &DanmakuController::onFrame);
    m_frameTimer.start();

    if (m_workerEnabled) {
        m_updateWorker = new DanmakuUpdateWorker();
        m_updateWorker->setSimdMode(resolvedSimdMode);
        m_updateWorker->moveToThread(&m_updateThread);
        connect(&m_updateThread, &QThread::finished, m_updateWorker, &QObject::deleteLater);
        connect(
            m_updateWorker,
            &DanmakuUpdateWorker::frameProcessed,
            this,
            &DanmakuController::handleWorkerFrame,
            Qt::QueuedConnection);
        m_updateThread.start();
    }

    ensureLaneStateSize();
    resetLaneStates();
    resetGlyphSession();
    queueFullSpatialRebuild();
    queueFullSnapshotRebuild();
    flushPendingDiffs(false);
    qInfo().noquote() << QString("[danmaku-simd] mode=%1").arg(m_simdModeName);
    qInfo().noquote() << QString("[danmaku-worker] enabled=%1").arg(m_workerEnabled ? 1 : 0);
}

DanmakuController::~DanmakuController() {
    m_workerBusy = false;
    if (m_updateThread.isRunning()) {
        m_updateThread.quit();
        m_updateThread.wait();
    }
}

void DanmakuController::setViewportSize(qreal width, qreal height) {
    m_viewportWidth = width;
    m_viewportHeight = height;
    ensureLaneStateSize();
    queueFullSpatialRebuild();
    queueFullSnapshotRebuild();
    flushPendingDiffs(false);
}

void DanmakuController::setLaneMetrics(int fontPx, int laneGap) {
    m_fontPx = std::max(fontPx, 12);
    m_laneGap = std::max(laneGap, 0);
    ensureLaneStateSize();
    queueFullSpatialRebuild();
    queueFullSnapshotRebuild();
    flushPendingDiffs(false);
}

void DanmakuController::setPlaybackPaused(bool paused) {
    if (m_playbackPaused == paused) {
        return;
    }
    m_playbackPaused = paused;
    invalidateWorkerGeneration();
    emit playbackPausedChanged();
}

void DanmakuController::setPlaybackRate(double rate) {
    const double normalized = std::clamp(rate, 0.5, 3.0);
    if (qFuzzyCompare(m_playbackRate + 1.0, normalized + 1.0)) {
        return;
    }
    m_playbackRate = normalized;
    invalidateWorkerGeneration();
    emit playbackRateChanged();
}

void DanmakuController::setTargetFps(int fps) {
    const int normalized = std::clamp(fps, 10, 120);
    if (m_targetFps == normalized) {
        return;
    }
    m_targetFps = normalized;
    updateFrameTimerInterval();
    emit targetFpsChanged();
}

void DanmakuController::setPerfLogEnabled(bool enabled) {
    if (m_perfLogEnabled == enabled) {
        return;
    }

    m_perfLogEnabled = enabled;
    m_perfLogWindowStartMs = QDateTime::currentMSecsSinceEpoch();
    m_perfLogFrameCount = 0;
    m_perfFrameSamplesMs.clear();
    m_perfLogAppendCount = 0;
    m_perfLogGeometryUpdateCount = 0;
    m_perfLogRemovedCount = 0;
    m_perfLanePickCount = 0;
    m_perfLaneReadyCount = 0;
    m_perfLaneForcedCount = 0;
    m_perfLaneWaitTotalMs = 0;
    m_perfLaneWaitMaxMs = 0;
    m_perfSpatialFullRebuildCount = 0;
    m_perfSpatialRowUpdateCount = 0;
    m_perfSnapshotFullRebuildCount = 0;
    m_perfSnapshotRowUpdateCount = 0;
    m_perfCompactedSinceLastLog = false;
    m_perfGlyphNewCodepoints = 0;
    m_perfGlyphNewNonAsciiCodepoints = 0;
    m_perfGlyphWarmupSentCodepoints = 0;
    m_perfGlyphWarmupBatchCount = 0;
    m_perfGlyphWarmupDroppedCodepoints = 0;
    emit perfLogEnabledChanged();
}

void DanmakuController::setGlyphWarmupEnabled(bool enabled) {
    if (m_glyphWarmupEnabled == enabled) {
        return;
    }

    m_glyphWarmupEnabled = enabled;
    emit glyphWarmupEnabledChanged();

    m_glyphWarmupQueue.clear();
    m_queuedGlyphCodepoints.clear();
    m_lastGlyphWarmupDispatchMs = 0;

    if (!m_glyphWarmupEnabled) {
        clearGlyphWarmupText();
        return;
    }

    queueGlyphSeedCharacters();
    for (const char32_t codepoint : m_seenGlyphCodepoints) {
        queueGlyphCodepoint(codepoint);
    }
}

void DanmakuController::appendFromCore(const QVariantList &comments, qint64 playbackPositionMs) {
    invalidateWorkerGeneration();
    ensureLaneStateSize();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QVector<int> appendedRows;
    appendedRows.reserve(comments.size());
    bool appendedAny = false;
    for (const QVariant &entry : comments) {
        const QVariantMap map = entry.toMap();
        Item item;
        item.commentId = map.value("comment_id").toString();
        item.userId = map.value("user_id").toString();
        item.text = map.value("text").toString();
        if (item.commentId.isEmpty()) {
            continue;
        }
        observeGlyphText(item.text);

        const qint64 atMs = map.value("at_ms").toLongLong();
        const int textWidthEstimate = static_cast<int>(item.text.size()) * (m_fontPx / 2 + 4);
        item.widthEstimate = std::max(80, textWidthEstimate);
        item.speedPxPerSec = 120 + (qHash(item.commentId) % 70);
        item.active = true;

        item.lane = pickLane(nowMs);
        item.originalLane = item.lane;
        item.x = m_viewportWidth + kSpawnOffset;
        item.y = item.lane * (m_fontPx + m_laneGap) + kLaneTopMargin;

        const qint64 lagMs = std::clamp(playbackPositionMs - atMs, qint64(0), kMaxLagCompensationMs);
        const qreal lagSec = lagMs / 1000.0;
        item.x -= (item.speedPxPerSec * m_playbackRate) * lagSec;
        if (item.x + item.widthEstimate < kItemCullThreshold) {
            continue;
        }

        const int row = acquireRow();
        m_items[row] = item;
        appendedRows.push_back(row);
        LaneState &laneState = m_laneStates[item.lane];
        laneState.nextAvailableAtMs = std::max(laneState.nextAvailableAtMs, nowMs) + estimateLaneCooldownMs(item);
        laneState.lastAssignedRow = row;

        if (m_perfLogEnabled) {
            ++m_perfLogAppendCount;
        }
        appendedAny = true;
    }

    if (appendedAny) {
        queueSpatialUpsertRows(appendedRows);
        queueSnapshotUpsertRows(appendedRows);
        flushPendingDiffs(true);
    }
}

void DanmakuController::setNgDropZoneRect(qreal x, qreal y, qreal width, qreal height) {
    invalidateWorkerGeneration();
    m_ngZoneX = x;
    m_ngZoneY = y;
    m_ngZoneWidth = std::max(0.0, width);
    m_ngZoneHeight = std::max(0.0, height);

    if (!hasDragging()) {
        return;
    }

    bool changed = false;
    QVector<int> changedRows;
    for (int i = 0; i < m_items.size(); ++i) {
        Item &item = m_items[i];
        if (!item.active || !item.dragging) {
            continue;
        }
        const bool hovered = isItemInNgZone(item);
        if (hovered == item.ngDropHovered) {
            continue;
        }
        item.ngDropHovered = hovered;
        changedRows.push_back(i);
        changed = true;
    }

    if (changed) {
        queueSnapshotUpsertRows(changedRows);
        flushPendingDiffs(true);
    }
}

bool DanmakuController::beginDragAt(qreal x, qreal y) {
    const int index = findItemIndexAt(x, y);
    if (index < 0) {
        return false;
    }

    return beginDragInternal(index, x, y, true);
}

void DanmakuController::moveActiveDrag(qreal x, qreal y) {
    if (m_activeDragRow < 0) {
        return;
    }

    moveDragInternal(m_activeDragRow, x, y, true);
}

void DanmakuController::dropActiveDrag(bool inNgZone) {
    if (m_activeDragRow < 0) {
        return;
    }

    dropDragInternal(m_activeDragRow, inNgZone);
}

void DanmakuController::cancelActiveDrag() {
    dropActiveDrag(false);
}

bool DanmakuController::beginDragInternal(int index, qreal pointerX, qreal pointerY, bool hasPointerPosition) {
    if (index < 0 || index >= m_items.size()) {
        return false;
    }
    Item &item = m_items[index];
    if (!item.active || item.dragging) {
        return false;
    }

    item.frozen = true;
    item.dragging = true;
    item.originalLane = item.lane;
    item.ngDropHovered = isItemInNgZone(item);
    m_activeDragRow = index;
    if (hasPointerPosition) {
        m_activeDragOffsetX = pointerX - item.x;
        m_activeDragOffsetY = pointerY - item.y;
    } else {
        m_activeDragOffsetX = 0.0;
        m_activeDragOffsetY = 0.0;
    }

    updateNgZoneVisibility();
    invalidateWorkerGeneration();
    queueSnapshotUpsertRow(index);
    flushPendingDiffs(true);
    return true;
}

void DanmakuController::moveDragInternal(int index, qreal pointerX, qreal pointerY, bool hasPointerPosition) {
    if (index < 0 || index >= m_items.size()) {
        return;
    }

    Item &item = m_items[index];
    if (!item.active || !item.dragging) {
        return;
    }

    if (hasPointerPosition) {
        item.x = pointerX - m_activeDragOffsetX;
        item.y = pointerY - m_activeDragOffsetY;
    } else {
        item.x = pointerX;
        item.y = pointerY;
    }

    const bool hovered = isItemInNgZone(item);
    if (hovered != item.ngDropHovered) {
        item.ngDropHovered = hovered;
    }
    invalidateWorkerGeneration();
    queueSpatialUpsertRow(index);
    queueSnapshotUpsertRow(index);
    flushPendingDiffs(true);
}

void DanmakuController::dropDragInternal(int index, bool inNgZone) {
    if (index < 0 || index >= m_items.size()) {
        return;
    }

    Item &item = m_items[index];
    if (!item.active) {
        return;
    }

    const bool resolvedInNgZone = inNgZone || isItemInNgZone(item);
    invalidateWorkerGeneration();
    m_activeDragRow = -1;
    m_activeDragOffsetX = 0.0;
    m_activeDragOffsetY = 0.0;

    if (resolvedInNgZone) {
        const QString userId = item.userId;
        releaseRow(index);
        emit ngDropRequested(userId);
    } else {
        item.dragging = false;
        item.frozen = false;
        item.ngDropHovered = false;
        recoverToLane(item);
    }

    updateNgZoneVisibility();
    if (!resolvedInNgZone) {
        queueSpatialUpsertRow(index);
        queueSnapshotUpsertRow(index);
    }
    flushPendingDiffs(true);
}

void DanmakuController::applyNgUserFade(const QString &userId) {
    invalidateWorkerGeneration();
    bool changed = false;
    QVector<int> changedRows;
    for (int row = 0; row < m_items.size(); ++row) {
        Item &item = m_items[row];
        if (item.active && item.userId == userId) {
            item.fading = true;
            item.fadeRemainingMs = 300;
            changedRows.push_back(row);
            changed = true;
        }
    }
    if (changed) {
        queueSnapshotUpsertRows(changedRows);
        flushPendingDiffs(true);
    }
}

void DanmakuController::resetForSeek() {
    invalidateWorkerGeneration();
    QVector<int> activeRows;
    activeRows.reserve(m_items.size());
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].active) {
            activeRows.push_back(i);
        }
    }
    releaseRowsDescending(activeRows);
    maybeCompactRows();
    resetLaneStates();
    m_activeDragRow = -1;
    m_activeDragOffsetX = 0.0;
    m_activeDragOffsetY = 0.0;
    updateNgZoneVisibility();
    queueFullSpatialRebuild();
    queueFullSnapshotRebuild();
    flushPendingDiffs(true);
}

void DanmakuController::resetGlyphSession() {
    m_seenGlyphCodepoints.clear();
    m_warmedGlyphCodepoints.clear();
    m_queuedGlyphCodepoints.clear();
    m_glyphWarmupQueue.clear();
    m_lastGlyphWarmupDispatchMs = 0;
    clearGlyphWarmupText();
    if (m_glyphWarmupEnabled) {
        queueGlyphSeedCharacters();
    }
}

bool DanmakuController::ngDropZoneVisible() const {
    return m_ngDropZoneVisible;
}

bool DanmakuController::playbackPaused() const {
    return m_playbackPaused;
}

double DanmakuController::playbackRate() const {
    return m_playbackRate;
}

int DanmakuController::targetFps() const {
    return m_targetFps;
}

bool DanmakuController::perfLogEnabled() const {
    return m_perfLogEnabled;
}

bool DanmakuController::glyphWarmupEnabled() const {
    return m_glyphWarmupEnabled;
}

QString DanmakuController::glyphWarmupText() const {
    return m_glyphWarmupText;
}

QSharedPointer<const QVector<DanmakuController::RenderItem>> DanmakuController::renderSnapshot() const {
    QMutexLocker locker(&m_renderSnapshotMutex);
    return m_renderSnapshot;
}

void DanmakuController::onFrame() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int elapsedMs = static_cast<int>(now - m_lastTickMs);
    m_lastTickMs = now;

    if (elapsedMs <= 0) {
        return;
    }
    if (m_perfLogEnabled) {
        ++m_perfLogFrameCount;
        m_perfFrameSamplesMs.push_back(elapsedMs);
    }
    dispatchGlyphWarmupIfDue(now);

    if (activeItemCount() == 0) {
        maybeWritePerfLog(now);
        return;
    }

    if (m_workerEnabled && m_updateWorker) {
        m_workerAccumulatedElapsedMs += elapsedMs;
        if (m_workerBusy) {
            maybeWritePerfLog(now);
            return;
        }

        const int workerElapsedMs = std::clamp(m_workerAccumulatedElapsedMs, 1, static_cast<int>(kWorkerElapsedCapMs));
        m_workerAccumulatedElapsedMs = 0;
        scheduleWorkerFrame(workerElapsedMs, now);
        maybeWritePerfLog(now);
        return;
    }

    runFrameSingleThread(elapsedMs, now);
}

void DanmakuController::runFrameSingleThread(int elapsedMs, qint64 nowMs) {
    const qreal elapsedSec = elapsedMs / 1000.0;
    QVector<int> changedRows;
    changedRows.reserve(m_items.size());
    QVector<int> removeRows;
    removeRows.reserve(m_items.size());
    int frameGeometryUpdates = 0;
    bool frameStateChanged = false;

    for (int i = 0; i < m_items.size(); ++i) {
        Item &item = m_items[i];
        if (!item.active) {
            continue;
        }

        bool geometryChanged = false;
        if (!m_playbackPaused && !item.frozen) {
            item.x -= (item.speedPxPerSec * m_playbackRate) * elapsedSec;
            geometryChanged = true;
        }

        if (item.fading) {
            item.fadeRemainingMs -= elapsedMs;
            if (item.fadeRemainingMs <= 0) {
                item.alpha = 0.0;
            } else {
                item.alpha = std::clamp(item.fadeRemainingMs / 300.0, 0.0, 1.0);
            }
            geometryChanged = true;
        }

        if (item.dragging) {
            const bool hovered = isItemInNgZone(item);
            if (hovered != item.ngDropHovered) {
                item.ngDropHovered = hovered;
                changedRows.push_back(i);
                frameStateChanged = true;
            }
        }

        if (geometryChanged) {
            ++frameGeometryUpdates;
            changedRows.push_back(i);
            frameStateChanged = true;
        }

        const bool outOfHorizontalBounds = item.x + item.widthEstimate < kItemCullThreshold;
        const bool outOfVerticalBounds = item.y > m_viewportHeight || item.y + kItemHeight < 0.0;
        const bool canCull = !item.dragging && (item.alpha <= 0.0 || outOfHorizontalBounds || outOfVerticalBounds);
        if (canCull) {
            removeRows.push_back(i);
        }
    }

    if (m_perfLogEnabled) {
        m_perfLogGeometryUpdateCount += frameGeometryUpdates;
    }
    if (!changedRows.isEmpty()) {
        queueSpatialUpsertRows(changedRows);
        queueSnapshotUpsertRows(changedRows);
    }
    if (!removeRows.isEmpty()) {
        releaseRowsDescending(removeRows);
        if (m_perfLogEnabled) {
            m_perfLogRemovedCount += removeRows.size();
        }
        frameStateChanged = true;
    }
    const int totalBeforeCompact = m_items.size();
    const int freeBeforeCompact = m_freeRows.size();
    const bool compacted = maybeCompactRows();
    if (compacted || m_items.size() != totalBeforeCompact || m_freeRows.size() != freeBeforeCompact) {
        frameStateChanged = true;
    }

    if (frameStateChanged) {
        flushPendingDiffs(true);
    }
    maybeWritePerfLog(nowMs);
}

int DanmakuController::laneCount() const {
    const int laneHeight = m_fontPx + m_laneGap;
    if (laneHeight <= 0) {
        return 1;
    }
    return std::max(1, static_cast<int>(m_viewportHeight / laneHeight));
}

void DanmakuController::ensureLaneStateSize() {
    const int lanes = laneCount();
    if (m_laneStates.size() == lanes) {
        return;
    }

    m_laneStates.resize(lanes);
    if (lanes <= 0) {
        m_laneCursor = 0;
    } else if (m_laneCursor >= lanes || m_laneCursor < 0) {
        m_laneCursor = m_laneCursor % lanes;
        if (m_laneCursor < 0) {
            m_laneCursor += lanes;
        }
    }
}

void DanmakuController::resetLaneStates() {
    ensureLaneStateSize();
    for (LaneState &state : m_laneStates) {
        state.nextAvailableAtMs = 0;
        state.lastAssignedRow = -1;
    }
    m_laneCursor = 0;
}

qint64 DanmakuController::estimateLaneCooldownMs(const Item &item) const {
    const qreal effectiveSpeed = std::max<qreal>(1.0, item.speedPxPerSec * m_playbackRate);
    const qreal travelMs = ((item.widthEstimate + kLaneSpawnGapPx) * 1000.0) / effectiveSpeed;
    return std::max<qint64>(1, static_cast<qint64>(std::llround(travelMs)));
}

int DanmakuController::pickLane(qint64 nowMs) {
    ensureLaneStateSize();
    const int lanes = m_laneStates.size();
    if (lanes <= 0) {
        return 0;
    }

    const int start = (m_laneCursor >= 0) ? (m_laneCursor % lanes) : 0;
    int chosenReady = -1;
    for (int offset = 0; offset < lanes; ++offset) {
        const int lane = (start + offset) % lanes;
        if (m_laneStates[lane].nextAvailableAtMs <= nowMs) {
            chosenReady = lane;
            break;
        }
    }

    if (chosenReady >= 0) {
        m_laneCursor = (chosenReady + 1) % lanes;
        if (m_perfLogEnabled) {
            ++m_perfLanePickCount;
            ++m_perfLaneReadyCount;
        }
        return chosenReady;
    }

    int chosenForced = start;
    qint64 bestAt = m_laneStates[chosenForced].nextAvailableAtMs;
    for (int offset = 1; offset < lanes; ++offset) {
        const int lane = (start + offset) % lanes;
        if (m_laneStates[lane].nextAvailableAtMs < bestAt) {
            bestAt = m_laneStates[lane].nextAvailableAtMs;
            chosenForced = lane;
        }
    }

    const qint64 waitMs = std::max<qint64>(0, bestAt - nowMs);
    m_laneCursor = (chosenForced + 1) % lanes;
    if (m_perfLogEnabled) {
        ++m_perfLanePickCount;
        ++m_perfLaneForcedCount;
        m_perfLaneWaitTotalMs += waitMs;
        m_perfLaneWaitMaxMs = std::max(m_perfLaneWaitMaxMs, waitMs);
    }
    return chosenForced;
}

bool DanmakuController::laneHasCollision(int lane, const Item &candidate) {
    const QRectF candidateRect(candidate.x, candidate.y, candidate.widthEstimate, kItemHeight);
    const QVector<int> rows = m_spatialGrid.queryRect(candidateRect);
    for (const int row : rows) {
        if (row < 0 || row >= m_items.size()) {
            continue;
        }
        const Item &item = m_items[row];
        if (!item.active || item.commentId == candidate.commentId || item.lane != lane) {
            continue;
        }

        const qreal left = candidate.x;
        const qreal right = candidate.x + candidate.widthEstimate;
        const qreal otherLeft = item.x;
        const qreal otherRight = item.x + item.widthEstimate;

        const bool overlap = !(right < otherLeft || otherRight < left);
        if (overlap) {
            return true;
        }
    }
    return false;
}

void DanmakuController::recoverToLane(Item &item) {
    item.y = item.originalLane * (m_fontPx + m_laneGap) + kLaneTopMargin;
    item.lane = item.originalLane;

    if (!laneHasCollision(item.lane, item)) {
        return;
    }

    const int lanes = laneCount();
    for (int offset = 1; offset < lanes; ++offset) {
        const int up = item.originalLane - offset;
        const int down = item.originalLane + offset;

        if (up >= 0) {
            item.lane = up;
            item.y = up * (m_fontPx + m_laneGap) + kLaneTopMargin;
            if (!laneHasCollision(item.lane, item)) {
                return;
            }
        }

        if (down < lanes) {
            item.lane = down;
            item.y = down * (m_fontPx + m_laneGap) + kLaneTopMargin;
            if (!laneHasCollision(item.lane, item)) {
                return;
            }
        }
    }
}

int DanmakuController::findItemIndexAt(qreal x, qreal y) {
    const QPointF point(x, y);
    QVector<int> candidates = m_spatialGrid.queryPoint(point);
    if (candidates.isEmpty()) {
        const QRectF slopRect(
            x - kDragPickSlopPx,
            y - kDragPickSlopPx,
            kDragPickSlopPx * 2.0,
            kDragPickSlopPx * 2.0);
        candidates = m_spatialGrid.queryRect(slopRect);
    }
    if (candidates.isEmpty()) {
        return -1;
    }

    std::sort(candidates.begin(), candidates.end(), std::greater<int>());
    for (const int row : candidates) {
        if (row < 0 || row >= m_items.size()) {
            continue;
        }

        const Item &item = m_items[row];
        if (!item.active) {
            continue;
        }
        const QRectF rect(item.x, item.y, item.widthEstimate, kItemHeight);
        if (rect.contains(point)) {
            return row;
        }
    }

    return -1;
}

int DanmakuController::acquireRow() {
    if (!m_freeRows.isEmpty()) {
        const int row = m_freeRows.back();
        m_freeRows.pop_back();
        return row;
    }

    const int row = m_items.size();
    m_items.push_back(Item{});
    return row;
}

void DanmakuController::releaseRow(int row) {
    if (row < 0 || row >= m_items.size()) {
        return;
    }

    Item &item = m_items[row];
    if (!item.active) {
        return;
    }

    item.active = false;
    item.frozen = false;
    item.dragging = false;
    item.fading = false;
    item.ngDropHovered = false;
    item.fadeRemainingMs = 0;
    item.alpha = 1.0;
    item.commentId.clear();
    item.userId.clear();
    item.text.clear();

    m_freeRows.push_back(row);
    if (m_activeDragRow == row) {
        m_activeDragRow = -1;
        m_activeDragOffsetX = 0.0;
        m_activeDragOffsetY = 0.0;
    }
    queueSpatialRemoveRow(row);
    queueSnapshotRemoveRow(row);
}

void DanmakuController::releaseRowsDescending(const QVector<int> &rowsDescending) {
    if (rowsDescending.isEmpty()) {
        return;
    }

    QVector<int> rows = rowsDescending;
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (const int row : rows) {
        releaseRow(row);
    }
}

bool DanmakuController::maybeCompactRows() {
    if (hasDragging()) {
        return false;
    }

    const int totalRows = m_items.size();
    const int freeRows = m_freeRows.size();
    if (totalRows == 0 || freeRows <= kFreeRowsSoftLimit) {
        return false;
    }

    const double freeRatio = totalRows > 0 ? (static_cast<double>(freeRows) / totalRows) : 0.0;
    if (freeRatio < kCompactTriggerRatio) {
        return false;
    }

    QVector<Item> compactItems;
    compactItems.reserve(totalRows - freeRows);

    for (const Item &item : m_items) {
        if (!item.active) {
            continue;
        }
        compactItems.push_back(item);
    }

    m_items = std::move(compactItems);
    m_freeRows.clear();
    for (LaneState &state : m_laneStates) {
        state.lastAssignedRow = -1;
    }
    m_perfCompactedSinceLastLog = true;
    queueFullSpatialRebuild();
    queueFullSnapshotRebuild();
    return true;
}

int DanmakuController::activeItemCount() const {
    const int total = m_items.size();
    const int free = m_freeRows.size();
    return std::max(0, total - free);
}

bool DanmakuController::hasDragging() const {
    for (const Item &item : m_items) {
        if (item.active && item.dragging) {
            return true;
        }
    }
    return false;
}

void DanmakuController::updateNgZoneVisibility() {
    bool visible = false;
    for (const Item &item : m_items) {
        if (item.active && item.dragging) {
            visible = true;
            break;
        }
    }

    if (visible != m_ngDropZoneVisible) {
        m_ngDropZoneVisible = visible;
        emit ngDropZoneVisibleChanged();
    }
}

bool DanmakuController::isItemInNgZone(const Item &item) const {
    if (!item.active) {
        return false;
    }
    if (m_ngZoneWidth <= 0 || m_ngZoneHeight <= 0) {
        return false;
    }

    const qreal itemLeft = item.x;
    const qreal itemTop = item.y;
    const qreal itemRight = itemLeft + item.widthEstimate;
    const qreal itemBottom = itemTop + kItemHeight;

    const qreal zoneLeft = m_ngZoneX;
    const qreal zoneTop = m_ngZoneY;
    const qreal zoneRight = zoneLeft + m_ngZoneWidth;
    const qreal zoneBottom = zoneTop + m_ngZoneHeight;

    const bool overlap = !(itemRight < zoneLeft || zoneRight < itemLeft || itemBottom < zoneTop || zoneBottom < itemTop);
    if (overlap) {
        return true;
    }

    const qreal centerX = itemLeft + item.widthEstimate / 2.0;
    const qreal centerY = itemTop + (kItemHeight / 2.0);
    return centerX >= zoneLeft && centerX <= zoneRight && centerY >= zoneTop && centerY <= zoneBottom;
}

void DanmakuController::observeGlyphText(const QString &text) {
    if (text.isEmpty()) {
        return;
    }

    const QVector<uint> codepoints = text.toUcs4();
    for (const uint value : codepoints) {
        const char32_t codepoint = static_cast<char32_t>(value);
        if (!isTrackableGlyphCodepoint(codepoint)) {
            continue;
        }

        if (m_seenGlyphCodepoints.contains(codepoint)) {
            continue;
        }
        m_seenGlyphCodepoints.insert(codepoint);

        ++m_perfGlyphNewCodepoints;
        if (codepoint > 0x7F) {
            ++m_perfGlyphNewNonAsciiCodepoints;
        }
        queueGlyphCodepoint(codepoint);
    }
}

void DanmakuController::queueGlyphCodepoint(char32_t codepoint) {
    if (!m_glyphWarmupEnabled) {
        return;
    }
    if (!isTrackableGlyphCodepoint(codepoint)) {
        return;
    }
    if (m_warmedGlyphCodepoints.contains(codepoint) || m_queuedGlyphCodepoints.contains(codepoint)) {
        return;
    }
    if (m_glyphWarmupQueue.size() >= kGlyphWarmupQueueMax) {
        ++m_perfGlyphWarmupDroppedCodepoints;
        return;
    }

    m_glyphWarmupQueue.enqueue(codepoint);
    m_queuedGlyphCodepoints.insert(codepoint);
}

void DanmakuController::queueGlyphSeedCharacters() {
    const QString seed = QString::fromUtf8(kGlyphWarmupSeed);
    const QVector<uint> codepoints = seed.toUcs4();
    for (const uint value : codepoints) {
        queueGlyphCodepoint(static_cast<char32_t>(value));
    }
}

void DanmakuController::dispatchGlyphWarmupIfDue(qint64 nowMs) {
    if (!m_glyphWarmupEnabled) {
        return;
    }
    if (m_lastGlyphWarmupDispatchMs > 0 && nowMs - m_lastGlyphWarmupDispatchMs < kGlyphWarmupIntervalMs) {
        return;
    }
    if (m_glyphWarmupQueue.isEmpty()) {
        clearGlyphWarmupText();
        return;
    }

    QString batch;
    batch.reserve(kGlyphWarmupBatchChars * 2);

    int sent = 0;
    while (sent < kGlyphWarmupBatchChars && !m_glyphWarmupQueue.isEmpty()) {
        const char32_t codepoint = m_glyphWarmupQueue.dequeue();
        m_queuedGlyphCodepoints.remove(codepoint);
        if (m_warmedGlyphCodepoints.contains(codepoint)) {
            continue;
        }

        const char32_t raw[] = {codepoint};
        batch.append(QString::fromUcs4(raw, 1));
        m_warmedGlyphCodepoints.insert(codepoint);
        ++sent;
    }

    if (sent <= 0) {
        clearGlyphWarmupText();
        return;
    }

    m_lastGlyphWarmupDispatchMs = nowMs;
    m_perfGlyphWarmupSentCodepoints += sent;
    ++m_perfGlyphWarmupBatchCount;

    if (m_glyphWarmupText != batch) {
        m_glyphWarmupText = std::move(batch);
        emit glyphWarmupTextChanged();
    }
}

void DanmakuController::clearGlyphWarmupText() {
    if (m_glyphWarmupText.isEmpty()) {
        return;
    }
    m_glyphWarmupText.clear();
    emit glyphWarmupTextChanged();
}

void DanmakuController::buildSoAState(DanmakuSoAState &state) const {
    const int count = activeItemCount();
    state.resize(count);
    int index = 0;
    for (int row = 0; row < m_items.size(); ++row) {
        const Item &item = m_items[row];
        if (!item.active) {
            continue;
        }
        quint8 flags = 0;
        if (item.frozen) {
            flags |= DanmakuSoAFlagFrozen;
        }
        if (item.dragging) {
            flags |= DanmakuSoAFlagDragging;
        }
        if (item.fading) {
            flags |= DanmakuSoAFlagFading;
        }

        state.rows[index] = row;
        state.x[index] = item.x;
        state.y[index] = item.y;
        state.speed[index] = item.speedPxPerSec;
        state.alpha[index] = item.alpha;
        state.widthEstimate[index] = item.widthEstimate;
        state.fadeRemainingMs[index] = item.fadeRemainingMs;
        state.flags[index] = flags;
        ++index;
    }
    Q_ASSERT(index == count);
}

void DanmakuController::scheduleWorkerFrame(int elapsedMs, qint64 nowMs) {
    Q_UNUSED(nowMs)

    if (!m_updateWorker || m_workerBusy) {
        return;
    }

    DanmakuWorkerFramePtr frameInput = m_workerReusableFrame;
    if (!frameInput) {
        frameInput = DanmakuWorkerFramePtr::create();
    } else {
        m_workerReusableFrame.clear();
    }

    buildSoAState(frameInput->state);
    frameInput->changedRows.clear();
    frameInput->removeRows.clear();
    if (frameInput->state.size() <= 0) {
        m_workerReusableFrame = std::move(frameInput);
        return;
    }

    m_workerBusy = true;
    frameInput->seq = ++m_workerSeq;
    frameInput->playbackPaused = m_playbackPaused;
    frameInput->playbackRate = static_cast<qreal>(m_playbackRate);
    frameInput->elapsedMs = elapsedMs;
    frameInput->viewportHeight = m_viewportHeight;
    frameInput->cullThreshold = kItemCullThreshold;
    frameInput->itemHeight = kItemHeight;
    const bool enqueued = QMetaObject::invokeMethod(
        m_updateWorker,
        "processFrame",
        Qt::QueuedConnection,
        Q_ARG(DanmakuWorkerFramePtr, frameInput));
    if (!enqueued) {
        m_workerBusy = false;
        m_workerReusableFrame = std::move(frameInput);
    }
}

void DanmakuController::handleWorkerFrame(DanmakuWorkerFramePtr frame) {
    m_workerBusy = false;
    if (!frame) {
        return;
    }

    auto reclaimFrame = [this](DanmakuWorkerFramePtr &processedFrame) {
        m_workerReusableFrame = std::move(processedFrame);
    };

    if (frame->seq != m_workerSeq) {
        reclaimFrame(frame);
        return;
    }

    const QVector<int> &rows = frame->state.rows;
    const QVector<qreal> &x = frame->state.x;
    const QVector<qreal> &y = frame->state.y;
    const QVector<qreal> &alpha = frame->state.alpha;
    const QVector<int> &fadeRemainingMs = frame->state.fadeRemainingMs;
    const QVector<quint8> &flags = frame->state.flags;
    const QVector<int> &changedRows = frame->changedRows;
    const QVector<int> &removeRows = frame->removeRows;

    const int count = rows.size();
    if (count <= 0) {
        reclaimFrame(frame);
        return;
    }
    if (x.size() != count || y.size() != count || alpha.size() != count || fadeRemainingMs.size() != count || flags.size() != count) {
        reclaimFrame(frame);
        return;
    }

    for (int i = 0; i < count; ++i) {
        const int row = rows[i];
        if (row < 0 || row >= m_items.size()) {
            continue;
        }
        Item &item = m_items[row];
        if (!item.active) {
            continue;
        }
        item.x = x[i];
        item.y = y[i];
        item.alpha = alpha[i];
        item.fadeRemainingMs = fadeRemainingMs[i];
        item.frozen = (flags[i] & DanmakuSoAFlagFrozen) != 0;
        item.dragging = (flags[i] & DanmakuSoAFlagDragging) != 0;
        item.fading = (flags[i] & DanmakuSoAFlagFading) != 0;
    }

    int geometryUpdateCount = 0;
    bool hasGeometryUpdates = false;
    for (const int row : changedRows) {
        if (row < 0 || row >= m_items.size()) {
            continue;
        }
        const Item &item = m_items[row];
        if (!item.active) {
            continue;
        }
        ++geometryUpdateCount;
        hasGeometryUpdates = true;
    }
    if (hasGeometryUpdates) {
        queueSpatialUpsertRows(changedRows);
        queueSnapshotUpsertRows(changedRows);
    }

    if (m_perfLogEnabled) {
        m_perfLogGeometryUpdateCount += geometryUpdateCount;
    }

    if (!removeRows.isEmpty()) {
        releaseRowsDescending(removeRows);
        if (m_perfLogEnabled) {
            m_perfLogRemovedCount += removeRows.size();
        }
    }

    const int totalBeforeCompact = m_items.size();
    const int freeBeforeCompact = m_freeRows.size();
    const bool compacted = maybeCompactRows()
        || (m_items.size() != totalBeforeCompact || m_freeRows.size() != freeBeforeCompact);

    if (hasGeometryUpdates || !removeRows.isEmpty() || compacted) {
        flushPendingDiffs(true);
    }

    reclaimFrame(frame);
}

void DanmakuController::invalidateWorkerGeneration() {
    if (!m_workerEnabled) {
        return;
    }
    if (m_workerBusy) {
        ++m_workerSeq;
    }
}

void DanmakuController::updateFrameTimerInterval() {
    const int fps = std::clamp(m_targetFps, 10, 120);
    const int intervalMs = std::max(1, static_cast<int>(std::lround(1000.0 / fps)));
    m_frameTimer.setInterval(intervalMs);
}

DanmakuController::RenderItem DanmakuController::buildRenderItem(const Item &item) const {
    RenderItem renderItem;
    renderItem.commentId = item.commentId;
    renderItem.text = item.text;
    renderItem.x = item.x;
    renderItem.y = item.y;
    renderItem.alpha = item.alpha;
    renderItem.widthEstimate = item.widthEstimate;
    renderItem.ngDropHovered = item.ngDropHovered;
    return renderItem;
}

void DanmakuController::queueSpatialUpsertRow(int row) {
    if (row < 0) {
        return;
    }
    if (m_pendingFullSpatialRebuild) {
        return;
    }
    m_pendingSpatialRemoveRows.remove(row);
    m_pendingSpatialUpsertRows.insert(row);
}

void DanmakuController::queueSpatialUpsertRows(const QVector<int> &rows) {
    for (const int row : rows) {
        queueSpatialUpsertRow(row);
    }
}

void DanmakuController::queueSpatialRemoveRow(int row) {
    if (row < 0) {
        return;
    }
    if (m_pendingFullSpatialRebuild) {
        return;
    }
    m_pendingSpatialUpsertRows.remove(row);
    m_pendingSpatialRemoveRows.insert(row);
}

void DanmakuController::queueSpatialRemoveRows(const QVector<int> &rows) {
    for (const int row : rows) {
        queueSpatialRemoveRow(row);
    }
}

void DanmakuController::queueSnapshotUpsertRow(int row) {
    if (row < 0) {
        return;
    }
    if (m_pendingFullSnapshotRebuild) {
        return;
    }
    m_pendingSnapshotRemoveRows.remove(row);
    m_pendingSnapshotUpsertRows.insert(row);
}

void DanmakuController::queueSnapshotUpsertRows(const QVector<int> &rows) {
    for (const int row : rows) {
        queueSnapshotUpsertRow(row);
    }
}

void DanmakuController::queueSnapshotRemoveRow(int row) {
    if (row < 0) {
        return;
    }
    if (m_pendingFullSnapshotRebuild) {
        return;
    }
    m_pendingSnapshotUpsertRows.remove(row);
    m_pendingSnapshotRemoveRows.insert(row);
}

void DanmakuController::queueSnapshotRemoveRows(const QVector<int> &rows) {
    for (const int row : rows) {
        queueSnapshotRemoveRow(row);
    }
}

void DanmakuController::queueFullSpatialRebuild() {
    m_pendingFullSpatialRebuild = true;
    m_pendingSpatialUpsertRows.clear();
    m_pendingSpatialRemoveRows.clear();
}

void DanmakuController::queueFullSnapshotRebuild() {
    m_pendingFullSnapshotRebuild = true;
    m_pendingSnapshotUpsertRows.clear();
    m_pendingSnapshotRemoveRows.clear();
}

void DanmakuController::rebuildSpatialIndex() {
    QVector<DanmakuSpatialGrid::Entry> entries;
    entries.reserve(activeItemCount());
    for (int row = 0; row < m_items.size(); ++row) {
        const Item &item = m_items[row];
        if (!item.active) {
            continue;
        }

        DanmakuSpatialGrid::Entry entry;
        entry.row = row;
        entry.rect = QRectF(item.x, item.y, item.widthEstimate, kItemHeight);
        entries.push_back(entry);
    }

    const qreal cellHeight = std::max<qreal>(kItemHeight, static_cast<qreal>(m_fontPx + m_laneGap));
    m_spatialGrid.rebuild(entries, kSpatialCellWidthPx, cellHeight);
}

void DanmakuController::rebuildRenderSnapshot() {
    m_renderCache.clear();
    m_renderRows.clear();
    m_renderCache.reserve(activeItemCount());
    m_renderRows.reserve(activeItemCount());
    m_rowToRenderIndex.resize(m_items.size());
    std::fill(m_rowToRenderIndex.begin(), m_rowToRenderIndex.end(), -1);

    for (int row = 0; row < m_items.size(); ++row) {
        const Item &item = m_items[row];
        if (!item.active) {
            continue;
        }

        m_rowToRenderIndex[row] = m_renderCache.size();
        m_renderRows.push_back(row);
        m_renderCache.push_back(buildRenderItem(item));
    }
    publishRenderSnapshot();
}

void DanmakuController::ensureRowToRenderIndexSize() {
    if (m_rowToRenderIndex.size() >= m_items.size()) {
        return;
    }

    const int oldSize = m_rowToRenderIndex.size();
    m_rowToRenderIndex.resize(m_items.size());
    std::fill(m_rowToRenderIndex.begin() + oldSize, m_rowToRenderIndex.end(), -1);
}

bool DanmakuController::applySnapshotRowUpsert(int row) {
    if (row < 0 || row >= m_items.size()) {
        return applySnapshotRowRemoval(row);
    }

    ensureRowToRenderIndexSize();
    const Item &item = m_items[row];
    if (!item.active) {
        return applySnapshotRowRemoval(row);
    }

    const int existingIndex = (row < m_rowToRenderIndex.size()) ? m_rowToRenderIndex[row] : -1;
    if (existingIndex >= 0 && existingIndex < m_renderCache.size() && existingIndex < m_renderRows.size()) {
        m_renderCache[existingIndex] = buildRenderItem(item);
        return true;
    }

    const auto it = std::lower_bound(m_renderRows.begin(), m_renderRows.end(), row);
    const int insertIndex = static_cast<int>(std::distance(m_renderRows.begin(), it));
    m_renderRows.insert(insertIndex, row);
    m_renderCache.insert(insertIndex, buildRenderItem(item));
    m_rowToRenderIndex[row] = insertIndex;
    for (int i = insertIndex + 1; i < m_renderRows.size(); ++i) {
        const int remappedRow = m_renderRows[i];
        if (remappedRow >= 0 && remappedRow < m_rowToRenderIndex.size()) {
            m_rowToRenderIndex[remappedRow] = i;
        }
    }
    return true;
}

bool DanmakuController::applySnapshotRowRemoval(int row) {
    if (row < 0 || row >= m_rowToRenderIndex.size()) {
        return false;
    }
    const int index = m_rowToRenderIndex[row];
    if (index < 0 || index >= m_renderCache.size() || index >= m_renderRows.size()) {
        m_rowToRenderIndex[row] = -1;
        return false;
    }

    m_renderCache.removeAt(index);
    m_renderRows.removeAt(index);
    m_rowToRenderIndex[row] = -1;
    for (int i = index; i < m_renderRows.size(); ++i) {
        const int remappedRow = m_renderRows[i];
        if (remappedRow >= 0 && remappedRow < m_rowToRenderIndex.size()) {
            m_rowToRenderIndex[remappedRow] = i;
        }
    }
    return true;
}

void DanmakuController::publishRenderSnapshot() {
    auto newSnapshot = QSharedPointer<QVector<RenderItem>>::create(m_renderCache);
    QMutexLocker locker(&m_renderSnapshotMutex);
    m_renderSnapshot = newSnapshot;
}

void DanmakuController::flushPendingDiffs(bool emitSnapshotSignal) {
    const auto sortedRows = [](const QSet<int> &rows, bool descending) {
        QVector<int> sorted = rows.values();
        if (descending) {
            std::sort(sorted.begin(), sorted.end(), std::greater<int>());
        } else {
            std::sort(sorted.begin(), sorted.end());
        }
        return sorted;
    };

    if (m_pendingFullSpatialRebuild) {
        rebuildSpatialIndex();
        m_pendingFullSpatialRebuild = false;
        m_pendingSpatialUpsertRows.clear();
        m_pendingSpatialRemoveRows.clear();
        if (m_perfLogEnabled) {
            ++m_perfSpatialFullRebuildCount;
        }
    } else if (!m_pendingSpatialUpsertRows.isEmpty() || !m_pendingSpatialRemoveRows.isEmpty()) {
        const qreal cellHeight = std::max<qreal>(kItemHeight, static_cast<qreal>(m_fontPx + m_laneGap));
        m_spatialGrid.setCellSize(kSpatialCellWidthPx, cellHeight);
        const QVector<int> removedRows = sortedRows(m_pendingSpatialRemoveRows, false);
        const QVector<int> upsertRows = sortedRows(m_pendingSpatialUpsertRows, false);
        for (const int row : removedRows) {
            m_spatialGrid.removeRow(row);
        }
        for (const int row : upsertRows) {
            if (row < 0 || row >= m_items.size()) {
                m_spatialGrid.removeRow(row);
                continue;
            }
            const Item &item = m_items[row];
            if (!item.active) {
                m_spatialGrid.removeRow(row);
                continue;
            }
            m_spatialGrid.upsertRow(row, QRectF(item.x, item.y, item.widthEstimate, kItemHeight));
        }
        if (m_perfLogEnabled) {
            m_perfSpatialRowUpdateCount += removedRows.size() + upsertRows.size();
        }
        m_pendingSpatialUpsertRows.clear();
        m_pendingSpatialRemoveRows.clear();
    }

    bool snapshotChanged = false;
    if (m_pendingFullSnapshotRebuild) {
        rebuildRenderSnapshot();
        m_pendingFullSnapshotRebuild = false;
        m_pendingSnapshotUpsertRows.clear();
        m_pendingSnapshotRemoveRows.clear();
        snapshotChanged = true;
        if (m_perfLogEnabled) {
            ++m_perfSnapshotFullRebuildCount;
        }
    } else if (!m_pendingSnapshotUpsertRows.isEmpty() || !m_pendingSnapshotRemoveRows.isEmpty()) {
        const QVector<int> removedRows = sortedRows(m_pendingSnapshotRemoveRows, true);
        const QVector<int> upsertRows = sortedRows(m_pendingSnapshotUpsertRows, false);
        bool rowChanged = false;
        for (const int row : removedRows) {
            rowChanged = applySnapshotRowRemoval(row) || rowChanged;
        }
        for (const int row : upsertRows) {
            rowChanged = applySnapshotRowUpsert(row) || rowChanged;
        }
        if (rowChanged) {
            publishRenderSnapshot();
            snapshotChanged = true;
        }
        if (m_perfLogEnabled) {
            m_perfSnapshotRowUpdateCount += removedRows.size() + upsertRows.size();
        }
        m_pendingSnapshotUpsertRows.clear();
        m_pendingSnapshotRemoveRows.clear();
    }

    if (snapshotChanged && emitSnapshotSignal) {
        emit renderSnapshotChanged();
    }
}

void DanmakuController::maybeWritePerfLog(qint64 nowMs) {
    if (!m_perfLogEnabled) {
        return;
    }

    if (m_perfLogWindowStartMs <= 0) {
        m_perfLogWindowStartMs = nowMs;
        return;
    }

    const qint64 elapsedMs = nowMs - m_perfLogWindowStartMs;
    if (elapsedMs < 2000) {
        return;
    }

    QVector<int> sortedSamples = m_perfFrameSamplesMs;
    std::sort(sortedSamples.begin(), sortedSamples.end());
    const int sampleCount = sortedSamples.size();
    const auto percentileMs = [&sortedSamples](double p) -> double {
        if (sortedSamples.isEmpty()) {
            return 0.0;
        }

        const double rank = std::ceil((p / 100.0) * sortedSamples.size());
        const int maxIndex = static_cast<int>(sortedSamples.size()) - 1;
        const int index = std::clamp(static_cast<int>(rank) - 1, 0, maxIndex);
        return sortedSamples[index];
    };
    const double avgMs = sampleCount > 0
        ? static_cast<double>(std::accumulate(sortedSamples.begin(), sortedSamples.end(), 0LL)) / sampleCount
        : 0.0;
    const double p50Ms = percentileMs(50.0);
    const double p95Ms = percentileMs(95.0);
    const double p99Ms = percentileMs(99.0);
    const double maxMs = sampleCount > 0 ? sortedSamples.last() : 0.0;
    const double fps = elapsedMs > 0 ? (m_perfLogFrameCount * 1000.0 / elapsedMs) : 0.0;
    const int rowsTotal = m_items.size();
    const int rowsFree = m_freeRows.size();
    const int rowsActive = activeItemCount();
    const double laneWaitAvgMs = m_perfLanePickCount > 0
        ? (static_cast<double>(m_perfLaneWaitTotalMs) / m_perfLanePickCount)
        : 0.0;
    qInfo().noquote()
        << QString("[perf-danmaku] window_ms=%1 frame_count=%2 fps=%3 avg_ms=%4 p50_ms=%5 p95_ms=%6 p99_ms=%7 max_ms=%8 rows_total=%9 rows_active=%10 rows_free=%11 compacted=%12 appended=%13 updates=%14 removed=%15 lane_pick_count=%16 lane_ready_count=%17 lane_forced_count=%18 lane_wait_ms_avg=%19 lane_wait_ms_max=%20 dragging=%21 paused=%22 rate=%23 spatial_full_rebuilds=%24 spatial_row_updates=%25 snapshot_full_rebuilds=%26 snapshot_row_updates=%27")
               .arg(elapsedMs)
               .arg(m_perfLogFrameCount)
               .arg(fps, 0, 'f', 1)
               .arg(avgMs, 0, 'f', 2)
               .arg(p50Ms, 0, 'f', 2)
               .arg(p95Ms, 0, 'f', 2)
               .arg(p99Ms, 0, 'f', 2)
               .arg(maxMs, 0, 'f', 2)
               .arg(rowsTotal)
               .arg(rowsActive)
               .arg(rowsFree)
               .arg(m_perfCompactedSinceLastLog ? 1 : 0)
               .arg(m_perfLogAppendCount)
               .arg(m_perfLogGeometryUpdateCount)
               .arg(m_perfLogRemovedCount)
               .arg(m_perfLanePickCount)
               .arg(m_perfLaneReadyCount)
               .arg(m_perfLaneForcedCount)
               .arg(laneWaitAvgMs, 0, 'f', 2)
               .arg(m_perfLaneWaitMaxMs)
               .arg(hasDragging() ? 1 : 0)
               .arg(m_playbackPaused ? 1 : 0)
               .arg(m_playbackRate, 0, 'f', 2)
               .arg(m_perfSpatialFullRebuildCount)
               .arg(m_perfSpatialRowUpdateCount)
               .arg(m_perfSnapshotFullRebuildCount)
               .arg(m_perfSnapshotRowUpdateCount);
    qInfo().noquote()
        << QString("[perf-glyph] window_ms=%1 new_cp_total=%2 new_cp_non_ascii=%3 warmup_sent_cp=%4 warmup_batches=%5 warmup_pending_cp=%6 warmup_dropped_cp=%7 warmup_enabled=%8 p95_ms=%9 p99_ms=%10")
               .arg(elapsedMs)
               .arg(m_perfGlyphNewCodepoints)
               .arg(m_perfGlyphNewNonAsciiCodepoints)
               .arg(m_perfGlyphWarmupSentCodepoints)
               .arg(m_perfGlyphWarmupBatchCount)
               .arg(m_glyphWarmupQueue.size())
               .arg(m_perfGlyphWarmupDroppedCodepoints)
               .arg(m_glyphWarmupEnabled ? 1 : 0)
               .arg(p95Ms, 0, 'f', 2)
               .arg(p99Ms, 0, 'f', 2);

    m_perfLogWindowStartMs = nowMs;
    m_perfLogFrameCount = 0;
    m_perfFrameSamplesMs.clear();
    m_perfLogAppendCount = 0;
    m_perfLogGeometryUpdateCount = 0;
    m_perfLogRemovedCount = 0;
    m_perfLanePickCount = 0;
    m_perfLaneReadyCount = 0;
    m_perfLaneForcedCount = 0;
    m_perfLaneWaitTotalMs = 0;
    m_perfLaneWaitMaxMs = 0;
    m_perfSpatialFullRebuildCount = 0;
    m_perfSpatialRowUpdateCount = 0;
    m_perfSnapshotFullRebuildCount = 0;
    m_perfSnapshotRowUpdateCount = 0;
    m_perfCompactedSinceLastLog = false;
    m_perfGlyphNewCodepoints = 0;
    m_perfGlyphNewNonAsciiCodepoints = 0;
    m_perfGlyphWarmupSentCodepoints = 0;
    m_perfGlyphWarmupBatchCount = 0;
    m_perfGlyphWarmupDroppedCodepoints = 0;
}
