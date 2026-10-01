#include "../support/MockHttpServer.hpp"
#include "app/CommentService.hpp"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
using namespace niconeon;
class ServiceTest : public QObject {
    Q_OBJECT
  private:
    static ServiceOptions options(MockHttpServer &server) {
        ServiceOptions options;
        options.memoryStore = true;
        options.fetch.watchBaseUrl = server.baseUrl().resolved(QUrl("watch/"));
        options.fetch.allowLoopbackHttp = true;
        return options;
    }
  private slots:
    void networkSessionAndStaleResult() {
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [&server](const QByteArray &request) {
            return MockHttpServer::Reply{request.startsWith("GET") ? server.watch() : server.threads(), 200,
                                         request.contains("sm1?") ? 150 : 0};
        };
        CommentService service(options(server));
        QSignalSpy ready(&service, &CommentService::ready), opened(&service, &CommentService::opened),
            comments(&service, &CommentService::commentsReady);
        QTRY_COMPARE(ready.size(), 1);
        service.openVideo("sm1.mp4", "sm1");
        QTRY_COMPARE(server.requests.size(), 1);
        const auto generation = service.openVideo("sm2.mp4", "sm2");
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(opened[0][0].toULongLong(), generation);
        QCOMPARE(qvariant_cast<CommentSource>(opened[0][1]), CommentSource::Network);
        service.requestTick(150, false, false);
        QTRY_COMPARE(comments.size(), 1);
        QCOMPARE(qvariant_cast<PlaybackBatchResult>(comments[0][0]).emitComments.size(), 1);
        service.shutdown();
        QTRY_VERIFY(service.isStopped());
    }
    void cacheFallbackAndCorruptionIsolation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [](const QByteArray &) { return MockHttpServer::Reply{"{}", 503}; };
        auto config = options(server);
        config.memoryStore = false;
        config.storePaths = StorePaths{dir.filePath("data.db"), dir.filePath("cache.db")};
        {
            auto store = Store::openWithPaths(config.storePaths->dataPath, config.storePaths->cachePath);
            QVERIFY(store);
            QVERIFY((*store)->saveCommentCache("sm9", {{"cached", 100, "user", "cached text"}}));
        }
        CommentService service(config);
        QSignalSpy ready(&service, &CommentService::ready), opened(&service, &CommentService::opened);
        QTRY_COMPARE(ready.size(), 1);
        service.openVideo("sm9.mp4", "sm9");
        QTRY_COMPARE(opened.size(), 1);
        QCOMPARE(qvariant_cast<CommentSource>(opened[0][1]), CommentSource::Cache);
        QCOMPARE(opened[0][2].toLongLong(), 1LL);
        service.openVideo("sm10.mp4", "sm10");
        QTRY_COMPARE(opened.size(), 2);
        QCOMPARE(qvariant_cast<CommentSource>(opened[1][1]), CommentSource::None);
        service.shutdown();
        QTRY_VERIFY(service.isStopped());
    }
    void boundedQueueFilterFailureAndShutdown() {
        ServiceOptions config;
        config.memoryStore = true;
        CommentService service(config);
        QSignalSpy ready(&service, &CommentService::ready), errors(&service, &CommentService::operationFailed),
            ng(&service, &CommentService::ngAdded);
        QTRY_COMPARE(ready.size(), 1);
        QVERIFY(service.addRegexFilter("("));
        QTRY_COMPARE(errors.size(), 1);
        int admitted = 0;
        for (int i = 0; i < 500; ++i)
            if (service.addNgUser(QStringLiteral("user-%1").arg(i)))
                ++admitted;
        QVERIFY(admitted > 0);
        QVERIFY(admitted <= 32);
        QVERIFY(service.queueDepth() <= 32);
        QTRY_COMPARE(ng.size(), admitted);
        QVERIFY(service.highWaterMark() <= 32);
        service.shutdown();
        QTRY_VERIFY(service.isStopped());
        QVERIFY(!service.addNgUser("late"));
    }
    void repeatedStartStop() {
        for (int round = 0; round < 12; ++round) {
            ServiceOptions options;
            options.memoryStore = true;
            CommentService service(options);
            QSignalSpy ready(&service, &CommentService::ready);
            QTRY_COMPARE(ready.size(), 1);
            for (int i = 0; i < 32; ++i)
                service.addNgUser(QString::number(i));
            service.shutdown();
            QTRY_VERIFY(service.isStopped());
        }
    }
};
QTEST_GUILESS_MAIN(ServiceTest)
#include "comment_service_test.moc"
