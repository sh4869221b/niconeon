#define private public
#include "ipc/CoreClient.hpp"
#undef private

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>
#include <QVariantMap>

namespace {

QString executableName(const QString &baseName) {
#if defined(Q_OS_WIN)
    if (baseName.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive)) {
        return baseName;
    }
    return baseName + QStringLiteral(".exe");
#else
    return baseName;
#endif
}

QString fakeCorePath() {
    const QString fromEnv = qEnvironmentVariable("NICONEON_FAKE_CORE_BIN");
    if (!fromEnv.trimmed().isEmpty()) {
        return fromEnv.trimmed();
    }
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(executableName(QStringLiteral("niconeon-fake-core")));
}

class ScopedEnvVar {
public:
    ScopedEnvVar(const char *name, const QByteArray &value) : m_name(name), m_hadValue(qEnvironmentVariableIsSet(name)) {
        if (m_hadValue) {
            m_previousValue = qgetenv(name);
        }
        qputenv(name, value);
    }

    ~ScopedEnvVar() {
        if (m_hadValue) {
            qputenv(m_name.constData(), m_previousValue);
        } else {
            qunsetenv(m_name.constData());
        }
    }

private:
    QByteArray m_name;
    QByteArray m_previousValue;
    bool m_hadValue = false;
};

class ScopedClientStop {
public:
    explicit ScopedClientStop(CoreClient &client) : m_client(client) {}

    ~ScopedClientStop() {
        m_client.stop();
    }

private:
    CoreClient &m_client;
};

QVariantMap responseResult(const QList<QVariant> &args) {
    return args.value(1).toMap();
}

QString responseError(const QList<QVariant> &args) {
    return args.value(2).toString();
}

} // namespace

class CoreClientTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void stderrOnlyDoesNotEmitCrash();
    void stalePlaybackTickResponseIsDroppedAfterOpenVideo();
    void jsonRpcErrorObjectIsExposedAsMessage();
};

void CoreClientTest::initTestCase() {
    const QString path = fakeCorePath();
    QVERIFY2(QFileInfo::exists(path), qPrintable(QStringLiteral("fake core not found: %1").arg(path)));
}

void CoreClientTest::stderrOnlyDoesNotEmitCrash() {
    ScopedEnvVar fakeBin("NICONEON_CORE_BIN", fakeCorePath().toUtf8());
    ScopedEnvVar scenario("NICONEON_FAKE_CORE_SCENARIO", "stderr_on_start");

    CoreClient client;
    ScopedClientStop stopClient(client);
    QSignalSpy crashSpy(&client, &CoreClient::coreCrashed);

    client.startDefault();
    QVERIFY(client.m_process.waitForStarted(1000));
    QTest::qWait(50);

    QCOMPARE(crashSpy.count(), 0);
    QVERIFY(client.running());
}

void CoreClientTest::stalePlaybackTickResponseIsDroppedAfterOpenVideo() {
    ScopedEnvVar fakeBin("NICONEON_CORE_BIN", fakeCorePath().toUtf8());
    ScopedEnvVar scenario("NICONEON_FAKE_CORE_SCENARIO", "delay_playback_tick");

    CoreClient client;
    ScopedClientStop stopClient(client);
    QSignalSpy responseSpy(&client, &CoreClient::responseReceived);

    client.startDefault();
    QVERIFY(client.m_process.waitForStarted(1000));
    QTest::qWait(50);

    client.enqueuePlaybackTick(QStringLiteral("stale-session"), 120, false, false);
    QVERIFY(client.m_process.waitForBytesWritten(1000));
    QTest::qWait(20);
    client.openVideo(QStringLiteral("movie_sm9.mp4"), QStringLiteral("sm9"));
    QVERIFY(client.m_process.waitForBytesWritten(1000));

    QVERIFY(client.m_process.waitForReadyRead(1000));
    client.onReadyReadStandardOutput();
    QTRY_VERIFY_WITH_TIMEOUT(responseSpy.count() >= 1, 1000);
    QTest::qWait(350);
    if (client.m_process.bytesAvailable() > 0 || client.m_process.waitForReadyRead(100)) {
        client.onReadyReadStandardOutput();
    }

    QCOMPARE(responseSpy.count(), 1);
    const QList<QVariant> args = responseSpy.takeFirst();
    QCOMPARE(args.value(0).toString(), QStringLiteral("open_video"));
    QVERIFY(responseError(args).isEmpty());
    QVERIFY(!responseResult(args).value(QStringLiteral("session_id")).toString().isEmpty());
}

void CoreClientTest::jsonRpcErrorObjectIsExposedAsMessage() {
    ScopedEnvVar fakeBin("NICONEON_CORE_BIN", fakeCorePath().toUtf8());
    ScopedEnvVar scenario("NICONEON_FAKE_CORE_SCENARIO", "playback_tick_error");
    ScopedEnvVar message(
        "NICONEON_FAKE_CORE_ERROR_MESSAGE", "unknown session from fake core");

    CoreClient client;
    ScopedClientStop stopClient(client);
    QSignalSpy responseSpy(&client, &CoreClient::responseReceived);

    client.startDefault();
    QVERIFY(client.m_process.waitForStarted(1000));
    QTest::qWait(50);

    client.enqueuePlaybackTick(QStringLiteral("missing-session"), 240, false, false);
    QVERIFY(client.m_process.waitForBytesWritten(1000));

    QVERIFY(client.m_process.waitForReadyRead(1000));
    client.onReadyReadStandardOutput();
    QTRY_COMPARE_WITH_TIMEOUT(responseSpy.count(), 1, 1000);
    const QList<QVariant> args = responseSpy.takeFirst();
    QCOMPARE(args.value(0).toString(), QStringLiteral("playback_tick_batch"));
    QCOMPARE(responseError(args), QStringLiteral("unknown session from fake core"));
}

QTEST_APPLESS_MAIN(CoreClientTest)

#include "core_client_test.moc"
