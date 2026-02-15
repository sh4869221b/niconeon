#pragma once

#include <QHash>
#include <QPointF>
#include <QRectF>
#include <QVector>

class DanmakuSpatialGrid {
public:
    struct Entry {
        int row = -1;
        QRectF rect;
    };

    void clear();
    void setCellSize(qreal cellWidth, qreal cellHeight);
    void upsertRow(int row, const QRectF &rect);
    void removeRow(int row);
    void rebuild(const QVector<Entry> &entries, qreal cellWidth, qreal cellHeight);
    QVector<int> queryPoint(const QPointF &point) const;
    QVector<int> queryRect(const QRectF &rect) const;

private:
    static quint64 cellKey(int cellX, int cellY);
    static void appendUnique(QVector<int> &rows, int row);
    static QVector<quint64> cellKeysForRect(const QRectF &rect, qreal cellWidth, qreal cellHeight);

    qreal m_cellWidth = 160.0;
    qreal m_cellHeight = 48.0;
    QHash<quint64, QVector<int>> m_cells;
    QHash<int, QRectF> m_rowRects;
    QHash<int, QVector<quint64>> m_rowCellKeys;
};
