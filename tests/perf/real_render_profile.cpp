#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuRenderNodeItem.hpp"
#include "danmaku/DanmakuRenderStyle.hpp"
#include "playback/GraphicsEnvironment.hpp"
#include "playback/MpvItem.hpp"

#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QPainter>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <mpv/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <mutex>
#include <type_traits>

// This source is intentionally compatible with the pre-raster-worker controller.
// Apply the same renderer diagnostics patch to both revisions. Missing diagnostics
// is a failed evidence prerequisite, never a successful empty-work benchmark.
namespace {
constexpr int width = 1280;
constexpr int height = 720;
constexpr int maxSamples = 100000;
qint64 monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
QString mpvVersion() {
    auto *handle = mpv_create();
    if (!handle)
        return {};
    mpv_set_option_string(handle, "vo", "null");
    mpv_set_option_string(handle, "ao", "null");
    QString result;
    if (mpv_initialize(handle) >= 0) {
        char *version = mpv_get_property_string(handle, "mpv-version");
        if (version) {
            result = QString::fromUtf8(version);
            mpv_free(version);
        }
    }
    mpv_terminate_destroy(handle);
    return result;
}
QString phaseName(int phase) {
    return phase == 1   ? QStringLiteral("feed")
           : phase == 2 ? QStringLiteral("drain")
           : phase == 3 ? QStringLiteral("transition")
                        : QStringLiteral("other");
}
struct Sample {
    qint64 elapsedNs = 0;
    qint64 intervalNs = 0;
    int phase = 0;
};
QJsonArray samplesJson(const QVector<Sample> &samples) {
    QJsonArray values;
    for (const auto &sample : samples)
        values.append(QJsonObject{
            {"elapsed_ns", sample.elapsedNs}, {"interval_ns", sample.intervalNs}, {"phase", phaseName(sample.phase)}});
    return values;
}
bool waitFor(const std::function<bool()> &predicate, int timeoutMs) {
    QElapsedTimer clock;
    clock.start();
    while (!predicate() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return predicate();
}
template <class Controller> qreal motionTime(const Controller &controller) {
    if constexpr (requires { controller.motionTime(); })
        return controller.motionTime();
    return -1;
}
template <class Controller>
int append(Controller &controller, const QVariantList &comments, qint64 positionMs, qreal motion) {
    if constexpr (requires { controller.appendComments(comments, positionMs, motion); })
        return controller.appendComments(comments, positionMs, motion);
    else if constexpr (std::is_void_v<decltype(controller.appendComments(comments, positionMs))>) {
        controller.appendComments(comments, positionMs);
        return static_cast<int>(comments.size());
    } else
        return controller.appendComments(comments, positionMs);
}
template <class Controller> QJsonObject rasterSummary(const Controller &controller) {
    if constexpr (requires { controller.rasterMetrics(); }) {
        const auto stats = controller.rasterMetrics();
        return {{"pending", stats.pending + controller.pendingCommentCountForTesting()},
                {"failed", static_cast<qint64>(stats.failed)},
                {"expired", static_cast<qint64>(controller.expiredRasterComments())},
                {"rasterized", static_cast<qint64>(stats.rasterized)},
                {"raster_counters_available", true}};
    }
    return {{"pending", 0}, {"failed", 0}, {"expired", 0}, {"raster_counters_available", false}};
}
QString workloadText(int sequence, bool warm) {
    const auto key = warm ? sequence % 32 : sequence;
    const QStringList stems{QStringLiteral("日本語 弾幕 漢字かなカナ"), QStringLiteral("A e\u0301 العربية"),
                            QStringLiteral("混合文字 👩‍💻 ❤️"),
                            QStringLiteral("比較用コメント カタカナ")};
    return stems.at(key % stems.size()) + QStringLiteral(" [%1]").arg(key, 6, 10, QLatin1Char('0'));
}
QVariantMap comment(int sequence, const QString &text, qint64 atMs) {
    return {{"comment_id", QStringLiteral("perf-%1").arg(sequence)},
            {"user_id", QStringLiteral("fixture")},
            {"text", text},
            {"at_ms", atMs}};
}
struct Diagnostics {
    bool available = false;
    qint64 missingSprites = 0;
    qint64 missingImages = 0;
    qint64 unresident = 0;
    qint64 overflow = 0;
    QSet<QString> submittedIds;
    QVector<DanmakuRenderFrameDiagnostics> frames;
    QVector<DanmakuRenderSubmissionEvent> submissions;
};
// Only move raw structs during timing; JSON, ID sets and file I/O wait until
// measurement ends. The renderer's bounded observer is identical in both arms.
void collectDiagnostics(DanmakuRenderNodeItem &item, Diagnostics &output) {
    auto batch = item.takeRenderDiagnostics();
    output.available = batch.enabled;
    output.overflow = std::max(output.overflow, qint64(batch.droppedFrames + batch.droppedSubmissionObservations));
    output.frames.append(std::move(batch.frames));
    output.submissions.append(std::move(batch.submissions));
}
QJsonArray renderSamplesJson(const Diagnostics &diagnostics, qint64 epoch) {
    QJsonArray output;
    for (const auto &frame : diagnostics.frames)
        output.append(QJsonObject{{"frame_sequence", static_cast<qint64>(frame.frameSequence)},
                                  {"elapsed_ns", frame.capturedAtNs - epoch},
                                  {"set_frame_ns", frame.setFrameNs},
                                  {"normalize_ns", frame.normalizeNs},
                                  {"residency_ns", frame.residencyNs},
                                  {"repack_ns", frame.repackNs},
                                  {"sprite_copy_ns", frame.spriteCopyNs},
                                  {"page_clear_ns", frame.pageClearNs},
                                  {"gl_allocation_ns", frame.glAllocationNs},
                                  {"gl_upload_ns", frame.glUploadNs},
                                  {"render_cpu_ns", frame.renderCpuNs},
                                  {"gl_allocation_bytes", static_cast<qint64>(frame.glAllocationBytes)},
                                  {"gl_upload_bytes", static_cast<qint64>(frame.glUploadBytes)},
                                  {"sprite_copy_bytes", static_cast<qint64>(frame.spriteCopyBytes)},
                                  {"page_clear_bytes", static_cast<qint64>(frame.pageClearBytes)},
                                  {"atlas_pages_uploaded", frame.atlasPagesUploaded},
                                  {"repack_attempts", frame.repackAttempts},
                                  {"repack_successes", frame.repackSuccesses},
                                  {"repacked_sprites", frame.repackedSprites},
                                  {"received_sprites", frame.receivedSprites},
                                  {"received_sprite_bytes", static_cast<qint64>(frame.receivedSpriteBytes)},
                                  {"active_instances", frame.activeInstances},
                                  {"active_unique_sprites", frame.activeUniqueSprites},
                                  {"missing_image_unique", frame.missingImageUnique},
                                  {"unresident_unique", frame.unresidentUnique},
                                  {"submitted_instances", frame.submittedInstances},
                                  {"draw_calls", frame.drawCalls}});
    return output;
}
QJsonArray submissionsJson(const Diagnostics &diagnostics, qint64 epoch) {
    QJsonArray output;
    for (const auto &submission : diagnostics.submissions)
        output.append(QJsonObject{{"comment_id", submission.commentId},
                                  {"sprite_id", static_cast<qint64>(submission.spriteId)},
                                  {"frame_sequence", static_cast<qint64>(submission.frameSequence)},
                                  {"elapsed_ns", submission.capturedAtNs - epoch}});
    return output;
}
bool writeGrayFixture(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const QByteArray header("YUV4MPEG2 W160 H90 F30:1 Ip A1:1 C420jpeg\n");
    if (file.write(header) != header.size())
        return false;
    const QByteArray luma(160 * 90, char(100));
    const QByteArray chroma(160 * 90 / 2, char(128));
    for (int frame = 0; frame < 300; ++frame)
        if (file.write("FRAME\n", 6) != 6 || file.write(luma) != luma.size() || file.write(chroma) != chroma.size())
            return false;
    return file.flush();
}
QImage referenceSprite(const QString &text, qreal dpr) {
    QFont font = QGuiApplication::font();
    font.setPixelSize(DanmakuRenderStyle::kTextPixelSize);
    const int logicalWidth =
        std::max(DanmakuRenderStyle::kMinWidthPx,
                 QFontMetrics(font).horizontalAdvance(text) + 2 * DanmakuRenderStyle::kHorizontalPaddingPx);
    QImage image(QSize(static_cast<int>(std::ceil(logicalWidth * dpr)),
                       static_cast<int>(std::ceil(DanmakuRenderStyle::kItemHeightPx * dpr))),
                 QImage::Format_RGBA8888_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRectF(0, 0, logicalWidth, DanmakuRenderStyle::kItemHeightPx), Qt::AlignVCenter | Qt::AlignHCenter,
                     text);
    return image;
}
bool cleanGray(const QImage &image) {
    if (image.isNull())
        return false;
    for (int y = 4; y < image.height() - 4; ++y)
        for (int x = 4; x < image.width() - 4; ++x) {
            const auto pixel = image.pixelColor(x, y);
            if (std::abs(pixel.red() - 98) > 8 || std::abs(pixel.green() - 98) > 8 || std::abs(pixel.blue() - 98) > 8)
                return false;
        }
    return true;
}
QJsonArray pixelOracle(QQuickWindow &window, MpvItem &player, DanmakuController &controller,
                       DanmakuRenderNodeItem &overlay, const QString &outputPath, QStringList &errors) {
    QJsonArray results;
    controller.setPlaybackPaused(true);
    controller.setGlyphWarmupEnabled(false);
    player.setPaused(true);
    waitFor([&] { return player.paused(); }, 2000);
    overlay.setVisible(false);
    QImage background;
    const bool videoOk = waitFor(
        [&] {
            window.requestUpdate();
            background = window.grabWindow();
            return cleanGray(background);
        },
        5000);
    if (!videoOk) {
        background.save(outputPath + QStringLiteral(".bad-video.png"));
        errors.append(QStringLiteral("Known-gray video pixels are missing/corrupt"));
        return results;
    }
    const QStringList texts{QStringLiteral("日本語コメント 漢字かなカナ"), QStringLiteral("A e\u0301 العربية"),
                            QStringLiteral("👩‍💻 ❤️ 比較用コメント"),
                            QStringLiteral("Latin 0123456789")};
    for (int index = 0; index < texts.size(); ++index) {
        controller.resetForSeek();
        overlay.setVisible(true);
        append(controller, {comment(index, texts[index], 0)}, 2000, motionTime(controller));
        QImage actual;
        QImage expected;
        qint64 incorrect = 0;
        qint64 ink = 0;
        int maxError = 0;
        const bool equal = waitFor(
            [&] {
                window.requestUpdate();
                actual = window.grabWindow();
                const auto snapshot = controller.renderSnapshot();
                if (!snapshot || snapshot->instances.size() != 1 || actual.size() != background.size())
                    return false;
                const qreal dpr = window.effectiveDevicePixelRatio();
                const auto sprite = referenceSprite(texts[index], dpr);
                expected = background.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
                expected.setDevicePixelRatio(dpr);
                {
                    QPainter painter(&expected);
                    const auto &instance = snapshot->instances.first();
                    painter.drawImage(QPointF(instance.x, instance.y), sprite);
                }
                incorrect = ink = 0;
                maxError = 0;
                for (int y = 4; y < actual.height() - 4; ++y)
                    for (int x = 4; x < actual.width() - 4; ++x) {
                        const auto wanted = expected.pixelColor(x, y);
                        const auto found = actual.pixelColor(x, y);
                        const int difference =
                            std::max({std::abs(wanted.red() - found.red()), std::abs(wanted.green() - found.green()),
                                      std::abs(wanted.blue() - found.blue())});
                        maxError = std::max(maxError, difference);
                        incorrect += difference > 8;
                        ink += std::min({wanted.red(), wanted.green(), wanted.blue()}) >= 220;
                    }
                return incorrect == 0 && ink >= 120;
            },
            5000);
        const QString capture = outputPath + QStringLiteral(".text-%1.png").arg(index);
        actual.save(capture);
        if (!equal) {
            expected.save(outputPath + QStringLiteral(".expected-%1.png").arg(index));
            errors.append(QStringLiteral("Pixel reference mismatch for text case %1").arg(index));
        }
        results.append(QJsonObject{{"text", texts[index]},
                                   {"success", equal},
                                   {"incorrect_pixels", incorrect},
                                   {"reference_ink_pixels", ink},
                                   {"max_channel_error", maxError},
                                   {"capture", capture}});
    }
    return results;
}
} // namespace

int main(int argc, char **argv) {
    // qHash(commentId) sets speed. Its randomized default changes the workload.
    qputenv("QT_HASH_SEED", "0");
    niconeon::configureGraphicsEnvironment();
    QGuiApplication app(argc, argv);
    QGuiApplication::setFont(QFont(QStringLiteral("DejaVu Sans")));
    QCoreApplication::setApplicationName(QStringLiteral("real_render_profile"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Equal-work OpenGL/mpv renderer benchmark; no adaptive QoS"));
    parser.addHelpOption();
    parser.addOptions(
        {QCommandLineOption(QStringList{"video"}, "Identical local video fixture", "path"),
         QCommandLineOption(QStringList{"output"}, "Raw JSON output (required)", "path"),
         QCommandLineOption(QStringList{"cps"}, "Comments per second", "integer", "200"),
         QCommandLineOption(QStringList{"duration-ms"}, "Comment generation duration", "integer", "30000"),
         QCommandLineOption(QStringList{"tail-ms"}, "Fixed drain tail after generation", "integer", "15000"),
         QCommandLineOption(QStringList{"text-mode"}, "unique or warm (32 texts)", "mode", "unique"),
         QCommandLineOption(QStringList{"sample-mode"}, "timing or pixels (separate, untimed correctness run)", "mode",
                            "timing"),
         QCommandLineOption(QStringList{"worker"}, "Simulation worker on or off", "mode", "on"),
         QCommandLineOption(QStringList{"renderer"}, "atlas or frame_image", "mode", "atlas")});
    parser.process(app);
    const QString outputPath = parser.value("output");
    const QString textMode = parser.value("text-mode");
    const QString sampleMode = parser.value("sample-mode");
    const QString worker = parser.value("worker");
    const QString renderer = parser.value("renderer");
    bool cpsOk = false, durationOk = false, tailOk = false;
    const int cps = parser.value("cps").toInt(&cpsOk);
    const int durationMs = parser.value("duration-ms").toInt(&durationOk);
    const int tailMs = parser.value("tail-ms").toInt(&tailOk);
    if (outputPath.isEmpty() || !cpsOk || cps < 1 || cps > 2000 || !durationOk || durationMs < 1000 ||
        durationMs > 120000 || !tailOk || tailMs < 1000 || tailMs > 60000 || qint64(cps) * durationMs / 1000 > 60000 ||
        (textMode != "unique" && textMode != "warm") || (sampleMode != "timing" && sampleMode != "pixels") ||
        (worker != "on" && worker != "off") || (renderer != "atlas" && renderer != "frame_image")) {
        fprintf(stderr, "invalid/unsafe profile options; use --help\n");
        return 2;
    }
    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    qputenv("NICONEON_DANMAKU_WORKER", worker.toUtf8());
    qputenv("NICONEON_DANMAKU_RENDERER", renderer.toUtf8());
    qputenv("NICONEON_RENDER_DIAGNOSTICS", "1");
    qputenv("NICONEON_MPV_AO", "null");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QString videoPath = parser.value("video");
    QStringList errors;
    if (sampleMode == "pixels") {
        videoPath = outputPath + QStringLiteral(".gray.y4m");
        if (!writeGrayFixture(videoPath))
            errors.append(QStringLiteral("Unable to generate known-gray pixel fixture"));
    }
    QFile videoFile(videoPath);
    QByteArray videoSha;
    if (videoFile.open(QIODevice::ReadOnly)) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&videoFile);
        videoSha = hash.result().toHex();
        videoFile.close();
    } else
        errors.append(QStringLiteral("Unable to open video fixture"));
    QJsonObject glMetadata;
    QJsonObject metadata{{"cps", cps},
                         {"duration_ms", durationMs},
                         {"tail_ms", tailMs},
                         {"text_mode", textMode},
                         {"sample_mode", sampleMode},
                         {"worker", worker},
                         {"renderer", renderer},
                         {"qos", "disabled-in-dedicated-render-harness"},
                         {"qt", qVersion()},
                         {"qt_version", qVersion()},
                         {"mpv_version", mpvVersion()},
                         {"mpv_client_api", static_cast<qint64>(mpv_client_api_version())},
                         {"font", QGuiApplication::font().toString()},
                         {"snapshot_tracking", "not sampled; actual draw IDs are the completion oracle"},
                         {"qt_hash_seed", "0"},
                         {"video_sha256", QString::fromLatin1(videoSha)},
                         {"width", width},
                         {"height", height},
                         {"frame_metric", "QQuickWindow::frameSwapped interval; not physical scanout"},
                         {"completion_metric", "distinct IDs submitted to successful GL draw path"}};
    std::atomic<int> phase = 0;
    const qint64 epoch = monotonicNs();
    std::mutex sampleMutex;
    QVector<Sample> frames;
    QVector<Sample> heartbeat;
    frames.reserve(16000);
    heartbeat.reserve(16000);
    qint64 previousFrame = 0;
    int previousFramePhase = 0;
    int sampleOverflow = 0;
    QQuickWindow window;
    window.setTitle(QStringLiteral("Niconeon equal-work renderer profile"));
    window.resize(width, height);
    window.setColor(Qt::magenta);
    auto *player = new MpvItem(window.contentItem());
    player->setSize(QSizeF(width, height));
    DanmakuController controller;
    controller.setViewportSize(width, height);
    controller.setLaneMetrics(36, 6);
    controller.setTargetFps(60);
    controller.setPlaybackRate(1);
    controller.setPlaybackPaused(true);
    controller.setGlyphWarmupEnabled(true);
    auto *overlay = new DanmakuRenderNodeItem(window.contentItem());
    overlay->setSize(QSizeF(width, height));
    overlay->setClip(true);
    overlay->setZ(1);
    overlay->setController(&controller);
    // Retain the production QML glyph-warmup item, without unrelated application
    // controls. The real application/QoS run is a separate required experiment.
    QQmlEngine qml;
    QQmlComponent warmup(&qml);
    warmup.setData("import QtQuick\nText { property var controller; width: 1; height: 1; clip: true; "
                   "color: 'white'; opacity: 0.01; font.pixelSize: 24; z: -1; "
                   "visible: controller ? controller.glyphWarmupEnabled : false; "
                   "text: controller ? controller.glyphWarmupText : '' }",
                   QUrl());
    auto *warmupItem = qobject_cast<QQuickItem *>(warmup.createWithInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue<QObject *>(&controller)}}));
    if (warmupItem)
        warmupItem->setParentItem(overlay);
    else
        errors.append(QStringLiteral("Unable to create production-equivalent glyph warmup item"));
    QObject::connect(player, &MpvItem::errorOccurred, &app, [&](const QString &error) { errors.append(error); });
    QObject::connect(
        &window, &QQuickWindow::sceneGraphInitialized, &window,
        [&] {
            const auto *context = QOpenGLContext::currentContext();
            if (context) {
                auto *gl = context->functions();
                std::lock_guard guard(sampleMutex);
                glMetadata["gl_renderer"] =
                    QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_RENDERER)));
                glMetadata["gl_vendor"] =
                    QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_VENDOR)));
                glMetadata["gl_version"] =
                    QString::fromLatin1(reinterpret_cast<const char *>(gl->glGetString(GL_VERSION)));
            }
        },
        Qt::DirectConnection);
    QObject::connect(
        &window, &QQuickWindow::frameSwapped, &window,
        [&] {
            const int currentPhase = phase.load(std::memory_order_relaxed);
            const qint64 now = monotonicNs();
            std::lock_guard guard(sampleMutex);
            if (currentPhase > 0 && previousFramePhase > 0 && previousFrame) {
                if (frames.size() < maxSamples)
                    frames.append(
                        {now - epoch, now - previousFrame, currentPhase == previousFramePhase ? currentPhase : 3});
                else
                    ++sampleOverflow;
            }
            previousFrame = now;
            previousFramePhase = currentPhase;
        },
        Qt::DirectConnection);
    if (errors.isEmpty() && !player->openFile(videoPath))
        errors.append(QStringLiteral("mpv refused fixture"));
    window.show();
    const bool started = errors.isEmpty() && waitFor([&] { return player->positionMs() >= 500; }, 15000);
    {
        std::lock_guard guard(sampleMutex);
        for (auto it = glMetadata.constBegin(); it != glMetadata.constEnd(); ++it)
            metadata[it.key()] = it.value();
    }
    metadata["video_started"] = started;
    metadata["video_duration_ms"] = player->durationMs();
    metadata["device_pixel_ratio"] = window.effectiveDevicePixelRatio();
    if (!started)
        errors.append(QStringLiteral("Video did not start advancing"));
    if (window.width() != width || window.height() != height)
        errors.append(QStringLiteral("Window manager changed benchmark viewport dimensions"));
    if (window.rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL)
        errors.append(QStringLiteral("Real OpenGL scene graph is unavailable"));
    QJsonObject summary;
    QJsonArray pixels;
    Diagnostics diagnostics;
    int expected = cps * durationMs / 1000;
    int offered = 0;
    int accepted = 0;
    int sourcePending = 0;
    int feedOffered = -1;
    int feedAccepted = -1;
    if (errors.isEmpty() && sampleMode == "pixels") {
        pixels = pixelOracle(window, *player, controller, *overlay, outputPath, errors);
        collectDiagnostics(*overlay, diagnostics);
    } else if (errors.isEmpty()) {
        // Warm the identical video pipeline, then rewind while paused. A sampled
        // startup position would select different media intervals between runs.
        player->setPaused(true);
        const auto rewind = player->seek(0);
        if (!rewind || !waitFor([&] { return player->positionMs() == 0; }, 5000))
            errors.append(QStringLiteral("Could not establish exact media-zero start"));
        const qint64 mediaStart = 0;
        metadata["media_start_ms"] = mediaStart;
        if (player->durationMs() < mediaStart + durationMs + tailMs + 500)
            errors.append(QStringLiteral("Video is too short for fixed feed plus drain window"));
        struct Batch {
            QVariantList comments;
            qint64 position = 0;
            qreal motion = 0;
        };
        QQueue<Batch> queue;
        QTimer produce;
        produce.setTimerType(Qt::PreciseTimer);
        produce.setInterval(50);
        QObject::connect(&produce, &QTimer::timeout, &app, [&] {
            const auto elapsed = std::max(qint64(0), player->positionMs() - mediaStart);
            const int target = static_cast<int>(std::min(qint64(expected), elapsed * cps / 1000));
            if (queue.size() < 2 && offered < target) {
                Batch batch;
                batch.position = player->positionMs();
                batch.motion = motionTime(controller);
                for (; offered < target; ++offered)
                    batch.comments.append(comment(offered, workloadText(offered, textMode == "warm"),
                                                  mediaStart + qint64(offered) * 1000 / cps));
                queue.enqueue(std::move(batch));
            }
        });
        QTimer drain;
        drain.setTimerType(Qt::PreciseTimer);
        drain.setInterval(16);
        qint64 previousHeartbeat = monotonicNs();
        int previousHeartbeatPhase = 1;
        QObject::connect(&drain, &QTimer::timeout, &app, [&] {
            const qint64 now = monotonicNs();
            const qint64 elapsed = player->positionMs() - mediaStart;
            const int currentPhase = elapsed < durationMs ? 1 : 2;
            phase.store(currentPhase, std::memory_order_relaxed);
            if (currentPhase == 2 && previousHeartbeatPhase == 1) {
                metadata["feed_end_elapsed_ns"] = now - epoch;
                feedOffered = offered;
                feedAccepted = accepted;
            }
            {
                if (heartbeat.size() < maxSamples)
                    heartbeat.append({now - epoch, now - previousHeartbeat,
                                      currentPhase == previousHeartbeatPhase ? currentPhase : 3});
                else {
                    std::lock_guard guard(sampleMutex);
                    ++sampleOverflow;
                }
            }
            previousHeartbeat = now;
            previousHeartbeatPhase = currentPhase;
            if (!queue.isEmpty()) {
                auto &batch = queue.head();
                const auto chunk = batch.comments.mid(0, 64);
                const int count = append(controller, chunk, batch.position, batch.motion);
                if (count < 0 || count > chunk.size()) {
                    errors.append(QStringLiteral("Invalid accepted-prefix count"));
                    app.quit();
                    return;
                }
                batch.comments.erase(batch.comments.begin(), batch.comments.begin() + count);
                accepted += count;
                if (batch.comments.isEmpty())
                    queue.dequeue();
            }
            collectDiagnostics(*overlay, diagnostics);
            if (elapsed >= durationMs + tailMs)
                app.quit();
        });
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, &app, [&] {
            errors.append(QStringLiteral("Fixed media window did not finish before wall-clock safety deadline"));
            app.quit();
        });
        if (errors.isEmpty()) {
            // Clear warmup-only diagnostics before collecting the comment workload.
            collectDiagnostics(*overlay, diagnostics);
            diagnostics = {};
            phase.store(1, std::memory_order_relaxed);
            previousHeartbeat = monotonicNs();
            metadata["measurement_start_elapsed_ns"] = previousHeartbeat - epoch;
            controller.setPlaybackPaused(false);
            player->setPaused(false);
            produce.start();
            drain.start();
            deadline.start((durationMs + tailMs) * 2 + 10000);
            app.exec();
            metadata["measurement_end_elapsed_ns"] = monotonicNs() - epoch;
            phase.store(0, std::memory_order_relaxed);
            produce.stop();
            drain.stop();
            deadline.stop();
            controller.setPlaybackPaused(true);
            player->setPaused(true);
            collectDiagnostics(*overlay, diagnostics);
            for (const auto &batch : queue)
                sourcePending += batch.comments.size();
        }
        metadata["media_end_ms"] = player->positionMs();
        summary = rasterSummary(controller);
        for (const auto &frame : diagnostics.frames) {
            diagnostics.missingImages += frame.missingImageUnique;
            diagnostics.unresident += frame.unresidentUnique;
        }
        diagnostics.missingSprites = diagnostics.missingImages + diagnostics.unresident;
        int feedSubmitted = 0;
        qint64 lastSubmission = 0;
        for (const auto &submission : diagnostics.submissions) {
            diagnostics.submittedIds.insert(submission.commentId);
            const qint64 at = submission.capturedAtNs - epoch;
            feedSubmitted += at <= metadata["feed_end_elapsed_ns"].toInteger();
            lastSubmission = std::max(lastSubmission, at);
        }
        summary["feed_offered"] = feedOffered;
        summary["feed_accepted"] = feedAccepted;
        summary["feed_submitted_unique"] = feedSubmitted;
        summary["last_submission_elapsed_ns"] = lastSubmission;
        int unexpectedIds = 0;
        for (const auto &id : diagnostics.submittedIds) {
            bool parsed = false;
            const int index = id.mid(5).toInt(&parsed);
            unexpectedIds += !parsed || index < 0 || index >= expected || id != QStringLiteral("perf-%1").arg(index);
        }
        summary["expected"] = expected;
        summary["offered"] = offered;
        summary["accepted"] = accepted;
        summary["snapshot_unique"] = -1;
        summary["submitted_unique"] = diagnostics.submittedIds.size();
        summary["source_pending"] = sourcePending;
        summary["unexpected_ids"] = unexpectedIds;
        summary["missing_sprites"] = diagnostics.missingSprites;
        summary["missing_image_observations"] = diagnostics.missingImages;
        summary["unresident_observations"] = diagnostics.unresident;
        QJsonArray neverSubmitted;
        for (int index = 0; index < expected; ++index) {
            const auto id = QStringLiteral("perf-%1").arg(index);
            if (!diagnostics.submittedIds.contains(id))
                neverSubmitted.append(id);
        }
        summary["never_submitted_ids"] = neverSubmitted;
        summary["diagnostics_overflow"] = diagnostics.overflow;
        summary["dropped"] = 0;
        if (!diagnostics.available)
            errors.append(QStringLiteral("Renderer diagnostics unavailable; apply identical observer to both builds"));
        if (offered != expected || accepted != expected || diagnostics.submittedIds.size() != expected ||
            unexpectedIds || sourcePending || summary["pending"].toInteger() || summary["failed"].toInteger() ||
            summary["expired"].toInteger() || diagnostics.missingSprites || diagnostics.overflow)
            errors.append(QStringLiteral("Equal-work/complete-text prerequisites failed"));
    }
    metadata["media_end_ms"] = player->positionMs();
    window.hide();
    std::lock_guard guard(sampleMutex);
    if (sampleOverflow)
        errors.append(QStringLiteral("Raw sample capacity exceeded"));
    summary["sample_overflow"] = sampleOverflow;
    summary["frame_count"] = frames.size();
    summary["heartbeat_count"] = heartbeat.size();
    QJsonObject result{{"format_version", 1},
                       {"metadata", metadata},
                       {"success", errors.isEmpty()},
                       {"errors", QJsonArray::fromStringList(errors)},
                       {"summary", summary},
                       {"frame_samples", samplesJson(frames)},
                       {"heartbeat_samples", samplesJson(heartbeat)},
                       {"render_samples", renderSamplesJson(diagnostics, epoch)},
                       {"submission_samples", submissionsJson(diagnostics, epoch)},
                       {"pixel_checks", pixels}};
    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(result).toJson()) < 0 || !output.commit()) {
        fprintf(stderr, "cannot write benchmark JSON\n");
        return 3;
    }
    if (sampleMode == "pixels")
        fprintf(stdout, "mode=pixels success=%d checks=%lld output=%s\n", errors.isEmpty(),
                static_cast<long long>(pixels.size()), qPrintable(outputPath));
    else
        fprintf(stdout, "mode=timing success=%d expected=%d offered=%d accepted=%d submitted=%lld output=%s\n",
                errors.isEmpty(), expected, offered, accepted, static_cast<long long>(diagnostics.submittedIds.size()),
                qPrintable(outputPath));
    return errors.isEmpty() ? 0 : 1;
}
