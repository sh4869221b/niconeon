#include "app/ApplicationController.hpp"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <limits>
using namespace niconeon;
class ControllerTest : public QObject {
    Q_OBJECT
    QTemporaryDir m_settings;
  private slots:
    void initTestCase() {
        QCoreApplication::setOrganizationName("NiconeonTest");
        QCoreApplication::setApplicationName("ControllerTest");
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
        qputenv("NICONEON_DANMAKU_WORKER", "off");
    }
    void ratesAndFileUrls() {
        QCOMPARE(
            ApplicationController::normalizedPresets({3.5, 1.5, 1.5, 0.1, std::numeric_limits<double>::quiet_NaN()}),
            QVector<double>({0.5, 1.5, 3.0}));
        QCOMPARE(ApplicationController::normalizedPresets({}), QVector<double>({1.0, 1.5, 2.0}));
        QCOMPARE(ApplicationController::localPath("file:///tmp/hello%20%E6%97%A5.mp4"),
                 QStringLiteral("/tmp/hello 日.mp4"));
        QCOMPARE(ApplicationController::localPath(" /tmp/hello.mp4 "), QStringLiteral("/tmp/hello.mp4"));
    }
    void settingsAndCommands() {
        ServiceOptions options;
        options.memoryStore = true;
        ApplicationController controller(options);
        QSignalSpy stopped(&controller, &ApplicationController::readyToQuit);
        controller.addSpeedPreset("1.75");
        QVERIFY(controller.speedPresets().contains(1.75));
        controller.removeSpeedPreset(1.75);
        QVERIFY(!controller.speedPresets().contains(1.75));
        controller.setCommentsVisible(false);
        QVERIFY(!controller.commentsVisible());
        controller.setCommentsVisible(true);
        QVERIFY(controller.commentsVisible());
        controller.setFontSizeLevel(20);
        QCOMPARE(controller.fontSizeLevel(), 2);
        QCOMPARE(controller.appFontPixelSize(), 16);
        controller.setFontSizeLevel(-20);
        QCOMPARE(controller.fontSizeLevel(), 0);
        controller.setPerfLogEnabled(true);
        QVERIFY(controller.perfLogEnabled());
        const auto oldProfile = controller.perfProfile();
        controller.cycleRuntimeProfile();
        QVERIFY(controller.perfProfile() != oldProfile);
        controller.shutdown();
        QTRY_COMPARE(stopped.size(), 1);
    }
};
QTEST_MAIN(ControllerTest)
#include "application_controller_test.moc"
