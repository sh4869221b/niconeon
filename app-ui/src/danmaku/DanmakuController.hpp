#pragma once

#include "danmaku/DanmakuListModel.hpp"
#include "danmaku/DanmakuSpatialGrid.hpp"

#include <QObject>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVector>

class DanmakuController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *itemModel READ itemModel CONSTANT)
    Q_PROPERTY(bool ngDropZoneVisible READ ngDropZoneVisible NOTIFY ngDropZoneVisibleChanged)
    Q_PROPERTY(bool playbackPaused READ playbackPaused NOTIFY playbackPausedChanged)
    Q_PROPERTY(double playbackRate READ playbackRate NOTIFY playbackRateChanged)
    Q_PROPERTY(bool perfLogEnabled READ perfLogEnabled WRITE setPerfLogEnabled NOTIFY perfLogEnabledChanged)
    Q_PROPERTY(bool glyphWarmupEnabled READ glyphWarmupEnabled WRITE setGlyphWarmupEnabled NOTIFY glyphWarmupEnabledChanged)
    Q_PROPERTY(QString glyphWarmupText READ glyphWarmupText NOTIFY glyphWarmupTextChanged)

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

    Q_INVOKABLE void setViewportSize(qreal width, qreal height);
    Q_INVOKABLE void setLaneMetrics(int fontPx, int laneGap);
    Q_INVOKABLE void setPlaybackPaused(bool paused);
    Q_INVOKABLE void setPlaybackRate(double rate);
    Q_INVOKABLE void setPerfLogEnabled(bool enabled);
    Q_INVOKABLE void setGlyphWarmupEnabled(bool enabled);
    Q_INVOKABLE void appendFromCore(const QVariantList &comments, qint64 playbackPositionMs);
    Q_INVOKABLE void resetForSeek();
    Q_INVOKABLE void resetGlyphSession();

    Q_INVOKABLE void beginDrag(const QString &commentId);
    Q_INVOKABLE void moveDrag(const QString &commentId, qreal x, qreal y);
    Q_INVOKABLE void dropDrag(const QString &commentId, bool inNgZone);
    Q_INVOKABLE void cancelDrag(const QString &commentId);
    Q_INVOKABLE bool beginDragAt(qreal x, qreal y);
    Q_INVOKABLE void moveActiveDrag(qreal x, qreal y);
    Q_INVOKABLE void dropActiveDrag(bool inNgZone);
    Q_INVOKABLE void cancelActiveDrag();
    Q_INVOKABLE void setNgDropZoneRect(qreal x, qreal y, qreal width, qreal height);

    Q_INVOKABLE void applyNgUserFade(const QString &userId);
    QVector<RenderItem> renderSnapshot() const;

    QObject *itemModel();
    bool ngDropZoneVisible() const;
    bool playbackPaused() const;
    double playbackRate() const;
    bool perfLogEnabled() const;
    bool glyphWarmupEnabled() const;
    QString glyphWarmupText() const;

signals:
    void ngDropZoneVisibleChanged();
    void playbackPausedChanged();
    void playbackRateChanged();
    void perfLogEnabledChanged();
    void glyphWarmupEnabledChanged();
    void glyphWarmupTextChanged();
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
    int findItemIndex(const QString &commentId) const;
    int findItemIndexAt(qreal x, qreal y);
    int acquireRow();
    void releaseRow(int row);
    void releaseRowsDescending(const QVector<int> &rowsDescending);
    void maybeCompactRows();
    int activeItemCount() const;
    bool hasDragging() const;
    void updateNgZoneVisibility();
    bool isItemInNgZone(const Item &item) const;
    void observeGlyphText(const QString &text);
    void queueGlyphCodepoint(char32_t codepoint);
    void queueGlyphSeedCharacters();
    void dispatchGlyphWarmupIfDue(qint64 nowMs);
    void clearGlyphWarmupText();
    void maybeWritePerfLog(qint64 nowMs);
    void markSpatialDirty();
    void rebuildSpatialIndex();
    void ensureSpatialIndex();
    void rebuildRenderSnapshot();
    bool beginDragInternal(int index, qreal pointerX, qreal pointerY, bool hasPointerPosition);
    void moveDragInternal(int index, qreal pointerX, qreal pointerY, bool hasPointerPosition);
    void dropDragInternal(int index, bool inNgZone);
    DanmakuListModel::Row makeRow(const Item &item) const;

    QVector<Item> m_items;
    QVector<LaneState> m_laneStates;
    QVector<int> m_freeRows;
    DanmakuListModel m_itemModel;
    DanmakuSpatialGrid m_spatialGrid;
    bool m_spatialDirty = true;

    qreal m_viewportWidth = 1280;
    qreal m_viewportHeight = 720;
    int m_fontPx = 36;
    int m_laneGap = 6;
    int m_laneCursor = 0;

    bool m_ngDropZoneVisible = false;
    bool m_playbackPaused = true;
    double m_playbackRate = 1.0;
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
    QVector<RenderItem> m_renderSnapshot;

    QTimer m_frameTimer;
    qint64 m_lastTickMs = 0;
};
