#include "danmaku/DanmakuController.hpp"

#include <QFont>
#include <QGuiApplication>
#include <QTest>

namespace {
QVariantMap comment(int number, const QString &text = {}) {
    return {{"comment_id", QStringLiteral("id-%1").arg(number)},
            {"user_id", "user"},
            {"at_ms", 0},
            {"text", text.isEmpty() ? QStringLiteral("コメント%1 日本語 e\u0301").arg(number) : text}};
}
} // namespace
class DanmakuRasterPipelineTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        qputenv("NICONEON_DANMAKU_WORKER", "on");
    }
    void renderStallBackpressuresAndRetryLosesNoText() {
        DanmakuController controller;
        controller.setPlaybackPaused(true);
        QVariantList input;
        for (int i = 0; i < 350; ++i)
            input.push_back(comment(i));
        int accepted = controller.appendComments(input, 0);
        QVERIFY(accepted > 0);
        QVERIFY(accepted < input.size());
        QTest::qWait(150); // No render consumer: mailbox and completions fill.
        QVERIFY(controller.rasterMetrics().pending <= 128);
        QVERIFY(controller.rasterMetrics().completed <= 16);
        QVERIFY(controller.rasterMetrics().completionBytes <= DanmakuTextSpriteCache::MaxSpriteBytes);
        QHash<DanmakuSpriteId, QImage> pixels;
        QElapsedTimer timer;
        timer.start();
        while ((accepted != input.size() || controller.renderSnapshot()->instances.size() != input.size()) &&
               timer.elapsed() < 10000) {
            const auto uploads = controller.takePendingSpriteUploads();
            QVERIFY(uploads.size() <= 8);
            qint64 bytes = 0;
            for (const auto &upload : uploads) {
                QVERIFY(!upload.image.isNull());
                bytes += upload.image.sizeInBytes();
                pixels.insert(upload.spriteId, upload.image);
            }
            QVERIFY(bytes <= 512 * 1024 || (uploads.size() == 1 && bytes <= DanmakuTextSpriteCache::MaxSpriteBytes));
            accepted += controller.appendComments(input.mid(accepted), 0);
            QTest::qWait(10);
        }
        for (const auto &upload : controller.takePendingSpriteUploads())
            pixels.insert(upload.spriteId, upload.image);
        QCOMPARE(accepted, input.size());
        const auto snapshot = controller.renderSnapshot();
        QCOMPARE(snapshot->instances.size(), input.size());
        QSet<QString> ids;
        for (const auto &instance : snapshot->instances) {
            QVERIFY(pixels.contains(instance.spriteId));
            QVERIFY(instance.widthEstimate > 80);
            ids.insert(instance.commentId);
        }
        QCOMPARE(ids.size(), input.size());
        QCOMPARE(controller.pendingCommentCountForTesting(), 0);
        QCOMPARE(controller.rasterMetrics().failed, 0);
    }
    void duplicatePendingCommentsHaveBoundedAdmission() {
        DanmakuController controller;
        QVariantList input;
        for (int i = 0; i < 400; ++i)
            input.push_back(comment(i, QStringLiteral("同じコメント")));
        const int accepted = controller.appendComments(input, 0);
        QCOMPARE(accepted, 256);
        QTRY_COMPARE(controller.renderSnapshot()->instances.size(), accepted);
        QCOMPARE(controller.widthMeasurementCountForTesting(), 1);
        QCOMPARE(controller.takePendingSpriteUploads().size(), 1);
        QCOMPARE(controller.appendComments(input.mid(accepted), 0), 144);
        QCOMPARE(controller.renderSnapshot()->instances.size(), 400);
    }
    void seekCancelsOldCompletionsAndPendingComments() {
        DanmakuController controller;
        QVariantList input;
        for (int i = 0; i < 128; ++i)
            input.push_back(comment(i));
        controller.appendComments(input, 0);
        controller.resetForSeek();
        QCOMPARE(controller.pendingCommentCountForTesting(), 0);
        QCOMPARE(controller.appendComments({comment(999)}, 0), 1);
        QTRY_COMPARE(controller.renderSnapshot()->instances.size(), 1);
        QCOMPARE(controller.renderSnapshot()->instances[0].commentId, QStringLiteral("id-999"));
        const auto uploads = controller.takePendingSpriteUploads();
        QCOMPARE(uploads.size(), 1);
        QCOMPARE(uploads[0].spriteId, controller.renderSnapshot()->instances[0].spriteId);
    }
    void dprSwapKeepsReadableSpriteUntilReplacement() {
        DanmakuController controller;
        controller.appendComments({comment(1)}, 0);
        QTRY_COMPARE(controller.renderSnapshot()->instances.size(), 1);
        const auto oldId = controller.renderSnapshot()->instances[0].spriteId;
        // The old upload has not yet reached the renderer when DPR changes.
        controller.setRenderDevicePixelRatio(2);
        QCOMPARE(controller.renderSnapshot()->instances[0].spriteId, oldId);
        const auto oldUploads = controller.takePendingSpriteUploads();
        QCOMPARE(oldUploads.size(), 1);
        QCOMPARE(oldUploads[0].spriteId, oldId);
        QTRY_VERIFY(controller.renderSnapshot()->instances[0].spriteId != oldId);
        const auto uploads = controller.takePendingSpriteUploads();
        QCOMPARE(uploads.size(), 1);
        QCOMPARE(uploads[0].image.devicePixelRatio(), 2);
        QCOMPARE(uploads[0].spriteId, controller.renderSnapshot()->instances[0].spriteId);
        QCOMPARE(uploads[0].logicalSize.width(), controller.renderSnapshot()->instances[0].widthEstimate);
    }
    void queueLatencyAdvancesWithPauseAndRate() {
        DanmakuController controller;
        controller.setViewportSize(5000, 720);
        QVariantList input;
        for (int i = 0; i < 64; ++i)
            input.push_back(comment(i));
        QCOMPARE(controller.appendComments(input, 0), input.size());
        QTest::qWait(100);
        QVERIFY(controller.pendingCommentCountForTesting() > 0); // Stalled render mailbox.
        controller.setPlaybackRate(2);
        controller.setPlaybackPaused(false);
        QTest::qWait(300);
        controller.setPlaybackPaused(true);
        const qreal clock = controller.motionTime();
        QVERIFY(clock > 0.4);
        QElapsedTimer timer;
        timer.start();
        while (controller.pendingCommentCountForTesting() && timer.elapsed() < 3000) {
            controller.takePendingSpriteUploads();
            QTest::qWait(10);
        }
        QCOMPARE(controller.pendingCommentCountForTesting(), 0);
        QCOMPARE(controller.renderSnapshot()->instances.size(), input.size());
        const auto &last = controller.renderSnapshot()->instances.last();
        const qreal speed = 120 + qHash(last.commentId) % 70;
        QVERIFY(std::abs(last.x - (5012 - speed * clock)) < 1);
    }
    void seekReusesSpriteAndRetainsItsUnconsumedUpload() {
        DanmakuController controller;
        controller.appendComments({comment(1)}, 0);
        QTRY_COMPARE(controller.renderSnapshot()->instances.size(), 1);
        const auto sprite = controller.renderSnapshot()->instances[0].spriteId;
        for (int i = 0; i < 20; ++i) {
            controller.resetForSeek();
            QCOMPARE(controller.appendComments({comment(1)}, 0), 1);
            QCOMPARE(controller.renderSnapshot()->instances.size(), 1);
            QCOMPARE(controller.renderSnapshot()->instances[0].spriteId, sprite);
        }
        QCOMPARE(controller.widthMeasurementCountForTesting(), 1);
        const auto uploads = controller.takePendingSpriteUploads();
        QCOMPARE(uploads.size(), 1);
        QCOMPARE(uploads[0].spriteId, sprite);
    }
    void ngSuccessRemovesPendingAndFailureRestoresIt() {
        DanmakuController controller;
        controller.setViewportSize(5000, 720);
        controller.appendComments({comment(0)}, 0);
        QTRY_COMPARE(controller.renderSnapshot()->instances.size(), 1);
        controller.takePendingSpriteUploads();
        auto first = controller.renderSnapshot()->instances[0];
        QVERIFY(controller.beginDragAt(first.x + 8, first.y + 8));
        controller.dropActiveDrag(true);
        controller.appendComments({comment(1)}, 0);
        QTest::qWait(60);
        QCOMPARE(controller.pendingCommentCountForTesting(), 1);
        controller.rollbackPendingNgUserFade("user");
        QTRY_COMPARE(controller.pendingCommentCountForTesting(), 0);
        QCOMPARE(controller.renderSnapshot()->instances.size(), 2);
        controller.takePendingSpriteUploads();
        first = controller.renderSnapshot()->instances[0];
        QVERIFY(controller.beginDragAt(first.x + 8, first.y + 8));
        controller.dropActiveDrag(true);
        controller.appendComments({comment(2)}, 0);
        controller.applyNgUserFade("user");
        QCOMPARE(controller.pendingCommentCountForTesting(), 0);
        QTest::qWait(100);
        for (const auto &instance : controller.renderSnapshot()->instances)
            QVERIFY(instance.commentId != "id-2");
    }
    void workerElapsedCapIsSharedByDeferredAndVisibleComments() {
        DanmakuController controller;
        controller.setViewportSize(5000, 720);
        controller.appendComments({comment(1)}, 0);
        QTRY_COMPARE(controller.renderSnapshot()->instances.size(), 1);
        QVariantList input;
        for (int i = 2; i < 50; ++i)
            input.push_back(comment(i));
        controller.appendComments(input, 0);
        controller.setPlaybackPaused(false);
        // This is an intentional GUI stall, not an event-loop wait.
        QTest::qSleep(350);
        QTest::qWait(40);
        controller.setPlaybackPaused(true);
        QVERIFY(controller.motionTime() < 0.3);
        QElapsedTimer timer;
        timer.start();
        while (controller.pendingCommentCountForTesting() && timer.elapsed() < 3000) {
            controller.takePendingSpriteUploads();
            QTest::qWait(10);
        }
        QCOMPARE(controller.pendingCommentCountForTesting(), 0);
        const auto snapshot = controller.renderSnapshot();
        const auto &last = snapshot->instances.last();
        const qreal speed = 120 + qHash(last.commentId) % 70;
        QVERIFY(std::abs(last.x - (5012 - speed * controller.motionTime())) < 1);
    }
    void invalidPrefixIsConsumedAndShutdownRejectsAdmission() {
        DanmakuController controller;
        QCOMPARE(controller.appendComments({QVariantMap{}, comment(1)}, 0), 2);
        controller.shutdownRaster();
        QCOMPARE(controller.appendComments({comment(2)}, 0), 0);
        QTRY_VERIFY(controller.rasterStopped());
    }
};
QTEST_MAIN(DanmakuRasterPipelineTest)
#include "danmaku_raster_pipeline_test.moc"
