#include "danmaku/DanmakuSceneItem.hpp"

#include "danmaku/DanmakuController.hpp"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QRectF>
#include <QSGNode>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace {
constexpr int kItemHeightPx = 42;
constexpr int kTextPixelSize = 24;
}

DanmakuSceneItem::DanmakuSceneItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(QQuickItem::ItemHasContents, true);
}

DanmakuController *DanmakuSceneItem::controller() const {
    return m_controller.data();
}

void DanmakuSceneItem::setController(DanmakuController *controller) {
    if (m_controller == controller) {
        return;
    }

    if (m_controller) {
        disconnect(m_controller, &DanmakuController::renderSnapshotChanged, this, &DanmakuSceneItem::handleControllerRenderSnapshotChanged);
    }

    m_controller = controller;

    if (m_controller) {
        connect(
            m_controller,
            &DanmakuController::renderSnapshotChanged,
            this,
            &DanmakuSceneItem::handleControllerRenderSnapshotChanged,
            Qt::QueuedConnection);
    }

    emit controllerChanged();
    update();
}

QSGNode *DanmakuSceneItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *updatePaintNodeData) {
    Q_UNUSED(updatePaintNodeData)

    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
    }

    const int itemWidth = static_cast<int>(std::ceil(width()));
    const int itemHeight = static_cast<int>(std::ceil(height()));
    if (itemWidth <= 0 || itemHeight <= 0 || !window()) {
        node->setTexture(nullptr);
        return node;
    }

    const qreal dpr = window()->effectiveDevicePixelRatio();
    const QSize imageSize(
        std::max(1, static_cast<int>(std::ceil(itemWidth * dpr))),
        std::max(1, static_cast<int>(std::ceil(itemHeight * dpr))));
    QImage image(imageSize, QImage::Format_RGBA8888_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    if (m_controller) {
        const QVector<DanmakuController::RenderItem> items = m_controller->renderSnapshot();

        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        QFont font;
        font.setPixelSize(kTextPixelSize);
        painter.setFont(font);

        const QColor fillColor(QStringLiteral("#88000000"));
        const QColor normalBorder(QStringLiteral("#AAFFFFFF"));
        const QColor ngBorder(QStringLiteral("#FFFF4466"));
        const QColor textColor(Qt::white);

        for (const DanmakuController::RenderItem &item : items) {
            if (item.alpha <= 0.0) {
                continue;
            }

            painter.setOpacity(std::clamp(item.alpha, 0.0, 1.0));
            const QRectF rect(item.x, item.y, item.widthEstimate, kItemHeightPx);
            painter.setBrush(fillColor);
            painter.setPen(QPen(item.ngDropHovered ? ngBorder : normalBorder, item.ngDropHovered ? 2.0 : 1.0));
            painter.drawRoundedRect(rect, 8.0, 8.0);

            painter.setPen(textColor);
            const QRectF textRect = rect.adjusted(8.0, 0.0, -8.0, 0.0);
            painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignHCenter, item.text);
        }
    }

    QSGTexture *newTexture = window()->createTextureFromImage(image, QQuickWindow::TextureHasAlphaChannel);
    node->setTexture(newTexture);
    node->setRect(0, 0, itemWidth, itemHeight);

    return node;
}

void DanmakuSceneItem::handleControllerRenderSnapshotChanged() {
    update();
}
