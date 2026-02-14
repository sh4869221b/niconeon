#pragma once

#include <QVector>
#include <QString>
#include <QtGlobal>

enum class DanmakuSimdMode {
    Auto,
    Scalar,
    Avx2,
};

class DanmakuSimdUpdater {
public:
    static DanmakuSimdMode parseMode(const QString &raw);
    static QString modeName(DanmakuSimdMode mode);
    static DanmakuSimdMode resolveMode(DanmakuSimdMode requested);

    static void updatePositions(
        QVector<qreal> &x,
        const QVector<qreal> &speed,
        const QVector<quint8> &movableMask,
        qreal movementFactor,
        QVector<quint8> &changedMask,
        DanmakuSimdMode mode);

private:
    static bool hasAvx2Runtime();
};
