#include "danmaku/DanmakuUpdateWorker.hpp"

#include <algorithm>

DanmakuUpdateWorker::DanmakuUpdateWorker(QObject *parent) : QObject(parent) {}

void DanmakuUpdateWorker::setSimdMode(DanmakuSimdMode mode) {
    m_simdMode = mode;
}

void DanmakuUpdateWorker::processFrame(DanmakuWorkerFramePtr frame) {
    if (frame.isNull()) {
        emit frameProcessed({});
        return;
    }

    QVector<int> &rows = frame->state.rows;
    QVector<qreal> &x = frame->state.x;
    QVector<qreal> &y = frame->state.y;
    QVector<qreal> &speed = frame->state.speed;
    QVector<qreal> &alpha = frame->state.alpha;
    QVector<int> &widthEstimate = frame->state.widthEstimate;
    QVector<int> &fadeRemainingMs = frame->state.fadeRemainingMs;
    QVector<quint8> &flags = frame->state.flags;
    QVector<int> &changedRows = frame->changedRows;
    QVector<int> &removeRows = frame->removeRows;
    changedRows.clear();
    removeRows.clear();

    const int count = rows.size();
    if (count <= 0
        || x.size() != count
        || y.size() != count
        || speed.size() != count
        || alpha.size() != count
        || widthEstimate.size() != count
        || fadeRemainingMs.size() != count
        || flags.size() != count) {
        emit frameProcessed(frame);
        return;
    }

    if (m_movableMask.size() != count) {
        m_movableMask.resize(count);
    }
    if (m_changedMask.size() != count) {
        m_changedMask.resize(count);
    }
    std::fill(m_changedMask.begin(), m_changedMask.end(), 0);
    for (int i = 0; i < count; ++i) {
        const bool frozen = (flags[i] & DanmakuSoAFlagFrozen) != 0;
        m_movableMask[i] = (!frame->playbackPaused && !frozen) ? 1 : 0;
    }

    const qreal movementFactor = (frame->elapsedMs / 1000.0) * frame->playbackRate;
    DanmakuSimdUpdater::updatePositions(x, speed, m_movableMask, movementFactor, m_changedMask, m_simdMode);
    changedRows.reserve(count);
    removeRows.reserve(count / 4);

    for (int i = 0; i < count; ++i) {
        const bool fading = (flags[i] & DanmakuSoAFlagFading) != 0;
        const bool dragging = (flags[i] & DanmakuSoAFlagDragging) != 0;

        if (fading) {
            fadeRemainingMs[i] -= frame->elapsedMs;
            if (fadeRemainingMs[i] <= 0) {
                alpha[i] = 0.0;
            } else {
                alpha[i] = std::clamp(fadeRemainingMs[i] / 300.0, 0.0, 1.0);
            }
            m_changedMask[i] = 1;
        }

        if (m_changedMask[i]) {
            changedRows.push_back(rows[i]);
        }

        const bool outOfHorizontalBounds = x[i] + widthEstimate[i] < frame->cullThreshold;
        const bool outOfVerticalBounds = y[i] > frame->viewportHeight || y[i] + frame->itemHeight < 0.0;
        const bool canCull = !dragging && (alpha[i] <= 0.0 || outOfHorizontalBounds || outOfVerticalBounds);
        if (canCull) {
            removeRows.push_back(rows[i]);
        }
    }

    emit frameProcessed(frame);
}
