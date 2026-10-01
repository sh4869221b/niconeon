#include "danmaku/DanmakuAtlasPacker.hpp"

#include <QSet>
#include <QTest>
#include <algorithm>

namespace {
void verifyPlacements(const DanmakuAtlasRepackPlan &plan, const QSize &pageSize) {
    const QRect page(QPoint(0, 0), pageSize);
    QSet<quint32> ids;
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

    void retainedPackStateAcceptsFutureSpritesWithoutAnotherRepack() {
        auto plan = planDanmakuAtlasRepack({100, 100}, {{1, {100, 40}}}, {{2, {100, 20}}});
        QVERIFY(plan.canCommit);
        QCOMPARE(plan.packer.insert({100, 40}), QRect(0, 60, 100, 40));
        QVERIFY(!plan.packer.insert({1, 1}).isValid());
    }
};

QTEST_GUILESS_MAIN(DanmakuAtlasRepackTest)
#include "danmaku_atlas_repack_test.moc"
