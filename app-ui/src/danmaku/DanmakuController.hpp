#pragma once

#include "danmaku/DanmakuListModel.hpp"

#include <QObject>
#include <QTimer>
#include <QVariantList>
#include <QVector>

class DanmakuController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *itemModel READ itemModel CONSTANT)
    Q_PROPERTY(bool ngDropZoneVisible READ ngDropZoneVisible NOTIFY ngDropZoneVisibleChanged)
    Q_PROPERTY(bool playbackPaused READ playbackPaused NOTIFY playbackPausedChanged)
    Q_PROPERTY(double playbackRate READ playbackRate NOTIFY playbackRateChanged)
    Q_PROPERTY(qint64 dragVisualElapsedMs READ dragVisualElapsedMs NOTIFY dragVisualElapsedMsChanged)
    Q_PROPERTY(bool perfLogEnabled READ perfLogEnabled WRITE setPerfLogEnabled NOTIFY perfLogEnabledChanged)

public:
    explicit DanmakuController(QObject *parent = nullptr);

    Q_INVOKABLE void setViewportSize(qreal width, qreal height);
    Q_INVOKABLE void setLaneMetrics(int fontPx, int laneGap);
    Q_INVOKABLE void setPlaybackPaused(bool paused);
    Q_INVOKABLE void setPlaybackRate(double rate);
    Q_INVOKABLE void setPerfLogEnabled(bool enabled);
    Q_INVOKABLE void appendFromCore(const QVariantList &comments, qint64 playbackPositionMs);
    Q_INVOKABLE void resetForSeek();

    Q_INVOKABLE void beginDrag(const QString &commentId);
    Q_INVOKABLE void moveDrag(const QString &commentId, qreal x, qreal y);
    Q_INVOKABLE void dropDrag(const QString &commentId, bool inNgZone);
    Q_INVOKABLE void cancelDrag(const QString &commentId);
    Q_INVOKABLE void setNgDropZoneRect(qreal x, qreal y, qreal width, qreal height);

    Q_INVOKABLE void applyNgUserFade(const QString &userId);

    QObject *itemModel();
    bool ngDropZoneVisible() const;
    bool playbackPaused() const;
    double playbackRate() const;
    qint64 dragVisualElapsedMs() const;
    bool perfLogEnabled() const;

signals:
    void ngDropZoneVisibleChanged();
    void playbackPausedChanged();
    void playbackRateChanged();
    void dragVisualElapsedMsChanged();
    void perfLogEnabledChanged();
    void ngDropRequested(const QString &userId);

private:
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
        int fadeRemainingMs = 0;
    };

    void onFrame();
    int laneCount() const;
    int pickLane(int widthEstimate) const;
    bool laneHasCollision(int lane, const Item &candidate) const;
    void recoverToLane(Item &item);
    int findItemIndex(const QString &commentId) const;
    bool hasDragging() const;
    void updateNgZoneVisibility();
    bool isItemInNgZone(const Item &item) const;
    void maybeWritePerfLog(qint64 nowMs);

    QVector<Item> m_items;
    DanmakuListModel m_itemModel;

    qreal m_viewportWidth = 1280;
    qreal m_viewportHeight = 720;
    int m_fontPx = 36;
    int m_laneGap = 6;

    bool m_ngDropZoneVisible = false;
    bool m_playbackPaused = true;
    double m_playbackRate = 1.0;
    qint64 m_dragVisualElapsedMs = 0;
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

    QTimer m_frameTimer;
    qint64 m_lastTickMs = 0;
};
