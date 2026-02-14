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
    void processFrame(DanmakuWorkerFramePtr frame);

signals:
    void frameProcessed(DanmakuWorkerFramePtr frame);

private:
    QVector<quint8> m_movableMask;
    QVector<quint8> m_changedMask;
    DanmakuSimdMode m_simdMode = DanmakuSimdMode::Scalar;
};
