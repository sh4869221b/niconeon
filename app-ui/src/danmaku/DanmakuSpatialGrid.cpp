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

void DanmakuSpatialGrid::clear() {
    m_cells.clear();
    m_rowRects.clear();
    m_rowCellKeys.clear();
}

void DanmakuSpatialGrid::setCellSize(qreal cellWidth, qreal cellHeight) {
    m_cellWidth = std::max(cellWidth, kMinCellSize);
    m_cellHeight = std::max(cellHeight, kMinCellSize);
}

void DanmakuSpatialGrid::upsertRow(int row, const QRectF &rect) {
    if (row < 0) {
        return;
    }

    removeRow(row);
    if (rect.isEmpty()) {
        return;
    }

    QVector<quint64> keys = cellKeysForRect(rect, m_cellWidth, m_cellHeight);
    m_rowRects.insert(row, rect);
    m_rowCellKeys.insert(row, keys);
    for (const quint64 key : keys) {
        m_cells[key].push_back(row);
    }
}

void DanmakuSpatialGrid::removeRow(int row) {
    if (row < 0) {
        return;
    }

    auto rowKeyIt = m_rowCellKeys.find(row);
    if (rowKeyIt != m_rowCellKeys.end()) {
        const QVector<quint64> keys = rowKeyIt.value();
        for (const quint64 key : keys) {
            auto cellIt = m_cells.find(key);
            if (cellIt == m_cells.end()) {
                continue;
            }
            QVector<int> &rows = cellIt.value();
            rows.removeAll(row);
            if (rows.isEmpty()) {
                m_cells.erase(cellIt);
            }
        }
        m_rowCellKeys.erase(rowKeyIt);
    }

    m_rowRects.remove(row);
}

void DanmakuSpatialGrid::rebuild(const QVector<Entry> &entries, qreal cellWidth, qreal cellHeight) {
    setCellSize(cellWidth, cellHeight);
    clear();

    for (const Entry &entry : entries) {
        if (entry.row < 0) {
            continue;
        }
        if (entry.rect.isEmpty()) {
            continue;
        }
        upsertRow(entry.row, entry.rect);
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

QVector<quint64> DanmakuSpatialGrid::cellKeysForRect(const QRectF &rect, qreal cellWidth, qreal cellHeight) {
    QVector<quint64> keys;
    if (rect.isEmpty()) {
        return keys;
    }

    const int minX = cellFloor(rect.left(), cellWidth);
    const int maxX = cellFloor(rect.right(), cellWidth);
    const int minY = cellFloor(rect.top(), cellHeight);
    const int maxY = cellFloor(rect.bottom(), cellHeight);

    keys.reserve((maxX - minX + 1) * (maxY - minY + 1));
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            keys.push_back(cellKey(x, y));
        }
    }
    return keys;
}
