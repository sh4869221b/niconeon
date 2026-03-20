#pragma once

#include "danmaku/DanmakuRenderFrame.hpp"

#include <QHash>
#include <QQueue>
#include <QString>

class DanmakuTextSpriteCache {
public:
    struct WidthKey {
        QString text;
        int fontPixelSize = 0;

        friend bool operator==(const WidthKey &lhs, const WidthKey &rhs) {
            return lhs.fontPixelSize == rhs.fontPixelSize && lhs.text == rhs.text;
        }
    };

    struct SpriteKey {
        QString text;
        int fontPixelSize = 0;
        int devicePixelRatioMilli = 1000;

        friend bool operator==(const SpriteKey &lhs, const SpriteKey &rhs) {
            return lhs.fontPixelSize == rhs.fontPixelSize
                && lhs.devicePixelRatioMilli == rhs.devicePixelRatioMilli
                && lhs.text == rhs.text;
        }
    };

    struct EnsureResult {
        DanmakuSpriteId spriteId = 0;
        int widthEstimate = 0;
        bool queuedRaster = false;
    };

    DanmakuTextSpriteCache() = default;

    void clear();
    EnsureResult ensureSprite(const QString &text, int fontPixelSize, qreal devicePixelRatio);
    DanmakuSpriteUpload takePendingUpload(const QString &text, int fontPixelSize, qreal devicePixelRatio);
    QVector<DanmakuSpriteUpload> rasterizePendingSprites(int maxSprites, qint64 maxUploadBytes);
    int widthMeasurementCountForTesting() const;
    int pendingRasterCountForTesting() const;

private:
    struct PendingRaster {
        SpriteKey key;
        DanmakuSpriteId spriteId = 0;
        int widthEstimate = 0;
    };

    int ensureWidthEstimate(const QString &text, int fontPixelSize);
    QImage rasterizeSprite(const QString &text, int fontPixelSize, int widthEstimate, qreal devicePixelRatio) const;

    QHash<WidthKey, int> m_widthCache;
    QHash<SpriteKey, DanmakuSpriteId> m_spriteIds;
    QHash<SpriteKey, PendingRaster> m_pendingRasters;
    QQueue<PendingRaster> m_pendingRasterQueue;
    quint32 m_nextSpriteId = 1;
    int m_widthMeasurementCount = 0;
};

size_t qHash(const DanmakuTextSpriteCache::WidthKey &key, size_t seed = 0) noexcept;
size_t qHash(const DanmakuTextSpriteCache::SpriteKey &key, size_t seed = 0) noexcept;
