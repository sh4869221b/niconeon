#include "../support/MockHttpServer.hpp"
#include "app/CommentService.hpp"

#include <QSignalSpy>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace niconeon;

namespace {
ServiceOptions serviceOptions(MockHttpServer &server) {
    ServiceOptions options;
    options.memoryStore = true;
    options.fetch.watchBaseUrl = server.baseUrl().resolved(QUrl(QStringLiteral("watch/")));
    options.fetch.allowLoopbackHttp = true;
    return options;
}

QByteArray seekComments() {
    return R"({"meta":{"status":200},"data":{"threads":[{"comments":[{"id":"old","vposMs":100,"body":"in flight","userId":"u1"},{"id":"exact","vposMs":600,"body":"exact","userId":"u2"}]}]}})";
}

bool corruptCache(const QString &path) {
    const auto name = QUuid::createUuid().toString();
    bool succeeded = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(path);
        if (database.open()) {
            QSqlQuery query(database);
            succeeded =
                query.exec(QStringLiteral("UPDATE comment_cache SET payload_json='{not-json}' WHERE video_id='sm9'"));
        }
    }
    QSqlDatabase::removeDatabase(name);
    return succeeded;
}
} // namespace

class ServiceRegressionTest : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() {
        qunsetenv("NICONEON_SYNTHETIC_COMMENTS");
        qunsetenv("NICONEON_AUTO_VIDEO_PATH");
        qunsetenv("NICONEON_AUTO_PERF_LOG");
    }
    void latestOpenSurvivesFullIngressQueue() {
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [&server](const QByteArray &request) {
            return MockHttpServer::Reply{request.startsWith("GET") ? server.watch() : server.threads()};
        };
        CommentService service(serviceOptions(server));
        QSignalSpy ready(&service, &CommentService::ready), opened(&service, &CommentService::opened);
        QTRY_COMPARE(ready.size(), 1);
        // GUI acknowledgements cannot run during this loop, so the admission queue fills
        // deterministically even if the worker processes those commands very quickly.
        for (int index = 0; index < 32; ++index)
            QVERIFY(service.listFilters());
        QCOMPARE(service.queueDepth(), 32);
        service.openVideo(QStringLiteral("sm1.mp4"), QStringLiteral("sm1"));
        const auto latest = service.openVideo(QStringLiteral("sm2.mp4"), QStringLiteral("sm2"));
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(opened.first()[0].toULongLong(), latest);
        QCOMPARE(qvariant_cast<CommentSource>(opened.first()[1]), CommentSource::Network);
        QVERIFY(service.highWaterMark() <= 32);
        QVERIFY(std::any_of(server.requests.cbegin(), server.requests.cend(),
                            [](const auto &request) { return request.startsWith("GET /watch/sm2?"); }));
        QVERIFY(std::none_of(server.requests.cbegin(), server.requests.cend(),
                             [](const auto &request) { return request.startsWith("GET /watch/sm1?"); }));
        service.shutdown();
        QTRY_VERIFY(service.isStopped());
    }

    void noVideoIdStillProcessesPlaybackTicks() {
        ServiceOptions options;
        options.memoryStore = true;
        CommentService service(options);
        QSignalSpy ready(&service, &CommentService::ready), opened(&service, &CommentService::opened),
            comments(&service, &CommentService::commentsReady);
        QTRY_COMPARE(ready.size(), 1);
        service.openVideo(QStringLiteral("ordinary-local-video.mp4"), {});
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(qvariant_cast<CommentSource>(opened.first()[1]), CommentSource::None);
        service.requestTick(750, false, false);
        QTRY_COMPARE(comments.size(), 1);
        const auto result = qvariant_cast<PlaybackBatchResult>(comments.first()[0]);
        QCOMPARE(result.processedTicks, 1);
        QCOMPARE(result.lastPositionMs, 750);
        QVERIFY(result.emitComments.isEmpty());
        service.shutdown();
        QTRY_VERIFY(service.isStopped());
    }

    void pendingSeekSurvivesTickCoalescingAndRejectsOldResult() {
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [&server](const QByteArray &request) {
            return MockHttpServer::Reply{request.startsWith("GET") ? server.watch() : seekComments()};
        };
        CommentService service(serviceOptions(server));
        QSignalSpy ready(&service, &CommentService::ready), opened(&service, &CommentService::opened),
            comments(&service, &CommentService::commentsReady);
        QTRY_COMPARE(ready.size(), 1);
        service.openVideo(QStringLiteral("sm9.mp4"), QStringLiteral("sm9"));
        QTRY_COMPARE(opened.size(), 1);
        service.requestTick(150, false, false);
        service.requestTick(500, false, true);
        service.requestTick(600, false, false);
        QTRY_COMPARE(comments.size(), 1);
        auto result = qvariant_cast<PlaybackBatchResult>(comments.first()[0]);
        QCOMPARE(result.emitComments.size(), 1);
        QCOMPARE(result.emitComments.first().commentId, QStringLiteral("old"));
        QCOMPARE(result.lastPositionMs, 599);
        QVERIFY(service.coalescedTicks() >= 1);
        QVERIFY(service.staleResults() >= 1);
        service.requestTick(600, false, false);
        QTRY_COMPARE(comments.size(), 2);
        result = qvariant_cast<PlaybackBatchResult>(comments.last()[0]);
        QCOMPARE(result.emitComments.size(), 1);
        QCOMPARE(result.emitComments.first().commentId, QStringLiteral("exact"));
        service.shutdown();
        QTRY_VERIFY(service.isStopped());
    }

    void replacingDelayedSecondStageNeverCommitsOldSessionOrCache() {
        QTemporaryDir directory;
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [&server](const QByteArray &request) {
            if (request.startsWith("GET")) {
                const auto key = request.contains("sm1?") ? QByteArray("old-key") : QByteArray("new-key");
                return MockHttpServer::Reply{server.watch().replace("fixture-key", key)};
            }
            const bool old = request.contains("old-key");
            return MockHttpServer::Reply{server.threads(old ? QStringLiteral("old") : QStringLiteral("latest")), 200,
                                         old ? 250 : 0};
        };
        auto options = serviceOptions(server);
        options.memoryStore = false;
        options.storePaths =
            StorePaths{directory.filePath(QStringLiteral("data.db")), directory.filePath(QStringLiteral("cache.db"))};
        CommentService service(options);
        QSignalSpy ready(&service, &CommentService::ready), opened(&service, &CommentService::opened),
            comments(&service, &CommentService::commentsReady);
        QTRY_COMPARE(ready.size(), 1);
        service.openVideo(QStringLiteral("sm1.mp4"), QStringLiteral("sm1"));
        QTRY_VERIFY(std::any_of(server.requests.cbegin(), server.requests.cend(), [](const auto &request) {
            return request.startsWith("POST") && request.contains("old-key");
        }));
        const auto latest = service.openVideo(QStringLiteral("sm2.mp4"), QStringLiteral("sm2"));
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(opened.first()[0].toULongLong(), latest);
        QTest::qWait(300);
        QCOMPARE(opened.size(), 1);
        service.requestTick(200, false, false);
        QTRY_COMPARE(comments.size(), 1);
        QCOMPARE(qvariant_cast<PlaybackBatchResult>(comments.first()[0]).emitComments.first().text,
                 QStringLiteral("latest"));
        service.shutdown();
        QTRY_VERIFY(service.isStopped());
        auto store = Store::openWithPaths(options.storePaths->dataPath, options.storePaths->cachePath);
        QVERIFY(store);
        auto old = (*store)->loadCommentCache(QStringLiteral("sm1"));
        QVERIFY(old && !old->has_value());
        auto current = (*store)->loadCommentCache(QStringLiteral("sm2"));
        QVERIFY(current && current->has_value());
    }

    void malformedStoredCacheFallsBackToEmptySession() {
        QTemporaryDir directory;
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [](const QByteArray &) { return MockHttpServer::Reply{"{}", 503}; };
        auto options = serviceOptions(server);
        options.memoryStore = false;
        options.storePaths =
            StorePaths{directory.filePath(QStringLiteral("data.db")), directory.filePath(QStringLiteral("cache.db"))};
        {
            auto store = Store::openWithPaths(options.storePaths->dataPath, options.storePaths->cachePath);
            QVERIFY(store);
            QVERIFY((*store)->saveCommentCache(
                QStringLiteral("sm9"), {{QStringLiteral("c"), 0, QStringLiteral("u"), QStringLiteral("cached")}}));
        }
        QVERIFY(corruptCache(options.storePaths->cachePath));
        CommentService service(options);
        QSignalSpy ready(&service, &CommentService::ready), opened(&service, &CommentService::opened),
            warnings(&service, &CommentService::warning), comments(&service, &CommentService::commentsReady);
        QTRY_COMPARE(ready.size(), 1);
        service.openVideo(QStringLiteral("sm9.mp4"), QStringLiteral("sm9"));
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(qvariant_cast<CommentSource>(opened.first()[1]), CommentSource::None);
        QCOMPARE(opened.first()[2].toLongLong(), 0);
        QVERIFY(std::any_of(warnings.cbegin(), warnings.cend(), [](const auto &warning) {
            return warning.first().toString().contains(QStringLiteral("decode comment cache"));
        }));
        service.requestTick(900, false, false);
        QTRY_COMPARE(comments.size(), 1);
        QCOMPARE(qvariant_cast<PlaybackBatchResult>(comments.first()[0]).lastPositionMs, 900);
        service.shutdown();
        QTRY_VERIFY(service.isStopped());
    }

    void shutdownDuringNetworkAndQueuedMutations() {
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [&server](const QByteArray &) { return MockHttpServer::Reply{server.watch(), 200, 500}; };
        CommentService service(serviceOptions(server));
        QSignalSpy ready(&service, &CommentService::ready), opened(&service, &CommentService::opened),
            comments(&service, &CommentService::commentsReady);
        QTRY_COMPARE(ready.size(), 1);
        service.openVideo(QStringLiteral("sm9.mp4"), QStringLiteral("sm9"));
        QTRY_COMPARE(server.requests.size(), 1);
        for (int index = 0; index < 32; ++index)
            service.addNgUser(QString::number(index));
        service.shutdown();
        service.requestTick(500, false, true);
        QVERIFY(!service.addNgUser(QStringLiteral("after-shutdown")));
        QTRY_VERIFY_WITH_TIMEOUT(service.isStopped(), 3000);
        QTest::qWait(550);
        QVERIFY(opened.isEmpty());
        QVERIFY(comments.isEmpty());
    }

    void immediateShutdownBeforeReadyAndDestructionAreSafe() {
        for (int round = 0; round < 12; ++round) {
            ServiceOptions options;
            options.memoryStore = true;
            CommentService service(options);
            service.shutdown();
            QTRY_VERIFY_WITH_TIMEOUT(service.isStopped(), 3000);
        }
    }
};

QTEST_GUILESS_MAIN(ServiceRegressionTest)
#include "service_regression_test.moc"
