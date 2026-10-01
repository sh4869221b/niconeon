#pragma once
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <functional>
#include <memory>

class MockHttpServer : public QTcpServer {
  public:
    struct Reply {
        QByteArray body;
        int status = 200;
        int delayMs = 0;
    };
    std::function<Reply(const QByteArray &)> handler;
    QList<QByteArray> requests;
    explicit MockHttpServer(QObject *parent = nullptr) : QTcpServer(parent) {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                auto bytes = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, bytes] {
                    bytes->append(socket->readAll());
                    const auto split = bytes->indexOf("\r\n\r\n");
                    if (split < 0)
                        return;
                    qsizetype length = 0;
                    for (const auto &line : bytes->left(split).split('\n'))
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(15).trimmed().toLongLong();
                    if (bytes->size() < split + 4 + length)
                        return;
                    disconnect(socket, &QTcpSocket::readyRead, socket, nullptr);
                    requests.push_back(*bytes);
                    const Reply reply = handler ? handler(*bytes) : Reply{"{}", 500, 0};
                    QPointer<QTcpSocket> guard(socket);
                    QTimer::singleShot(reply.delayMs, socket, [guard, reply] {
                        if (!guard)
                            return;
                        guard->write("HTTP/1.1 " + QByteArray::number(reply.status) +
                                     " Test\r\nContent-Type: application/json\r\nContent-Length: " +
                                     QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n\r\n" +
                                     reply.body);
                        guard->disconnectFromHost();
                    });
                });
            }
        });
    }
    bool start() {
        return listen(QHostAddress::LocalHost, 0);
    }
    QUrl baseUrl() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(serverPort()));
    }
    QByteArray watch() const {
        return QJsonDocument(
                   QJsonObject{
                       {"data",
                        QJsonObject{
                            {"response",
                             QJsonObject{
                                 {"comment",
                                  QJsonObject{{"nvComment",
                                               QJsonObject{{"server", baseUrl().toString()},
                                                           {"threadKey", "fixture-key"},
                                                           {"params",
                                                            QJsonObject{{"language", "ja-jp"},
                                                                        {"targets", QJsonArray{QJsonObject{
                                                                                        {"id", "9"},
                                                                                        {"fork", "main"}}}}}}}}}}}}}}})
            .toJson(QJsonDocument::Compact);
    }
    static QByteArray threads(const QString &text = QStringLiteral("fixture 日本語")) {
        return QJsonDocument(
                   QJsonObject{
                       {"meta", QJsonObject{{"status", 200}}},
                       {"data",
                        QJsonObject{{"threads",
                                     QJsonArray{QJsonObject{
                                         {"comments",
                                          QJsonArray{QJsonObject{
                                              {"id", "c1"}, {"vposMs", 100}, {"body", text}, {"userId", "u1"}}}}}}}}}})
            .toJson(QJsonDocument::Compact);
    }
};
