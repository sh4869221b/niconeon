#pragma once

#include "domain/Domain.hpp"
#include <QNetworkAccessManager>
#include <QPointer>
#include <QTimer>
#include <QUrl>

namespace niconeon {

struct FetchOptions {
    QUrl watchBaseUrl{QStringLiteral("https://www.nicovideo.jp/watch/")};
    QByteArray cookie;
    int timeoutMs = 15000;
    // Only explicit loopback fixtures may use HTTP; production never sends cookies there.
    bool allowLoopbackHttp = false;
};

class NiconicoFetcher : public QObject {
    Q_OBJECT
  public:
    explicit NiconicoFetcher(FetchOptions options = {}, QObject *parent = nullptr);
    void fetch(const QString &videoId, quint64 generation);
    void cancel();
    static Result<CommentList> decodeThreads(const QByteArray &body);

  signals:
    void completed(quint64 generation, niconeon::CommentList comments, const QString &error);

  private:
    void startRequest(QNetworkRequest request, const QByteArray &body, bool post);
    void finishRequest();
    void fail(const QString &error);
    bool permittedServer(const QUrl &url) const;
    FetchOptions m_options;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTimer m_deadline;
    QByteArray m_body;
    quint64 m_generation = 0;
    bool m_threadsStage = false;
};
} // namespace niconeon
