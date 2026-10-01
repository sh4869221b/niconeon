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
    QSet<quint32> seen;
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
