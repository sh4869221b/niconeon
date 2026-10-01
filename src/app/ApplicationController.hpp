#pragma once
#include "app/CommentService.hpp"
#include "danmaku/DanmakuController.hpp"
#include "playback/MpvItem.hpp"
#include <QQueue>
#include <QSettings>
#include <QTimer>
#include <QVariantList>

namespace niconeon {
class ApplicationController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString selectedVideoPath READ selectedVideoPath NOTIFY changed)
    Q_PROPERTY(bool commentsVisible READ commentsVisible NOTIFY changed)
    Q_PROPERTY(bool perfLogEnabled READ perfLogEnabled NOTIFY changed)
    Q_PROPERTY(QString perfProfile READ perfProfile NOTIFY changed)
    Q_PROPERTY(int targetFps READ targetFps NOTIFY changed)
    Q_PROPERTY(int maxEmitPerTick READ maxEmitPerTick NOTIFY changed)
    Q_PROPERTY(bool coalesceSameContent READ coalesceSameContent NOTIFY changed)
    Q_PROPERTY(qsizetype totalComments READ totalComments NOTIFY changed)
    Q_PROPERTY(int fontSizeLevel READ fontSizeLevel NOTIFY changed)
    Q_PROPERTY(int appFontPixelSize READ appFontPixelSize NOTIFY changed)
    Q_PROPERTY(QVariantList speedPresets READ speedPresets NOTIFY changed)
    Q_PROPERTY(QStringList ngUsers READ ngUsers NOTIFY changed)
    Q_PROPERTY(QVariantList regexFilters READ regexFilters NOTIFY changed)
    Q_PROPERTY(DanmakuController *danmaku READ danmaku CONSTANT)
  public:
    explicit ApplicationController(ServiceOptions options = {}, QObject *parent = nullptr);
    ~ApplicationController() override;
    QString selectedVideoPath() const {
        return m_selectedPath;
    }
    bool commentsVisible() const {
        return m_commentsVisible;
    }
    bool perfLogEnabled() const {
        return m_perfLog;
    }
    QString perfProfile() const {
        return profileName(m_profile.profile);
    }
    int targetFps() const {
        return m_profile.targetFps;
    }
    int maxEmitPerTick() const {
        return static_cast<int>(m_profile.maxEmitPerTick);
    }
    bool coalesceSameContent() const {
        return m_profile.coalesceSameContent;
    }
    qsizetype totalComments() const {
        return m_totalComments;
    }
    int fontSizeLevel() const {
        return m_fontLevel;
    }
    int appFontPixelSize() const {
        return 12 + m_fontLevel * 2;
    }
    QVariantList speedPresets() const;
    QStringList ngUsers() const {
        return m_ngUsers;
    }
    QVariantList regexFilters() const {
        return m_regexFilters;
    }
    DanmakuController *danmaku() {
        return &m_danmaku;
    }
    Q_INVOKABLE void attachPlayer(MpvItem *player);
    Q_INVOKABLE void openVideo(const QString &path);
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void seek(qint64 positionMs);
    Q_INVOKABLE void setVolume(double value);
    Q_INVOKABLE void setPlaybackRate(double rate);
    Q_INVOKABLE void cyclePlaybackSpeed();
    Q_INVOKABLE void addSpeedPreset(const QString &value);
    Q_INVOKABLE void removeSpeedPreset(double rate);
    Q_INVOKABLE void setCommentsVisible(bool visible);
    Q_INVOKABLE void setPerfLogEnabled(bool enabled);
    Q_INVOKABLE void cycleRuntimeProfile();
    Q_INVOKABLE void setFontSizeLevel(int level);
    Q_INVOKABLE void addRegexFilter(const QString &pattern);
    Q_INVOKABLE void removeRegexFilter(qint64 id);
    Q_INVOKABLE void removeNgUser(const QString &id);
    Q_INVOKABLE void refreshFilters();
    Q_INVOKABLE void undoNg();
    Q_INVOKABLE void shutdown();
    static QVector<double> normalizedPresets(const QVector<double> &values);
    static QString localPath(const QString &value);
    bool seekPending() const {
        return m_waitingSeek;
    }

  signals:
    void changed();
    void toastRequested(const QString &message, const QString &actionText);
    void readyToQuit();

  private:
    struct RenderBatch {
        CommentList comments;
        qsizetype offset = 0;
        qint64 positionMs = 0;
    };
    void toast(const QString &text, const QString &action = {});
    void applyProfile(RuntimeProfileConfig profile, bool persist);
    void resetComments();
    void reconcileSeek();
    void playbackTick();
    void drainRenderBatch();
    void performanceWindow();
    bool degradeQos();
    bool recoverQos();
    double nearestPreset(double value) const;
    void persistPresets();
    void queueFailure(bool accepted);
    QSettings m_settings;
    CommentService m_service;
    DanmakuController m_danmaku;
    QPointer<MpvItem> m_player;
    QTimer m_tickTimer;
    QTimer m_seekTimer;
    QTimer m_renderTimer;
    QTimer m_perfTimer;
    QQueue<RenderBatch> m_renderQueue;
    QString m_selectedPath;
    QString m_undoToken;
    QStringList m_ngUsers;
    QVariantList m_regexFilters;
    QVector<double> m_presets;
    RuntimeProfileConfig m_profile;
    RuntimeProfile m_selectedProfile = RuntimeProfile::LowSpec;
    bool m_commentsVisible = true;
    bool m_perfLog = false;
    bool m_closing = false;
    bool m_waitingSeek = false;
    quint64 m_seekRequest = 0;
    qint64 m_seekTarget = 0;
    qsizetype m_totalComments = 0;
    int m_fontLevel = 1;
    int m_sent = 0;
    int m_results = 0;
    qsizetype m_dropped = 0;
    qsizetype m_coalesced = 0;
    int m_overBudget = 0;
    int m_overloadStreak = 0;
    int m_stableStreak = 0;
};
} // namespace niconeon
