#include "../support/MockHttpServer.hpp"
#include "comments/NiconicoFetcher.hpp"
#include <QSignalSpy>
#include <QTest>
using namespace niconeon;
class FetcherTest : public QObject {
    Q_OBJECT
  private slots:
    void decodeAndValidate() {
        auto result = NiconicoFetcher::decodeThreads(MockHttpServer::threads());
        QVERIFY(result);
        QCOMPARE(result->size(), 1);
        QCOMPARE(result->first().text, QStringLiteral("fixture 日本語"));
        QVERIFY(!NiconicoFetcher::decodeThreads("not json"));
        QVERIFY(!NiconicoFetcher::decodeThreads(R"({"meta":{"status":403},"data":{"threads":[]}})"));
        QVERIFY(!NiconicoFetcher::decodeThreads(
            R"({"meta":{"status":200},"data":{"threads":[{"comments":[{"id":"x","vposMs":1.25,"body":"x"}]}]}})"));
        auto anonymous = NiconicoFetcher::decodeThreads(
            R"({"meta":{"status":200},"data":{"threads":[{"comments":[{"id":"x","vposMs":0,"body":"x"}]}]}})");
        QVERIFY(anonymous);
        QCOMPARE(anonymous->first().userId, QStringLiteral("anonymous"));
        QVERIFY(!NiconicoFetcher::decodeThreads(MockHttpServer::threads(QString(17000, 'x'))));
    }
    void twoStageHttp() {
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [&server](const QByteArray &request) {
            return MockHttpServer::Reply{request.startsWith("GET") ? server.watch() : server.threads()};
        };
        FetchOptions options;
        options.watchBaseUrl = server.baseUrl().resolved(QUrl("watch/"));
        options.allowLoopbackHttp = true;
        options.cookie = "private-fixture-cookie";
        NiconicoFetcher fetcher(options);
        QSignalSpy completed(&fetcher, &NiconicoFetcher::completed);
        fetcher.fetch("sm9", 4);
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(completed[0][0].toULongLong(), 4ULL);
        QVERIFY(completed[0][2].toString().isEmpty());
        QCOMPARE(qvariant_cast<CommentList>(completed[0][1]).size(), 1);
        QCOMPARE(server.requests.size(), 2);
        QVERIFY(server.requests[0].startsWith("GET /watch/sm9?responseType=json"));
        QVERIFY(server.requests[1].startsWith("POST /v1/threads"));
        QVERIFY(server.requests[1].contains("fixture-key"));
        QVERIFY(!server.requests[1].contains("private-fixture-cookie"));
    }
    void timeoutAndCancellation() {
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [](const QByteArray &) { return MockHttpServer::Reply{"{}", 200, 300}; };
        FetchOptions options;
        options.watchBaseUrl = server.baseUrl();
        options.allowLoopbackHttp = true;
        options.timeoutMs = 30;
        NiconicoFetcher fetcher(options);
        QSignalSpy completed(&fetcher, &NiconicoFetcher::completed);
        fetcher.fetch("sm9", 1);
        QTRY_COMPARE(completed.size(), 1);
        QVERIFY(!completed[0][2].toString().isEmpty());
        fetcher.fetch("sm9", 2);
        fetcher.cancel();
        QTest::qWait(100);
        QCOMPARE(completed.size(), 1);
    }
    void replacementAndUntrustedServer() {
        MockHttpServer server;
        QVERIFY(server.start());
        server.handler = [&server](const QByteArray &request) {
            return MockHttpServer::Reply{request.startsWith("GET") ? server.watch() : server.threads(), 200,
                                         request.contains("sm1?") ? 200 : 0};
        };
        FetchOptions options;
        options.watchBaseUrl = server.baseUrl();
        options.allowLoopbackHttp = true;
        NiconicoFetcher fetcher(options);
        QSignalSpy completed(&fetcher, &NiconicoFetcher::completed);
        fetcher.fetch("sm1", 1);
        QTRY_COMPARE(server.requests.size(), 1);
        fetcher.fetch("sm2", 2);
        QTRY_COMPARE(completed.size(), 1);
        QCOMPARE(completed[0][0].toULongLong(), 2ULL);
        server.handler = [&server](const QByteArray &) {
            return MockHttpServer::Reply{
                server.watch().replace(server.baseUrl().toString().toUtf8(), "https://untrusted.invalid/")};
        };
        fetcher.fetch("sm3", 3);
        QTRY_COMPARE(completed.size(), 2);
        QVERIFY(completed[1][2].toString().contains("untrusted"));
    }
};
QTEST_GUILESS_MAIN(FetcherTest)
#include "niconico_fetcher_test.moc"
