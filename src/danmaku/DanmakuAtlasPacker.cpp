#include "danmaku/DanmakuAtlasPacker.hpp"

#include <QSet>
#include <algorithm>
#include <utility>

DanmakuAtlasPacker::DanmakuAtlasPacker(const QSize &pageSize) {
    reset(pageSize);
}

void DanmakuAtlasPacker::reset(const QSize &pageSize) {
    m_pageSize = pageSize.expandedTo(QSize(1, 1));
    m_shelves.clear();
}

QRect DanmakuAtlasPacker::insert(const QSize &size) {
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0) {
        return {};
    }
    if (size.width() > m_pageSize.width() || size.height() > m_pageSize.height()) {
        return {};
    }

    for (Shelf &shelf : m_shelves) {
        if (size.height() > shelf.height) {
            continue;
        }
        if (shelf.nextX + size.width() > m_pageSize.width()) {
            continue;
        }

        QRect rect(shelf.nextX, shelf.y, size.width(), size.height());
        shelf.nextX += size.width();
        return rect;
    }

    int nextY = 0;
    if (!m_shelves.isEmpty()) {
        const Shelf &lastShelf = m_shelves.last();
        nextY = lastShelf.y + lastShelf.height;
    }
    if (nextY + size.height() > m_pageSize.height()) {
        return {};
    }

    Shelf shelf;
    shelf.y = nextY;
    shelf.height = size.height();
    shelf.nextX = size.width();
    m_shelves.push_back(shelf);
    return QRect(0, nextY, size.width(), size.height());
}

QSize DanmakuAtlasPacker::pageSize() const {
    return m_pageSize;
}

DanmakuAtlasRepackPlan planDanmakuAtlasRepack(const QSize &pageSize,
                                              QVector<DanmakuAtlasRepackCandidate> protectedSprites,
                                              QVector<DanmakuAtlasRepackCandidate> pendingSprites) {
    if (pageSize.isEmpty())
        return {};
    const auto sizeOrder = [](const auto &lhs, const auto &rhs) {
        if (lhs.size.height() != rhs.size.height())
            return lhs.size.height() > rhs.size.height();
        if (lhs.size.width() != rhs.size.width())
            return lhs.size.width() > rhs.size.width();
        return lhs.spriteId < rhs.spriteId;
    };
    std::sort(protectedSprites.begin(), protectedSprites.end(), sizeOrder);
    std::sort(pendingSprites.begin(), pendingSprites.end(), sizeOrder);
    DanmakuAtlasRepackPlan plan;
    plan.packer.reset(pageSize);
    plan.placements.reserve(protectedSprites.size() + pendingSprites.size());
    QSet<quint64> seen;
    seen.reserve(protectedSprites.size() + pendingSprites.size());
    for (const auto &candidate : std::as_const(protectedSprites)) {
        if (!candidate.spriteId || seen.contains(candidate.spriteId))
            return {};
        const QRect rect = plan.packer.insert(candidate.size);
        if (!rect.isValid())
            return {};
        seen.insert(candidate.spriteId);
        plan.placements.push_back({candidate.spriteId, rect});
    }
    for (const auto &candidate : std::as_const(pendingSprites)) {
        if (!candidate.spriteId || seen.contains(candidate.spriteId))
            continue;
        seen.insert(candidate.spriteId);
        const QRect rect = plan.packer.insert(candidate.size);
        if (!rect.isValid())
            continue;
        plan.placements.push_back({candidate.spriteId, rect});
        ++plan.admittedSprites;
    }
    plan.canCommit = plan.admittedSprites > 0;
    return plan;
}

QVector<DanmakuAtlasTileRegion> danmakuAtlasTiles(const QImage &image, int pageSize) {
    if (image.isNull() || pageSize < 3 || image.format() != QImage::Format_RGBA8888_Premultiplied)
        return {};
    int left = image.width(), top = image.height(), right = -1, bottom = -1;
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            if (row[x * 4 + 3] == 0)
                continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    // A transparent sprite still has one transparent draw, preserving logical
    // comment accounting without allocating its invisible rectangle in the atlas.
    QRect bounds(0, 0, 1, 1);
    if (right >= left)
        bounds = QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-1, -1, 1, 1).intersected(image.rect());
    const int coreLimit = pageSize - 2;
    QVector<DanmakuAtlasTileRegion> result;
    for (int y = bounds.top(); y <= bounds.bottom(); y += coreLimit) {
        for (int x = bounds.left(); x <= bounds.right(); x += coreLimit) {
            const QRect core(x, y, std::min(coreLimit, bounds.right() - x + 1),
                             std::min(coreLimit, bounds.bottom() - y + 1));
            result.push_back({core, core.adjusted(-1, -1, 1, 1).intersected(image.rect())});
        }
    }
    return result;
}
QRectF danmakuAtlasTileLogicalRect(const QRect &core, const QSize &imageSize, const QSize &logicalSize,
                                   const QPointF &position) {
    if (imageSize.isEmpty())
        return {};
    // Use the original whole-image mapping, not merely 1/DPR: ceil rounding of
    // physical image dimensions at fractional DPR must not move glyph pixels.
    const qreal sx = qreal(logicalSize.width()) / imageSize.width();
    const qreal sy = qreal(logicalSize.height()) / imageSize.height();
    return {position.x() + core.x() * sx, position.y() + core.y() * sy, core.width() * sx, core.height() * sy};
}
