#include "ipc/CoreClient.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

namespace {
QString processErrorName(QProcess::ProcessError error) {
    switch (error) {
    case QProcess::FailedToStart:
        return QStringLiteral("FailedToStart");
    case QProcess::Crashed:
        return QStringLiteral("Crashed");
    case QProcess::Timedout:
        return QStringLiteral("Timedout");
    case QProcess::WriteError:
        return QStringLiteral("WriteError");
    case QProcess::ReadError:
        return QStringLiteral("ReadError");
    case QProcess::UnknownError:
    default:
        return QStringLiteral("UnknownError");
    }
}
} // namespace

CoreClient::CoreClient(QObject *parent) : QObject(parent) {
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &CoreClient::onReadyReadStandardOutput);
    connect(&m_process, &QProcess::readyReadStandardError, this, &CoreClient::onReadyReadStandardError);
    connect(&m_process, &QProcess::finished, this, &CoreClient::onProcessFinished);
    connect(&m_process, &QProcess::errorOccurred, this, &CoreClient::onProcessErrorOccurred);
}

QString CoreClient::executableName(const QString &baseName) {
#if defined(Q_OS_WIN)
    if (baseName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        return baseName;
    }
    return baseName + QStringLiteral(".exe");
#else
    return baseName;
#endif
}

QString CoreClient::resolveCoreProgram(QStringList *triedCandidates) const {
    QStringList candidates;
    auto addCandidate = [&candidates](const QString &candidate) {
        const QString cleaned = QDir::cleanPath(candidate);
        if (!cleaned.isEmpty() && !candidates.contains(cleaned)) {
            candidates.append(cleaned);
        }
    };

    const QString envProgram = qEnvironmentVariable("NICONEON_CORE_BIN");
    if (!envProgram.trimmed().isEmpty()) {
        addCandidate(envProgram.trimmed());
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    addCandidate(QDir(appDir).absoluteFilePath(executableName(QStringLiteral("niconeon-core"))));
    addCandidate(QDir(appDir).absoluteFilePath(
        QStringLiteral("../") + executableName(QStringLiteral("niconeon-core"))));
    addCandidate(QDir(appDir).absoluteFilePath(
        QStringLiteral("../../core/target/debug/") + executableName(QStringLiteral("niconeon-core"))));
    addCandidate(QDir(appDir).absoluteFilePath(
        QStringLiteral("../../core/target/release/") + executableName(QStringLiteral("niconeon-core"))));

    if (triedCandidates) {
        *triedCandidates = candidates;
    }

    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile()) {
            return info.absoluteFilePath();
        }
    }
    return QString();
}

void CoreClient::startDefault() {
    if (m_process.state() != QProcess::NotRunning) {
        return;
    }
    m_pendingTickSessionId.clear();
    m_pendingTicks.clear();
    m_playbackTickBatchInFlight = false;
    m_inFlightPlaybackTickRequestIds.clear();

    QStringList triedCandidates;
    const QString program = resolveCoreProgram(&triedCandidates);
    if (program.isEmpty()) {
        emit coreCrashed(
            QStringLiteral("core binary not found. tried: %1").arg(triedCandidates.join(QStringLiteral(", "))));
        return;
    }

    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_process.start(program, {"--stdio"});
    emit runningChanged();
}

void CoreClient::stop() {
    if (m_process.state() == QProcess::NotRunning) {
        return;
    }
    m_pendingTickSessionId.clear();
    m_pendingTicks.clear();
    m_playbackTickBatchInFlight = false;
    m_inFlightPlaybackTickRequestIds.clear();
    m_process.terminate();
    if (!m_process.waitForFinished(1000)) {
        m_process.kill();
    }
    emit runningChanged();
}

void CoreClient::openVideo(const QString &videoPath, const QString &videoId) {
    m_pendingTickSessionId.clear();
    m_pendingTicks.clear();
    m_playbackTickBatchInFlight = false;
    m_inFlightPlaybackTickRequestIds.clear();
    sendRequest("open_video", {
                                 {"video_path", videoPath},
                                 {"video_id", videoId},
                             });
}

void CoreClient::enqueuePlaybackTick(const QString &sessionId, qint64 positionMs, bool paused, bool isSeek) {
    if (sessionId.trimmed().isEmpty()) {
        return;
    }
    if (m_process.state() == QProcess::NotRunning) {
        emit responseReceived("playback_tick_batch", QVariant(), QStringLiteral("core is not running"));
        return;
    }

    if (!m_pendingTickSessionId.isEmpty() && m_pendingTickSessionId != sessionId) {
        m_pendingTicks.clear();
    }
    m_pendingTickSessionId = sessionId;
    m_pendingTicks.push_back(PendingPlaybackTick {
        positionMs,
        paused,
        isSeek,
    });

    flushPlaybackTickBatch();
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

        if (method == QStringLiteral("playback_tick_batch")
            && m_inFlightPlaybackTickRequestIds.remove(id) > 0) {
            m_playbackTickBatchInFlight = false;
            flushPlaybackTickBatch();
        }

        emit responseReceived(method, result, error);
    }
}

void CoreClient::onReadyReadStandardError() {
    m_stderrBuffer.append(m_process.readAllStandardError());

    while (true) {
        const int newline = m_stderrBuffer.indexOf('\n');
        if (newline < 0) {
            break;
        }

        const QByteArray line = m_stderrBuffer.left(newline).trimmed();
        m_stderrBuffer.remove(0, newline + 1);
        if (line.isEmpty()) {
            continue;
        }

        const QString message = QString::fromUtf8(line);
        qWarning().noquote() << "core stderr:" << message;
        emit coreCrashed(QStringLiteral("core stderr: %1").arg(message));
    }
}

void CoreClient::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    Q_UNUSED(status)

    const QString tail = QString::fromUtf8(m_stderrBuffer).trimmed();
    m_stderrBuffer.clear();
    if (!tail.isEmpty()) {
        qWarning().noquote() << "core stderr:" << tail;
        emit coreCrashed(QStringLiteral("core stderr: %1").arg(tail));
    }

    emit runningChanged();
    m_pendingTickSessionId.clear();
    m_pendingTicks.clear();
    m_playbackTickBatchInFlight = false;
    m_inFlightPlaybackTickRequestIds.clear();
    emit coreCrashed(QStringLiteral("core exited with code %1").arg(exitCode));
}

void CoreClient::onProcessErrorOccurred(QProcess::ProcessError error) {
    const QString message = QStringLiteral("core process error (%1): %2")
                                .arg(processErrorName(error), m_process.errorString());
    m_pendingTickSessionId.clear();
    m_pendingTicks.clear();
    m_playbackTickBatchInFlight = false;
    m_inFlightPlaybackTickRequestIds.clear();
    emit coreCrashed(message);
}

qint64 CoreClient::sendRequest(const QString &method, const QVariantMap &params) {
    if (m_process.state() == QProcess::NotRunning) {
        emit responseReceived(method, QVariant(), QStringLiteral("core is not running"));
        return -1;
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
    return id;
}

void CoreClient::flushPlaybackTickBatch() {
    if (m_playbackTickBatchInFlight || m_pendingTicks.isEmpty() || m_pendingTickSessionId.isEmpty()) {
        return;
    }
    if (m_process.state() == QProcess::NotRunning) {
        m_pendingTicks.clear();
        return;
    }

    QVariantList ticks;
    ticks.reserve(m_pendingTicks.size());
    for (const PendingPlaybackTick &tick : std::as_const(m_pendingTicks)) {
        ticks.push_back(QVariantMap {
            {"position_ms", tick.positionMs},
            {"paused", tick.paused},
            {"is_seek", tick.isSeek},
        });
    }

    const QString sessionId = m_pendingTickSessionId;
    m_pendingTicks.clear();
    const qint64 requestId = sendRequest(
        "playback_tick_batch",
        {
            {"session_id", sessionId},
            {"ticks", ticks},
        });
    if (requestId < 0) {
        m_playbackTickBatchInFlight = false;
        return;
    }
    m_playbackTickBatchInFlight = true;
    m_inFlightPlaybackTickRequestIds.insert(requestId);
}
