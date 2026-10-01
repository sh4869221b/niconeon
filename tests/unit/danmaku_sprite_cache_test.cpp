#include "danmaku/DanmakuAtlasPacker.hpp"
#include "danmaku/DanmakuTextSpriteCache.hpp"

#include "danmaku/DanmakuRenderStyle.hpp"
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QTest>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    bool open = false;
    std::atomic<bool> entered = false;
    void wait() {
        entered = true;
        std::unique_lock lock(mutex);
        changed.wait_for(lock, 2s, [&] { return open; });
    }
    void release() {
        std::lock_guard lock(mutex);
        open = true;
        changed.notify_all();
    }
};
bool hasInk(const QImage &image) {
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
            if (image.pixelColor(x, y).alpha() > 0)
                return true;
    return false;
}
} // namespace

class DanmakuSpriteCacheTest : public QObject {
    Q_OBJECT
  private slots:
    void init() {
        qputenv("NICONEON_RENDER_DIAGNOSTICS", "0");
    }
    void cleanup() {
        qunsetenv("NICONEON_RENDER_DIAGNOSTICS");
    }
    void atlasPackerDoesNotOverlap() {
        DanmakuAtlasPacker packer(QSize(256, 256));
        QVector<QRect> rects;
        for (int width : {80, 96, 120, 64})
            rects.push_back(packer.insert(QSize(width, 42)));
        for (int i = 0; i < rects.size(); ++i) {
            QVERIFY(rects[i].isValid());
            for (int j = i + 1; j < rects.size(); ++j)
                QVERIFY(!rects[i].intersects(rects[j]));
        }
    }
    void duplicateWorkIsCoalescedOffGui() {
        const auto guiThread = std::this_thread::get_id();
        std::atomic<bool> wrongThread = false;
        DanmakuTextSpriteCache cache({}, [&] { wrongThread = std::this_thread::get_id() == guiThread; });
        const auto first = cache.ensureSprite(QStringLiteral("同じテキスト e\u0301 👩‍💻"), 24, 1);
        const auto second = cache.ensureSprite(QStringLiteral("同じテキスト e\u0301 👩‍💻"), 24, 1);
        QCOMPARE(first.spriteId, second.spriteId);
        QVERIFY(first.queuedRaster);
        QVERIFY(!second.queuedRaster);
        QVERIFY(!first.ready);
        QTRY_COMPARE(cache.metrics().completed, 1);
        auto images = cache.takeCompleted(8, 512 * 1024);
        QCOMPARE(images.size(), 1);
        QVERIFY(hasInk(images[0].image));
        QVERIFY(!wrongThread);
        QCOMPARE(cache.widthMeasurementCountForTesting(), 1);
        QVERIFY(cache.ensureSprite(QStringLiteral("同じテキスト e\u0301 👩‍💻"), 24, 1).ready);
        QCOMPARE(cache.metrics().coalesced, 1);
    }
    void pressureRetriesEverySpriteWithoutLoss() {
        DanmakuTextSpriteCache cache({4, 1024, 2, 32 * 1024});
        int accepted = 0;
        QSet<DanmakuSpriteId> seen;
        QElapsedTimer timer;
        timer.start();
        while (seen.size() < 200 && timer.elapsed() < 10000) {
            while (accepted < 200) {
                const auto result = cache.ensureSprite(QStringLiteral("文字%1").arg(accepted), 24, 1);
                if (!result.spriteId)
                    break;
                ++accepted;
            }
            const auto stats = cache.metrics();
            QVERIFY(stats.pending <= 4);
            QVERIFY(stats.requestBytes <= 1024);
            QVERIFY(stats.completed <= 2);
            QVERIFY(stats.completionBytes <= 32 * 1024);
            for (const auto &upload : cache.takeCompleted(1, 32 * 1024)) {
                QVERIFY(!seen.contains(upload.spriteId));
                QVERIFY(hasInk(upload.image));
                seen.insert(upload.spriteId);
            }
            QTest::qWait(1);
        }
        QCOMPARE(accepted, 200);
        QCOMPARE(seen.size(), 200);
        QVERIFY(cache.metrics().backpressured > 0);
        QCOMPARE(cache.metrics().failed, 0);
    }
    void pixelsEqualSynchronousReference_data() {
        QTest::addColumn<QString>("text");
        QTest::addColumn<double>("dpr");
        for (const double dpr : {1.0, 1.5, 2.0}) {
            QTest::newRow(qPrintable(QString("japanese-%1").arg(dpr)))
                << QStringLiteral("日本語コメント 漢字かなカナ") << dpr;
            QTest::newRow(qPrintable(QString("unicode-%1").arg(dpr)))
                << QStringLiteral("A e\u0301 👩‍💻 ❤️ العربية") << dpr;
        }
    }
    void pixelsEqualSynchronousReference() {
        QFETCH(QString, text);
        QFETCH(double, dpr);
        DanmakuTextSpriteCache cache;
        cache.ensureSprite(text, 24, dpr);
        QTRY_COMPARE(cache.metrics().completed, 1);
        const auto upload = cache.takeCompleted(1, 0).first();
        QFont font = QGuiApplication::font();
        font.setPixelSize(24);
        const int width = std::max(80, QFontMetrics(font).horizontalAdvance(text) + 16);
        QImage expected(QSize(static_cast<int>(std::ceil(width * dpr)), static_cast<int>(std::ceil(42 * dpr))),
                        QImage::Format_RGBA8888_Premultiplied);
        expected.setDevicePixelRatio(dpr);
        expected.fill(Qt::transparent);
        {
            QPainter painter(&expected);
            painter.setRenderHint(QPainter::TextAntialiasing, true);
            painter.setFont(font);
            painter.setPen(Qt::white);
            painter.drawText(QRectF(0, 0, width, 42), Qt::AlignVCenter | Qt::AlignHCenter, text);
        }
        QCOMPARE(upload.logicalSize, QSize(width, 42));
        QCOMPARE(upload.image, expected);
    }
    void invalidAndOversizeInputsFailWithoutStalling() {
        DanmakuTextSpriteCache cache;
        QVERIFY(cache.ensureSprite(QString(16385, 'x'), 24, 1).failed);
        QVERIFY(cache.ensureSprite("x", 0, 1).failed);
        QVERIFY(cache.ensureSprite("x", 24, std::numeric_limits<double>::infinity()).failed);
        cache.ensureSprite(QString(16384, 'W'), 24, 16);
        QTRY_COMPARE(cache.metrics().completed, 1);
        const auto upload = cache.takeCompleted(1, 0).first();
        QVERIFY(upload.image.isNull());
        QCOMPARE(cache.metrics().pending, 0);
        QCOMPARE(cache.metrics().failed, 1);
    }
    void cancelPendingPreservesPublishedReusableSprites() {
        DanmakuTextSpriteCache cache;
        const auto id = cache.ensureSprite("reusable", 24, 1).spriteId;
        QTRY_COMPARE(cache.metrics().completed, 1);
        cache.takeCompleted(1, 0);
        for (int i = 0; i < 20; ++i) {
            cache.cancelPending();
            const auto again = cache.ensureSprite("reusable", 24, 1);
            QCOMPARE(again.spriteId, id);
            QVERIFY(again.ready);
        }
        QCOMPARE(cache.widthMeasurementCountForTesting(), 1);
    }
    void completedQueueStopsWorkerAndBoundsBytes() {
        DanmakuTextSpriteCache cache({8, 1024, 1, 32 * 1024});
        for (int i = 0; i < 8; ++i)
            QVERIFY(cache.ensureSprite(QString::number(i), 24, 1).spriteId);
        QTRY_COMPARE(cache.metrics().completed, 1);
        QTest::qWait(30);
        QVERIFY(cache.metrics().rasterized <= 2); // one held completion + one bounded paint
        QCOMPARE(cache.pendingRasterCountForTesting(), 8);
        cache.shutdown();
        QTRY_VERIFY(cache.isStopped());
        QCOMPARE(cache.metrics().pending, 0);
        QVERIFY(cache.takeCompleted(8, 0).empty());
    }
    void generationCancelsInflightAndIdsStayMonotonic() {
        Gate gate;
        DanmakuTextSpriteCache cache({}, [&] { gate.wait(); });
        const auto old = cache.ensureSprite(QStringLiteral("旧セッション"), 24, 1);
        QTRY_VERIFY(gate.entered.load());
        cache.clear();
        const auto current = cache.ensureSprite(QStringLiteral("新セッション"), 24, 2);
        QVERIFY(current.spriteId > old.spriteId);
        gate.release();
        QTRY_COMPARE(cache.metrics().completed, 1);
        auto uploads = cache.takeCompleted(8, 0);
        QCOMPARE(uploads.size(), 1);
        QCOMPARE(uploads[0].spriteId, current.spriteId);
        QCOMPARE(uploads[0].image.devicePixelRatio(), 2);
        QCOMPARE(cache.metrics().stale, 1);
        QCOMPARE(cache.metrics().cancelled, 1);
    }
    void fontGenerationCannotPublishOldPixels() {
        Gate gate;
        DanmakuTextSpriteCache cache({}, [&] { gate.wait(); });
        const auto old = cache.ensureSprite(QStringLiteral("font"), 24, 1);
        QTRY_VERIFY(gate.entered.load());
        QFont font;
        font.setBold(true);
        cache.setFont(font);
        const auto current = cache.ensureSprite(QStringLiteral("font"), 24, 1);
        gate.release();
        QTRY_COMPARE(cache.metrics().completed, 1);
        auto uploads = cache.takeCompleted(8, 0);
        QCOMPARE(uploads[0].spriteId, current.spriteId);
        QVERIFY(current.spriteId > old.spriteId);
        QCOMPARE(cache.metrics().stale, 1);
    }
    void rasterFailureIsTerminalAndShutdownIsNonblocking() {
        DanmakuTextSpriteCache cache({}, [] { throw std::runtime_error("test raster failure"); });
        cache.ensureSprite(QStringLiteral("failed"), 24, 1);
        QTRY_COMPARE(cache.metrics().completed, 1);
        QCOMPARE(cache.takeCompleted(1, 0).size(), 1);
        QVERIFY(cache.ensureSprite(QStringLiteral("failed"), 24, 1).failed);
        QCOMPARE(cache.metrics().failed, 1);
        cache.shutdown();
        QTRY_VERIFY(cache.isStopped());
        QVERIFY(!cache.ensureSprite(QStringLiteral("closed"), 24, 1).spriteId);
    }
    void completionBudgetDefersAndOversizeMakesProgress() {
        DanmakuTextSpriteCache cache;
        for (int i = 0; i < 3; ++i)
            cache.ensureSprite(QString::number(i), 24, 1);
        QTRY_COMPARE(cache.metrics().completed, 3);
        QCOMPARE(cache.takeCompleted(2, 0).size(), 2);
        QCOMPARE(cache.pendingRasterCountForTesting(), 1);
        QVERIFY(cache.takeCompleted(2, 1, false).empty()); // nonempty mailbox has no oversize exception
        QCOMPARE(cache.pendingRasterCountForTesting(), 1);
        QCOMPARE(cache.takeCompleted(2, 1).size(), 1); // bounded single-image exception
        QCOMPARE(cache.pendingRasterCountForTesting(), 0);
    }
    void repeatedStartCancelStop() {
        for (int cycle = 0; cycle < 20; ++cycle) {
            DanmakuTextSpriteCache cache;
            for (int i = 0; i < 30; ++i)
                cache.ensureSprite(QStringLiteral("再起動 %1").arg(i), 24, 1.5);
            cache.clear();
            cache.shutdown();
            QTRY_VERIFY(cache.isStopped());
            QCOMPARE(cache.metrics().pending, 0);
        }
    }
    void diagnosticTimestampsSeparateRasterFromGuiCommit() {
        qputenv("NICONEON_RENDER_DIAGNOSTICS", "1");
        DanmakuTextSpriteCache cache;
        cache.ensureSprite("timed", 24, 1);
        QTRY_COMPARE(cache.metrics().completed, 1);
        const auto beforeCommit =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        const auto uncommitted = cache.lookupSprite("timed", 24, 1);
        QVERIFY(!uncommitted.ready);
        QCOMPARE(uncommitted.rasterCompletedAtNs, 0); // Worker state is not read by the GUI index.
        QTest::qWait(25);
        cache.takeCompleted(1, 0);
        const auto ready = cache.lookupSprite("timed", 24, 1);
        QVERIFY(ready.ready);
        QVERIFY(ready.rasterCompletedAtNs > 0);
        QVERIFY(ready.rasterCompletedAtNs <= beforeCommit);
        QVERIFY(ready.guiReadyAtNs > beforeCommit);
        cache.cancelPending();
        const auto reused = cache.ensureSprite("timed", 24, 1);
        QCOMPARE(reused.rasterCompletedAtNs, ready.rasterCompletedAtNs);
        QCOMPARE(reused.guiReadyAtNs, ready.guiReadyAtNs);
    }
    void disabledDiagnosticsKeepZeroTimestamps() {
        DanmakuTextSpriteCache cache;
        cache.ensureSprite("untimed", 24, 1);
        QTRY_COMPARE(cache.metrics().completed, 1);
        cache.takeCompleted(1, 0);
        const auto ready = cache.ensureSprite("untimed", 24, 1);
        QVERIFY(ready.ready);
        QCOMPARE(ready.rasterCompletedAtNs, 0);
        QCOMPARE(ready.guiReadyAtNs, 0);
    }
};
QTEST_MAIN(DanmakuSpriteCacheTest)
#include "danmaku_sprite_cache_test.moc"
