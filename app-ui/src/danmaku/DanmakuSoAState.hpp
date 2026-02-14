#pragma once

#include <QMetaType>
#include <QSharedPointer>
#include <QVector>
#include <QtGlobal>

enum DanmakuSoAFlag : quint8 {
    DanmakuSoAFlagFrozen = 1 << 0,
    DanmakuSoAFlagDragging = 1 << 1,
    DanmakuSoAFlagFading = 1 << 2,
};

struct DanmakuSoAState {
    QVector<int> rows;
    QVector<qreal> x;
    QVector<qreal> y;
    QVector<qreal> speed;
    QVector<qreal> alpha;
    QVector<int> widthEstimate;
    QVector<int> fadeRemainingMs;
    QVector<quint8> flags;

    void clear() {
        rows.clear();
        x.clear();
        y.clear();
        speed.clear();
        alpha.clear();
        widthEstimate.clear();
        fadeRemainingMs.clear();
        flags.clear();
    }

    void reserve(int count) {
        rows.reserve(count);
        x.reserve(count);
        y.reserve(count);
        speed.reserve(count);
        alpha.reserve(count);
        widthEstimate.reserve(count);
        fadeRemainingMs.reserve(count);
        flags.reserve(count);
    }

    void resize(int count) {
        rows.resize(count);
        x.resize(count);
        y.resize(count);
        speed.resize(count);
        alpha.resize(count);
        widthEstimate.resize(count);
        fadeRemainingMs.resize(count);
        flags.resize(count);
    }

    int size() const {
        return rows.size();
    }
};

struct DanmakuWorkerFrame {
    qint64 seq = 0;
    bool playbackPaused = false;
    qreal playbackRate = 1.0;
    int elapsedMs = 0;
    qreal viewportHeight = 0;
    qreal cullThreshold = 0;
    qreal itemHeight = 0;
    DanmakuSoAState state;
    QVector<int> changedRows;
    QVector<int> removeRows;
};

using DanmakuWorkerFramePtr = QSharedPointer<DanmakuWorkerFrame>;

Q_DECLARE_METATYPE(DanmakuWorkerFramePtr)
