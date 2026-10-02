#include "../perf/bilinear_reference.hpp"
#include "danmaku/DanmakuAtlasPacker.hpp"

#include <QPainter>
#include <QRegion>
#include <QSet>
#include <QTest>
#include <algorithm>

namespace {
void verifyPlacements(const DanmakuAtlasRepackPlan &plan, const QSize &pageSize) {
    const QRect page(QPoint(0, 0), pageSize);
    QSet<quint64> ids;
    for (qsizetype i = 0; i < plan.placements.size(); ++i) {
        const auto &placement = plan.placements[i];
        QVERIFY(page.contains(placement.rect));
        QVERIFY(!ids.contains(placement.spriteId));
        ids.insert(placement.spriteId);
        for (qsizetype j = 0; j < i; ++j)
            QVERIFY(!placement.rect.intersects(plan.placements[j].rect));
    }
}
} // namespace

class DanmakuAtlasRepackTest : public QObject {
    Q_OBJECT
  private slots:
    void bilinearReferenceUsesPhysicalPixelCenters() {
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            QImage source(4, 4, QImage::Format_RGBA8888_Premultiplied);
            source.setDevicePixelRatio(dpr);
            source.fill(Qt::transparent);
            source.setPixelColor(1, 1, QColor(255, 0, 0, 128));
            QImage target(8, 8, QImage::Format_RGBA8888_Premultiplied);
            target.setDevicePixelRatio(dpr);
            target.fill(QColor(96, 96, 96));
            niconeon::perf::drawBilinearReference(target, source, {1.25 / dpr, 1.0 / dpr});
            QCOMPARE(target.pixelColor(2, 2), QColor(156, 60, 60));
            QCOMPARE(target.pixelColor(3, 2), QColor(116, 84, 84));
            QCOMPARE(target.pixelColor(0, 0), QColor(96, 96, 96));
            target.fill(QColor(96, 96, 96));
            niconeon::perf::drawBilinearReference(target, source, {-0.25 / dpr, 0});
            QCOMPARE(target.pixelColor(0, 1), QColor(116, 84, 84));
        }
    }
    void bilinearReferenceRejectsSnappingAndDisplacement_data() {
        QTest::addColumn<QString>("family");
        QTest::addColumn<qreal>("dpr");
        QTest::addColumn<qreal>("offset");
        for (const auto &family :
             {QStringLiteral("DejaVu Sans"), QStringLiteral("DejaVu Serif"), QStringLiteral("Noto Sans CJK JP")})
            for (const qreal dpr : {1.0, 1.5, 2.0})
                for (const qreal offset : {-0.5, -0.25, 0.25, 0.5})
                    QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(family).arg(dpr).arg(offset)))
                        << family << dpr << offset;
    }
    void bilinearReferenceRejectsSnappingAndDisplacement() {
        QFETCH(QString, family);
        QFETCH(qreal, dpr);
        QFETCH(qreal, offset);
        QImage source(QSize(320 * dpr, 48 * dpr), QImage::Format_RGBA8888_Premultiplied);
        source.setDevicePixelRatio(dpr);
        source.fill(Qt::transparent);
        {
            QPainter paint(&source);
            QFont font(family);
            font.setPixelSize(24);
            paint.setFont(font);
            paint.setPen(Qt::white);
            paint.drawText(QRectF(0, 0, 320, 48), Qt::AlignCenter, QStringLiteral("WHITE é 日本語 123"));
        }
        QImage background(QSize(350 * dpr, 70 * dpr), QImage::Format_RGBA8888_Premultiplied);
        background.setDevicePixelRatio(dpr);
        background.fill(QColor(96, 96, 96));
        const auto differingPixels = [](const QImage &a, const QImage &b, int tolerance) {
            int count = 0;
            for (int y = 0; y < a.height(); ++y)
                for (int x = 0; x < a.width(); ++x) {
                    const auto *pa = a.constScanLine(y) + x * 4;
                    const auto *pb = b.constScanLine(y) + x * 4;
                    for (int c = 0; c < 4; ++c)
                        if (std::abs(int(pa[c]) - int(pb[c])) > tolerance) {
                            ++count;
                            break;
                        }
                }
            return count;
        };
        QImage integerPainter = background, integerOracle = background;
        {
            QPainter paint(&integerPainter);
            paint.drawImage(QPointF(12 / dpr, 8 / dpr), source);
        }
        niconeon::perf::drawBilinearReference(integerOracle, source, {12 / dpr, 8 / dpr});
        QCOMPARE(differingPixels(integerPainter, integerOracle, 1), 0);
        QImage linear = background, snapped = background, displaced = background;
        const QPointF position((12 + offset) / dpr, 8 / dpr);
        niconeon::perf::drawBilinearReference(linear, source, position);
        {
            QPainter paint(&snapped);
            paint.setRenderHint(QPainter::SmoothPixmapTransform);
            paint.drawImage(position, source); // Known nearest/snap negative control.
        }
        niconeon::perf::drawBilinearReference(displaced, source, position + QPointF(1 / dpr, 0));
        QVERIFY(differingPixels(linear, snapped, 8) > 50);
        QVERIFY(differingPixels(linear, displaced, 8) > 50);
    }
    void batchesAllFittingPendingSprites() {
        const auto plan =
            planDanmakuAtlasRepack({100, 100}, {{1, {100, 30}}}, {{2, {50, 30}}, {3, {50, 30}}, {4, {100, 40}}});
        QVERIFY(plan.canCommit);
        QCOMPARE(plan.admittedSprites, 3);
        QCOMPARE(plan.placements.size(), 4);
        QCOMPARE(plan.placements.first().spriteId, 1u);
        verifyPlacements(plan, {100, 100});
    }

    void protectsEveryActiveSpriteWhenOnlyPartOfTheBatchFits() {
        const auto plan = planDanmakuAtlasRepack({100, 100}, {{1, {100, 60}}},
                                                 {{2, {100, 50}}, {3, {50, 40}}, {4, {50, 40}}, {5, {1, 1}}});
        QVERIFY(plan.canCommit);
        QCOMPARE(plan.admittedSprites, 2);
        QCOMPARE(plan.placements.size(), 3);
        QCOMPARE(plan.placements[0].spriteId, 1u);
        QCOMPARE(plan.placements[1].spriteId, 3u);
        QCOMPARE(plan.placements[2].spriteId, 4u);
        verifyPlacements(plan, {100, 100});
    }

    void failureDoesNotMutateExistingPackingState() {
        DanmakuAtlasPacker original({100, 100});
        QCOMPARE(original.insert({80, 60}), QRect(0, 0, 80, 60));
        const auto impossibleActive = planDanmakuAtlasRepack({100, 100}, {{1, {80, 60}}, {2, {80, 60}}}, {{3, {1, 1}}});
        QVERIFY(!impossibleActive.canCommit);
        const auto noNewSprite = planDanmakuAtlasRepack({100, 100}, {{1, {80, 60}}}, {{3, {101, 1}}});
        QVERIFY(!noNewSprite.canCommit);
        QCOMPARE(original.insert({20, 60}), QRect(80, 0, 20, 60));
        QCOMPARE(original.insert({100, 40}), QRect(0, 60, 100, 40));
    }

    void deduplicatesPendingAndSkipsInvalidSizes() {
        const auto plan = planDanmakuAtlasRepack(
            {100, 100}, {{1, {50, 50}}},
            {{1, {50, 50}}, {2, {50, 50}}, {2, {50, 50}}, {3, {101, 1}}, {4, {1, 101}}, {5, {0, 1}}, {0, {1, 1}}});
        QVERIFY(plan.canCommit);
        QCOMPARE(plan.admittedSprites, 1);
        QCOMPARE(plan.placements.size(), 2);
        verifyPlacements(plan, {100, 100});
    }

    void packingIsDeterministicAcrossResidentHashOrder() {
        QVector<DanmakuAtlasRepackCandidate> active{{1, {30, 40}}, {2, {50, 30}}, {3, {20, 20}}};
        QVector<DanmakuAtlasRepackCandidate> pending{{4, {10, 10}}, {5, {20, 10}}, {6, {10, 10}}};
        const auto first = planDanmakuAtlasRepack({100, 100}, active, pending);
        std::reverse(active.begin(), active.end());
        std::reverse(pending.begin(), pending.end());
        const auto second = planDanmakuAtlasRepack({100, 100}, active, pending);
        QVERIFY(first.canCommit);
        QVERIFY(second.canCommit);
        QCOMPARE(first.admittedSprites, second.admittedSprites);
        QCOMPARE(first.placements.size(), second.placements.size());
        for (qsizetype i = 0; i < first.placements.size(); ++i) {
            QCOMPARE(first.placements[i].spriteId, second.placements[i].spriteId);
            QCOMPARE(first.placements[i].rect, second.placements[i].rect);
        }
        verifyPlacements(first, {100, 100});
    }

    void fullWidthIdsDoNotAliasPieces() {
        const quint64 a = quint64(1) << 32, b = quint64(2) << 32;
        const auto plan = planDanmakuAtlasRepack({100, 100}, {{a, {25, 25}}}, {{a + 1, {25, 25}}, {b, {25, 25}}});
        QVERIFY(plan.canCommit);
        QCOMPARE(plan.admittedSprites, 2);
        verifyPlacements(plan, {100, 100});
    }
    void croppedTilesRetainEveryPixelAndGutter() {
        QImage image(4714, 84, QImage::Format_RGBA8888_Premultiplied);
        image.fill(Qt::transparent);
        for (int y = 13; y < 70; ++y)
            for (int x = 7; x < 4704; ++x)
                image.setPixelColor(x, y, QColor((x * 7) % 255, (y * 3) % 255, (x + y) % 255, 1 + (x + y) % 255));
        const auto regions = danmakuAtlasTiles(image);
        QCOMPARE(regions.size(), 3);
        QImage restored(image.size(), image.format());
        restored.fill(Qt::transparent);
        QRegion covered;
        QPainter painter(&restored);
        for (const auto &region : regions) {
            QVERIFY(region.source.width() <= 2048 && region.source.height() <= 2048);
            QVERIFY(image.rect().contains(region.source));
            QVERIFY(region.source.contains(region.core));
            QVERIFY(covered.intersected(region.core).isEmpty());
            covered += region.core;
            // Reconstruct through exactly the same source/core offset used by UVs.
            const auto uploaded = image.copy(region.source);
            painter.drawImage(region.core, uploaded,
                              QRect(region.core.topLeft() - region.source.topLeft(), region.core.size()));
            if (region.core.left() > 0)
                QCOMPARE(region.source.left(), region.core.left() - 1);
            if (region.core.right() < image.width() - 1)
                QCOMPARE(region.source.right(), region.core.right() + 1);
        }
        painter.end();
        QCOMPARE(restored, image);
        QVERIFY(covered.contains(QRect(7, 13, 4697, 57)));
    }
    void transparentAndEdgeImagesStayValid() {
        QImage blank(80, 42, QImage::Format_RGBA8888_Premultiplied);
        blank.fill(Qt::transparent);
        const auto empty = danmakuAtlasTiles(blank);
        QCOMPARE(empty.size(), 1);
        QCOMPARE(empty.first().core, QRect(0, 0, 1, 1));
        QImage edge(4097, 42, QImage::Format_RGBA8888_Premultiplied);
        edge.fill(QColor(255, 255, 255, 1));
        const auto regions = danmakuAtlasTiles(edge);
        QCOMPARE(regions.size(), 3);
        QRegion covered;
        for (const auto &r : regions)
            covered += r.core;
        QCOMPARE(covered, QRegion(edge.rect()));
        QVERIFY(danmakuAtlasTiles({}, 2048).isEmpty());
        QVERIFY(danmakuAtlasTiles(edge, 2).isEmpty());
    }
    void fractionalMappingPreservesWholeSpriteGeometry() {
        const QSize physical(503, 63), logical(335, 42);
        const QPointF origin(10.25, 20.5);
        const auto whole = danmakuAtlasTileLogicalRect(QRect(QPoint(0, 0), physical), physical, logical, origin);
        QCOMPARE(whole, QRectF(origin, QSizeF(logical)));
        const auto left = danmakuAtlasTileLogicalRect({0, 0, 250, 63}, physical, logical, origin);
        const auto right = danmakuAtlasTileLogicalRect({250, 0, 253, 63}, physical, logical, origin);
        QVERIFY(qAbs(left.right() - right.left()) < 1e-10);
        QVERIFY(qAbs(right.right() - whole.right()) < 1e-10);
    }

    void retainedPackStateAcceptsFutureSpritesWithoutAnotherRepack() {
        auto plan = planDanmakuAtlasRepack({100, 100}, {{1, {100, 40}}}, {{2, {100, 20}}});
        QVERIFY(plan.canCommit);
        QCOMPARE(plan.packer.insert({100, 40}), QRect(0, 60, 100, 40));
        QVERIFY(!plan.packer.insert({1, 1}).isValid());
    }
};

QTEST_MAIN(DanmakuAtlasRepackTest)
#include "danmaku_atlas_repack_test.moc"
