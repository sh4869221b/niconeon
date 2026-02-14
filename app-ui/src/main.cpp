#include <clocale>

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtGlobal>

#include "LicenseProvider.hpp"
#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuSceneItem.hpp"
#include "ipc/CoreClient.hpp"
#include "mpv/MpvItem.hpp"

int main(int argc, char *argv[]) {
    // libmpv requires C numeric locale.
    setlocale(LC_NUMERIC, "C");

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
    qmlRegisterType<DanmakuSceneItem>("Niconeon", 1, 0, "DanmakuSceneItem");
    qmlRegisterType<LicenseProvider>("Niconeon", 1, 0, "LicenseProvider");

    QQmlApplicationEngine engine;
    QString danmakuBackend = qEnvironmentVariable("NICONEON_DANMAKU_BACKEND").trimmed().toLower();
    if (danmakuBackend != QStringLiteral("legacy") && danmakuBackend != QStringLiteral("scenegraph")) {
        danmakuBackend = QStringLiteral("scenegraph");
    }
    engine.rootContext()->setContextProperty(QStringLiteral("niconeonDanmakuBackend"), danmakuBackend);

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
