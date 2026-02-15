#include "danmaku/DanmakuSpatialGrid.hpp"

#include <QPointF>
#include <QRectF>
#include <QTest>
#include <QVector>

#include <algorithm>

namespace {
QVector<int> sortedRows(QVector<int> rows) {
    std::sort(rows.begin(), rows.end());
    return rows;
}
} // namespace

class SpatialGridIncrementalTest : public QObject {
    Q_OBJECT

private slots:
    void upsertAndQueryFindsRow();
    void upsertReplacesPreviousCells();
    void removeRowClearsRowHits();
    void incrementalUpdatesMatchRebuild();
};

void SpatialGridIncrementalTest::upsertAndQueryFindsRow() {
    DanmakuSpatialGrid grid;
    grid.setCellSize(96.0, 42.0);
    grid.upsertRow(7, QRectF(20.0, 10.0, 120.0, 42.0));

    QCOMPARE(sortedRows(grid.queryPoint(QPointF(40.0, 25.0))), QVector<int>{7});
    QCOMPARE(sortedRows(grid.queryRect(QRectF(0.0, 0.0, 220.0, 120.0))), QVector<int>{7});
}

void SpatialGridIncrementalTest::upsertReplacesPreviousCells() {
    DanmakuSpatialGrid grid;
    grid.setCellSize(96.0, 42.0);
    grid.upsertRow(12, QRectF(10.0, 10.0, 100.0, 42.0));
    grid.upsertRow(12, QRectF(420.0, 12.0, 100.0, 42.0));

    QCOMPARE(sortedRows(grid.queryPoint(QPointF(20.0, 20.0))), QVector<int>{});
    QCOMPARE(sortedRows(grid.queryPoint(QPointF(440.0, 24.0))), QVector<int>{12});
}

void SpatialGridIncrementalTest::removeRowClearsRowHits() {
    DanmakuSpatialGrid grid;
    grid.setCellSize(96.0, 42.0);
    grid.upsertRow(3, QRectF(60.0, 14.0, 80.0, 42.0));
    grid.removeRow(3);

    QCOMPARE(sortedRows(grid.queryPoint(QPointF(72.0, 20.0))), QVector<int>{});
    QCOMPARE(sortedRows(grid.queryRect(QRectF(0.0, 0.0, 200.0, 120.0))), QVector<int>{});
}

void SpatialGridIncrementalTest::incrementalUpdatesMatchRebuild() {
    DanmakuSpatialGrid incremental;
    incremental.setCellSize(96.0, 42.0);
    incremental.upsertRow(1, QRectF(20.0, 10.0, 120.0, 42.0));
    incremental.upsertRow(2, QRectF(220.0, 10.0, 120.0, 42.0));
    incremental.upsertRow(3, QRectF(20.0, 70.0, 120.0, 42.0));
    incremental.upsertRow(2, QRectF(280.0, 10.0, 120.0, 42.0));
    incremental.removeRow(1);
    incremental.upsertRow(4, QRectF(280.0, 70.0, 120.0, 42.0));

    DanmakuSpatialGrid rebuilt;
    QVector<DanmakuSpatialGrid::Entry> entries{
        DanmakuSpatialGrid::Entry{2, QRectF(280.0, 10.0, 120.0, 42.0)},
        DanmakuSpatialGrid::Entry{3, QRectF(20.0, 70.0, 120.0, 42.0)},
        DanmakuSpatialGrid::Entry{4, QRectF(280.0, 70.0, 120.0, 42.0)},
    };
    rebuilt.rebuild(entries, 96.0, 42.0);

    const QVector<QPointF> probes{
        QPointF(300.0, 20.0),
        QPointF(40.0, 80.0),
        QPointF(300.0, 80.0),
        QPointF(40.0, 20.0),
    };
    for (const QPointF &probe : probes) {
        QCOMPARE(sortedRows(incremental.queryPoint(probe)), sortedRows(rebuilt.queryPoint(probe)));
    }

    const QVector<QRectF> ranges{
        QRectF(0.0, 0.0, 500.0, 200.0),
        QRectF(0.0, 0.0, 180.0, 80.0),
        QRectF(240.0, 0.0, 220.0, 80.0),
        QRectF(240.0, 40.0, 220.0, 80.0),
    };
    for (const QRectF &range : ranges) {
        QCOMPARE(sortedRows(incremental.queryRect(range)), sortedRows(rebuilt.queryRect(range)));
    }
}

QTEST_APPLESS_MAIN(SpatialGridIncrementalTest)

#include "spatial_grid_incremental_test.moc"
