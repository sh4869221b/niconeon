#include "danmaku/DanmakuSpatialGrid.hpp"

#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace {
constexpr qreal kMinCellSize = 8.0;

int cellFloor(qreal value, qreal cellSize) {
    return static_cast<int>(std::floor(value / cellSize));
}
}

void DanmakuSpatialGrid::rebuild(const QVector<Entry> &entries, qreal cellWidth, qreal cellHeight) {
    m_cellWidth = std::max(cellWidth, kMinCellSize);
    m_cellHeight = std::max(cellHeight, kMinCellSize);
    m_cells.clear();
    m_rowRects.clear();

    for (const Entry &entry : entries) {
        if (entry.row < 0) {
            continue;
        }
        if (entry.rect.isEmpty()) {
            continue;
        }

        m_rowRects.insert(entry.row, entry.rect);
        const int minX = cellFloor(entry.rect.left(), m_cellWidth);
        const int maxX = cellFloor(entry.rect.right(), m_cellWidth);
        const int minY = cellFloor(entry.rect.top(), m_cellHeight);
        const int maxY = cellFloor(entry.rect.bottom(), m_cellHeight);

        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                m_cells[cellKey(x, y)].push_back(entry.row);
            }
        }
    }
}

QVector<int> DanmakuSpatialGrid::queryPoint(const QPointF &point) const {
    QVector<int> rows;
    const int cellX = cellFloor(point.x(), m_cellWidth);
    const int cellY = cellFloor(point.y(), m_cellHeight);
    const auto it = m_cells.constFind(cellKey(cellX, cellY));
    if (it == m_cells.constEnd()) {
        return rows;
    }

    const QVector<int> &cellRows = it.value();
    rows.reserve(cellRows.size());
    for (const int row : cellRows) {
        const auto rectIt = m_rowRects.constFind(row);
        if (rectIt == m_rowRects.constEnd() || !rectIt.value().contains(point)) {
            continue;
        }
        appendUnique(rows, row);
    }
    return rows;
}

QVector<int> DanmakuSpatialGrid::queryRect(const QRectF &rect) const {
    QVector<int> rows;
    if (rect.isEmpty()) {
        return rows;
    }

    const int minX = cellFloor(rect.left(), m_cellWidth);
    const int maxX = cellFloor(rect.right(), m_cellWidth);
    const int minY = cellFloor(rect.top(), m_cellHeight);
    const int maxY = cellFloor(rect.bottom(), m_cellHeight);

    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            const auto it = m_cells.constFind(cellKey(x, y));
            if (it == m_cells.constEnd()) {
                continue;
            }

            const QVector<int> &cellRows = it.value();
            for (const int row : cellRows) {
                const auto rectIt = m_rowRects.constFind(row);
                if (rectIt == m_rowRects.constEnd() || !rectIt.value().intersects(rect)) {
                    continue;
                }
                appendUnique(rows, row);
            }
        }
    }

    return rows;
}

quint64 DanmakuSpatialGrid::cellKey(int cellX, int cellY) {
    return (static_cast<quint64>(static_cast<quint32>(cellX)) << 32)
        | static_cast<quint32>(cellY);
}

void DanmakuSpatialGrid::appendUnique(QVector<int> &rows, int row) {
    if (!rows.contains(row)) {
        rows.push_back(row);
    }
}
