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

    QVERIFY(first.createdUpload);
    QVERIFY(!second.createdUpload);
    QCOMPARE(first.spriteId, second.spriteId);
    QCOMPARE(cache.widthMeasurementCountForTesting(), 1);
}

void DanmakuSpriteCacheTest::differentDevicePixelRatioCreatesDifferentSprite() {
    DanmakuTextSpriteCache cache;

    const auto first = cache.ensureSprite(QStringLiteral("dpi text"), 24, 1.0);
    const auto second = cache.ensureSprite(QStringLiteral("dpi text"), 24, 2.0);

    QVERIFY(first.createdUpload);
    QVERIFY(second.createdUpload);
    QVERIFY(first.spriteId != second.spriteId);
}

QTEST_MAIN(DanmakuSpriteCacheTest)

#include "danmaku_sprite_cache_test.moc"
