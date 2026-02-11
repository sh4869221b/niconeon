#include "ipc/CoreClient.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

CoreClient::CoreClient(QObject *parent) : QObject(parent) {
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &CoreClient::onReadyReadStandardOutput);
    connect(&m_process, &QProcess::finished, this, &CoreClient::onProcessFinished);
}

void CoreClient::startDefault() {
    if (m_process.state() != QProcess::NotRunning) {
        return;
    }

    QString program = qEnvironmentVariable("NICONEON_CORE_BIN");
    if (program.isEmpty()) {
        const QString appDir = QCoreApplication::applicationDirPath();
        program = QDir(appDir).absoluteFilePath("../../core/target/debug/niconeon-core");
    }

    m_process.start(program, {"--stdio"});
    emit runningChanged();
}

void CoreClient::stop() {
    if (m_process.state() == QProcess::NotRunning) {
        return;
    }
    m_process.terminate();
    if (!m_process.waitForFinished(1000)) {
        m_process.kill();
    }
    emit runningChanged();
}

void CoreClient::openVideo(const QString &videoPath, const QString &videoId) {
    sendRequest("open_video", {
                                 {"video_path", videoPath},
                                 {"video_id", videoId},
                             });
}

void CoreClient::playbackTick(const QString &sessionId, qint64 positionMs, bool paused, bool isSeek) {
    sendRequest("playback_tick", {
                                    {"session_id", sessionId},
                                    {"position_ms", positionMs},
                                    {"paused", paused},
                                    {"is_seek", isSeek},
                                });
}

void CoreClient::addNgUser(const QString &userId) {
    sendRequest("add_ng_user", {{"user_id", userId}});
}

void CoreClient::removeNgUser(const QString &userId) {
    sendRequest("remove_ng_user", {{"user_id", userId}});
}

void CoreClient::undoLastNg(const QString &undoToken) {
    sendRequest("undo_last_ng", {{"undo_token", undoToken}});
}

void CoreClient::addRegexFilter(const QString &pattern) {
    sendRequest("add_regex_filter", {{"pattern", pattern}});
}

void CoreClient::removeRegexFilter(qint64 filterId) {
    sendRequest("remove_regex_filter", {{"filter_id", filterId}});
}

void CoreClient::listFilters() {
    sendRequest("list_filters", {});
}

bool CoreClient::running() const {
    return m_process.state() != QProcess::NotRunning;
}

void CoreClient::onReadyReadStandardOutput() {
    m_stdoutBuffer.append(m_process.readAllStandardOutput());

    while (true) {
        const int newline = m_stdoutBuffer.indexOf('\n');
        if (newline < 0) {
            break;
        }

        const QByteArray line = m_stdoutBuffer.left(newline).trimmed();
        m_stdoutBuffer.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            emit responseReceived("", QVariant(), QStringLiteral("invalid JSON-RPC response"));
            continue;
        }

        const QJsonObject obj = doc.object();
        const qint64 id = obj.value("id").toInteger(-1);
        const QString method = m_pendingMethods.take(id);

        QVariant result;
        QVariant error;
        if (obj.contains("result")) {
            result = obj.value("result").toVariant();
        }
        if (obj.contains("error")) {
            error = obj.value("error").toVariant();
        }

        emit responseReceived(method, result, error);
    }
}

void CoreClient::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status)
    emit runningChanged();
    emit coreCrashed(QStringLiteral("core exited with code %1").arg(exitCode));
}

void CoreClient::sendRequest(const QString &method, const QVariantMap &params) {
    if (m_process.state() == QProcess::NotRunning) {
        emit responseReceived(method, QVariant(), QStringLiteral("core is not running"));
        return;
    }

    const qint64 id = m_nextRequestId++;
    m_pendingMethods.insert(id, method);

    QJsonObject payload;
    payload.insert("jsonrpc", "2.0");
    payload.insert("id", id);
    payload.insert("method", method);
    payload.insert("params", QJsonObject::fromVariantMap(params));

    const QByteArray line = QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n";
    m_process.write(line);
}
