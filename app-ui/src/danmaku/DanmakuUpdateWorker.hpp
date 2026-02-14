#pragma once

#include "danmaku/DanmakuSimdUpdater.hpp"

#include <QObject>
#include <QVector>
#include <QtGlobal>

class DanmakuUpdateWorker : public QObject {
    Q_OBJECT

public:
    explicit DanmakuUpdateWorker(QObject *parent = nullptr);

    void setSimdMode(DanmakuSimdMode mode);

public slots:
    void processFrame(
        qint64 seq,
        bool playbackPaused,
        qreal playbackRate,
        int elapsedMs,
        qreal viewportHeight,
        qreal cullThreshold,
        qreal itemHeight,
        QVector<int> rows,
        QVector<qreal> x,
        QVector<qreal> y,
        QVector<qreal> speed,
        QVector<qreal> alpha,
        QVector<int> widthEstimate,
        QVector<int> fadeRemainingMs,
        QVector<quint8> flags);

signals:
    void frameProcessed(
        qint64 seq,
        QVector<int> rows,
        QVector<qreal> x,
        QVector<qreal> y,
        QVector<qreal> alpha,
        QVector<int> fadeRemainingMs,
        QVector<quint8> flags,
        QVector<int> changedRows,
        QVector<int> removeRows);

private:
    DanmakuSimdMode m_simdMode = DanmakuSimdMode::Scalar;
};
