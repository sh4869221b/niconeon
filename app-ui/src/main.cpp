#include <clocale>

#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include "danmaku/DanmakuController.hpp"
#include "ipc/CoreClient.hpp"
#include "mpv/MpvItem.hpp"

int main(int argc, char *argv[]) {
    // libmpv requires C numeric locale.
    setlocale(LC_NUMERIC, "C");

    QGuiApplication app(argc, argv);

    qmlRegisterType<MpvItem>("Niconeon", 1, 0, "MpvItem");
    qmlRegisterType<CoreClient>("Niconeon", 1, 0, "CoreClient");
    qmlRegisterType<DanmakuController>("Niconeon", 1, 0, "DanmakuController");

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    engine.loadFromModule("Niconeon", "Main");

    return app.exec();
}
