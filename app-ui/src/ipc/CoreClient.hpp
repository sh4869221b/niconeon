#pragma once

#include <QHash>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QVariant>

class CoreClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)

public:
    explicit CoreClient(QObject *parent = nullptr);

    Q_INVOKABLE void startDefault();
    Q_INVOKABLE void stop();

    Q_INVOKABLE void openVideo(const QString &videoPath, const QString &videoId);
    Q_INVOKABLE void playbackTick(const QString &sessionId, qint64 positionMs, bool paused, bool isSeek);
    Q_INVOKABLE void addNgUser(const QString &userId);
    Q_INVOKABLE void undoLastNg(const QString &undoToken);
    Q_INVOKABLE void addRegexFilter(const QString &pattern);
    Q_INVOKABLE void removeRegexFilter(qint64 filterId);
    Q_INVOKABLE void listFilters();

    bool running() const;

signals:
    void runningChanged();
    void responseReceived(const QString &method, const QVariant &result, const QVariant &error);
    void coreCrashed(const QString &reason);

private slots:
    void onReadyReadStandardOutput();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);

private:
    void sendRequest(const QString &method, const QVariantMap &params);

    QProcess m_process;
    QByteArray m_stdoutBuffer;
    qint64 m_nextRequestId = 1;
    QHash<qint64, QString> m_pendingMethods;
};
