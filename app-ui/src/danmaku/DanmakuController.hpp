#pragma once

#include <QObject>
#include <QTimer>
#include <QVariantList>

class DanmakuController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(bool ngDropZoneVisible READ ngDropZoneVisible NOTIFY ngDropZoneVisibleChanged)
    Q_PROPERTY(bool playbackPaused READ playbackPaused NOTIFY playbackPausedChanged)

public:
    explicit DanmakuController(QObject *parent = nullptr);

    Q_INVOKABLE void setViewportSize(qreal width, qreal height);
    Q_INVOKABLE void setLaneMetrics(int fontPx, int laneGap);
    Q_INVOKABLE void setPlaybackPaused(bool paused);
    Q_INVOKABLE void appendFromCore(const QVariantList &comments);
    Q_INVOKABLE void resetForSeek();

    Q_INVOKABLE void beginDrag(const QString &commentId);
    Q_INVOKABLE void moveDrag(const QString &commentId, qreal x, qreal y);
    Q_INVOKABLE void dropDrag(const QString &commentId, bool inNgZone);
    Q_INVOKABLE void cancelDrag(const QString &commentId);

    Q_INVOKABLE void applyNgUserFade(const QString &userId);

    QVariantList items() const;
    bool ngDropZoneVisible() const;
    bool playbackPaused() const;

signals:
    void itemsChanged();
    void ngDropZoneVisibleChanged();
    void playbackPausedChanged();
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
        int fadeRemainingMs = 0;
    };

    void onFrame();
    void rebuildModel();
    int laneCount() const;
    int pickLane(int widthEstimate) const;
    bool laneHasCollision(int lane, const Item &candidate) const;
    void recoverToLane(Item &item);
    Item *findItem(const QString &commentId);
    void updateNgZoneVisibility();

    QVector<Item> m_items;
    QVariantList m_itemsModel;

    qreal m_viewportWidth = 1280;
    qreal m_viewportHeight = 720;
    int m_fontPx = 36;
    int m_laneGap = 6;

    bool m_ngDropZoneVisible = false;
    bool m_playbackPaused = true;

    QTimer m_frameTimer;
    qint64 m_lastTickMs = 0;
};
