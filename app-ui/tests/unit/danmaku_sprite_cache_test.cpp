#include "danmaku/DanmakuAtlasPacker.hpp"
#include "danmaku/DanmakuTextSpriteCache.hpp"

#include <QRect>
#include <QSize>
#include <QTest>
#include <QVector>

class DanmakuSpriteCacheTest : public QObject {
    Q_OBJECT

private slots:
    void atlasPackerDoesNotOverlap();
    void repeatedEnsureSpriteReusesWidthMeasurement();
    void differentDevicePixelRatioCreatesDifferentSprite();
    void pendingRasterBudgetDefersRemainingSprites();
    void takePendingUploadRemovesQueuedSprite();
    void clearKeepsSpriteIdsMonotonic();
};

void DanmakuSpriteCacheTest::atlasPackerDoesNotOverlap() {
    DanmakuAtlasPacker packer(QSize(256, 256));

    QVector<QRect> rects;
    rects.push_back(packer.insert(QSize(80, 42)));
    rects.push_back(packer.insert(QSize(96, 42)));
    rects.push_back(packer.insert(QSize(120, 42)));
    rects.push_back(packer.insert(QSize(64, 42)));

    for (const QRect &rect : rects) {
        QVERIFY(rect.isValid());
    }

    for (int i = 0; i < rects.size(); ++i) {
        for (int j = i + 1; j < rects.size(); ++j) {
            QVERIFY2(!rects[i].intersects(rects[j]), "atlas rectangles should not overlap");
        }
    }
}

void DanmakuSpriteCacheTest::repeatedEnsureSpriteReusesWidthMeasurement() {
    DanmakuTextSpriteCache cache;

    const auto first = cache.ensureSprite(QStringLiteral("same text"), 24, 1.0);
    const auto second = cache.ensureSprite(QStringLiteral("same text"), 24, 1.0);

    QVERIFY(first.queuedRaster);
    QVERIFY(!second.queuedRaster);
    QCOMPARE(first.spriteId, second.spriteId);
    QCOMPARE(cache.widthMeasurementCountForTesting(), 1);
}

void DanmakuSpriteCacheTest::differentDevicePixelRatioCreatesDifferentSprite() {
    DanmakuTextSpriteCache cache;

    const auto first = cache.ensureSprite(QStringLiteral("dpi text"), 24, 1.0);
    const auto second = cache.ensureSprite(QStringLiteral("dpi text"), 24, 2.0);

    QVERIFY(first.queuedRaster);
    QVERIFY(second.queuedRaster);
    QVERIFY(first.spriteId != second.spriteId);
}

void DanmakuSpriteCacheTest::pendingRasterBudgetDefersRemainingSprites() {
    DanmakuTextSpriteCache cache;

    cache.ensureSprite(QStringLiteral("alpha"), 24, 1.0);
    cache.ensureSprite(QStringLiteral("beta"), 24, 1.0);
    cache.ensureSprite(QStringLiteral("gamma"), 24, 1.0);

    const QVector<DanmakuSpriteUpload> firstBatch = cache.rasterizePendingSprites(2, 0);
    QCOMPARE(firstBatch.size(), 2);
    QCOMPARE(cache.pendingRasterCountForTesting(), 1);

    const QVector<DanmakuSpriteUpload> secondBatch = cache.rasterizePendingSprites(2, 0);
    QCOMPARE(secondBatch.size(), 1);
    QCOMPARE(cache.pendingRasterCountForTesting(), 0);
}

void DanmakuSpriteCacheTest::takePendingUploadRemovesQueuedSprite() {
    DanmakuTextSpriteCache cache;

    const auto first = cache.ensureSprite(QStringLiteral("eager"), 24, 1.0);
    QVERIFY(first.queuedRaster);

    const DanmakuSpriteUpload upload = cache.takePendingUpload(QStringLiteral("eager"), 24, 1.0);
    QCOMPARE(upload.spriteId, first.spriteId);
    QVERIFY(!upload.image.isNull());
    QCOMPARE(cache.pendingRasterCountForTesting(), 0);

    const QVector<DanmakuSpriteUpload> remaining = cache.rasterizePendingSprites(4, 0);
    QVERIFY(remaining.isEmpty());
}

void DanmakuSpriteCacheTest::clearKeepsSpriteIdsMonotonic() {
    DanmakuTextSpriteCache cache;

    const auto first = cache.ensureSprite(QStringLiteral("before clear"), 24, 1.0);
    QVERIFY(first.queuedRaster);

    cache.clear();

    const auto second = cache.ensureSprite(QStringLiteral("after clear"), 24, 1.0);
    QVERIFY(second.queuedRaster);
    QVERIFY(second.spriteId > first.spriteId);
}

QTEST_MAIN(DanmakuSpriteCacheTest)

#include "danmaku_sprite_cache_test.moc"
