#include "danmaku/DanmakuController.hpp"

#include <QDateTime>
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
}

DanmakuController::DanmakuController(QObject *parent) : QObject(parent) {
    m_lastTickMs = QDateTime::currentMSecsSinceEpoch();
    m_perfLogWindowStartMs = m_lastTickMs;
    m_frameTimer.setInterval(33);
    connect(&m_frameTimer, &QTimer::timeout, this, &DanmakuController::onFrame);
    m_frameTimer.start();
    ensureLaneStateSize();
    resetLaneStates();
}

void DanmakuController::setViewportSize(qreal width, qreal height) {
    m_viewportWidth = width;
    m_viewportHeight = height;
    ensureLaneStateSize();
}

void DanmakuController::setLaneMetrics(int fontPx, int laneGap) {
    m_fontPx = std::max(fontPx, 12);
    m_laneGap = std::max(laneGap, 0);
    ensureLaneStateSize();
}

void DanmakuController::setPlaybackPaused(bool paused) {
    if (m_playbackPaused == paused) {
        return;
    }
    m_playbackPaused = paused;
    emit playbackPausedChanged();
}

void DanmakuController::setPlaybackRate(double rate) {
    const double normalized = std::clamp(rate, 0.5, 3.0);
    if (qFuzzyCompare(m_playbackRate + 1.0, normalized + 1.0)) {
        return;
    }
    m_playbackRate = normalized;
    emit playbackRateChanged();
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
    m_perfCompactedSinceLastLog = false;
    emit perfLogEnabledChanged();
}

void DanmakuController::appendFromCore(const QVariantList &comments, qint64 playbackPositionMs) {
    ensureLaneStateSize();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (const QVariant &entry : comments) {
        const QVariantMap map = entry.toMap();
        Item item;
        item.commentId = map.value("comment_id").toString();
        item.userId = map.value("user_id").toString();
        item.text = map.value("text").toString();
        if (item.commentId.isEmpty()) {
            continue;
        }

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
        LaneState &laneState = m_laneStates[item.lane];
        laneState.nextAvailableAtMs = std::max(laneState.nextAvailableAtMs, nowMs) + estimateLaneCooldownMs(item);
        laneState.lastAssignedRow = row;

        const DanmakuListModel::Row modelRow = makeRow(item);
        if (row < m_itemModel.rowCount()) {
            m_itemModel.overwriteRow(row, modelRow);
        } else {
            m_itemModel.append(modelRow);
        }

        if (m_perfLogEnabled) {
            ++m_perfLogAppendCount;
        }
    }
}

void DanmakuController::beginDrag(const QString &commentId) {
    const int index = findItemIndex(commentId);
    if (index < 0) {
        return;
    }
    Item &item = m_items[index];
    if (!item.active || item.dragging) {
        return;
    }
    item.frozen = true;
    item.dragging = true;
    item.originalLane = item.lane;
    item.ngDropHovered = isItemInNgZone(item);
    m_itemModel.setDragState(index, true);
    m_itemModel.setNgDropHovered(index, item.ngDropHovered);
    updateNgZoneVisibility();
}

void DanmakuController::moveDrag(const QString &commentId, qreal x, qreal y) {
    const int index = findItemIndex(commentId);
    if (index < 0) {
        return;
    }
    Item &item = m_items[index];
    if (!item.active || !item.dragging) {
        return;
    }
    item.x = x;
    item.y = y;
    const bool hovered = isItemInNgZone(item);
    if (hovered != item.ngDropHovered) {
        item.ngDropHovered = hovered;
        m_itemModel.setNgDropHovered(index, hovered);
    }
    m_itemModel.setGeometry(index, item.x, item.y, item.alpha);
}

void DanmakuController::setNgDropZoneRect(qreal x, qreal y, qreal width, qreal height) {
    m_ngZoneX = x;
    m_ngZoneY = y;
    m_ngZoneWidth = std::max(0.0, width);
    m_ngZoneHeight = std::max(0.0, height);

    if (!hasDragging()) {
        return;
    }

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
        m_itemModel.setNgDropHovered(i, hovered);
    }
}

void DanmakuController::dropDrag(const QString &commentId, bool inNgZone) {
    const int index = findItemIndex(commentId);
    if (index < 0) {
        return;
    }
    Item &item = m_items[index];
    if (!item.active) {
        return;
    }

    const bool resolvedInNgZone = inNgZone || isItemInNgZone(item);

    if (resolvedInNgZone) {
        const QString userId = item.userId;
        releaseRow(index);
        emit ngDropRequested(userId);
    } else {
        item.dragging = false;
        item.frozen = false;
        item.ngDropHovered = false;
        recoverToLane(item);
        m_itemModel.setDragState(index, false);
        m_itemModel.setLane(index, item.lane);
        m_itemModel.setNgDropHovered(index, false);
        m_itemModel.setGeometry(index, item.x, item.y, item.alpha);
    }

    updateNgZoneVisibility();
}

void DanmakuController::cancelDrag(const QString &commentId) {
    dropDrag(commentId, false);
}

void DanmakuController::applyNgUserFade(const QString &userId) {
    for (Item &item : m_items) {
        if (item.active && item.userId == userId) {
            item.fading = true;
            item.fadeRemainingMs = 300;
        }
    }
}

void DanmakuController::resetForSeek() {
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
    updateNgZoneVisibility();
}

QObject *DanmakuController::itemModel() {
    return &m_itemModel;
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

bool DanmakuController::perfLogEnabled() const {
    return m_perfLogEnabled;
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

    if (activeItemCount() == 0) {
        maybeWritePerfLog(now);
        return;
    }

    const qreal elapsedSec = elapsedMs / 1000.0;
    QVector<int> removeRows;
    removeRows.reserve(m_items.size());
    QVector<DanmakuListModel::GeometryUpdate> geometryUpdates;
    geometryUpdates.reserve(m_items.size());
    int frameGeometryUpdates = 0;

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
                m_itemModel.setNgDropHovered(i, hovered);
            }
        }

        if (geometryChanged) {
            geometryUpdates.push_back({i, item.x, item.y, item.alpha});
            ++frameGeometryUpdates;
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
    if (!geometryUpdates.isEmpty()) {
        m_itemModel.setGeometryBatch(geometryUpdates);
    }
    if (!removeRows.isEmpty()) {
        releaseRowsDescending(removeRows);
        if (m_perfLogEnabled) {
            m_perfLogRemovedCount += removeRows.size();
        }
    }
    maybeCompactRows();
    maybeWritePerfLog(now);
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

bool DanmakuController::laneHasCollision(int lane, const Item &candidate) const {
    for (const Item &item : m_items) {
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

int DanmakuController::findItemIndex(const QString &commentId) const {
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].active && m_items[i].commentId == commentId) {
            return i;
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

    m_itemModel.setDragState(row, false);
    m_itemModel.setNgDropHovered(row, false);
    m_itemModel.setActive(row, false);
    m_freeRows.push_back(row);
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

void DanmakuController::maybeCompactRows() {
    if (hasDragging()) {
        return;
    }

    const int totalRows = m_items.size();
    const int freeRows = m_freeRows.size();
    if (totalRows == 0 || freeRows <= kFreeRowsSoftLimit) {
        return;
    }

    const double freeRatio = totalRows > 0 ? (static_cast<double>(freeRows) / totalRows) : 0.0;
    if (freeRatio < kCompactTriggerRatio) {
        return;
    }

    QVector<Item> compactItems;
    compactItems.reserve(totalRows - freeRows);
    QVector<DanmakuListModel::Row> compactRows;
    compactRows.reserve(totalRows - freeRows);

    for (const Item &item : m_items) {
        if (!item.active) {
            continue;
        }
        compactItems.push_back(item);
        compactRows.push_back(makeRow(item));
    }

    m_items = std::move(compactItems);
    m_itemModel.resetRows(compactRows);
    m_freeRows.clear();
    for (LaneState &state : m_laneStates) {
        state.lastAssignedRow = -1;
    }
    m_perfCompactedSinceLastLog = true;
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

DanmakuListModel::Row DanmakuController::makeRow(const Item &item) const {
    DanmakuListModel::Row row;
    row.commentId = item.commentId;
    row.userId = item.userId;
    row.text = item.text;
    row.posX = item.x;
    row.posY = item.y;
    row.alpha = item.alpha;
    row.lane = item.lane;
    row.dragging = item.dragging;
    row.widthEstimate = item.widthEstimate;
    row.speedPxPerSec = item.speedPxPerSec;
    row.ngDropHovered = item.ngDropHovered;
    row.active = item.active;
    return row;
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
        << QString("[perf-danmaku] window_ms=%1 frame_count=%2 fps=%3 avg_ms=%4 p50_ms=%5 p95_ms=%6 p99_ms=%7 max_ms=%8 rows_total=%9 rows_active=%10 rows_free=%11 compacted=%12 appended=%13 updates=%14 removed=%15 lane_pick_count=%16 lane_ready_count=%17 lane_forced_count=%18 lane_wait_ms_avg=%19 lane_wait_ms_max=%20 dragging=%21 paused=%22 rate=%23")
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
               .arg(m_playbackRate, 0, 'f', 2);

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
    m_perfCompactedSinceLastLog = false;
}
