#pragma once

#include "danmaku/DanmakuSoAState.hpp"
#include "danmaku/DanmakuSpatialGrid.hpp"

#include <QObject>
#include <QMutex>
#include <QQueue>
#include <QSharedPointer>
#include <QSet>
#include <QString>
#include <QThread>
#include <QTimer>
#include <QVariantList>
#include <QVector>

class DanmakuUpdateWorker;

class DanmakuController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ngDropZoneVisible READ ngDropZoneVisible NOTIFY ngDropZoneVisibleChanged)
    Q_PROPERTY(bool playbackPaused READ playbackPaused NOTIFY playbackPausedChanged)
    Q_PROPERTY(double playbackRate READ playbackRate NOTIFY playbackRateChanged)
    Q_PROPERTY(int targetFps READ targetFps WRITE setTargetFps NOTIFY targetFpsChanged)
    Q_PROPERTY(bool perfLogEnabled READ perfLogEnabled WRITE setPerfLogEnabled NOTIFY perfLogEnabledChanged)
    Q_PROPERTY(bool glyphWarmupEnabled READ glyphWarmupEnabled WRITE setGlyphWarmupEnabled NOTIFY glyphWarmupEnabledChanged)
    Q_PROPERTY(QString glyphWarmupText READ glyphWarmupText NOTIFY glyphWarmupTextChanged)
    Q_PROPERTY(double commentRenderFps READ commentRenderFps NOTIFY commentRenderFpsChanged)
    Q_PROPERTY(int activeCommentCount READ activeCommentCountMetric NOTIFY activeCommentCountChanged)
    Q_PROPERTY(qint64 overlayMetricsUpdatedAtMs READ overlayMetricsUpdatedAtMs NOTIFY overlayMetricsUpdatedAtMsChanged)

public:
    struct RenderItem {
        QString commentId;
        QString text;
        qreal x = 0;
        qreal y = 0;
        qreal alpha = 1.0;
        int widthEstimate = 120;
        bool ngDropHovered = false;
    };

    explicit DanmakuController(QObject *parent = nullptr);
    ~DanmakuController() override;

    Q_INVOKABLE void setViewportSize(qreal width, qreal height);
    Q_INVOKABLE void setLaneMetrics(int fontPx, int laneGap);
    Q_INVOKABLE void setPlaybackPaused(bool paused);
    Q_INVOKABLE void setPlaybackRate(double rate);
    Q_INVOKABLE void setTargetFps(int fps);
    Q_INVOKABLE void setPerfLogEnabled(bool enabled);
    Q_INVOKABLE void setGlyphWarmupEnabled(bool enabled);
    Q_INVOKABLE void appendFromCore(const QVariantList &comments, qint64 playbackPositionMs);
    Q_INVOKABLE void resetForSeek();
    Q_INVOKABLE void resetGlyphSession();

    Q_INVOKABLE bool beginDragAt(qreal x, qreal y);
    Q_INVOKABLE void moveActiveDrag(qreal x, qreal y);
    Q_INVOKABLE void dropActiveDrag(bool inNgZone);
    Q_INVOKABLE void cancelActiveDrag();
    Q_INVOKABLE void setNgDropZoneRect(qreal x, qreal y, qreal width, qreal height);

    Q_INVOKABLE void applyNgUserFade(const QString &userId);
    QSharedPointer<const QVector<RenderItem>> renderSnapshot() const;

    bool ngDropZoneVisible() const;
    bool playbackPaused() const;
    double playbackRate() const;
    int targetFps() const;
    bool perfLogEnabled() const;
    bool glyphWarmupEnabled() const;
    QString glyphWarmupText() const;
    double commentRenderFps() const;
    int activeCommentCountMetric() const;
    qint64 overlayMetricsUpdatedAtMs() const;

signals:
    void ngDropZoneVisibleChanged();
    void playbackPausedChanged();
    void playbackRateChanged();
    void targetFpsChanged();
    void perfLogEnabledChanged();
    void glyphWarmupEnabledChanged();
    void glyphWarmupTextChanged();
    void commentRenderFpsChanged();
    void activeCommentCountChanged();
    void overlayMetricsUpdatedAtMsChanged();
    void ngDropRequested(const QString &userId);
    void renderSnapshotChanged();

private:
    struct LaneState {
        qint64 nextAvailableAtMs = 0;
        int lastAssignedRow = -1;
    };

    struct Item {
        QString commentId;
        QString userId;
        QString text;
        qreal x = 0;
        qreal y = 0;
        qreal speedPxPerSec = 120;
        qreal alpha = 1.0;
        int lane = 0;
        int originalLane = 0;
        int widthEstimate = 120;
        bool frozen = false;
        bool dragging = false;
        bool fading = false;
        bool ngDropHovered = false;
        bool active = false;
        int fadeRemainingMs = 0;
    };

    void onFrame();
    int laneCount() const;
    int pickLane(qint64 nowMs);
    bool laneHasCollision(int lane, const Item &candidate);
    void recoverToLane(Item &item);
    void ensureLaneStateSize();
    void resetLaneStates();
    qint64 estimateLaneCooldownMs(const Item &item) const;
    int findItemIndexAt(qreal x, qreal y);
    int acquireRow();
    void releaseRow(int row);
    void releaseRowsDescending(const QVector<int> &rowsDescending);
    bool maybeCompactRows();
    int activeItemCount() const;
    bool hasDragging() const;
    void updateNgZoneVisibility();
    bool isItemInNgZone(const Item &item) const;
    void observeGlyphText(const QString &text);
    void queueGlyphCodepoint(char32_t codepoint);
    void queueGlyphSeedCharacters();
    void dispatchGlyphWarmupIfDue(qint64 nowMs);
    void clearGlyphWarmupText();
    void updateOverlayMetrics(qint64 nowMs);
    void maybeWritePerfLog(qint64 nowMs);
    void runFrameSingleThread(int elapsedMs, qint64 nowMs);
    void rebuildSpatialIndex();
    void rebuildRenderSnapshot();
    RenderItem buildRenderItem(const Item &item) const;
    void queueSpatialUpsertRow(int row);
    void queueSpatialUpsertRows(const QVector<int> &rows);
    void queueSpatialRemoveRow(int row);
    void queueSpatialRemoveRows(const QVector<int> &rows);
    void queueSnapshotUpsertRow(int row);
    void queueSnapshotUpsertRows(const QVector<int> &rows);
    void queueSnapshotRemoveRow(int row);
    void queueSnapshotRemoveRows(const QVector<int> &rows);
    void queueFullSpatialRebuild();
    void queueFullSnapshotRebuild();
    bool applySnapshotRowUpsert(int row);
    bool applySnapshotRowRemoval(int row);
    void ensureRowToRenderIndexSize();
    void publishRenderSnapshot();
    void flushPendingDiffs(bool emitSnapshotSignal);
    void buildSoAState(DanmakuSoAState &state) const;
    void scheduleWorkerFrame(int elapsedMs, qint64 nowMs);
    void handleWorkerFrame(DanmakuWorkerFramePtr frame);
    void invalidateWorkerGeneration();
    void updateFrameTimerInterval();
    bool beginDragInternal(int index, qreal pointerX, qreal pointerY, bool hasPointerPosition);
    void moveDragInternal(int index, qreal pointerX, qreal pointerY, bool hasPointerPosition);
    void dropDragInternal(int index, bool inNgZone);
    QVector<Item> m_items;
    QVector<LaneState> m_laneStates;
    QVector<int> m_freeRows;
    DanmakuSpatialGrid m_spatialGrid;
    QSet<int> m_pendingSpatialUpsertRows;
    QSet<int> m_pendingSpatialRemoveRows;
    QSet<int> m_pendingSnapshotUpsertRows;
    QSet<int> m_pendingSnapshotRemoveRows;
    bool m_pendingFullSpatialRebuild = true;
    bool m_pendingFullSnapshotRebuild = true;

    qreal m_viewportWidth = 1280;
    qreal m_viewportHeight = 720;
    int m_fontPx = 36;
    int m_laneGap = 6;
    int m_laneCursor = 0;

    bool m_ngDropZoneVisible = false;
    bool m_playbackPaused = true;
    double m_playbackRate = 1.0;
    int m_targetFps = 30;
    qreal m_ngZoneX = 0;
    qreal m_ngZoneY = 0;
    qreal m_ngZoneWidth = 0;
    qreal m_ngZoneHeight = 0;
    bool m_perfLogEnabled = false;
    qint64 m_perfLogWindowStartMs = 0;
    int m_perfLogFrameCount = 0;
    QVector<int> m_perfFrameSamplesMs;
    int m_perfLogAppendCount = 0;
    int m_perfLogGeometryUpdateCount = 0;
    int m_perfLogRemovedCount = 0;
    int m_perfLanePickCount = 0;
    int m_perfLaneReadyCount = 0;
    int m_perfLaneForcedCount = 0;
    qint64 m_perfLaneWaitTotalMs = 0;
    qint64 m_perfLaneWaitMaxMs = 0;
    bool m_perfCompactedSinceLastLog = false;
    bool m_glyphWarmupEnabled = true;
    QString m_glyphWarmupText;
    QSet<char32_t> m_seenGlyphCodepoints;
    QSet<char32_t> m_warmedGlyphCodepoints;
    QSet<char32_t> m_queuedGlyphCodepoints;
    QQueue<char32_t> m_glyphWarmupQueue;
    qint64 m_lastGlyphWarmupDispatchMs = 0;
    int m_perfGlyphNewCodepoints = 0;
    int m_perfGlyphNewNonAsciiCodepoints = 0;
    int m_perfGlyphWarmupSentCodepoints = 0;
    int m_perfGlyphWarmupBatchCount = 0;
    int m_perfGlyphWarmupDroppedCodepoints = 0;
    int m_activeDragRow = -1;
    qreal m_activeDragOffsetX = 0;
    qreal m_activeDragOffsetY = 0;
    mutable QMutex m_renderSnapshotMutex;
    QVector<RenderItem> m_renderCache;
    QVector<int> m_renderRows;
    QVector<int> m_rowToRenderIndex;
    QSharedPointer<const QVector<RenderItem>> m_renderSnapshot;
    bool m_workerEnabled = true;
    bool m_workerBusy = false;
    qint64 m_workerSeq = 0;
    int m_workerAccumulatedElapsedMs = 0;
    DanmakuWorkerFramePtr m_workerReusableFrame;
    DanmakuUpdateWorker *m_updateWorker = nullptr;
    QThread m_updateThread;
    QString m_simdModeName = QStringLiteral("auto");

    QTimer m_frameTimer;
    qint64 m_lastTickMs = 0;
    int m_perfSpatialFullRebuildCount = 0;
    int m_perfSpatialRowUpdateCount = 0;
    int m_perfSnapshotFullRebuildCount = 0;
    int m_perfSnapshotRowUpdateCount = 0;
    qint64 m_overlayMetricWindowStartMs = 0;
    int m_overlayMetricFrameCount = 0;
    double m_commentRenderFps = 0.0;
    int m_activeCommentCount = 0;
    qint64 m_overlayMetricsUpdatedAtMs = 0;
};
