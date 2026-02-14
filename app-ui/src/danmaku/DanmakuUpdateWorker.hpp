#pragma once

#include "danmaku/DanmakuSimdUpdater.hpp"
#include "danmaku/DanmakuSoAState.hpp"

#include <QObject>
#include <QVector>
#include <QtGlobal>

class DanmakuUpdateWorker : public QObject {
    Q_OBJECT

public:
    explicit DanmakuUpdateWorker(QObject *parent = nullptr);

    void setSimdMode(DanmakuSimdMode mode);

public slots:
    void processFrame(DanmakuFrameInput input);

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
