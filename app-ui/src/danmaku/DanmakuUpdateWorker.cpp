#include "danmaku/DanmakuUpdateWorker.hpp"

#include <algorithm>

DanmakuUpdateWorker::DanmakuUpdateWorker(QObject *parent) : QObject(parent) {}

void DanmakuUpdateWorker::setSimdMode(DanmakuSimdMode mode) {
    m_simdMode = mode;
}

void DanmakuUpdateWorker::processFrame(DanmakuFrameInput input) {
    QVector<int> &rows = input.state.rows;
    QVector<qreal> &x = input.state.x;
    QVector<qreal> &y = input.state.y;
    QVector<qreal> &speed = input.state.speed;
    QVector<qreal> &alpha = input.state.alpha;
    QVector<int> &widthEstimate = input.state.widthEstimate;
    QVector<int> &fadeRemainingMs = input.state.fadeRemainingMs;
    QVector<quint8> &flags = input.state.flags;

    const int count = rows.size();
    if (count <= 0
        || x.size() != count
        || y.size() != count
        || speed.size() != count
        || alpha.size() != count
        || widthEstimate.size() != count
        || fadeRemainingMs.size() != count
        || flags.size() != count) {
        emit frameProcessed(input.seq, {}, {}, {}, {}, {}, {}, {}, {});
        return;
    }

    QVector<quint8> movableMask(count, 0);
    QVector<quint8> changedMask(count, 0);
    for (int i = 0; i < count; ++i) {
        const bool frozen = (flags[i] & DanmakuSoAFlagFrozen) != 0;
        movableMask[i] = (!input.playbackPaused && !frozen) ? 1 : 0;
    }

    const qreal movementFactor = (input.elapsedMs / 1000.0) * input.playbackRate;
    DanmakuSimdUpdater::updatePositions(x, speed, movableMask, movementFactor, changedMask, m_simdMode);

    QVector<int> changedRows;
    QVector<int> removeRows;
    changedRows.reserve(count);
    removeRows.reserve(count / 4);

    for (int i = 0; i < count; ++i) {
        const bool fading = (flags[i] & DanmakuSoAFlagFading) != 0;
        const bool dragging = (flags[i] & DanmakuSoAFlagDragging) != 0;

        if (fading) {
            fadeRemainingMs[i] -= input.elapsedMs;
            if (fadeRemainingMs[i] <= 0) {
                alpha[i] = 0.0;
            } else {
                alpha[i] = std::clamp(fadeRemainingMs[i] / 300.0, 0.0, 1.0);
            }
            changedMask[i] = 1;
        }

        if (changedMask[i]) {
            changedRows.push_back(rows[i]);
        }

        const bool outOfHorizontalBounds = x[i] + widthEstimate[i] < input.cullThreshold;
        const bool outOfVerticalBounds = y[i] > input.viewportHeight || y[i] + input.itemHeight < 0.0;
        const bool canCull = !dragging && (alpha[i] <= 0.0 || outOfHorizontalBounds || outOfVerticalBounds);
        if (canCull) {
            removeRows.push_back(rows[i]);
        }
    }

    emit frameProcessed(
        input.seq,
        std::move(rows),
        std::move(x),
        std::move(y),
        std::move(alpha),
        std::move(fadeRemainingMs),
        std::move(flags),
        std::move(changedRows),
        std::move(removeRows));
}
