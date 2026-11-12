#pragma once

#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QVector>

class DanmakuAtlasPacker {
  public:
    DanmakuAtlasPacker() = default;
    explicit DanmakuAtlasPacker(const QSize &pageSize);

    void reset(const QSize &pageSize);
    QRect insert(const QSize &size);
    QSize pageSize() const;

  private:
    struct Shelf {
        int y = 0;
        int height = 0;
        int nextX = 0;
    };

    QSize m_pageSize;
    QVector<Shelf> m_shelves;
};

struct DanmakuAtlasRepackCandidate {
    quint64 spriteId = 0;
    QSize size;
};

struct DanmakuAtlasPlacement {
    quint64 spriteId = 0;
    QRect rect;
};

struct DanmakuAtlasRepackPlan {
    DanmakuAtlasPacker packer;
    QVector<DanmakuAtlasPlacement> placements;
    int admittedSprites = 0;
    bool canCommit = false;
};

// Pure, transactional planning: every protected sprite must fit before any new
// sprite is admitted. Each unique pending ID is tried once, in descending size
// order. An uncommittable plan must never replace the caller's current page.
DanmakuAtlasRepackPlan planDanmakuAtlasRepack(const QSize &pageSize,
                                              QVector<DanmakuAtlasRepackCandidate> protectedSprites,
                                              QVector<DanmakuAtlasRepackCandidate> pendingSprites);

// Regions retain exact source pixels. Core quads are disjoint; a one-pixel
// neighboring gutter keeps GL_LINEAR sampling continuous at internal seams.
struct DanmakuAtlasTileRegion {
    QRect core;
    QRect source;
};
QVector<DanmakuAtlasTileRegion> danmakuAtlasTiles(const QImage &rgbaImage, int pageSize = 2048);
QRectF danmakuAtlasTileLogicalRect(const QRect &core, const QSize &imageSize, const QSize &logicalSize,
                                   const QPointF &position);
