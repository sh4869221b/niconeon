#include "danmaku/DanmakuAtlasPacker.hpp"

#include <algorithm>

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
