#pragma once

#include "danmaku/DanmakuController.hpp"

#include <QPointer>
#include <QQuickItem>

class QSGNode;

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

    QPointer<DanmakuController> m_controller;
};
