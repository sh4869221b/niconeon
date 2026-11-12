#include "danmaku/DanmakuController.hpp"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

// Also compiles unchanged against the pre-worker controller (void admission).
// This intentionally excludes video/GPU/QoS to isolate GUI raster scheduling.
template <class Controller> int append(Controller &controller, const QVariantList &input, qint64 position) {
    if constexpr (std::is_void_v<decltype(controller.appendComments(input, position))>) {
        controller.appendComments(input, position);
        return input.size();
    } else {
        return controller.appendComments(input, position);
    }
}
template <class Controller> bool drainedCleanly(const Controller &controller) {
    if constexpr (requires { controller.rasterMetrics(); }) {
        const auto metrics = controller.rasterMetrics();
        return metrics.pending == 0 && metrics.failed == 0 && controller.pendingCommentCountForTesting() == 0 &&
               controller.expiredRasterComments() == 0;
    } else {
        return true; // Exact expected upload count below is the baseline oracle.
    }
}
static double percentile(QVector<qint64> values, double quantile) {
    std::sort(values.begin(), values.end());
    return values.isEmpty() ? 0 : values[std::min(values.size() - 1, qsizetype(quantile * values.size()))] / 1e6;
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    const int cps = argc > 1 ? std::clamp(atoi(argv[1]), 1, 400) : 200;
    const bool warm = argc > 2 && QString::fromLocal8Bit(argv[2]) == "warm";
    qputenv("NICONEON_DANMAKU_WORKER", "off");
    DanmakuController controller;
    controller.setViewportSize(1280, 720);
    controller.setGlyphWarmupEnabled(false);
    controller.setPlaybackPaused(false);
    QElapsedTimer clock;
    clock.start();
    qint64 previous = clock.nsecsElapsed();
    QVector<qint64> intervals;
    QVector<qint64> admission;
    QVariantList pending;
    int offered = 0;
    int accepted = 0;
    QSet<DanmakuSpriteId> uploadedIds;
    int badUploads = 0;
    const int expectedUploads = warm ? 32 : cps * 20;
    QTimer sample;
    sample.setTimerType(Qt::PreciseTimer);
    sample.setInterval(16);
    QObject::connect(&sample, &QTimer::timeout, &app, [&] {
        const qint64 now = clock.nsecsElapsed();
        intervals.push_back(now - previous);
        previous = now;
        for (const auto &upload : controller.takePendingSpriteUploads()) {
            if (upload.image.isNull() || !upload.spriteId || uploadedIds.contains(upload.spriteId))
                ++badUploads;
            uploadedIds.insert(upload.spriteId);
        }
        if (!pending.empty()) {
            const qint64 start = clock.nsecsElapsed();
            const int count = append(controller, pending, clock.elapsed());
            admission.push_back(clock.nsecsElapsed() - start);
            accepted += count;
            pending.erase(pending.begin(), pending.begin() + count);
        }
    });
    QTimer produce;
    produce.setTimerType(Qt::PreciseTimer);
    produce.setInterval(50);
    QObject::connect(&produce, &QTimer::timeout, &app, [&] {
        const int target = std::min(cps * 20, static_cast<int>(clock.elapsed() * cps / 1000));
        for (; offered < target; ++offered) {
            const int key = warm ? offered % 32 : offered;
            pending.push_back(QVariantMap{{"comment_id", QString::number(offered)},
                                          {"user_id", "fixture"},
                                          {"at_ms", offered * 1000 / cps},
                                          {"text", QStringLiteral("日本語 弾幕 %1 e\u0301").arg(key)}});
        }
        if (pending.size() > 1024) {
            fprintf(stderr, "benchmark producer overflow\n");
            app.exit(2);
        }
        if (offered == cps * 20 && pending.empty() && clock.elapsed() > 21000 &&
            uploadedIds.size() == expectedUploads && drainedCleanly(controller))
            app.quit();
    });
    QTimer::singleShot(30000, &app, [&] { app.exit(3); });
    sample.start();
    produce.start();
    int result = app.exec();
    if (accepted != cps * 20 || uploadedIds.size() != expectedUploads || badUploads || !drainedCleanly(controller))
        result = result ? result : 4;
    printf("cps=%d mode=%s offered=%d accepted=%d uploads=%d samples=%lld interval_p95_ms=%.3f interval_p99_ms=%.3f "
           "admission_p95_ms=%.3f admission_p99_ms=%.3f expected_uploads=%d bad_uploads=%d status=%d\n",
           cps, warm ? "warm" : "unique", offered, accepted, static_cast<int>(uploadedIds.size()),
           static_cast<long long>(intervals.size()), percentile(intervals, .95), percentile(intervals, .99),
           percentile(admission, .95), percentile(admission, .99), expectedUploads, badUploads, result);
    return result;
}
