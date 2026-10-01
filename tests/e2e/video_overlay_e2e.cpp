#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuRenderNodeItem.hpp"
#include "playback/GraphicsEnvironment.hpp"
#include "playback/MpvItem.hpp"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QOpenGLContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QtTest>

#include <algorithm>
#include <cmath>

namespace {
constexpr int videoWidth = 160;
constexpr int videoHeight = 90;
constexpr int windowWidth = 640;
constexpr int windowHeight = 360;
constexpr int expectedGray = 98; // Limited-range Y=100, Cb=Cr=128 -> RGB approximately 98.
constexpr int colorTolerance = 8;

bool writeFixture(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray header("YUV4MPEG2 W160 H90 F30:1 Ip A1:1 C420jpeg\n");
    if (file.write(header) != header.size())
        return false;
    const QByteArray luma(videoWidth * videoHeight, char(100));
    const QByteArray chroma(videoWidth * videoHeight / 2, char(128));
    for (int frame = 0; frame < 180; ++frame) {
        if (file.write("FRAME\n", 6) != 6 || file.write(luma) != luma.size() || file.write(chroma) != chroma.size())
            return false;
    }
    return file.flush();
}

struct PixelStats {
    qsizetype pixels = 0;
    qsizetype incorrect = 0;
    qsizetype dark = 0;
    qsizetype white = 0;
    int minimum = 255;
    int maximum = 0;
};

QRect interior(const QImage &image, int topLogical = 4) {
    const auto scale = image.width() / static_cast<double>(windowWidth);
    const auto margin = std::max(1, static_cast<int>(std::lround(4 * scale)));
    const auto top = std::max(margin, static_cast<int>(std::lround(topLogical * scale)));
    return QRect(margin, top, image.width() - 2 * margin, image.height() - top - margin);
}

PixelStats inspectPixels(const QImage &image, const QRect &area) {
    PixelStats stats;
    if (image.isNull())
        return stats;
    const auto bounded = area.intersected(image.rect());
    for (int y = bounded.top(); y <= bounded.bottom(); ++y) {
        for (int x = bounded.left(); x <= bounded.right(); ++x) {
            const auto pixel = image.pixelColor(x, y);
            ++stats.pixels;
            const auto low = std::min({pixel.red(), pixel.green(), pixel.blue()});
            const auto high = std::max({pixel.red(), pixel.green(), pixel.blue()});
            stats.minimum = std::min(stats.minimum, low);
            stats.maximum = std::max(stats.maximum, high);
            if (std::abs(pixel.red() - expectedGray) > colorTolerance ||
                std::abs(pixel.green() - expectedGray) > colorTolerance ||
                std::abs(pixel.blue() - expectedGray) > colorTolerance)
                ++stats.incorrect;
            if (high < expectedGray - colorTolerance)
                ++stats.dark;
            if (low >= 220)
                ++stats.white;
        }
    }
    return stats;
}

bool isCleanGray(const QImage &image, int topLogical = 4) {
    const auto stats = inspectPixels(image, interior(image, topLogical));
    return stats.pixels > 10000 && stats.incorrect == 0;
}

QString failureDiagnostic(const QString &phase, const QByteArray &backend, const QImage &image, int topLogical = 4) {
    const auto stats = inspectPixels(image, interior(image, topLogical));
    const auto filename = QStringLiteral("video-overlay-%1-%2.png").arg(QString::fromLatin1(backend), phase);
    const auto path = QDir::current().absoluteFilePath(filename);
    const bool saved = !image.isNull() && image.save(path);
    return QStringLiteral("%1 (%2): pixels=%3 incorrect=%4 dark=%5 white=%6 range=%7..%8; capture=%9")
        .arg(phase, QString::fromLatin1(backend))
        .arg(stats.pixels)
        .arg(stats.incorrect)
        .arg(stats.dark)
        .arg(stats.white)
        .arg(stats.minimum)
        .arg(stats.maximum)
        .arg(saved ? path : QStringLiteral("unavailable"));
}

QImage capture(QQuickWindow &window, const MpvItem &player) {
    window.requestUpdate();
    QTest::qWait(35);
    const auto image = window.grabWindow();
    if (image.isNull() || window.width() <= 0 || window.height() <= 0)
        return {};
    // Window managers may override resize(). Inspect the actual video item, never
    // the surrounding magenta window or an assumed window-size/device-pixel ratio.
    const auto itemRect = player.mapRectToScene(player.boundingRect());
    const qreal scaleX = image.width() / static_cast<qreal>(window.width());
    const qreal scaleY = image.height() / static_cast<qreal>(window.height());
    const QRect pixels(static_cast<int>(std::lround(itemRect.x() * scaleX)),
                       static_cast<int>(std::lround(itemRect.y() * scaleY)),
                       static_cast<int>(std::lround(itemRect.width() * scaleX)),
                       static_cast<int>(std::lround(itemRect.height() * scaleY)));
    return image.copy(pixels.intersected(image.rect()));
}
} // namespace

class VideoOverlayE2E : public QObject {
    Q_OBJECT
  private slots:
    void videoAndOverlayKeepIndependentPixels_data() {
        QTest::addColumn<QByteArray>("backend");
        QTest::newRow("atlas") << QByteArray("atlas");
        QTest::newRow("frame_image") << QByteArray("frame_image");
    }

    void videoAndOverlayKeepIndependentPixels() {
        QFETCH(QByteArray, backend);
        qputenv("NICONEON_DANMAKU_RENDERER", backend);
        qputenv("NICONEON_DANMAKU_WORKER", "off");
        qputenv("NICONEON_SIMD_MODE", "scalar");
        qputenv("NICONEON_MPV_AO", "null");
        QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

        // Use the test working directory: a GUI executor's temporary directory
        // may not be shared with the process that inspects failure artifacts.
        const auto fixture = QDir::current().absoluteFilePath(
            QStringLiteral("video-overlay-%1-fixture.y4m").arg(QString::fromLatin1(backend)));
        QVERIFY2(writeFixture(fixture), "could not create deterministic Y4M fixture");
        qInfo() << "video fixture=" << fixture;

        QQuickWindow window;
        QObject::connect(
            &window, &QQuickWindow::sceneGraphInitialized, &window,
            [] {
                const auto *context = QOpenGLContext::currentContext();
                if (!context)
                    return;
                const auto format = context->format();
                qInfo() << "video test actual context=" << format;
            },
            Qt::DirectConnection);
        window.setTitle(
            QStringLiteral("Niconeon video/overlay pixel regression: %1").arg(QString::fromLatin1(backend)));
        window.resize(windowWidth, windowHeight);
        // A non-gray window clear also detects a missing/invisible video surface.
        window.setColor(Qt::magenta);
        auto *player = new MpvItem(window.contentItem());
        player->setSize(QSizeF(windowWidth, windowHeight));
        QSignalSpy errors(player, &MpvItem::errorOccurred);

        // Exercise initial load before the scenegraph creates the libmpv renderer.
        QVERIFY(player->openFile(fixture));
        window.show();
        QVERIFY2(QTest::qWaitForWindowExposed(&window), "OpenGL test window did not become exposed");
        QCOMPARE(window.rendererInterface()->graphicsApi(), QSGRendererInterface::OpenGL);
        QTRY_VERIFY_WITH_TIMEOUT(player->positionMs() >= 150, 10000);
        QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));

        QImage baseline;
        for (int attempt = 0; attempt < 80; ++attempt) {
            baseline = capture(window, *player);
            if (isCleanGray(baseline))
                break;
        }
        QVERIFY2(isCleanGray(baseline), qPrintable(failureDiagnostic(QStringLiteral("mpv-only"), backend, baseline)));
        for (int frame = 0; frame < 4; ++frame) {
            const auto image = capture(window, *player);
            QVERIFY2(isCleanGray(image), qPrintable(failureDiagnostic(QStringLiteral("mpv-playing"), backend, image)));
        }
        player->setPaused(true);

        // Create the overlay only after establishing an uncontaminated mpv-only frame.
        auto *controller = new DanmakuController(window.contentItem());
        controller->setViewportSize(windowWidth, windowHeight);
        controller->setLaneMetrics(36, 6);
        controller->setPlaybackPaused(true);
        auto *overlay = new DanmakuRenderNodeItem(window.contentItem());
        overlay->setSize(QSizeF(windowWidth, windowHeight));
        overlay->setZ(1);
        overlay->setController(controller);
        controller->appendComments({QVariantMap{{QStringLiteral("comment_id"), QStringLiteral("pixel-check")},
                                                {QStringLiteral("user_id"), QStringLiteral("pixel-user")},
                                                {QStringLiteral("text"), QStringLiteral("WHITE OVERLAY CHECK")},
                                                {QStringLiteral("at_ms"), 0}}},
                                   2000);

        QImage withOverlay;
        PixelStats overlayStats;
        for (int attempt = 0; attempt < 80; ++attempt) {
            withOverlay = capture(window, *player);
            overlayStats = inspectPixels(withOverlay, interior(withOverlay));
            if (overlayStats.white >= 120)
                break;
        }
        QVERIFY2(overlayStats.white >= 120,
                 qPrintable(failureDiagnostic(QStringLiteral("missing-white-glyph"), backend, withOverlay)));
        // Text lives in the first lane. Check every remaining background pixel for dark grid lines.
        QVERIFY2(isCleanGray(withOverlay, 100),
                 qPrintable(failureDiagnostic(QStringLiteral("overlay-background"), backend, withOverlay, 100)));

        // Continue mpv updates while the renderer and its atlas/texture resources coexist.
        player->setPaused(false);
        for (int frame = 0; frame < 6; ++frame) {
            const auto image = capture(window, *player);
            QVERIFY2(isCleanGray(image, 100),
                     qPrintable(failureDiagnostic(QStringLiteral("playing-with-overlay"), backend, image, 100)));
        }
        overlay->setVisible(false);
        for (int frame = 0; frame < 6; ++frame) {
            const auto image = capture(window, *player);
            QVERIFY2(isCleanGray(image),
                     qPrintable(failureDiagnostic(QStringLiteral("hidden-overlay"), backend, image)));
            QCOMPARE(image.size(), baseline.size());
        }

        player->setPaused(true);
        for (const qint64 position : {qint64{0}, qint64{900}, qint64{0}}) {
            player->seek(position);
            QTRY_VERIFY_WITH_TIMEOUT(std::abs(player->positionMs() - position) <= 100, 5000);
            for (int frame = 0; frame < 3; ++frame) {
                const auto image = capture(window, *player);
                QVERIFY2(isCleanGray(image),
                         qPrintable(failureDiagnostic(QStringLiteral("seek-%1").arg(position), backend, image)));
            }
        }
        QVERIFY(errors.isEmpty());
        window.close();
    }
};

int main(int argc, char **argv) {
    niconeon::configureGraphicsEnvironment();
    QGuiApplication app(argc, argv);
    VideoOverlayE2E test;
    return QTest::qExec(&test, argc, argv);
}
#include "video_overlay_e2e.moc"
