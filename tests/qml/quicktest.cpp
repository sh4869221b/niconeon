#include "app/ApplicationController.hpp"
#include "danmaku/DanmakuRenderNodeItem.hpp"
#include "ui/LicenseProvider.hpp"
#include <QQmlContext>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtQuickTest/quicktest.h>
class ViewSetup : public QObject {
    Q_OBJECT
  public slots:
    void applicationAvailable() {
        qputenv("NICONEON_DANMAKU_WORKER", "off");
        qunsetenv("NICONEON_AUTO_VIDEO_PATH");
        QCoreApplication::setOrganizationName("NiconeonTest");
        QCoreApplication::setApplicationName("ViewsTest");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
        qmlRegisterType<MpvItem>("Niconeon", 1, 0, "MpvItem");
        qmlRegisterType<DanmakuController>("Niconeon", 1, 0, "DanmakuController");
        qmlRegisterType<DanmakuRenderNodeItem>("Niconeon", 1, 0, "DanmakuRenderNodeItem");
        qmlRegisterType<LicenseProvider>("Niconeon", 1, 0, "LicenseProvider");
        niconeon::ServiceOptions options;
        options.memoryStore = true;
        controller = std::make_unique<niconeon::ApplicationController>(options);
    }
    void qmlEngineAvailable(QQmlEngine *engine) {
        engine->rootContext()->setContextProperty("application", controller.get());
    }
    void cleanupTestCase() {
        QSignalSpy stopped(controller.get(), &niconeon::ApplicationController::readyToQuit);
        controller->shutdown();
        if (stopped.isEmpty())
            QVERIFY2(stopped.wait(5000), "The comment worker must finish while QCoreApplication is alive");
        controller.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

  private:
    QTemporaryDir settings;
    std::unique_ptr<niconeon::ApplicationController> controller;
};
QUICK_TEST_MAIN_WITH_SETUP(niconeon_views, ViewSetup)
#include "quicktest.moc"
