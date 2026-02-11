#include "danmaku/DanmakuController.hpp"

#include <QDateTime>
#include <QVariantMap>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr qreal kLaneTopMargin = 10.0;
constexpr qreal kSpawnOffset = 12.0;
constexpr qreal kItemHeight = 42.0;
constexpr qreal kItemCullThreshold = -20.0;
constexpr qint64 kMaxLagCompensationMs = 2000;
}

DanmakuController::DanmakuController(QObject *parent) : QObject(parent) {
    m_lastTickMs = QDateTime::currentMSecsSinceEpoch();
    m_perfLogWindowStartMs = m_lastTickMs;
    m_frameTimer.setInterval(33);
    connect(&m_frameTimer, &QTimer::timeout, this, &DanmakuController::onFrame);
    m_frameTimer.start();
}

void DanmakuController::setViewportSize(qreal width, qreal height) {
    m_viewportWidth = width;
    m_viewportHeight = height;
}

void DanmakuController::setLaneMetrics(int fontPx, int laneGap) {
    m_fontPx = std::max(fontPx, 12);
    m_laneGap = std::max(laneGap, 0);
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
    m_perfLogAppendCount = 0;
    m_perfLogGeometryUpdateCount = 0;
    m_perfLogRemovedCount = 0;
    emit perfLogEnabledChanged();
}

void DanmakuController::appendFromCore(const QVariantList &comments, qint64 playbackPositionMs) {
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

        item.lane = pickLane(item.widthEstimate);
        item.originalLane = item.lane;
        item.x = m_viewportWidth + kSpawnOffset;
        item.y = item.lane * (m_fontPx + m_laneGap) + kLaneTopMargin;

        const qint64 lagMs = std::clamp(playbackPositionMs - atMs, qint64(0), kMaxLagCompensationMs);
        const qreal lagSec = lagMs / 1000.0;
        item.x -= (item.speedPxPerSec * m_playbackRate) * lagSec;
        if (item.x + item.widthEstimate < kItemCullThreshold) {
            continue;
        }

        m_items.push_back(item);
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
        m_itemModel.append(row);
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
    if (item.dragging) {
        return;
    }
    item.frozen = true;
    item.dragging = true;
    item.originalLane = item.lane;
    item.ngDropHovered = isItemInNgZone(item);
    m_itemModel.setDragState(index, true);
    m_itemModel.setNgDropHovered(index, item.ngDropHovered);
    if (m_dragVisualElapsedMs != 0) {
        m_dragVisualElapsedMs = 0;
        emit dragVisualElapsedMsChanged();
    }
    updateNgZoneVisibility();
}

void DanmakuController::moveDrag(const QString &commentId, qreal x, qreal y) {
    const int index = findItemIndex(commentId);
    if (index < 0) {
        return;
    }
    Item &item = m_items[index];
    if (!item.dragging) {
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
        if (!item.dragging) {
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

    const bool resolvedInNgZone = inNgZone || isItemInNgZone(item);

    if (resolvedInNgZone) {
        const QString userId = item.userId;
        m_items.removeAt(index);
        m_itemModel.removeAt(index);
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
    if (!hasDragging() && m_dragVisualElapsedMs != 0) {
        m_dragVisualElapsedMs = 0;
        emit dragVisualElapsedMsChanged();
    }
}

void DanmakuController::cancelDrag(const QString &commentId) {
    dropDrag(commentId, false);
}

void DanmakuController::applyNgUserFade(const QString &userId) {
    for (Item &item : m_items) {
        if (item.userId == userId) {
            item.fading = true;
            item.fadeRemainingMs = 300;
        }
    }
}

void DanmakuController::resetForSeek() {
    if (m_dragVisualElapsedMs != 0) {
        m_dragVisualElapsedMs = 0;
        emit dragVisualElapsedMsChanged();
    }
    if (!m_items.isEmpty()) {
        m_items.clear();
        m_itemModel.clear();
    }
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

qint64 DanmakuController::dragVisualElapsedMs() const {
    return m_dragVisualElapsedMs;
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
    }

    const bool dragging = hasDragging();
    if (dragging) {
        if (!m_playbackPaused) {
            const qint64 scaledElapsedMs = static_cast<qint64>(std::llround(elapsedMs * m_playbackRate));
            if (scaledElapsedMs > 0) {
                m_dragVisualElapsedMs += scaledElapsedMs;
                emit dragVisualElapsedMsChanged();
            }
        }
    } else if (m_dragVisualElapsedMs != 0) {
        m_dragVisualElapsedMs = 0;
        emit dragVisualElapsedMsChanged();
    }

    if (m_items.isEmpty()) {
        maybeWritePerfLog(now);
        return;
    }

    const qreal elapsedSec = elapsedMs / 1000.0;
    QVector<int> removeRows;
    removeRows.reserve(m_items.size());
    int frameGeometryUpdates = 0;

    for (int i = 0; i < m_items.size(); ++i) {
        Item &item = m_items[i];
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
            m_itemModel.setGeometry(i, item.x, item.y, item.alpha);
            ++frameGeometryUpdates;
        }

        if (!dragging && (item.alpha <= 0.0 || item.x + item.widthEstimate < kItemCullThreshold)) {
            removeRows.push_back(i);
        }
    }

    if (removeRows.isEmpty()) {
        if (m_perfLogEnabled) {
            m_perfLogGeometryUpdateCount += frameGeometryUpdates;
            maybeWritePerfLog(now);
        }
        return;
    }

    std::sort(removeRows.begin(), removeRows.end(), std::greater<int>());
    removeRows.erase(std::unique(removeRows.begin(), removeRows.end()), removeRows.end());
    for (const int row : removeRows) {
        m_items.removeAt(row);
    }
    m_itemModel.removeRowsDescending(removeRows);
    if (m_perfLogEnabled) {
        m_perfLogGeometryUpdateCount += frameGeometryUpdates;
        m_perfLogRemovedCount += removeRows.size();
    }
    maybeWritePerfLog(now);
}

int DanmakuController::laneCount() const {
    const int laneHeight = m_fontPx + m_laneGap;
    if (laneHeight <= 0) {
        return 1;
    }
    return std::max(1, static_cast<int>(m_viewportHeight / laneHeight));
}

int DanmakuController::pickLane(int widthEstimate) const {
    const int lanes = laneCount();
    int bestLane = 0;
    qreal bestTail = std::numeric_limits<qreal>::max();

    for (int lane = 0; lane < lanes; ++lane) {
        qreal tail = -1e9;
        for (const Item &item : m_items) {
            if (item.lane != lane || item.dragging) {
                continue;
            }
            tail = std::max(tail, item.x + item.widthEstimate + 20.0);
        }

        if (tail < bestTail) {
            bestTail = tail;
            bestLane = lane;
        }
    }

    Q_UNUSED(widthEstimate)
    return bestLane;
}

bool DanmakuController::laneHasCollision(int lane, const Item &candidate) const {
    for (const Item &item : m_items) {
        if (item.commentId == candidate.commentId || item.lane != lane) {
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
        if (m_items[i].commentId == commentId) {
            return i;
        }
    }
    return -1;
}

bool DanmakuController::hasDragging() const {
    for (const Item &item : m_items) {
        if (item.dragging) {
            return true;
        }
    }
    return false;
}

void DanmakuController::updateNgZoneVisibility() {
    bool visible = false;
    for (const Item &item : m_items) {
        if (item.dragging) {
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

    const double fps = elapsedMs > 0 ? (m_perfLogFrameCount * 1000.0 / elapsedMs) : 0.0;
    qInfo().noquote()
        << QString("[perf] window_ms=%1 fps=%2 active=%3 appended=%4 updates=%5 removed=%6 dragging=%7 paused=%8 rate=%9")
               .arg(elapsedMs)
               .arg(fps, 0, 'f', 1)
               .arg(m_items.size())
               .arg(m_perfLogAppendCount)
               .arg(m_perfLogGeometryUpdateCount)
               .arg(m_perfLogRemovedCount)
               .arg(hasDragging() ? 1 : 0)
               .arg(m_playbackPaused ? 1 : 0)
               .arg(m_playbackRate, 0, 'f', 2);

    m_perfLogWindowStartMs = nowMs;
    m_perfLogFrameCount = 0;
    m_perfLogAppendCount = 0;
    m_perfLogGeometryUpdateCount = 0;
    m_perfLogRemovedCount = 0;
}
