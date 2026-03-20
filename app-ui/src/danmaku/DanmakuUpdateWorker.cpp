#include "danmaku/DanmakuUpdateWorker.hpp"

#include <algorithm>

DanmakuUpdateWorker::DanmakuUpdateWorker(QObject *parent) : QObject(parent) {}

void DanmakuUpdateWorker::setSimdMode(DanmakuSimdMode mode) {
    m_simdMode = mode;
}

void DanmakuUpdateWorker::syncState(DanmakuWorkerSyncBatchPtr batch) {
    if (batch.isNull()) {
        return;
    }

    if (batch->fullReset) {
        clearState();
    }
    if (!batch->removeRows.isEmpty()) {
        removeRows(batch->removeRows);
    }
    if (!batch->upsertRows.isEmpty()) {
        upsertRows(batch->upsertRows);
    }
}

void DanmakuUpdateWorker::processFrame(DanmakuWorkerFramePtr frame) {
    if (frame.isNull()) {
        emit frameProcessed({});
        return;
    }

    QVector<int> &rows = m_state.rows;
    QVector<qreal> &x = m_state.x;
    QVector<qreal> &y = m_state.y;
    QVector<qreal> &speed = m_state.speed;
    QVector<qreal> &alpha = m_state.alpha;
    QVector<int> &widthEstimate = m_state.widthEstimate;
    QVector<int> &fadeRemainingMs = m_state.fadeRemainingMs;
    QVector<quint8> &flags = m_state.flags;
    QVector<DanmakuWorkerRowState> &changedRows = frame->changedRows;
    QVector<int> &removeRowsOut = frame->removeRows;
    changedRows.clear();
    removeRowsOut.clear();

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
    removeRowsOut.reserve(count / 4);

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

        const bool outOfHorizontalBounds = x[i] + widthEstimate[i] < frame->cullThreshold;
        const bool outOfVerticalBounds = y[i] > frame->viewportHeight || y[i] + frame->itemHeight < 0.0;
        const bool canCull = !dragging && (alpha[i] <= 0.0 || outOfHorizontalBounds || outOfVerticalBounds);
        if (canCull) {
            removeRowsOut.push_back(rows[i]);
            continue;
        }

        if (m_changedMask[i]) {
            changedRows.push_back(buildRowStateAt(i));
        }
    }

    if (!removeRowsOut.isEmpty()) {
        removeRows(removeRowsOut);
    }

    emit frameProcessed(frame);
}

void DanmakuUpdateWorker::clearState() {
    m_state.clear();
    m_rowToIndex.clear();
}

void DanmakuUpdateWorker::upsertRows(const QVector<DanmakuWorkerRowState> &rows) {
    for (const DanmakuWorkerRowState &rowState : rows) {
        if (rowState.row < 0) {
            continue;
        }

        const auto it = m_rowToIndex.constFind(rowState.row);
        if (it != m_rowToIndex.constEnd()) {
            const int index = it.value();
            if (index < 0 || index >= m_state.size()) {
                continue;
            }
            m_state.rows[index] = rowState.row;
            m_state.x[index] = rowState.x;
            m_state.y[index] = rowState.y;
            m_state.speed[index] = rowState.speed;
            m_state.alpha[index] = rowState.alpha;
            m_state.widthEstimate[index] = rowState.widthEstimate;
            m_state.fadeRemainingMs[index] = rowState.fadeRemainingMs;
            m_state.flags[index] = rowState.flags;
            continue;
        }

        const int index = m_state.size();
        m_state.rows.push_back(rowState.row);
        m_state.x.push_back(rowState.x);
        m_state.y.push_back(rowState.y);
        m_state.speed.push_back(rowState.speed);
        m_state.alpha.push_back(rowState.alpha);
        m_state.widthEstimate.push_back(rowState.widthEstimate);
        m_state.fadeRemainingMs.push_back(rowState.fadeRemainingMs);
        m_state.flags.push_back(rowState.flags);
        m_rowToIndex.insert(rowState.row, index);
    }
}

void DanmakuUpdateWorker::removeRows(const QVector<int> &rows) {
    QVector<int> uniqueRows = rows;
    std::sort(uniqueRows.begin(), uniqueRows.end());
    uniqueRows.erase(std::unique(uniqueRows.begin(), uniqueRows.end()), uniqueRows.end());
    for (auto it = uniqueRows.crbegin(); it != uniqueRows.crend(); ++it) {
        const auto rowIt = m_rowToIndex.constFind(*it);
        if (rowIt == m_rowToIndex.constEnd()) {
            continue;
        }
        removeIndex(rowIt.value());
    }
}

void DanmakuUpdateWorker::removeIndex(int index) {
    if (index < 0 || index >= m_state.size()) {
        return;
    }

    const int lastIndex = m_state.size() - 1;
    const int removedRow = m_state.rows[index];
    if (index != lastIndex) {
        const int movedRow = m_state.rows[lastIndex];
        m_state.rows[index] = m_state.rows[lastIndex];
        m_state.x[index] = m_state.x[lastIndex];
        m_state.y[index] = m_state.y[lastIndex];
        m_state.speed[index] = m_state.speed[lastIndex];
        m_state.alpha[index] = m_state.alpha[lastIndex];
        m_state.widthEstimate[index] = m_state.widthEstimate[lastIndex];
        m_state.fadeRemainingMs[index] = m_state.fadeRemainingMs[lastIndex];
        m_state.flags[index] = m_state.flags[lastIndex];
        m_rowToIndex.insert(movedRow, index);
    }

    m_state.rows.removeLast();
    m_state.x.removeLast();
    m_state.y.removeLast();
    m_state.speed.removeLast();
    m_state.alpha.removeLast();
    m_state.widthEstimate.removeLast();
    m_state.fadeRemainingMs.removeLast();
    m_state.flags.removeLast();
    m_rowToIndex.remove(removedRow);
}

DanmakuWorkerRowState DanmakuUpdateWorker::buildRowStateAt(int index) const {
    DanmakuWorkerRowState rowState;
    if (index < 0 || index >= m_state.size()) {
        return rowState;
    }

    rowState.row = m_state.rows[index];
    rowState.x = m_state.x[index];
    rowState.y = m_state.y[index];
    rowState.speed = m_state.speed[index];
    rowState.alpha = m_state.alpha[index];
    rowState.widthEstimate = m_state.widthEstimate[index];
    rowState.fadeRemainingMs = m_state.fadeRemainingMs[index];
    rowState.flags = m_state.flags[index];
    return rowState;
}
