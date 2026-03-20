#pragma once

#include "danmaku/DanmakuSimdUpdater.hpp"
#include "danmaku/DanmakuSoAState.hpp"

#include <QHash>
#include <QObject>
#include <QVector>
#include <QtGlobal>

class DanmakuUpdateWorker : public QObject {
    Q_OBJECT

public:
    explicit DanmakuUpdateWorker(QObject *parent = nullptr);

    void setSimdMode(DanmakuSimdMode mode);

public slots:
    void syncState(DanmakuWorkerSyncBatchPtr batch);
    void processFrame(DanmakuWorkerFramePtr frame);

signals:
    void frameProcessed(DanmakuWorkerFramePtr frame);

private:
    void clearState();
    void upsertRows(const QVector<DanmakuWorkerRowState> &rows);
    void removeRows(const QVector<int> &rows);
    void removeIndex(int index);
    DanmakuWorkerRowState buildRowStateAt(int index) const;

    DanmakuSoAState m_state;
    QHash<int, int> m_rowToIndex;
    QVector<quint8> m_movableMask;
    QVector<quint8> m_changedMask;
    DanmakuSimdMode m_simdMode = DanmakuSimdMode::Scalar;
};
