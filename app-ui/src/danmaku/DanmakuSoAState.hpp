#pragma once

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

    int size() const {
        return rows.size();
    }
};
