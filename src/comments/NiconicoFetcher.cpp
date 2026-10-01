#include "comments/NiconicoFetcher.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QThread>
#include <cmath>
#include <limits>

namespace niconeon {
namespace {
constexpr qsizetype maxBodyBytes = 64 * 1024 * 1024;
Result<QJsonObject> objectDocument(const QByteArray &body) {
    if (body.size() > maxBodyBytes)
        return std::unexpected(AppError{QStringLiteral("response exceeds 64 MiB limit")});
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return std::unexpected(AppError{QStringLiteral("invalid JSON response at byte %1").arg(error.offset)});
    return doc.object();
}
} // namespace

NiconicoFetcher::NiconicoFetcher(FetchOptions options, QObject *parent)
    : QObject(parent), m_options(std::move(options)), m_network(this), m_deadline(this) {
    m_deadline.setSingleShot(true);
    connect(&m_deadline, &QTimer::timeout, this, [this] { fail(QStringLiteral("comment request timed out")); });
}

bool NiconicoFetcher::permittedServer(const QUrl &url) const {
    if (!url.isValid() || !url.userInfo().isEmpty() || url.hasFragment())
        return false;
    const auto host = url.host().toLower();
    if (m_options.allowLoopbackHttp && url.scheme() == QStringLiteral("http") &&
        (host == QStringLiteral("127.0.0.1") || host == QStringLiteral("localhost") || host == QStringLiteral("::1")))
        return true;
    return url.scheme() == QStringLiteral("https") &&
           (host == QStringLiteral("nicovideo.jp") || host.endsWith(QStringLiteral(".nicovideo.jp")));
}

void NiconicoFetcher::fetch(const QString &videoId, quint64 generation) {
    Q_ASSERT(thread() == QThread::currentThread());
    cancel();
    m_generation = generation;
    m_threadsStage = false;
    static const QRegularExpression validId(QStringLiteral("^(sm|nm|so)[0-9]+$"));
    if (!validId.match(videoId).hasMatch()) {
        fail(QStringLiteral("invalid video ID"));
        return;
    }
    auto url = m_options.watchBaseUrl.resolved(QUrl(videoId));
    url.setQuery(QStringLiteral("responseType=json"));
    if (!permittedServer(url)) {
        fail(QStringLiteral("untrusted watch endpoint"));
        return;
    }
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Niconeon)");
    startRequest(request, {}, false);
}

void NiconicoFetcher::cancel() {
    m_deadline.stop();
    if (m_reply) {
        auto *reply = m_reply.data();
        m_reply.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
    m_body.clear();
}

void NiconicoFetcher::fail(const QString &error) {
    const auto generation = m_generation;
    cancel();
    emit completed(generation, {}, error);
}

void NiconicoFetcher::startRequest(QNetworkRequest request, const QByteArray &body, bool post) {
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(std::clamp(m_options.timeoutMs, 1, 60000));
    m_body.clear();
    m_reply = post ? m_network.post(request, body) : m_network.get(request);
    m_reply->setReadBufferSize(1024 * 1024);
    connect(m_reply, &QNetworkReply::readyRead, this, [this] {
        if (!m_reply)
            return;
        const auto available = m_reply->bytesAvailable();
        if (available > maxBodyBytes - m_body.size()) {
            fail(QStringLiteral("comment response exceeds 64 MiB limit"));
            return;
        }
        m_body += m_reply->readAll();
    });
    connect(m_reply, &QNetworkReply::finished, this, &NiconicoFetcher::finishRequest);
    m_deadline.start(std::clamp(m_options.timeoutMs, 1, 60000));
}

void NiconicoFetcher::finishRequest() {
    if (!m_reply)
        return;
    auto *reply = m_reply.data();
    m_reply.clear();
    m_deadline.stop();
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status != 200) {
        // Do not log URLs, cookies or response bodies containing account/session data.
        fail(QStringLiteral("%1 request failed (HTTP %2, network %3)")
                 .arg(m_threadsStage ? QStringLiteral("comments") : QStringLiteral("watch"))
                 .arg(status)
                 .arg(static_cast<int>(reply->error())));
        return;
    }
    if (reply->bytesAvailable() > maxBodyBytes - m_body.size()) {
        fail(QStringLiteral("comment response exceeds 64 MiB limit"));
        return;
    }
    m_body += reply->readAll();
    if (m_threadsStage) {
        auto comments = decodeThreads(m_body);
        m_body.clear();
        if (!comments) {
            fail(comments.error().message);
            return;
        }
        emit completed(m_generation, std::move(*comments), {});
        return;
    }
    auto document = objectDocument(m_body);
    if (!document) {
        fail(document.error().message);
        return;
    }
    const auto nv = document->value("data")
                        .toObject()
                        .value("response")
                        .toObject()
                        .value("comment")
                        .toObject()
                        .value("nvComment")
                        .toObject();
    const auto params = nv.value("params").toObject();
    if (!nv.value("server").isString() || !nv.value("threadKey").isString() || !params.value("targets").isArray() ||
        !params.value("language").isString()) {
        fail(QStringLiteral("nvComment response is missing required fields"));
        return;
    }
    const auto targets = params.value("targets").toArray();
    if (targets.size() > 1000) {
        fail(QStringLiteral("too many comment thread targets"));
        return;
    }
    for (const auto target : targets) {
        const auto object = target.toObject();
        if (!object.value("id").isString() || !object.value("fork").isString()) {
            fail(QStringLiteral("invalid comment thread target"));
            return;
        }
    }
    QUrl endpoint(nv.value("server").toString());
    if (!permittedServer(endpoint)) {
        fail(QStringLiteral("untrusted comment endpoint"));
        return;
    }
    endpoint.setPath(endpoint.path().remove(QRegularExpression(QStringLiteral("/+$"))) + QStringLiteral("/v1/threads"));
    endpoint.setQuery(QString{});
    QNetworkRequest request(endpoint);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Origin", "https://www.nicovideo.jp");
    request.setRawHeader("Referer", "https://www.nicovideo.jp/");
    request.setRawHeader("X-Frontend-Id", "6");
    request.setRawHeader("X-Frontend-Version", "0");
    request.setRawHeader("X-Niconico-Language", "ja-jp");
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Niconeon)");
    if (!m_options.cookie.isEmpty() && endpoint.scheme() == QStringLiteral("https")) {
        if (std::any_of(m_options.cookie.begin(), m_options.cookie.end(),
                        [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; })) {
            fail(QStringLiteral("invalid cookie header"));
            return;
        }
        request.setRawHeader("Cookie", m_options.cookie);
    }
    const QJsonObject body{{"threadKey", nv.value("threadKey")}, {"params", params}, {"additionals", QJsonObject{}}};
    m_threadsStage = true;
    startRequest(request, QJsonDocument(body).toJson(QJsonDocument::Compact), true);
}

Result<CommentList> NiconicoFetcher::decodeThreads(const QByteArray &body) {
    auto document = objectDocument(body);
    if (!document)
        return std::unexpected(document.error());
    if (document->value("meta").toObject().value("status").toInt() != 200)
        return std::unexpected(AppError{QStringLiteral("nvcomment meta status is not 200")});
    const auto threadsValue = document->value("data").toObject().value("threads");
    if (!threadsValue.isArray())
        return std::unexpected(AppError{QStringLiteral("missing comment threads")});
    CommentList comments;
    for (const auto thread : threadsValue.toArray()) {
        if (!thread.toObject().value("comments").isArray())
            return std::unexpected(AppError{QStringLiteral("invalid comments array")});
        for (const auto value : thread.toObject().value("comments").toArray()) {
            if (comments.size() >= 250000)
                return std::unexpected(AppError{QStringLiteral("comment count exceeds 250000 limit")});
            const auto object = value.toObject();
            const auto at = object.value("vposMs");
            const double timestamp = at.toDouble();
            if (!object.value("id").isString() || !object.value("body").isString() || !at.isDouble() ||
                !std::isfinite(timestamp) || std::floor(timestamp) != timestamp ||
                (timestamp < static_cast<double>(std::numeric_limits<qint64>::min()) ||
                 (timestamp >= -static_cast<double>(std::numeric_limits<qint64>::min()) &&
                  at.toInteger() != std::numeric_limits<qint64>::max())) ||
                (!object.value("userId").isUndefined() && !object.value("userId").isNull() &&
                 !object.value("userId").isString()))
                return std::unexpected(AppError{QStringLiteral("invalid comment record")});
            CommentEvent comment;
            comment.commentId = object.value("id").toString();
            comment.atMs = at.toInteger();
            comment.userId =
                object.value("userId").isString() ? object.value("userId").toString() : QStringLiteral("anonymous");
            comment.text = object.value("body").toString();
            if (comment.text.toUtf8().size() > 16 * 1024 || comment.userId.toUtf8().size() > 1024 ||
                comment.commentId.toUtf8().size() > 1024)
                return std::unexpected(AppError{QStringLiteral("comment field exceeds safety limit")});
            comments.push_back(std::move(comment));
        }
    }
    auto valid = validateComments(comments);
    if (!valid)
        return std::unexpected(valid.error());
    std::stable_sort(comments.begin(), comments.end(), [](const auto &a, const auto &b) { return a.atMs < b.atMs; });
    return comments;
}
} // namespace niconeon
