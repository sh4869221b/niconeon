#include <clocale>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QVariantMap>
#include <QtGlobal>

#include "LicenseProvider.hpp"
#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuRenderNodeItem.hpp"
#include "ipc/CoreClient.hpp"
#include "mpv/MpvItem.hpp"

int main(int argc, char *argv[]) {
    // libmpv requires C numeric locale.
    setlocale(LC_NUMERIC, "C");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
        qputenv("QT_QUICK_CONTROLS_STYLE", "Fusion");
    }

    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("sh4869221b"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("github.com"));
    QCoreApplication::setApplicationName(QStringLiteral("Niconeon"));

#if defined(Q_OS_WIN)
    // QQuickFramebufferObject + libmpv rendering is stable on OpenGL backend.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif

    qmlRegisterType<MpvItem>("Niconeon", 1, 0, "MpvItem");
    qmlRegisterType<CoreClient>("Niconeon", 1, 0, "CoreClient");
    qmlRegisterType<DanmakuController>("Niconeon", 1, 0, "DanmakuController");
    qmlRegisterType<DanmakuRenderNodeItem>("Niconeon", 1, 0, "DanmakuRenderNodeItem");
    qmlRegisterType<LicenseProvider>("Niconeon", 1, 0, "LicenseProvider");

    QQmlApplicationEngine engine;
    QVariantMap initialProps;
    const QByteArray autoVideoPath = qgetenv("NICONEON_AUTO_VIDEO_PATH");
    if (!autoVideoPath.isEmpty()) {
        initialProps.insert(QStringLiteral("autoVideoPath"), QString::fromLocal8Bit(autoVideoPath));
    }
    const QByteArray autoPerfLog = qgetenv("NICONEON_AUTO_PERF_LOG");
    if (autoPerfLog == "1" || autoPerfLog.compare("true", Qt::CaseInsensitive) == 0) {
        initialProps.insert(QStringLiteral("autoPerfLogStart"), true);
    }
    bool autoExitOk = false;
    const int autoExitMs = qgetenv("NICONEON_AUTO_EXIT_MS").toInt(&autoExitOk);
    if (autoExitOk && autoExitMs > 0) {
        initialProps.insert(QStringLiteral("autoExitMs"), autoExitMs);
    }
    if (!initialProps.isEmpty()) {
        engine.setInitialProperties(initialProps);
    }
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    engine.loadFromModule("Niconeon", "Main");
#else
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Niconeon/Main.qml")));
#endif

    return app.exec();
}
