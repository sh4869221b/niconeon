#include "danmaku/DanmakuTextSpriteCache.hpp"

#include "danmaku/DanmakuRenderStyle.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>

void DanmakuTextSpriteCache::clear() {
    m_widthCache.clear();
    m_spriteIds.clear();
    m_nextSpriteId = 1;
    m_widthMeasurementCount = 0;
}

DanmakuTextSpriteCache::EnsureResult DanmakuTextSpriteCache::ensureSprite(
    const QString &text,
    int fontPixelSize,
    qreal devicePixelRatio) {
    EnsureResult result;
    result.widthEstimate = ensureWidthEstimate(text, fontPixelSize);

    const int dprMilli = std::max(1, static_cast<int>(std::lround(std::max(devicePixelRatio, 1.0) * 1000.0)));
    const SpriteKey key {
        text,
        fontPixelSize,
        dprMilli,
    };
    const auto spriteIt = m_spriteIds.constFind(key);
    if (spriteIt != m_spriteIds.constEnd()) {
        result.spriteId = spriteIt.value();
        return result;
    }

    result.spriteId = m_nextSpriteId++;
    result.createdUpload = true;
    result.upload.spriteId = result.spriteId;
    result.upload.logicalSize = QSize(result.widthEstimate, DanmakuRenderStyle::kItemHeightPx);
    result.upload.image = rasterizeSprite(text, fontPixelSize, result.widthEstimate, devicePixelRatio);
    m_spriteIds.insert(key, result.spriteId);
    return result;
}

int DanmakuTextSpriteCache::widthMeasurementCountForTesting() const {
    return m_widthMeasurementCount;
}

int DanmakuTextSpriteCache::ensureWidthEstimate(const QString &text, int fontPixelSize) {
    const WidthKey key {
        text,
        fontPixelSize,
    };
    const auto it = m_widthCache.constFind(key);
    if (it != m_widthCache.constEnd()) {
        return it.value();
    }

    QFont font;
    font.setPixelSize(fontPixelSize);
    QFontMetrics metrics(font);
    const int textWidth = metrics.horizontalAdvance(text);
    const int paddedWidth = textWidth + DanmakuRenderStyle::kHorizontalPaddingPx * 2;
    const int widthEstimate = std::max(DanmakuRenderStyle::kMinWidthPx, paddedWidth);
    m_widthCache.insert(key, widthEstimate);
    ++m_widthMeasurementCount;
    return widthEstimate;
}

QImage DanmakuTextSpriteCache::rasterizeSprite(
    const QString &text,
    int fontPixelSize,
    int widthEstimate,
    qreal devicePixelRatio) const {
    const qreal dpr = std::max(devicePixelRatio, 1.0);
    const QSize pixelSize(
        std::max(1, static_cast<int>(std::ceil(widthEstimate * dpr))),
        std::max(1, static_cast<int>(std::ceil(DanmakuRenderStyle::kItemHeightPx * dpr))));

    QImage image(pixelSize, QImage::Format_RGBA8888_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    QFont font;
    font.setPixelSize(fontPixelSize);
    painter.setFont(font);
    painter.setPen(QColor(Qt::white));
    painter.drawText(
        QRectF(
            0.0,
            0.0,
            static_cast<qreal>(widthEstimate),
            static_cast<qreal>(DanmakuRenderStyle::kItemHeightPx)),
        Qt::AlignVCenter | Qt::AlignHCenter,
        text);
    return image;
}

size_t qHash(const DanmakuTextSpriteCache::WidthKey &key, size_t seed) noexcept {
    seed = qHash(key.text, seed);
    return qHash(key.fontPixelSize, seed);
}

size_t qHash(const DanmakuTextSpriteCache::SpriteKey &key, size_t seed) noexcept {
    seed = qHash(key.text, seed);
    seed = qHash(key.fontPixelSize, seed);
    return qHash(key.devicePixelRatioMilli, seed);
}
