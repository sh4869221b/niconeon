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
    m_pendingRasters.clear();
    m_pendingRasterQueue.clear();
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
    m_spriteIds.insert(key, result.spriteId);
    const PendingRaster pending {
        key,
        result.spriteId,
        result.widthEstimate,
    };
    m_pendingRasters.insert(key, pending);
    m_pendingRasterQueue.enqueue(pending);
    result.queuedRaster = true;
    return result;
}

DanmakuSpriteUpload DanmakuTextSpriteCache::takePendingUpload(
    const QString &text,
    int fontPixelSize,
    qreal devicePixelRatio) {
    const int dprMilli = std::max(1, static_cast<int>(std::lround(std::max(devicePixelRatio, 1.0) * 1000.0)));
    const SpriteKey key {
        text,
        fontPixelSize,
        dprMilli,
    };
    const auto pendingIt = m_pendingRasters.constFind(key);
    if (pendingIt == m_pendingRasters.constEnd()) {
        return {};
    }

    const PendingRaster pending = pendingIt.value();
    DanmakuSpriteUpload upload;
    upload.spriteId = pending.spriteId;
    upload.logicalSize = QSize(pending.widthEstimate, DanmakuRenderStyle::kItemHeightPx);
    upload.image = rasterizeSprite(text, fontPixelSize, pending.widthEstimate, devicePixelRatio);
    m_pendingRasters.remove(key);
    for (qsizetype i = 0; i < m_pendingRasterQueue.size(); ++i) {
        const PendingRaster &queued = m_pendingRasterQueue.at(i);
        if (queued.spriteId == pending.spriteId && queued.key == key) {
            m_pendingRasterQueue.removeAt(i);
            break;
        }
    }
    return upload;
}

QVector<DanmakuSpriteUpload> DanmakuTextSpriteCache::rasterizePendingSprites(int maxSprites, qint64 maxUploadBytes) {
    QVector<DanmakuSpriteUpload> uploads;
    if (maxSprites <= 0) {
        return uploads;
    }

    qint64 totalUploadBytes = 0;
    while (uploads.size() < maxSprites && !m_pendingRasterQueue.isEmpty()) {
        const PendingRaster pending = m_pendingRasterQueue.dequeue();
        const auto pendingIt = m_pendingRasters.constFind(pending.key);
        if (pendingIt == m_pendingRasters.constEnd() || pendingIt->spriteId != pending.spriteId) {
            continue;
        }

        const qreal devicePixelRatio = std::max(1.0, pending.key.devicePixelRatioMilli / 1000.0);
        DanmakuSpriteUpload upload;
        upload.spriteId = pending.spriteId;
        upload.logicalSize = QSize(pending.widthEstimate, DanmakuRenderStyle::kItemHeightPx);
        upload.image = rasterizeSprite(
            pending.key.text,
            pending.key.fontPixelSize,
            pending.widthEstimate,
            devicePixelRatio);

        const qint64 uploadBytes = static_cast<qint64>(upload.image.sizeInBytes());
        const bool exceedsBudget = maxUploadBytes > 0
            && !uploads.isEmpty()
            && totalUploadBytes + uploadBytes > maxUploadBytes;
        if (exceedsBudget) {
            m_pendingRasterQueue.prepend(pending);
            break;
        }

        uploads.push_back(std::move(upload));
        totalUploadBytes += uploadBytes;
        m_pendingRasters.remove(pending.key);
    }

    return uploads;
}

int DanmakuTextSpriteCache::widthMeasurementCountForTesting() const {
    return m_widthMeasurementCount;
}

int DanmakuTextSpriteCache::pendingRasterCountForTesting() const {
    return m_pendingRasters.size();
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
