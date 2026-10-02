#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuRenderNodeItem.hpp"
#include "playback/GraphicsEnvironment.hpp"

#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QOpenGLContext>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <QVariantList>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>

namespace {
constexpr int kForegroundPixelMin = 120;
constexpr int kColorDistanceThreshold = 40;

struct ForegroundBounds {
    int pixelCount = 0;
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    int maxX = std::numeric_limits<int>::min();
    int maxY = std::numeric_limits<int>::min();
};

int colorDistance(const QColor &a, const QColor &b) {
    return std::abs(a.red() - b.red()) + std::abs(a.green() - b.green()) + std::abs(a.blue() - b.blue());
}

ForegroundBounds detectForeground(const QImage &image, const QRect &rect, const QColor &background) {
    ForegroundBounds bounds;
    const QRect safeRect = rect.intersected(QRect(0, 0, image.width(), image.height()));
    if (safeRect.isEmpty()) {
        return bounds;
    }

    for (int y = safeRect.top(); y <= safeRect.bottom(); ++y) {
        for (int x = safeRect.left(); x <= safeRect.right(); ++x) {
            const QColor pixel = image.pixelColor(x, y);
            if (colorDistance(pixel, background) < kColorDistanceThreshold) {
                continue;
            }

            ++bounds.pixelCount;
            bounds.minX = std::min(bounds.minX, x);
            bounds.minY = std::min(bounds.minY, y);
            bounds.maxX = std::max(bounds.maxX, x);
            bounds.maxY = std::max(bounds.maxY, y);
        }
    }
    return bounds;
}

QRect toDeviceRect(const QRect &logicalRect, qreal devicePixelRatio) {
    const qreal dpr = std::max(devicePixelRatio, 1.0);
    return QRect(static_cast<int>(std::lround(logicalRect.x() * dpr)),
                 static_cast<int>(std::lround(logicalRect.y() * dpr)),
                 static_cast<int>(std::lround(logicalRect.width() * dpr)),
                 static_cast<int>(std::lround(logicalRect.height() * dpr)));
}

const char *graphicsApiName(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
    case QSGRendererInterface::Unknown:
        return "Unknown";
    case QSGRendererInterface::Software:
        return "Software";
    case QSGRendererInterface::OpenVG:
        return "OpenVG";
    case QSGRendererInterface::OpenGL:
        return "OpenGL";
    case QSGRendererInterface::Direct3D11:
        return "Direct3D11";
    case QSGRendererInterface::Direct3D12:
        return "Direct3D12";
    case QSGRendererInterface::Vulkan:
        return "Vulkan";
    case QSGRendererInterface::Metal:
        return "Metal";
    case QSGRendererInterface::Null:
        return "Null";
    }

    return "Unrecognized";
}
} // namespace

class RenderNodeAlignmentE2E : public QObject {
    Q_OBJECT

  private slots:
    void renderNodeRespectsItemTranslation_data();
    void renderNodeRespectsItemTranslation();
};

void RenderNodeAlignmentE2E::renderNodeRespectsItemTranslation_data() {
    QTest::addColumn<bool>("gpuTiming");
    QTest::newRow("ordinary-render") << false;
    QTest::newRow("bounded-gpu-observer") << true;
}

void RenderNodeAlignmentE2E::renderNodeRespectsItemTranslation() {
    QFETCH(bool, gpuTiming);
    qputenv("NICONEON_RENDER_DIAGNOSTICS", "1");
    qputenv("NICONEON_RENDER_GPU_TIMING", gpuTiming ? "1" : "0");
    qputenv("NICONEON_DANMAKU_WORKER", "off");
    qputenv("NICONEON_SIMD_MODE", "scalar");
    // DanmakuRenderNodeItem uses OpenGL-backed QSGRenderNode implementation.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    qmlRegisterType<DanmakuController>("NiconeonTest", 1, 0, "DanmakuController");
    qmlRegisterType<DanmakuRenderNodeItem>("NiconeonTest", 1, 0, "DanmakuRenderNodeItem");

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
import QtQuick
import NiconeonTest 1.0

Item {
    id: root
    width: 800
    height: 480

    // Match Main.qml's ordinary background geometry before the custom render node.
    // Qt 6.8's custom-node-only path can calculate clipping from an uninitialized
    // cached native projection and leave the GL viewport at its initial 100x100.
    // The pixel-count and translated-container assertions remain unchanged.
    Rectangle {
        anchors.fill: parent
        color: "#33AA77"
        z: -1
    }

    DanmakuController {
        id: controller
        objectName: "controller"
    }

    Item {
        id: container
        objectName: "container"
        x: 80
        y: 60
        width: 420
        height: 220
        clip: true

        DanmakuRenderNodeItem {
            anchors.fill: parent
            controller: controller
        }
    }
}
)",
        QUrl(QStringLiteral("inline:rendernode_alignment_e2e.qml")));
    for (int attempt = 0; attempt < 200 && component.status() == QQmlComponent::Loading; ++attempt) {
        QTest::qWait(10);
    }
    QVERIFY2(component.status() == QQmlComponent::Ready, qPrintable(component.errorString()));

    std::unique_ptr<QObject> root(component.create());
    QVERIFY2(root, qPrintable(component.errorString()));

    auto *rootItem = qobject_cast<QQuickItem *>(root.get());
    QVERIFY(rootItem);

    QQuickWindow window;
    std::atomic<bool> supportsTimestamps{false};
    connect(
        &window, &QQuickWindow::sceneGraphInitialized, &window,
        [&] {
            const auto *context = QOpenGLContext::currentContext();
            supportsTimestamps.store(context && !context->isOpenGLES() &&
                                     (context->format().version() >= qMakePair(3, 3) ||
                                      context->hasExtension(QByteArrayLiteral("GL_ARB_timer_query"))));
        },
        Qt::DirectConnection);
    window.resize(800, 480);
    window.setColor(QColor(QStringLiteral("#33AA77")));
    rootItem->setParentItem(window.contentItem());

    root.release();
    window.show();
    QVERIFY2(QTest::qWaitForWindowExposed(&window), "failed to expose test window");
    const QSGRendererInterface::GraphicsApi graphicsApi = window.rendererInterface()->graphicsApi();
    if (graphicsApi != QSGRendererInterface::OpenGL) {
        QVERIFY2(!qEnvironmentVariableIsSet("NICONEON_REQUIRE_OPENGL"), "Required OpenGL backend is unavailable");
        QSKIP(qPrintable(QStringLiteral("OpenGL scenegraph backend is required for this test (actual: %1)")
                             .arg(QString::fromLatin1(graphicsApiName(graphicsApi)))));
    }

    auto *controller = rootItem->findChild<DanmakuController *>(QStringLiteral("controller"));
    auto *container = rootItem->findChild<QQuickItem *>(QStringLiteral("container"));
    auto *renderNode = rootItem->findChild<DanmakuRenderNodeItem *>();
    QVERIFY(controller);
    QVERIFY(container);
    QVERIFY(renderNode);
    QCOMPARE(renderNode->controller(), controller);

    controller->setViewportSize(container->width(), container->height());
    controller->setLaneMetrics(36, 6);
    controller->setPlaybackPaused(true);

    QVariantMap comment;
    comment.insert(QStringLiteral("comment_id"), QStringLiteral("e2e-comment-1"));
    comment.insert(QStringLiteral("user_id"), QStringLiteral("e2e-user-1"));
    comment.insert(QStringLiteral("text"), QStringLiteral("rendernode alignment"));
    comment.insert(QStringLiteral("at_ms"), 0);

    QVariantList comments;
    comments.push_back(comment);
    controller->appendComments(comments, 2000);

    const QPointF containerTopLeft = container->mapToScene(QPointF(0.0, 0.0));
    const QRect containerRect(
        static_cast<int>(std::lround(containerTopLeft.x())), static_cast<int>(std::lround(containerTopLeft.y())),
        static_cast<int>(std::lround(container->width())), static_cast<int>(std::lround(container->height())));
    const QColor background(QStringLiteral("#33AA77"));

    ForegroundBounds bounds;
    QImage lastFrame;
    qreal detectedDevicePixelRatio = 1.0;
    for (int attempt = 0; attempt < 60; ++attempt) {
        QTest::qWait(25);
        window.requestUpdate();
        const QImage frame = window.grabWindow();
        if (frame.isNull()) {
            continue;
        }
        lastFrame = frame;

        detectedDevicePixelRatio = std::max(frame.devicePixelRatio(), 1.0);
        const QRect deviceContainerRect = toDeviceRect(containerRect, detectedDevicePixelRatio);
        bounds = detectForeground(frame, deviceContainerRect, background);
        if (bounds.pixelCount >= kForegroundPixelMin) {
            break;
        }
    }

    if (bounds.pixelCount < kForegroundPixelMin && !lastFrame.isNull()) {
        const auto path = QDir::current().absoluteFilePath(QStringLiteral("rendernode-alignment-failure.png"));
        qWarning() << "alignment failure capture=" << path << "saved=" << lastFrame.save(path)
                   << "foregroundPixels=" << bounds.pixelCount << "container=" << containerRect;
    }
    QVERIFY2(bounds.pixelCount >= kForegroundPixelMin, "danmaku pixels were not rendered inside the viewport");
    const qreal minYInLogical = bounds.minY / detectedDevicePixelRatio;
    QVERIFY2(minYInLogical >= containerRect.top() + 6, "danmaku was rendered without item Y translation");
    bool sawFrame = false, sawGpuResult = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        window.requestUpdate();
        QTest::qWait(20);
        const auto batch = renderNode->takeRenderDiagnostics();
        QVERIFY(batch.enabled);
        for (const auto &frame : batch.frames) {
            sawFrame = true;
            QCOMPARE(frame.gpuTimingRequested, gpuTiming);
            QVERIFY(frame.gpuPendingQueries >= 0 && frame.gpuPendingQueries <= 8);
            QCOMPARE(frame.gpuInvalidResults, 0);
            if (gpuTiming)
                QCOMPARE(frame.gpuTimingSupported, supportsTimestamps.load());
            else {
                QVERIFY(!frame.gpuResultAvailable);
                QCOMPARE(frame.gpuPendingQueries, 0);
            }
            if (frame.gpuResultAvailable) {
                QVERIFY(frame.gpuElapsedNs >= 0);
                QVERIFY(frame.gpuMeasuredFrameSequence <= frame.frameSequence);
                QVERIFY(frame.gpuMeasuredCpuStartNs > 0 && frame.gpuMeasuredCpuStartNs <= frame.capturedAtNs);
                sawGpuResult |= frame.gpuMeasuredDrawCalls > 0;
            }
        }
        if (sawFrame && (!gpuTiming || !supportsTimestamps.load() || sawGpuResult))
            break;
    }
    QVERIFY(sawFrame);
    if (gpuTiming && supportsTimestamps.load())
        QVERIFY2(sawGpuResult, "Supported GL timestamps produced no completed drawing interval");
}

int main(int argc, char **argv) {
    niconeon::configureGraphicsEnvironment();
    QGuiApplication app(argc, argv);
    RenderNodeAlignmentE2E test;
    return QTest::qExec(&test, argc, argv);
}

#include "rendernode_alignment_e2e.moc"
