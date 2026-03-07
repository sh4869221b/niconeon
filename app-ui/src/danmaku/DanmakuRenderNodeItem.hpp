#pragma once

#include <atomic>
#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>

class DanmakuController;
class QSGNode;
class QQuickWindow;

class DanmakuRenderNodeItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(DanmakuController *controller READ controller WRITE setController NOTIFY controllerChanged)

public:
    explicit DanmakuRenderNodeItem(QQuickItem *parent = nullptr);

    DanmakuController *controller() const;
    void setController(DanmakuController *controller);

signals:
    void controllerChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *updatePaintNodeData) override;

private:
    void handleControllerRenderSnapshotChanged();
    void handleWindowChanged(QQuickWindow *window);
    void handleWindowFrameSwapped();

    QPointer<DanmakuController> m_controller;
    QMetaObject::Connection m_frameSwappedConnection;
    std::atomic_bool m_pendingPresentedFrame = false;
    qreal m_lastRenderDevicePixelRatio = 0.0;
};
