#include "playback/GraphicsEnvironment.hpp"
#include <QtTest>

class GraphicsEnvironmentTest : public QObject {
    Q_OBJECT
  private slots:
    void retainsExplicitSelection() {
        const auto xcb = qgetenv("QT_XCB_GL_INTEGRATION");
        const auto gallium = qgetenv("GALLIUM_DRIVER");
        qputenv("QT_XCB_GL_INTEGRATION", "xcb_glx");
        qputenv("GALLIUM_DRIVER", "test-explicit-driver");
        niconeon::configureGraphicsEnvironment();
        QCOMPARE(qgetenv("QT_XCB_GL_INTEGRATION"), QByteArray("xcb_glx"));
        QCOMPARE(qgetenv("GALLIUM_DRIVER"), QByteArray("test-explicit-driver"));
        qputenv("QT_XCB_GL_INTEGRATION", xcb);
        qputenv("GALLIUM_DRIVER", gallium);
    }
    void selectsPlatformDefaults() {
        const auto xcb = qgetenv("QT_XCB_GL_INTEGRATION");
        const auto gallium = qgetenv("GALLIUM_DRIVER");
        qunsetenv("QT_XCB_GL_INTEGRATION");
        qunsetenv("GALLIUM_DRIVER");
        niconeon::configureGraphicsEnvironment();
#ifdef Q_OS_LINUX
        QCOMPARE(qgetenv("QT_XCB_GL_INTEGRATION"), QByteArray("xcb_egl"));
        QVERIFY(qEnvironmentVariableIsEmpty("GALLIUM_DRIVER"));
#elif defined(Q_OS_WIN)
        QCOMPARE(qgetenv("GALLIUM_DRIVER"), QByteArray("llvmpipe"));
        QVERIFY(qEnvironmentVariableIsEmpty("QT_XCB_GL_INTEGRATION"));
#endif
        qputenv("QT_XCB_GL_INTEGRATION", xcb);
        qputenv("GALLIUM_DRIVER", gallium);
    }
};
QTEST_APPLESS_MAIN(GraphicsEnvironmentTest)
#include "graphics_environment_test.moc"
