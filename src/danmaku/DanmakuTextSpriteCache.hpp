#pragma once

#include "danmaku/DanmakuRenderFrame.hpp"

#include <QFont>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>

// GUI-affine, payload-free notifier. The cache state retains it until the raster
// worker has stopped; receivers use a queued connection with a QObject context.
class DanmakuRasterNotifier final : public QObject {
    Q_OBJECT
  signals:
    void completionReady();
};

// GUI-owned index with a single, privately owned CPU raster worker. Only
// takeCompleted() publishes a result into the GUI index; the worker never
// touches that index or graphics resources. It may emit the coalesced notifier.
class DanmakuTextSpriteCache {
  public:
    struct SpriteKey {
        QString text;
        int fontPixelSize = 0;
        int devicePixelRatioMilli = 1000;
        friend bool operator==(const SpriteKey &, const SpriteKey &) = default;
    };
    struct EnsureResult {
        DanmakuSpriteId spriteId = 0;
        int widthEstimate = 0;
        bool queuedRaster = false;
        bool ready = false;
        bool failed = false;
        qint64 rasterCompletedAtNs = 0;
        qint64 guiReadyAtNs = 0;
    };
    struct Limits {
        int pendingRequests = 128;
        qint64 requestBytes = 4 * 1024 * 1024;
        int completedSprites = 32;
        qint64 completedBytes = 8 * 1024 * 1024;
    };
    struct Metrics {
        int pending = 0;
        int queued = 0;
        int completed = 0;
        int highWater = 0;
        int completionHighWater = 0;
        qint64 requestBytes = 0;
        qint64 completionBytes = 0;
        qint64 completionBytesHighWater = 0;
        quint64 coalesced = 0;
        quint64 backpressured = 0;
        quint64 cancelled = 0;
        quint64 stale = 0;
        quint64 failed = 0;
        quint64 rasterized = 0;
        qint64 rasterNs = 0;
        qint64 maxRasterNs = 0;
        qint64 completionLatencyNs = 0;
        qint64 maxCompletionLatencyNs = 0;
        qint64 shutdownNs = 0;
        int wakePending = 0;
        int wakeActive = 0;
        int wakePendingHighWater = 0;
        int wakeOutstandingHighWater = 0;
        int wakeMaxCommittedSprites = 0;
        quint64 wakeNotifications = 0;
        quint64 wakeCoalesced = 0;
        quint64 wakeStarted = 0;
        quint64 wakeNoProgress = 0;
        quint64 wakeSuppressed = 0;
    };
    // The image allocation is independently bounded, including the one image
    // currently being painted. Oversize/invalid input is an explicit failure,
    // never a permanently pending queue entry.
    static constexpr qint64 MaxSpriteBytes = 8 * 1024 * 1024;
    static constexpr int MaxTextCodeUnits = 16384;
    using BeforeRaster = std::function<void()>; // deterministic cancellation/failure tests

    DanmakuTextSpriteCache();
    explicit DanmakuTextSpriteCache(Limits limits, BeforeRaster beforeRaster = {});
    ~DanmakuTextSpriteCache();
    DanmakuTextSpriteCache(const DanmakuTextSpriteCache &) = delete;
    DanmakuTextSpriteCache &operator=(const DanmakuTextSpriteCache &) = delete;

    void clear();
    void cancelPending();
    void setFont(const QFont &font);
    EnsureResult ensureSprite(const QString &text, int fontPixelSize, qreal devicePixelRatio);
    // Read-only GUI index lookup; never submits work or changes queue metrics.
    EnsureResult lookupSprite(const QString &text, int fontPixelSize, qreal devicePixelRatio) const;
    QVector<DanmakuSpriteUpload> takeCompleted(int maxSprites, qint64 maxBytes, bool allowOversize = true);
    Metrics metrics() const;
    DanmakuRasterNotifier *notifier() const;
    // Thread-safe, nonblocking request. maxBytes/allowOversize optionally avoid
    // scheduling when the first completion cannot fit the remaining mailbox.
    void requestCompletionWake(qint64 maxBytes = 0, bool allowOversize = true);
    // Retain a closed consumer gate across new completions/generation changes.
    // Only an explicit capacity-restored request reopens it. A reserved callback
    // is never cancelled; it still acknowledges its original wake exactly once.
    void blockCompletionWakes();
    // GUI-only: acknowledge only at the actual queued callback's entry, never
    // from takeCompleted(), timer/append drains, or generation cancellation.
    bool beginCompletionWake();
    void finishCompletionWake(int committedSprites);
    void shutdown();
    bool isStopped() const;
    int widthMeasurementCountForTesting() const;
    int pendingRasterCountForTesting() const;

  private:
    struct State;
    struct Record {
        DanmakuSpriteId spriteId = 0;
        int width = 0;
        bool ready = false;
        bool failed = false;
        qint64 rasterCompletedAtNs = 0;
        qint64 guiReadyAtNs = 0;
    };
    std::unique_ptr<State> m_state;
    QHash<SpriteKey, Record> m_records;
    QSet<SpriteKey> m_pendingKeys;
    QFont m_font;
    quint32 m_nextSpriteId = 1;
};

size_t qHash(const DanmakuTextSpriteCache::SpriteKey &key, size_t seed = 0) noexcept;
