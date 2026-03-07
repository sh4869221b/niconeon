#pragma once

#include <QImage>
#include <QSharedPointer>
#include <QString>
#include <QVector>
#include <QtGlobal>

using DanmakuSpriteId = quint32;

struct DanmakuSpriteUpload {
    DanmakuSpriteId spriteId = 0;
    QSize logicalSize;
    QImage image;
};

struct DanmakuRenderInstance {
    QString commentId;
    DanmakuSpriteId spriteId = 0;
    qreal x = 0.0;
    qreal y = 0.0;
    qreal alpha = 1.0;
    int widthEstimate = 0;
    bool ngDropHovered = false;
};

struct DanmakuRenderFrame {
    QVector<DanmakuRenderInstance> instances;
};

using DanmakuRenderFramePtr = QSharedPointer<DanmakuRenderFrame>;
using DanmakuRenderFrameConstPtr = QSharedPointer<const DanmakuRenderFrame>;
