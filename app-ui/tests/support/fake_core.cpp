#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTimer>

#include <cstdio>
#include <iostream>
#include <thread>

namespace {

QSet<QString> scenarioFlags() {
    const QString scenario = qEnvironmentVariable("NICONEON_FAKE_CORE_SCENARIO");
    QSet<QString> flags;
    for (const QString &entry : scenario.split(',', Qt::SkipEmptyParts)) {
        const QString token = entry.trimmed();
        if (!token.isEmpty()) {
            flags.insert(token);
        }
    }
    return flags;
}

void writeJsonLine(FILE *stream, const QJsonObject &object) {
    const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

void writeTextLine(FILE *stream, const QByteArray &line) {
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}

qint64 lastPositionMs(const QJsonObject &params) {
    const QJsonArray ticks = params.value(QStringLiteral("ticks")).toArray();
    if (ticks.isEmpty()) {
        return 0;
    }
    return ticks.last().toObject().value(QStringLiteral("position_ms")).toInteger(0);
}

} // namespace

class FakeCore : public QObject {
public:
    FakeCore()
        : m_flags(scenarioFlags()),
          m_delayMs(qEnvironmentVariableIntValue("NICONEON_FAKE_CORE_DELAY_MS")) {}

    void start() {
        if (m_flags.contains(QStringLiteral("stderr_on_start"))) {
            writeTextLine(stderr, "fake core warning");
        }

        std::thread([this]() {
            std::string line;
            while (std::getline(std::cin, line)) {
                QMetaObject::invokeMethod(
                    this,
                    [this, payload = QString::fromUtf8(line)]() { handleLine(payload); },
                    Qt::QueuedConnection);
            }
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                []() { QCoreApplication::quit(); },
                Qt::QueuedConnection);
        }).detach();
    }

private:
    void handleLine(const QString &line) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            return;
        }

        const QJsonObject request = doc.object();
        const qint64 id = request.value(QStringLiteral("id")).toInteger(-1);
        const QString method = request.value(QStringLiteral("method")).toString();
        const QJsonObject params = request.value(QStringLiteral("params")).toObject();

        if (method == QStringLiteral("open_video")) {
            ++m_openVideoCount;
            sendResult(id, QJsonObject {
                               {QStringLiteral("session_id"),
                                QStringLiteral("session-%1").arg(m_openVideoCount)},
                               {QStringLiteral("total_comments"), 0},
                               {QStringLiteral("comment_source"), QStringLiteral("none")},
                           });
            return;
        }

        if (method == QStringLiteral("playback_tick_batch")) {
            if (m_flags.contains(QStringLiteral("playback_tick_error"))) {
                sendError(
                    id,
                    -32000,
                    qEnvironmentVariable(
                        "NICONEON_FAKE_CORE_ERROR_MESSAGE",
                        "unknown session from fake core"));
                return;
            }

            const qint64 positionMs = lastPositionMs(params);
            auto sendTickResult = [this, id, positionMs]() {
                sendResult(id, QJsonObject {
                                   {QStringLiteral("emit_comments"), QJsonArray {}},
                                   {QStringLiteral("dropped_comments"), 0},
                                   {QStringLiteral("emit_over_budget"), false},
                                   {QStringLiteral("last_position_ms"), positionMs},
                               });
            };

            if (m_flags.contains(QStringLiteral("delay_playback_tick"))) {
                QTimer::singleShot(m_delayMs > 0 ? m_delayMs : 200, this, sendTickResult);
            } else {
                sendTickResult();
            }
            return;
        }

        if (method == QStringLiteral("add_ng_user")) {
            sendResult(id, QJsonObject {
                               {QStringLiteral("hidden_user_id"),
                                params.value(QStringLiteral("user_id")).toString()},
                           });
            return;
        }

        sendResult(id, QJsonObject {});
    }

    void sendResult(qint64 id, const QJsonObject &result) {
        writeJsonLine(stdout, QJsonObject {
                                  {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                  {QStringLiteral("id"), id},
                                  {QStringLiteral("result"), result},
                              });
    }

    void sendError(qint64 id, int code, const QString &message) {
        sendJson(QJsonObject {
            {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
            {QStringLiteral("id"), id},
            {QStringLiteral("error"),
             QJsonObject {
                 {QStringLiteral("code"), code},
                 {QStringLiteral("message"), message},
             }},
        });
    }

    void sendJson(const QJsonObject &object) {
        writeJsonLine(stdout, object);
    }

    const QSet<QString> m_flags;
    const int m_delayMs = 0;
    int m_openVideoCount = 0;
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    FakeCore core;
    core.start();
    return app.exec();
}
