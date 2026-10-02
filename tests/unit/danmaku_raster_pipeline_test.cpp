#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuRenderStyle.hpp"

#include <QElapsedTimer>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QTest>
#include <cmath>
#include <thread>

namespace {
QVariantMap comment(int number, const QString &text = {}) {
    return {{"comment_id", QStringLiteral("id-%1").arg(number)},
            {"user_id", "user"},
            {"at_ms", 0},
            {"text", text.isEmpty() ? QStringLiteral("コメント%1 日本語 e\u0301").arg(number) : text}};
}
bool pumpOnlyQueuedCallbacks(DanmakuController &controller, int expectedActive) {
    QElapsedTimer timer;
    timer.start();
    while (controller.renderSnapshot()->instances.size() != expectedActive && timer.elapsed() < 3000) {
        // Deliberately never deliver the controller's frame timer or a render
        // presentation event. Only payload-free completion wakes make progress.
        QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        QTest::qSleep(1);
    }
    return controller.renderSnapshot()->instances.size() == expectedActive;
}
bool waitForPublishedCompletions(DanmakuController &controller, int count, quint64 expectedWakeChecks) {
    const auto published = [&] {
        const auto stats = controller.rasterMetrics();
        return stats.completed >= count && stats.wakeNotifications + stats.wakeCoalesced >= expectedWakeChecks;
    };
    QElapsedTimer timer;
    timer.start();
    while (!published() && timer.elapsed() < 3000)
        QTest::qSleep(1);
    return published();
}
} // namespace
class DanmakuRasterPipelineTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        qputenv("NICONEON_DANMAKU_WORKER", "on");
    }
    void init() {
        qputenv("NICONEON_RENDER_DIAGNOSTICS", "0");
        qputenv("NICONEON_DANMAKU_WORKER", "on");
    }
    void cleanup() {
        qunsetenv("NICONEON_RENDER_DIAGNOSTICS");
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
        QVERIFY(controller.rasterMetrics().completed <= 32);
        QVERIFY(controller.rasterMetrics().completionBytes <= DanmakuTextSpriteCache::MaxSpriteBytes);
        QHash<DanmakuSpriteId, QImage> pixels;
        QElapsedTimer timer;
        timer.start();
        while ((accepted != input.size() || controller.renderSnapshot()->instances.size() != input.size()) &&
               timer.elapsed() < 10000) {
            const auto uploads = controller.takePendingSpriteUploads();
            QVERIFY(uploads.size() <= 32);
            qint64 bytes = 0;
            for (const auto &upload : uploads) {
                QVERIFY(!upload.image.isNull());
                bytes += upload.image.sizeInBytes();
                pixels.insert(upload.spriteId, upload.image);
            }
            QVERIFY(bytes <= 2 * 1024 * 1024 ||
                    (uploads.size() == 1 && bytes <= DanmakuTextSpriteCache::MaxSpriteBytes));
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
    void timingDiagnosticsAreDisabledByDefault() {
        qunsetenv("NICONEON_RENDER_DIAGNOSTICS");
        DanmakuController controller;
        QCOMPARE(controller.appendComments({comment(1)}, 0), 1);
        QTRY_COMPARE(controller.pendingCommentCountForTesting(), 0);
        controller.shutdownRaster();
        const auto batch = controller.takeCommentTimingDiagnostics();
        QVERIFY(!batch.enabled);
        QVERIFY(batch.records.isEmpty());
        QVERIFY(batch.pending.isEmpty());
        QCOMPARE(batch.recordedComments, 0);
        QCOMPARE(batch.droppedComments, 0);
    }
    void timingSeparatesSourceLagFromSourceMotion() {
        qputenv("NICONEON_RENDER_DIAGNOSTICS", "1");
        DanmakuController controller;
        controller.setViewportSize(0, 720);
        QCOMPARE(controller.appendComments({comment(1, "x")}, 30000), 1);
        QTRY_COMPARE(controller.pendingCommentCountForTesting(), 0);
        auto batch = controller.takeCommentTimingDiagnostics();
        QCOMPARE(batch.records.size(), 1);
        const auto lagged = batch.records.first();
        QCOMPARE(lagged.outcome, DanmakuCommentTimingOutcome::Expired);
        QCOMPARE(lagged.expiryOrigin, DanmakuCommentExpiryOrigin::SourceLag);
        QCOMPARE(lagged.sourceLagMs, 30000); // Raw lag, before the existing 15s compensation cap.
        QCOMPARE(lagged.sourceMotionDelaySeconds, 0);
        QVERIFY(lagged.initialX + lagged.widthEstimate < -20);
        QVERIFY(lagged.admittedAtNs > 0);
        QVERIFY(lagged.rasterCompletedAtNs > 0);
        QVERIFY(lagged.guiReadyAtNs >= lagged.admittedAtNs);
        QVERIFY(lagged.guiReadyAtNs >= lagged.rasterCompletedAtNs);
        QVERIFY(lagged.resolvedAtNs >= lagged.guiReadyAtNs);

        controller.setPlaybackRate(3);
        controller.setPlaybackPaused(false);
        QTRY_VERIFY(controller.motionTime() > 1.0);
        controller.setPlaybackPaused(true);
        const auto beforeAdmission = controller.motionTime();
        QCOMPARE(controller.appendComments({comment(2, "x")}, 0, 0), 1);
        batch = controller.takeCommentTimingDiagnostics();
        QCOMPARE(batch.records.size(), 1);
        const auto deferred = batch.records.first();
        QCOMPARE(deferred.outcome, DanmakuCommentTimingOutcome::Expired);
        QCOMPARE(deferred.expiryOrigin, DanmakuCommentExpiryOrigin::SourceMotion);
        QCOMPARE(deferred.sourceLagMs, 0);
        QCOMPARE(deferred.sourceMotionDelaySeconds, beforeAdmission);
        QCOMPARE(deferred.rasterMotionDelaySeconds, 0);
        QVERIFY(deferred.initialX + deferred.widthEstimate >= -20);
        QVERIFY(deferred.admissionX + deferred.widthEstimate < -20);
        // Warm shared sprites retain their true timestamps, even before admission.
        QCOMPARE(deferred.rasterCompletedAtNs, lagged.rasterCompletedAtNs);
        QCOMPARE(deferred.guiReadyAtNs, lagged.guiReadyAtNs);
        QVERIFY(deferred.guiReadyAtNs < deferred.admittedAtNs);
    }
    void timingIdentifiesPostAdmissionWaitExpiry() {
        qputenv("NICONEON_RENDER_DIAGNOSTICS", "1");
        DanmakuController controller;
        controller.setViewportSize(0, 720);
        QVariantList input;
        for (int i = 0; i < 64; ++i)
            input.push_back(comment(i, QStringLiteral("x%1").arg(i)));
        QCOMPARE(controller.appendComments(input, 0), input.size());
        QTest::qWait(100);
        QVERIFY(controller.pendingCommentCountForTesting() > 0); // Bounded upload mailbox is stalled.
        controller.setPlaybackRate(3);
        controller.setPlaybackPaused(false);
        QTRY_VERIFY(controller.motionTime() > 1.0);
        controller.setPlaybackPaused(true);
        QElapsedTimer timer;
        timer.start();
        while (controller.pendingCommentCountForTesting() && timer.elapsed() < 5000) {
            controller.takePendingSpriteUploads();
            QTest::qWait(10);
        }
        QCOMPARE(controller.pendingCommentCountForTesting(), 0);
        const auto batch = controller.takeCommentTimingDiagnostics();
        QCOMPARE(batch.records.size(), input.size());
        int expired = 0;
        for (const auto &record : batch.records) {
            QCOMPARE(record.sourceLagMs, 0);
            QCOMPARE(record.sourceMotionDelaySeconds, 0);
            if (record.outcome != DanmakuCommentTimingOutcome::Expired)
                continue;
            ++expired;
            QCOMPARE(record.expiryOrigin, DanmakuCommentExpiryOrigin::RasterWait);
            QVERIFY(record.admissionX + record.widthEstimate >= -20);
            QVERIFY(record.resolvedX + record.widthEstimate < -20);
            QVERIFY(record.rasterMotionDelaySeconds > 0);
            QVERIFY(record.rasterCompletedAtNs > 0);
            QVERIFY(record.guiReadyAtNs >= record.admittedAtNs);
            QVERIFY(record.guiReadyAtNs >= record.rasterCompletedAtNs);
            QVERIFY(record.resolvedAtNs >= record.guiReadyAtNs);
        }
        QVERIFY(expired > 0);
        QCOMPARE(controller.expiredRasterComments(), expired);
    }
    void timingRetainsPendingShutdownAndOtherCancellations_data() {
        QTest::addColumn<int>("reason");
        QTest::newRow("shutdown") << static_cast<int>(DanmakuCommentCancellationReason::Shutdown);
        QTest::newRow("seek") << static_cast<int>(DanmakuCommentCancellationReason::Seek);
        QTest::newRow("session") << static_cast<int>(DanmakuCommentCancellationReason::Session);
        QTest::newRow("ng") << static_cast<int>(DanmakuCommentCancellationReason::Ng);
    }
    void timingRetainsPendingShutdownAndOtherCancellations() {
        QFETCH(int, reason);
        qputenv("NICONEON_RENDER_DIAGNOSTICS", "1");
        DanmakuController controller;
        QCOMPARE(controller.appendComments({comment(1), comment(2), comment(3)}, 0), 3);
        const auto pending = controller.takeCommentTimingDiagnostics();
        QCOMPARE(pending.pending.size(), 3);
        QVERIFY(pending.records.isEmpty());
        QCOMPARE(pending.recordedComments, 0);
        const auto coalesced = controller.rasterMetrics().coalesced;
        QCOMPARE(controller.takeCommentTimingDiagnostics().pending.size(), 3);
        QCOMPARE(controller.rasterMetrics().coalesced, coalesced);
        for (const auto &record : pending.pending) {
            QCOMPARE(record.outcome, DanmakuCommentTimingOutcome::Pending);
            QVERIFY(record.admittedAtNs > 0);
            QCOMPARE(record.resolvedAtNs, 0);
        }
        const auto cancellation = static_cast<DanmakuCommentCancellationReason>(reason);
        switch (cancellation) {
        case DanmakuCommentCancellationReason::Shutdown:
            controller.shutdownRaster();
            break;
        case DanmakuCommentCancellationReason::Seek:
            controller.resetForSeek();
            break;
        case DanmakuCommentCancellationReason::Session:
            controller.resetGlyphSession();
            break;
        case DanmakuCommentCancellationReason::Ng:
            controller.applyNgUserFade("user");
            break;
        case DanmakuCommentCancellationReason::None:
            QFAIL("Unexpected cancellation reason");
        }
        const auto terminal = controller.takeCommentTimingDiagnostics();
        QCOMPARE(terminal.records.size(), 3);
        QCOMPARE(terminal.recordedComments, 3);
        QVERIFY(terminal.pending.isEmpty());
        for (const auto &record : terminal.records) {
            QCOMPARE(record.outcome, DanmakuCommentTimingOutcome::Cancelled);
            QCOMPARE(record.cancellationReason, cancellation);
            QVERIFY(record.resolvedAtNs >= record.admittedAtNs);
        }
        QVERIFY(controller.takeCommentTimingDiagnostics().records.isEmpty());
    }
    void timingLifetimeCapSurvivesDrainsAndSeeks() {
        qputenv("NICONEON_RENDER_DIAGNOSTICS", "1");
        qputenv("NICONEON_DANMAKU_WORKER", "off");
        DanmakuController controller;
        QCOMPARE(controller.appendComments({comment(1, "x")}, 0), 1);
        QTRY_COMPARE(controller.pendingCommentCountForTesting(), 0);
        quint64 totalRecorded = controller.takeCommentTimingDiagnostics().records.size();
        QVariantList input;
        for (int i = 0; i < 256; ++i)
            input.push_back(comment(i, "x"));
        quint64 totalAccepted = 1;
        for (int i = 0; i < DanmakuCommentTimingBatch::CommentCapacity / input.size() + 1; ++i) {
            controller.resetForSeek();
            totalAccepted += controller.appendComments(input, 0);
            if (i % 8 == 0)
                totalRecorded += controller.takeCommentTimingDiagnostics().records.size();
        }
        const auto batch = controller.takeCommentTimingDiagnostics();
        totalRecorded += batch.records.size();
        QCOMPARE(totalRecorded, DanmakuCommentTimingBatch::CommentCapacity);
        QCOMPARE(batch.recordedComments, totalRecorded);
        QCOMPARE(batch.droppedComments, totalAccepted - totalRecorded);
        QVERIFY(batch.droppedComments > 0);
        QVERIFY(batch.pending.isEmpty());
    }
    void timingIncludesImmediateRasterFailure() {
        qputenv("NICONEON_RENDER_DIAGNOSTICS", "1");
        DanmakuController controller;
        QCOMPARE(controller.appendComments({comment(1, QString(16385, 'x'))}, 0), 1);
        const auto batch = controller.takeCommentTimingDiagnostics();
        QCOMPARE(batch.records.size(), 1);
        QCOMPARE(batch.records.first().outcome, DanmakuCommentTimingOutcome::Failed);
        QCOMPARE(batch.records.first().spriteId, 0);
        QCOMPARE(batch.records.first().rasterCompletedAtNs, 0);
        QCOMPARE(batch.records.first().guiReadyAtNs, 0);
    }
    void completionWakesFillMailboxWithoutFrameOrPresentationTicks() {
        DanmakuController controller;
        QVariantList input;
        for (int i = 0; i < 96; ++i)
            input.push_back(comment(i, QStringLiteral("small %1").arg(i)));
        QCOMPARE(controller.appendComments(input, 0), input.size());
        QVERIFY(pumpOnlyQueuedCallbacks(controller, 32));
        QElapsedTimer timer;
        timer.start();
        while (controller.rasterMetrics().rasterized < 65 && timer.elapsed() < 3000)
            QTest::qSleep(1);
        QVERIFY(controller.rasterMetrics().rasterized >= 65); // 32 mailbox + 32 completions + one held.
        for (int i = 0; i < 10; ++i)
            QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        const auto stalled = controller.rasterMetrics();
        QCOMPARE(stalled.wakePending, 0);
        QCOMPARE(stalled.completed, 32);
        QCOMPARE(controller.renderSnapshot()->instances.size(), 32);
        for (int i = 0; i < 20; ++i)
            QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        QCOMPARE(controller.rasterMetrics().wakeNotifications, stalled.wakeNotifications);

        QSet<DanmakuSpriteId> delivered;
        for (int batch = 0; batch < 3; ++batch) {
            QVector<DanmakuSpriteUpload> uploads;
            // The real render consumer calls this entry on the render thread.
            std::thread render([&] { uploads = controller.takePendingSpriteUploads(); });
            render.join();
            QCOMPARE(uploads.size(), 32);
            for (const auto &upload : uploads) {
                QVERIFY(!delivered.contains(upload.spriteId));
                delivered.insert(upload.spriteId);
            }
            if (batch < 2)
                QVERIFY(pumpOnlyQueuedCallbacks(controller, (batch + 2) * 32));
        }
        QCOMPARE(delivered.size(), 96);
        QCOMPARE(controller.pendingCommentCountForTesting(), 0);
        const auto stats = controller.rasterMetrics();
        QVERIFY(stats.wakeStarted >= 12);
        QCOMPARE(stats.wakePendingHighWater, 1);
        QVERIFY(stats.wakeOutstandingHighWater <= 2);
        QVERIFY(stats.wakeMaxCommittedSprites <= 8);
        QVERIFY(stats.highWater <= 128);
        QVERIFY(stats.completionHighWater <= 32);
    }
    void byteBlockedMailboxSleepsUntilRenderConsumption_data() {
        QTest::addColumn<int>("spriteKiB");
        QTest::newRow("partial-byte-budget") << 1280;
        QTest::newRow("oversize-empty-only") << 3072;
    }
    void byteBlockedMailboxSleepsUntilRenderConsumption() {
        QFETCH(int, spriteKiB);
        DanmakuController controller;
        QFont font = QGuiApplication::font();
        font.setPixelSize(DanmakuRenderStyle::kTextPixelSize);
        const int count =
            (spriteKiB * 1024) / (DanmakuRenderStyle::kItemHeightPx * 4) / QFontMetrics(font).horizontalAdvance('W');
        const QString text(count, 'W');
        QCOMPARE(controller.appendComments({comment(1, text), comment(2, text + 'W')}, 0), 2);
        QVERIFY(waitForPublishedCompletions(controller, 2, 2));
        QVERIFY(pumpOnlyQueuedCallbacks(controller, 1));
        for (int i = 0; i < 20; ++i)
            QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        const auto stalled = controller.rasterMetrics();
        QCOMPARE(stalled.completed, 1);
        QCOMPARE(stalled.wakePending, 0);
        for (int i = 0; i < 20; ++i)
            QCoreApplication::sendPostedEvents(&controller, QEvent::MetaCall);
        QCOMPARE(controller.rasterMetrics().wakeNotifications, stalled.wakeNotifications);
        const auto first = controller.takePendingSpriteUploads();
        QCOMPARE(first.size(), 1);
        QVERIFY(first.first().image.sizeInBytes() > 1024 * 1024);
        if (spriteKiB < 2048)
            QVERIFY(first.first().image.sizeInBytes() < 2 * 1024 * 1024);
        else
            QVERIFY(first.first().image.sizeInBytes() > 2 * 1024 * 1024);
        QVERIFY(pumpOnlyQueuedCallbacks(controller, 2));
        QCOMPARE(controller.takePendingSpriteUploads().size(), 1);
        QCOMPARE(controller.pendingCommentCountForTesting(), 0);
    }
    void queuedWakeSurvivesSeekDprAndControllerDestruction() {
        for (int cycle = 0; cycle < 10; ++cycle) {
            auto controller = std::make_unique<DanmakuController>();
            QCOMPARE(controller->appendComments({comment(1)}, 0), 1);
            QVERIFY(waitForPublishedCompletions(*controller, 1, 1));
            QCOMPARE(controller->rasterMetrics().wakePending, 1);
            controller->resetForSeek();
            controller->setRenderDevicePixelRatio(2);
            QCOMPARE(controller->appendComments({comment(2)}, 0), 1);
            QVERIFY(pumpOnlyQueuedCallbacks(*controller, 1));
            QCOMPARE(controller->renderSnapshot()->instances.first().commentId, "id-2");
            const auto uploads = controller->takePendingSpriteUploads();
            QCOMPARE(uploads.size(), 1);
            QCOMPARE(uploads.first().image.devicePixelRatio(), 2);
            const auto before = controller->rasterMetrics();
            QCOMPARE(controller->appendComments({comment(3)}, 0), 1);
            QVERIFY(waitForPublishedCompletions(*controller, 1, before.wakeNotifications + before.wakeCoalesced + 1));
            QCOMPARE(controller->rasterMetrics().wakePending, 1);
            controller.reset();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        }
    }
};
QTEST_MAIN(DanmakuRasterPipelineTest)
#include "danmaku_raster_pipeline_test.moc"
