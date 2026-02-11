#pragma once

#include <QQuickFramebufferObject>
#include <QTimer>

struct mpv_handle;
struct mpv_render_context;

class MpvItem : public QQuickFramebufferObject {
    Q_OBJECT
    Q_PROPERTY(qint64 positionMs READ positionMs NOTIFY positionMsChanged)
    Q_PROPERTY(qint64 durationMs READ durationMs NOTIFY durationMsChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY pausedChanged)
    Q_PROPERTY(double volume READ volume WRITE setVolume NOTIFY volumeChanged)

public:
    explicit MpvItem(QQuickItem *parent = nullptr);
    ~MpvItem() override;

    Renderer *createRenderer() const override;

    Q_INVOKABLE bool openFile(const QString &path);
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void setPaused(bool paused);
    Q_INVOKABLE void seek(qint64 ms);

    qint64 positionMs() const;
    qint64 durationMs() const;
    bool paused() const;
    double volume() const;

public slots:
    void setVolume(double volume);

signals:
    void positionMsChanged();
    void durationMsChanged();
    void pausedChanged();
    void volumeChanged();

private slots:
    void pollProperties();

private:
    static void onMpvRenderUpdate(void *ctx);

    friend class MpvRenderer;

    mpv_handle *m_mpv = nullptr;
    mpv_render_context *m_renderContext = nullptr;

    QTimer m_pollTimer;

    qint64 m_positionMs = 0;
    qint64 m_durationMs = 0;
    bool m_paused = true;
    double m_volume = 100.0;
};
