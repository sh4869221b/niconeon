#include "app/ApplicationController.hpp"
#include "playback/MpvItem.hpp"
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <limits>
using namespace niconeon;
class ControllerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("NiconeonTest");
        QCoreApplication::setApplicationName("ControllerTest");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        qputenv("NICONEON_DANMAKU_WORKER", "off");
    }
    void ratesAndFileUrls() {
        QCOMPARE(
            ApplicationController::normalizedPresets({3.5, 1.5, 1.5, 0.1, std::numeric_limits<double>::quiet_NaN()}),
            QVector<double>({0.5, 1.5, 3.0}));
        QCOMPARE(ApplicationController::normalizedPresets({}), QVector<double>({1.0, 1.5, 2.0}));
        QCOMPARE(ApplicationController::localPath("file:///tmp/hello%20%E6%97%A5.mp4"),
                 QStringLiteral("/tmp/hello 日.mp4"));
        QCOMPARE(ApplicationController::localPath(" /tmp/hello.mp4 "), QStringLiteral("/tmp/hello.mp4"));
    }
    void failedSeekRecoversAndStaleFailureCannotCancelNewSeek() {
        ServiceOptions options;
        options.memoryStore = true;
        ApplicationController controller(options);
        MpvItem player;
        controller.attachPlayer(&player);
        controller.seek(0);
        QVERIFY(controller.seekPending());
        controller.seek(0);
        QVERIFY(controller.seekPending());
        emit player.seekFailed(1); // First request must not cancel the second.
        QVERIFY(controller.seekPending());
        emit player.seekFailed(2);
        QVERIFY(!controller.seekPending());
        controller.seek(0); // With no media loaded libmpv actually rejects this.
        QVERIFY(controller.seekPending());
        QTRY_VERIFY(!controller.seekPending());
        controller.shutdown();
    }

    void seekReconciliationTimerIsSingleAndClearsPendingState() {
        ServiceOptions options;
        options.memoryStore = true;
        ApplicationController controller(options);
        MpvItem player;
        controller.attachPlayer(&player);
        auto *timer = controller.findChild<QTimer *>(QStringLiteral("seekReconciliationTimer"));
        QVERIFY(timer);
        for (int index = 0; index < 20; ++index)
            controller.seek(0);
        QVERIFY(controller.seekPending());
        QVERIFY(timer->isActive());
        QCOMPARE(timer->interval(), 5000);
        QVERIFY(timer->isSingleShot());
        QCOMPARE(controller.findChildren<QTimer *>(QStringLiteral("seekReconciliationTimer")).size(), 1);
        // Deterministically simulate expiry before the media event loop runs.
        QVERIFY(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection));
        QVERIFY(!controller.seekPending());
        QVERIFY(!timer->isActive());
        controller.seek(0);
        QVERIFY(timer->isActive());
        controller.shutdown();
        controller.seek(0);
        QVERIFY(!controller.seekPending());
        QVERIFY(!timer->isActive());
    }

    void pausedSeekBatchUsesItsOwnClockBeforePlayerCatchesUp() {
        ServiceOptions options;
        options.memoryStore = true;
        ApplicationController controller(options);
        MpvItem player;
        controller.attachPlayer(&player);
        controller.setCommentsVisible(true);
        controller.danmaku()->setViewportSize(1280, 720);
        player.setPaused(true);
        controller.danmaku()->setPlaybackPaused(true);
        auto *service = controller.findChild<CommentService *>();
        QVERIFY(service);
        QCOMPARE(player.positionMs(), 0);
        PlaybackBatchResult batch;
        batch.lastPositionMs = 15000;
        batch.processedTicks = 1;
        batch.emitComments = {
            {QStringLiteral("restored"), 10000, QStringLiteral("u"), QStringLiteral("visible while paused")}};
        emit service->commentsReady(batch);
        QTRY_VERIFY(controller.danmaku()->renderSnapshot() &&
                    !controller.danmaku()->renderSnapshot()->instances.isEmpty());
        const auto snapshot = controller.danmaku()->renderSnapshot();
        QCOMPARE(snapshot->instances.size(), 1);
        const auto &item = snapshot->instances.first();
        QVERIFY(item.x < 1280);
        QVERIFY(item.x > 0);
        QCOMPARE(player.positionMs(), 0);
        QVERIFY(player.paused());
        QVERIFY(controller.danmaku()->playbackPaused());
        controller.shutdown();
    }

    void successfulNgRemovesUnadmittedSourceBatchRows() {
        ServiceOptions options;
        options.memoryStore = true;
        ApplicationController controller(options);
        controller.setCommentsVisible(true);
        controller.danmaku()->setPlaybackPaused(true);
        auto *service = controller.findChild<CommentService *>();
        QVERIFY(service);
        PlaybackBatchResult batch;
        batch.lastPositionMs = 0;
        for (int i = 0; i < 400; ++i)
            batch.emitComments.push_back({QString::number(i), 0, "blocked", QStringLiteral("pending %1").arg(i)});
        emit service->commentsReady(batch);
        QTest::qWait(120); // No render consumer: the source cursor is backpressured.
        QVERIFY(controller.danmaku()->pendingCommentCountForTesting() > 0);
        QSet<QString> alreadyVisible;
        for (const auto &item : controller.danmaku()->renderSnapshot()->instances)
            alreadyVisible.insert(item.commentId);
        emit service->ngAdded(AddNgUserResult{true, "token", "blocked"});
        QCOMPARE(controller.danmaku()->pendingCommentCountForTesting(), 0);
        PlaybackBatchResult next;
        next.emitComments = {{"allowed", 0, "other-user", "still visible"}};
        emit service->commentsReady(next);
        QElapsedTimer timer;
        timer.start();
        bool sawAllowed = false;
        while (timer.elapsed() < 700) {
            controller.danmaku()->takePendingSpriteUploads();
            QTest::qWait(10);
            for (const auto &item : controller.danmaku()->renderSnapshot()->instances) {
                if (item.commentId == "allowed")
                    sawAllowed = true;
                else
                    QVERIFY(alreadyVisible.contains(item.commentId));
            }
        }
        QVERIFY(sawAllowed);
        QCOMPARE(controller.danmaku()->renderSnapshot()->instances.size(), 1);
        controller.shutdown();
    }
    void shutdownWaitsForBothOwnersAndNotifiesOnlyOnce() {
        for (int cycle = 0; cycle < 5; ++cycle) {
            ServiceOptions options;
            options.memoryStore = true;
            ApplicationController controller(options);
            QSignalSpy stopped(&controller, &ApplicationController::readyToQuit);
            QVariantList input;
            for (int i = 0; i < 128; ++i)
                input.push_back(
                    QVariantMap{{"comment_id", QString::number(i)}, {"text", QStringLiteral("終了 %1").arg(i)}});
            controller.danmaku()->appendComments(input, 0);
            controller.shutdown();
            QTRY_COMPARE(stopped.size(), 1);
            QVERIFY(controller.danmaku()->rasterStopped());
            QVERIFY(controller.findChild<CommentService *>()->isStopped());
            controller.shutdown();
            QTest::qWait(60); // Include any queued thread-finished notification.
            QCOMPARE(stopped.size(), 1);
        }
    }
    void settingsAndCommands() {
        ServiceOptions options;
        options.memoryStore = true;
        ApplicationController controller(options);
        QSignalSpy stopped(&controller, &ApplicationController::readyToQuit);
        controller.addSpeedPreset("1.75");
        QVERIFY(controller.speedPresets().contains(1.75));
        controller.removeSpeedPreset(1.75);
        QVERIFY(!controller.speedPresets().contains(1.75));
        controller.setCommentsVisible(false);
        QVERIFY(!controller.commentsVisible());
        controller.setCommentsVisible(true);
        QVERIFY(controller.commentsVisible());
        controller.setFontSizeLevel(20);
        QCOMPARE(controller.fontSizeLevel(), 2);
        QCOMPARE(controller.appFontPixelSize(), 16);
        controller.setFontSizeLevel(-20);
        QCOMPARE(controller.fontSizeLevel(), 0);
        controller.setPerfLogEnabled(true);
        QVERIFY(controller.perfLogEnabled());
        const auto oldProfile = controller.perfProfile();
        controller.cycleRuntimeProfile();
        QVERIFY(controller.perfProfile() != oldProfile);
        controller.shutdown();
        QTRY_COMPARE(stopped.size(), 1);
    }
};
QTEST_MAIN(ControllerTest)
#include "application_controller_test.moc"
