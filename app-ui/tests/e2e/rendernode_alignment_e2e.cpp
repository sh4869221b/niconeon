#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuRenderNodeItem.hpp"

#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTest>
#include <QVariantList>

#include <algorithm>
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
    return QRect(
        static_cast<int>(std::lround(logicalRect.x() * dpr)),
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
    case QSGRendererInterface::Vulkan:
        return "Vulkan";
    case QSGRendererInterface::Metal:
        return "Metal";
    case QSGRendererInterface::Null:
        return "Null";
    case QSGRendererInterface::Direct3D12:
        return "Direct3D12";
    }

    return "Unrecognized";
}
} // namespace

class RenderNodeAlignmentE2E : public QObject {
    Q_OBJECT

private slots:
    void renderNodeRespectsItemTranslation();
};

void RenderNodeAlignmentE2E::renderNodeRespectsItemTranslation() {
    qputenv("NICONEON_DANMAKU_WORKER", "off");
    qputenv("NICONEON_SIMD_MODE", "scalar");
    // DanmakuRenderNodeItem uses OpenGL-backed QSGRenderNode implementation.
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    qmlRegisterType<DanmakuController>("Niconeon", 1, 0, "DanmakuController");
    qmlRegisterType<DanmakuRenderNodeItem>("Niconeon", 1, 0, "DanmakuRenderNodeItem");

    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(
        R"(
import QtQuick
import Niconeon 1.0

Item {
    id: root
    width: 800
    height: 480

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
    window.resize(800, 480);
    window.setColor(QColor(QStringLiteral("#33AA77")));
    rootItem->setParentItem(window.contentItem());

    root.release();
    window.show();
    QVERIFY2(QTest::qWaitForWindowExposed(&window), "failed to expose test window");
    const QSGRendererInterface::GraphicsApi graphicsApi = window.rendererInterface()->graphicsApi();
    if (graphicsApi != QSGRendererInterface::OpenGL) {
        QSKIP(qPrintable(QStringLiteral("OpenGL scenegraph backend is required for this test (actual: %1)")
                             .arg(QString::fromLatin1(graphicsApiName(graphicsApi)))));
    }

    auto *controller = rootItem->findChild<DanmakuController *>(QStringLiteral("controller"));
    auto *container = rootItem->findChild<QQuickItem *>(QStringLiteral("container"));
    QVERIFY(controller);
    QVERIFY(container);

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
    controller->appendFromCore(comments, 2000);

    const QPointF containerTopLeft = container->mapToScene(QPointF(0.0, 0.0));
    const QRect containerRect(
        static_cast<int>(std::lround(containerTopLeft.x())),
        static_cast<int>(std::lround(containerTopLeft.y())),
        static_cast<int>(std::lround(container->width())),
        static_cast<int>(std::lround(container->height())));
    const QColor background(QStringLiteral("#33AA77"));

    ForegroundBounds bounds;
    qreal detectedDevicePixelRatio = 1.0;
    for (int attempt = 0; attempt < 60; ++attempt) {
        QTest::qWait(25);
        window.requestUpdate();
        const QImage frame = window.grabWindow();
        if (frame.isNull()) {
            continue;
        }

        detectedDevicePixelRatio = std::max(frame.devicePixelRatio(), 1.0);
        const QRect deviceContainerRect = toDeviceRect(containerRect, detectedDevicePixelRatio);
        bounds = detectForeground(frame, deviceContainerRect, background);
        if (bounds.pixelCount >= kForegroundPixelMin) {
            break;
        }
    }

    QVERIFY2(bounds.pixelCount >= kForegroundPixelMin, "danmaku pixels were not rendered inside the viewport");
    const qreal minYInLogical = bounds.minY / detectedDevicePixelRatio;
    QVERIFY2(minYInLogical >= containerRect.top() + 6, "danmaku was rendered without item Y translation");
}

QTEST_MAIN(RenderNodeAlignmentE2E)

#include "rendernode_alignment_e2e.moc"
