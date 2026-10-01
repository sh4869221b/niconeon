#include "app/ApplicationController.hpp"
#include "danmaku/DanmakuRenderNodeItem.hpp"
#include "playback/MpvItem.hpp"
#include "ui/LicenseProvider.hpp"
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <clocale>

int main(int argc, char *argv[]) {
    setlocale(LC_NUMERIC, "C");
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE"))
        qputenv("QT_QUICK_CONTROLS_STYLE", "Fusion");
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("sh4869221b"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("github.com"));
    QCoreApplication::setApplicationName(QStringLiteral("Niconeon"));
    // Both the retained video integration and sprite renderer require OpenGL.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    qmlRegisterType<MpvItem>("Niconeon", 1, 0, "MpvItem");
    qmlRegisterType<DanmakuController>("Niconeon", 1, 0, "DanmakuController");
    qmlRegisterType<DanmakuRenderNodeItem>("Niconeon", 1, 0, "DanmakuRenderNodeItem");
    qmlRegisterType<LicenseProvider>("Niconeon", 1, 0, "LicenseProvider");
    niconeon::ServiceOptions options;
    options.fetch.cookie = qgetenv("NICONICO_COOKIE");
    if (options.fetch.cookie.isEmpty())
        options.fetch.cookie = qgetenv("NICONEON_NICONICO_COOKIE");
    niconeon::ApplicationController controller(options);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("application"), &controller);
    // Main.qml vetoes the initial close while the worker drains. quit() would
    // send another close request, which that handler vetoes again.
    QObject::connect(&controller, &niconeon::ApplicationController::readyToQuit, &app,
                     [] { QCoreApplication::exit(0); });
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app, [&controller] { controller.shutdown(); },
        Qt::QueuedConnection);
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/Niconeon/Main.qml")));
    bool exitOk = false;
    const int exitMs = qEnvironmentVariableIntValue("NICONEON_AUTO_EXIT_MS", &exitOk);
    if (exitOk && exitMs > 0)
        QTimer::singleShot(exitMs, &controller, &niconeon::ApplicationController::shutdown);
    const bool loadFailed = engine.rootObjects().isEmpty();
    const int exitCode = app.exec();
    return loadFailed ? 1 : exitCode;
}
