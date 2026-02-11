#include "danmaku/DanmakuController.hpp"

#include <QDateTime>
#include <QVariantMap>
#include <algorithm>
#include <limits>

DanmakuController::DanmakuController(QObject *parent) : QObject(parent) {
    m_lastTickMs = QDateTime::currentMSecsSinceEpoch();
    m_frameTimer.setInterval(16);
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

void DanmakuController::appendFromCore(const QVariantList &comments) {
    for (const QVariant &entry : comments) {
        const QVariantMap map = entry.toMap();
        Item item;
        item.commentId = map.value("comment_id").toString();
        item.userId = map.value("user_id").toString();
        item.text = map.value("text").toString();
        const int textWidthEstimate = static_cast<int>(item.text.size()) * (m_fontPx / 2 + 4);
        item.widthEstimate = std::max(80, textWidthEstimate);
        item.speedPxPerSec = 120 + (qHash(item.commentId) % 70);

        item.lane = pickLane(item.widthEstimate);
        item.originalLane = item.lane;
        item.x = m_viewportWidth + 12;
        item.y = item.lane * (m_fontPx + m_laneGap) + 10;

        m_items.push_back(item);
    }
    rebuildModel();
}

void DanmakuController::beginDrag(const QString &commentId) {
    Item *item = findItem(commentId);
    if (!item) {
        return;
    }
    item->frozen = true;
    item->dragging = true;
    item->originalLane = item->lane;
    updateNgZoneVisibility();
    rebuildModel();
}

void DanmakuController::moveDrag(const QString &commentId, qreal x, qreal y) {
    Item *item = findItem(commentId);
    if (!item || !item->dragging) {
        return;
    }
    item->x = x;
    item->y = y;
    rebuildModel();
}

void DanmakuController::dropDrag(const QString &commentId, bool inNgZone) {
    Item *item = findItem(commentId);
    if (!item) {
        return;
    }

    if (inNgZone) {
        const QString userId = item->userId;
        m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [&](const Item &candidate) {
                          return candidate.commentId == commentId;
                      }),
            m_items.end());
        emit ngDropRequested(userId);
    } else {
        item->dragging = false;
        item->frozen = false;
        recoverToLane(*item);
    }

    updateNgZoneVisibility();
    rebuildModel();
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
    rebuildModel();
}

void DanmakuController::resetForSeek() {
    if (m_items.isEmpty()) {
        return;
    }
    m_items.clear();
    rebuildModel();
}

QVariantList DanmakuController::items() const {
    return m_itemsModel;
}

bool DanmakuController::ngDropZoneVisible() const {
    return m_ngDropZoneVisible;
}

bool DanmakuController::playbackPaused() const {
    return m_playbackPaused;
}

void DanmakuController::onFrame() {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int elapsedMs = static_cast<int>(now - m_lastTickMs);
    m_lastTickMs = now;

    if (elapsedMs <= 0 || m_items.isEmpty()) {
        return;
    }

    const qreal elapsedSec = elapsedMs / 1000.0;
    bool changed = false;

    for (Item &item : m_items) {
        if (!m_playbackPaused && !item.frozen) {
            item.x -= item.speedPxPerSec * elapsedSec;
            changed = true;
        }

        if (item.fading) {
            item.fadeRemainingMs -= elapsedMs;
            if (item.fadeRemainingMs <= 0) {
                item.alpha = 0.0;
            } else {
                item.alpha = std::clamp(item.fadeRemainingMs / 300.0, 0.0, 1.0);
            }
            changed = true;
        }
    }

    const int before = m_items.size();
    m_items.erase(std::remove_if(m_items.begin(), m_items.end(), [&](const Item &item) {
                      return item.alpha <= 0.0 || item.x + item.widthEstimate < -20;
                  }),
        m_items.end());

    if (before != m_items.size()) {
        changed = true;
    }

    if (changed) {
        rebuildModel();
    }
}

void DanmakuController::rebuildModel() {
    QVariantList list;
    list.reserve(m_items.size());
    for (const Item &item : m_items) {
        QVariantMap map;
        map.insert("commentId", item.commentId);
        map.insert("userId", item.userId);
        map.insert("text", item.text);
        map.insert("x", item.x);
        map.insert("y", item.y);
        map.insert("alpha", item.alpha);
        map.insert("lane", item.lane);
        map.insert("dragging", item.dragging);
        map.insert("widthEstimate", item.widthEstimate);
        list.push_back(map);
    }

    m_itemsModel = list;
    emit itemsChanged();
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
    item.y = item.originalLane * (m_fontPx + m_laneGap) + 10;
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
            item.y = up * (m_fontPx + m_laneGap) + 10;
            if (!laneHasCollision(item.lane, item)) {
                return;
            }
        }

        if (down < lanes) {
            item.lane = down;
            item.y = down * (m_fontPx + m_laneGap) + 10;
            if (!laneHasCollision(item.lane, item)) {
                return;
            }
        }
    }
}

DanmakuController::Item *DanmakuController::findItem(const QString &commentId) {
    for (Item &item : m_items) {
        if (item.commentId == commentId) {
            return &item;
        }
    }
    return nullptr;
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
