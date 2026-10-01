#pragma once

#include "comments/NiconicoFetcher.hpp"
#include "domain/Domain.hpp"
#include "storage/Store.hpp"
#include <QObject>
#include <QPointer>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace niconeon {
class CommentWorker;
struct ServiceOptions {
    std::optional<StorePaths> storePaths;
    bool memoryStore = false;
    FetchOptions fetch;
};
struct SessionControl {
    std::atomic<quint64> generation{0};
    std::atomic<bool> stopping{false};
    std::atomic<quint64> revision{0};
};

// GUI facade. At most 32 admitted commands, one running tick and one coalesced tick.
// Network, decoding, SQLite and filtering execute only on the single owning worker.
class CommentService : public QObject {
    Q_OBJECT
  public:
    explicit CommentService(ServiceOptions options = {}, QObject *parent = nullptr);
    ~CommentService() override;
    quint64 openVideo(const QString &path, const QString &videoId);
    void clearSession();
    void requestTick(qint64 positionMs, bool paused, bool seek);
    bool setRuntimeProfile(const RuntimeProfileConfig &profile);
    bool addNgUser(const QString &userId);
    bool removeNgUser(const QString &userId);
    bool undoLastNg(const QString &token);
    bool addRegexFilter(const QString &pattern);
    bool removeRegexFilter(qint64 id);
    bool listFilters();
    void shutdown();
    bool isStopped() const;
    int queueDepth() const {
        return m_pendingCommands;
    }
    int highWaterMark() const {
        return m_highWater;
    }
    quint64 staleResults() const {
        return m_staleResults;
    }
    quint64 coalescedTicks() const {
        return m_coalescedTicks;
    }

  signals:
    void ready();
    void opened(quint64 generation, niconeon::CommentSource source, qsizetype count);
    void commentsReady(const niconeon::PlaybackBatchResult &result);
    void filtersChanged(const niconeon::FilterSnapshot &filters);
    void ngAdded(const niconeon::AddNgUserResult &result);
    void ngRemoved(const niconeon::RemoveNgUserResult &result);
    void ngUndone(const niconeon::UndoNgResult &result);
    void regexAdded(qint64 id);
    void regexRemoved(bool removed);
    void operationFailed(const QString &operation, const QString &detail, const QString &userId);
    void warning(const QString &message);
    void stopped();

  private:
    bool post(std::function<void(CommentWorker &)> action);
    void pumpTick();
    void pumpOpen();
    void pumpProfile();
    std::shared_ptr<SessionControl> m_control;
    QPointer<QThread> m_thread;
    CommentWorker *m_worker = nullptr;
    int m_pendingCommands = 0;
    int m_highWater = 0;
    bool m_tickInFlight = false;
    bool m_sessionReady = false;
    quint64 m_staleResults = 0;
    quint64 m_coalescedTicks = 0;
    std::optional<PlaybackTick> m_pendingTick;
    std::optional<RuntimeProfileConfig> m_pendingProfile;
    struct PendingOpen {
        QString path;
        QString id;
        quint64 generation;
    };
    std::optional<PendingOpen> m_pendingOpen;
};
} // namespace niconeon
Q_DECLARE_METATYPE(niconeon::CommentSource)
