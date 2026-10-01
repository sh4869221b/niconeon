#include "danmaku/DanmakuTextSpriteCache.hpp"
#include "danmaku/DanmakuRenderStyle.hpp"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
qint64 elapsedNs(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}
} // namespace

struct DanmakuTextSpriteCache::State {
    struct Request {
        SpriteKey key;
        QFont font;
        DanmakuSpriteId id = 0;
        quint64 generation = 0;
        qint64 bytes = 0;
        Clock::time_point submitted;
    };
    struct Completion {
        Request request;
        DanmakuSpriteUpload upload;
        qint64 rasterCompletedAtNs = 0;
    };
    explicit State(Limits requested, BeforeRaster hook)
        : limits{std::clamp(requested.pendingRequests, 1, 128),
                 std::clamp<qint64>(requested.requestBytes, 2, 4 * 1024 * 1024),
                 std::clamp(requested.completedSprites, 1, 32),
                 std::clamp<qint64>(requested.completedBytes, 1, MaxSpriteBytes)},
          beforeRaster(std::move(hook)), worker([this] { run(); }) {}

    void run() {
        for (;;) {
            Request request;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [this] { return closing || !requests.empty(); });
                if (closing)
                    break;
                request = std::move(requests.front());
                requests.pop_front();
                stats.queued = static_cast<int>(requests.size());
            }
            const auto started = Clock::now();
            DanmakuSpriteUpload upload;
            upload.spriteId = request.id;
            try {
                if (beforeRaster)
                    beforeRaster();
                // Both font shaping/measurement and painting happen exclusively
                // here. The GUI provides an independent QFont value snapshot.
                QFontMetrics metrics(request.font);
                const int width =
                    std::max(DanmakuRenderStyle::kMinWidthPx, metrics.horizontalAdvance(request.key.text) +
                                                                  2 * DanmakuRenderStyle::kHorizontalPaddingPx);
                const qreal dpr = request.key.devicePixelRatioMilli / 1000.0;
                const double pixelWidth = std::ceil(width * dpr);
                const double pixelHeight = std::ceil(DanmakuRenderStyle::kItemHeightPx * dpr);
                if (pixelWidth > 0 && pixelHeight > 0 && pixelWidth * pixelHeight * 4 <= MaxSpriteBytes) {
                    upload.logicalSize = QSize(width, DanmakuRenderStyle::kItemHeightPx);
                    upload.image = QImage(QSize(static_cast<int>(pixelWidth), static_cast<int>(pixelHeight)),
                                          QImage::Format_RGBA8888_Premultiplied);
                    upload.image.setDevicePixelRatio(dpr);
                    upload.image.fill(Qt::transparent);
                    QPainter painter(&upload.image);
                    painter.setRenderHint(QPainter::TextAntialiasing, true);
                    painter.setFont(request.font);
                    painter.setPen(Qt::white);
                    painter.drawText(QRectF(QPointF(0, 0), QSizeF(upload.logicalSize)),
                                     Qt::AlignVCenter | Qt::AlignHCenter, request.key.text);
                }
            } catch (...) {
                upload.image = {};
            }
            const qint64 duration = elapsedNs(started);
            const qint64 completedAtNs =
                diagnosticsEnabled
                    ? std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()
                    : 0;
            const qint64 bytes = upload.image.sizeInBytes();
            {
                std::unique_lock lock(mutex);
                ++stats.rasterized;
                stats.rasterNs += duration;
                stats.maxRasterNs = std::max(stats.maxRasterNs, duration);
                // One oversize completion may occupy an otherwise empty queue;
                // its allocation still cannot exceed MaxSpriteBytes.
                changed.wait(lock, [&] {
                    return closing || request.generation != generation ||
                           (completions.size() < static_cast<size_t>(limits.completedSprites) &&
                            (completions.empty() || stats.completionBytes + bytes <= limits.completedBytes));
                });
                if (closing || request.generation != generation) {
                    ++stats.stale;
                    continue;
                }
                if (upload.image.isNull())
                    ++stats.failed;
                stats.completionBytes += bytes;
                completions.push_back({std::move(request), std::move(upload), completedAtNs});
                stats.completed = static_cast<int>(completions.size());
                stats.completionHighWater = std::max(stats.completionHighWater, stats.completed);
                stats.completionBytesHighWater = std::max(stats.completionBytesHighWater, stats.completionBytes);
            }
        }
        std::lock_guard lock(mutex);
        stopped = true;
        stats.shutdownNs = elapsedNs(shutdownStarted);
    }

    const Limits limits;
    const BeforeRaster beforeRaster;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::deque<Request> requests;
    std::deque<Completion> completions;
    Metrics stats;
    quint64 generation = 1;
    bool closing = false;
    bool stopped = false;
    Clock::time_point shutdownStarted;
    const bool diagnosticsEnabled = qEnvironmentVariableIntValue("NICONEON_RENDER_DIAGNOSTICS") == 1;
    // Last member: all state is initialized before the worker may read it.
    std::thread worker;
};

DanmakuTextSpriteCache::DanmakuTextSpriteCache() : DanmakuTextSpriteCache(Limits{}) {}
DanmakuTextSpriteCache::DanmakuTextSpriteCache(Limits limits, BeforeRaster beforeRaster)
    : m_state(std::make_unique<State>(limits, std::move(beforeRaster))), m_font(QGuiApplication::font()) {}
DanmakuTextSpriteCache::~DanmakuTextSpriteCache() {
    shutdown();
    // Normal application close first waits asynchronously for isStopped(). A
    // direct owner destruction (including tests) joins only the current bounded
    // paint; queued work is cancelled and no GUI event delivery is needed.
    m_state->worker.join();
}

void DanmakuTextSpriteCache::clear() {
    cancelPending();
    m_records.clear();
}
void DanmakuTextSpriteCache::cancelPending() {
    for (const auto &key : std::as_const(m_pendingKeys))
        m_records.remove(key);
    m_pendingKeys.clear();
    {
        std::lock_guard lock(m_state->mutex);
        ++m_state->generation;
        m_state->stats.cancelled += m_state->stats.pending;
        m_state->requests.clear();
        m_state->completions.clear();
        m_state->stats.pending = m_state->stats.queued = m_state->stats.completed = 0;
        m_state->stats.requestBytes = m_state->stats.completionBytes = 0;
    }
    m_state->changed.notify_one();
}
void DanmakuTextSpriteCache::setFont(const QFont &font) {
    if (m_font == font)
        return;
    m_font = font;
    clear();
}
DanmakuTextSpriteCache::EnsureResult DanmakuTextSpriteCache::ensureSprite(const QString &text, int fontPixelSize,
                                                                          qreal devicePixelRatio) {
    if (!std::isfinite(devicePixelRatio) || devicePixelRatio > 16 || text.size() > MaxTextCodeUnits ||
        fontPixelSize <= 0 || fontPixelSize > 256)
        return {0, 0, false, false, true};
    const SpriteKey key{text, fontPixelSize,
                        static_cast<int>(std::lround(std::max<qreal>(1, devicePixelRatio) * 1000))};
    const auto it = m_records.constFind(key);
    if (it != m_records.cend()) {
        if (!it->ready && !it->failed) {
            std::lock_guard lock(m_state->mutex);
            ++m_state->stats.coalesced;
        }
        return {it->spriteId, it->width, false, it->ready, it->failed, it->rasterCompletedAtNs, it->guiReadyAtNs};
    }
    const qint64 bytes = text.size() * sizeof(QChar);
    std::lock_guard lock(m_state->mutex);
    if (m_state->closing)
        return {};
    if (bytes > m_state->limits.requestBytes)
        return {0, 0, false, false, true};
    if (m_state->stats.pending >= m_state->limits.pendingRequests ||
        m_state->stats.requestBytes + bytes > m_state->limits.requestBytes) {
        ++m_state->stats.backpressured;
        return {};
    }
    const DanmakuSpriteId id = m_nextSpriteId++;
    QFont font = m_font;
    font.setPixelSize(fontPixelSize);
    m_state->requests.push_back({key, std::move(font), id, m_state->generation, bytes, Clock::now()});
    m_records.insert(key, {id, 0, false, false});
    m_pendingKeys.insert(key);
    ++m_state->stats.pending;
    m_state->stats.queued = static_cast<int>(m_state->requests.size());
    m_state->stats.requestBytes += bytes;
    m_state->stats.highWater = std::max(m_state->stats.highWater, m_state->stats.pending);
    m_state->changed.notify_one();
    return {id, 0, true, false, false};
}
DanmakuTextSpriteCache::EnsureResult DanmakuTextSpriteCache::lookupSprite(const QString &text, int fontPixelSize,
                                                                          qreal devicePixelRatio) const {
    if (!std::isfinite(devicePixelRatio) || devicePixelRatio > 16)
        return {};
    const SpriteKey key{text, fontPixelSize,
                        static_cast<int>(std::lround(std::max<qreal>(1, devicePixelRatio) * 1000))};
    const auto it = m_records.constFind(key);
    if (it == m_records.cend())
        return {};
    return {it->spriteId, it->width, false, it->ready, it->failed, it->rasterCompletedAtNs, it->guiReadyAtNs};
}
QVector<DanmakuSpriteUpload> DanmakuTextSpriteCache::takeCompleted(int maxSprites, qint64 maxBytes,
                                                                   bool allowOversize) {
    QVector<DanmakuSpriteUpload> uploads;
    qint64 bytes = 0;
    std::lock_guard lock(m_state->mutex);
    while (!m_state->completions.empty() && uploads.size() < maxSprites) {
        const auto &front = m_state->completions.front();
        const qint64 nextBytes = front.upload.image.sizeInBytes();
        if (maxBytes > 0 && (!uploads.empty() || !allowOversize) && bytes + nextBytes > maxBytes)
            break;
        auto completed = std::move(m_state->completions.front());
        m_state->completions.pop_front();
        --m_state->stats.pending;
        m_state->stats.requestBytes -= completed.request.bytes;
        m_state->stats.completionBytes -= nextBytes;
        const qint64 latency = elapsedNs(completed.request.submitted);
        m_state->stats.completionLatencyNs += latency;
        m_state->stats.maxCompletionLatencyNs = std::max(m_state->stats.maxCompletionLatencyNs, latency);
        m_pendingKeys.remove(completed.request.key);
        const auto it = m_records.find(completed.request.key);
        if (it != m_records.end() && it->spriteId == completed.request.id) {
            it->width = completed.upload.logicalSize.width();
            it->failed = completed.upload.image.isNull();
            it->ready = !it->failed;
            if (m_state->diagnosticsEnabled) {
                it->rasterCompletedAtNs = completed.rasterCompletedAtNs;
                if (it->ready)
                    it->guiReadyAtNs =
                        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
            }
            bytes += nextBytes;
            uploads.push_back(std::move(completed.upload));
        }
    }
    m_state->stats.completed = static_cast<int>(m_state->completions.size());
    m_state->changed.notify_one();
    return uploads;
}
DanmakuTextSpriteCache::Metrics DanmakuTextSpriteCache::metrics() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->stats;
}
void DanmakuTextSpriteCache::shutdown() {
    {
        std::lock_guard lock(m_state->mutex);
        if (m_state->closing)
            return;
        m_state->shutdownStarted = Clock::now();
        m_state->closing = true;
        ++m_state->generation;
        m_state->stats.cancelled += m_state->stats.pending;
        m_state->requests.clear();
        m_state->completions.clear();
        m_state->stats.pending = m_state->stats.queued = m_state->stats.completed = 0;
        m_state->stats.requestBytes = m_state->stats.completionBytes = 0;
    }
    m_state->changed.notify_one();
}
bool DanmakuTextSpriteCache::isStopped() const {
    std::lock_guard lock(m_state->mutex);
    return m_state->stopped;
}
int DanmakuTextSpriteCache::widthMeasurementCountForTesting() const {
    return static_cast<int>(metrics().rasterized);
}
int DanmakuTextSpriteCache::pendingRasterCountForTesting() const {
    return metrics().pending;
}
size_t qHash(const DanmakuTextSpriteCache::SpriteKey &key, size_t seed) noexcept {
    return qHash(key.devicePixelRatioMilli, qHash(key.fontPixelSize, qHash(key.text, seed)));
}
