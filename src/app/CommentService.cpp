#include "app/CommentService.hpp"
#include "comments/CommentTimeline.hpp"
#include "comments/SyntheticComments.hpp"
#include "filters/FilterManager.hpp"
#include <QDebug>
#include <QElapsedTimer>

namespace niconeon {
class CommentWorker : public QObject {
    Q_OBJECT
  public:
    CommentWorker(ServiceOptions options, std::shared_ptr<SessionControl> control)
        : m_options(std::move(options)), m_control(std::move(control)) {}
    void initialize() {
        auto store = m_options.memoryStore ? Store::openMemory()
                     : m_options.storePaths
                         ? Store::openWithPaths(m_options.storePaths->dataPath, m_options.storePaths->cachePath)
                         : Store::openDefault();
        if (!store) {
            emit warning(QStringLiteral("永続化を開けません。一時メモリで継続します: ") + store.error().message);
            store = Store::openMemory();
        }
        if (store) {
            m_store = std::move(*store);
            m_filters = std::make_unique<FilterManager>(*m_store);
            auto initialized = m_filters->initialize();
            if (!initialized) {
                emit warning(initialized.error().message);
                m_filters.reset();
            }
        } else {
            emit warning(store.error().message);
        }
        m_fetcher = new NiconicoFetcher(m_options.fetch, this);
        connect(m_fetcher, &NiconicoFetcher::completed, this, &CommentWorker::fetchCompleted);
        emit ready();
        if (m_filters)
            emit filtersChanged(m_filters->snapshot());
    }
    void open(const QString &path, const QString &id, quint64 generation) {
        if (!current(generation))
            return;
        m_fetcher->cancel();
        m_timeline.reset();
        m_videoId = id;
        m_generation = generation;
        if (id.isEmpty()) {
            commit({}, CommentSource::None, generation);
            return;
        }
        if (m_store) {
            auto mapped = m_store->upsertVideoMap(path, id);
            if (!mapped)
                emit warning(mapped.error().message);
        }
        if (syntheticCommentModeEnabled()) {
            auto comments = generateSyntheticComments(id);
            if (!comments) {
                emit warning(comments.error().message);
                commit({}, CommentSource::None, generation);
            } else {
                commit(std::move(*comments), CommentSource::Network, generation);
            }
        } else {
            m_fetcher->fetch(id, generation);
        }
    }
    void tick(PlaybackTick tick, quint64 generation, quint64 revision) {
        PlaybackBatchResult result;
        QString error;
        if (current(generation) && m_timeline) {
            const FilterEngine emptyFilters;
            auto processed = m_timeline->process(
                {tick}, m_filters ? m_filters->engine() : emptyFilters, m_profile, [this, generation, revision] {
                    return !current(generation) || revision != m_control->revision.load();
                });
            if (processed)
                result = std::move(*processed);
            else
                error = processed.error().message;
        }
        emit tickCompleted(generation, revision, std::move(result), error);
    }
    void profile(const RuntimeProfileConfig &profile) {
        m_profile = profile;
    }
    void addNg(const QString &userId) {
        if (!requireFilters("add_ng", userId))
            return;
        auto result = m_filters->addNgUser(userId);
        if (!result)
            emit operationFailed("add_ng", result.error().message, userId);
        else {
            emit ngAdded(*result);
            emit filtersChanged(m_filters->snapshot());
        }
    }
    void removeNg(const QString &userId) {
        if (!requireFilters("remove_ng", userId))
            return;
        auto result = m_filters->removeNgUser(userId);
        if (!result)
            emit operationFailed("remove_ng", result.error().message, userId);
        else {
            emit ngRemoved(*result);
            emit filtersChanged(m_filters->snapshot());
        }
    }
    void undo(const QString &token) {
        if (!requireFilters("undo_ng"))
            return;
        auto result = m_filters->undoLastNg(token);
        if (!result)
            emit operationFailed("undo_ng", result.error().message, {});
        else {
            emit ngUndone(*result);
            emit filtersChanged(m_filters->snapshot());
        }
    }
    void addRegex(const QString &pattern) {
        if (!requireFilters("add_regex"))
            return;
        auto result = m_filters->addRegexFilter(pattern);
        if (!result)
            emit operationFailed("add_regex", result.error().message, {});
        else {
            emit regexAdded(*result);
            emit filtersChanged(m_filters->snapshot());
        }
    }
    void removeRegex(qint64 id) {
        if (!requireFilters("remove_regex"))
            return;
        auto result = m_filters->removeRegexFilter(id);
        if (!result)
            emit operationFailed("remove_regex", result.error().message, {});
        else {
            emit regexRemoved(*result);
            emit filtersChanged(m_filters->snapshot());
        }
    }
    void listFilters() {
        if (m_filters)
            emit filtersChanged(m_filters->snapshot());
    }
    void shutdown() {
        if (m_fetcher)
            m_fetcher->cancel();
        m_timeline.reset();
        m_filters.reset();
        m_store.reset();
        QThread::currentThread()->quit();
    }
    void execute(std::function<void(CommentWorker &)> action) {
        QElapsedTimer elapsed;
        elapsed.start();
        if (!m_control->stopping.load())
            action(*this);
        emit commandFinished(elapsed.nsecsElapsed());
    }
  signals:
    void ready();
    void opened(quint64 generation, niconeon::CommentSource source, qsizetype count);
    void tickCompleted(quint64 generation, quint64 revision, niconeon::PlaybackBatchResult result,
                       const QString &error);
    void filtersChanged(const niconeon::FilterSnapshot &filters);
    void ngAdded(const niconeon::AddNgUserResult &result);
    void ngRemoved(const niconeon::RemoveNgUserResult &result);
    void ngUndone(const niconeon::UndoNgResult &result);
    void regexAdded(qint64 id);
    void regexRemoved(bool removed);
    void operationFailed(const QString &operation, const QString &detail, const QString &userId);
    void warning(const QString &message);
    void commandFinished(qint64 elapsedNs);

  private:
    bool current(quint64 generation) const {
        return !m_control->stopping.load() && m_control->generation.load() == generation;
    }
    bool requireFilters(const QString &operation, const QString &userId = {}) {
        if (m_filters)
            return true;
        emit operationFailed(operation, QStringLiteral("persistent filters are unavailable"), userId);
        return false;
    }
    void commit(CommentList comments, CommentSource source, quint64 generation) {
        if (!current(generation))
            return;
        m_timeline = std::make_unique<CommentTimeline>(QString::number(generation), std::move(comments));
        emit opened(generation, source, m_timeline->size());
    }
    void fetchCompleted(quint64 generation, CommentList comments, const QString &error) {
        if (!current(generation))
            return;
        if (error.isEmpty()) {
            if (m_store) {
                auto saved = m_store->saveCommentCache(m_videoId, comments);
                if (!saved)
                    emit warning(saved.error().message);
            }
            commit(std::move(comments), CommentSource::Network, generation);
            return;
        }
        emit warning(QStringLiteral("コメント取得に失敗しました（動画再生は継続）: ") + error);
        if (m_store) {
            auto cached = m_store->loadCommentCache(m_videoId);
            if (cached && cached->has_value()) {
                commit(std::move(**cached), CommentSource::Cache, generation);
                return;
            }
            if (!cached)
                emit warning(cached.error().message);
        }
        commit({}, CommentSource::None, generation);
    }
    ServiceOptions m_options;
    std::shared_ptr<SessionControl> m_control;
    std::unique_ptr<Store> m_store;
    std::unique_ptr<FilterManager> m_filters;
    std::unique_ptr<CommentTimeline> m_timeline;
    NiconicoFetcher *m_fetcher = nullptr;
    RuntimeProfileConfig m_profile;
    QString m_videoId;
    quint64 m_generation = 0;
};

CommentService::CommentService(ServiceOptions options, QObject *parent)
    : QObject(parent), m_control(std::make_shared<SessionControl>()), m_thread(new QThread) {
    qRegisterMetaType<CommentList>();
    qRegisterMetaType<PlaybackBatchResult>();
    qRegisterMetaType<FilterSnapshot>();
    qRegisterMetaType<CommentSource>();
    qRegisterMetaType<AddNgUserResult>();
    qRegisterMetaType<RemoveNgUserResult>();
    qRegisterMetaType<UndoNgResult>();
    m_worker = new CommentWorker(std::move(options), m_control);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::started, m_worker, &CommentWorker::initialize);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this, &CommentService::stopped);
    connect(m_worker, &CommentWorker::ready, this, [this] {
        pumpProfile();
        pumpOpen();
        emit ready();
    });
    connect(m_worker, &CommentWorker::warning, this, &CommentService::warning);
    connect(m_worker, &CommentWorker::filtersChanged, this, &CommentService::filtersChanged);
    connect(m_worker, &CommentWorker::ngAdded, this, &CommentService::ngAdded);
    connect(m_worker, &CommentWorker::ngRemoved, this, &CommentService::ngRemoved);
    connect(m_worker, &CommentWorker::ngUndone, this, &CommentService::ngUndone);
    connect(m_worker, &CommentWorker::regexAdded, this, &CommentService::regexAdded);
    connect(m_worker, &CommentWorker::regexRemoved, this, &CommentService::regexRemoved);
    connect(m_worker, &CommentWorker::operationFailed, this, &CommentService::operationFailed);
    connect(m_worker, &CommentWorker::commandFinished, this, [this](qint64) {
        m_pendingCommands = std::max(0, m_pendingCommands - 1);
        pumpProfile();
        pumpOpen();
        pumpTick();
    });
    connect(m_worker, &CommentWorker::opened, this, [this](quint64 generation, CommentSource source, qsizetype count) {
        if (m_control->stopping.load() || generation != m_control->generation.load()) {
            ++m_staleResults;
            return;
        }
        m_sessionReady = true;
        emit opened(generation, source, count);
    });
    connect(m_worker, &CommentWorker::tickCompleted, this,
            [this](quint64 generation, quint64 revision, PlaybackBatchResult result, const QString &error) {
                m_tickInFlight = false;
                if (!m_control->stopping.load() && generation == m_control->generation.load() &&
                    revision == m_control->revision.load()) {
                    if (error.isEmpty())
                        emit commentsReady(result);
                    else
                        emit warning(error);
                } else
                    ++m_staleResults;
                pumpTick();
            });
    m_thread->setObjectName(QStringLiteral("comment-network-storage"));
    m_thread->start();
}

CommentService::~CommentService() {
    shutdown();
    if (m_thread) {
        // Normal window close waits asynchronously for stopped(). This bounded fallback
        // is only for emergency/application teardown; never destroy a running QThread.
        if (m_thread->wait(2000))
            delete m_thread.data();
        else {
            qWarning() << "comment worker shutdown exceeded 2000ms; retaining owner until thread finishes";
            connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
        }
    }
}

bool CommentService::post(std::function<void(CommentWorker &)> action) {
    if (m_control->stopping.load() || !m_thread || !m_thread->isRunning())
        return false;
    if (m_pendingCommands >= 32)
        return false;
    ++m_pendingCommands;
    m_highWater = std::max(m_highWater, m_pendingCommands);
    auto *worker = m_worker;
    QMetaObject::invokeMethod(
        worker, [worker, action = std::move(action)]() mutable { worker->execute(std::move(action)); },
        Qt::QueuedConnection);
    return true;
}

quint64 CommentService::openVideo(const QString &path, const QString &id) {
    if (m_control->stopping.load())
        return m_control->generation.load();
    const auto generation = ++m_control->generation;
    ++m_control->revision;
    m_sessionReady = false;
    m_pendingTick.reset();
    // Never lose the newest navigation when the mutation queue is saturated.
    m_pendingOpen = PendingOpen{path, id, generation};
    pumpOpen();
    return generation;
}
void CommentService::pumpOpen() {
    if (!m_pendingOpen || m_control->stopping.load())
        return;
    const auto open = *m_pendingOpen;
    if (post([open](CommentWorker &worker) { worker.open(open.path, open.id, open.generation); }))
        m_pendingOpen.reset();
}
void CommentService::clearSession() {
    openVideo({}, {});
}
void CommentService::requestTick(qint64 positionMs, bool paused, bool seek) {
    if (!m_sessionReady || m_control->stopping.load())
        return;
    if (seek)
        ++m_control->revision;
    if (m_pendingTick) {
        ++m_coalescedTicks;
        seek = seek || m_pendingTick->isSeek;
    }
    m_pendingTick = PlaybackTick{positionMs, paused, seek};
    pumpTick();
}
void CommentService::pumpTick() {
    if (m_tickInFlight || !m_pendingTick || m_pendingProfile || m_control->stopping.load())
        return;
    const auto tick = *m_pendingTick;
    const auto generation = m_control->generation.load();
    const auto revision = m_control->revision.load();
    if (post([tick, generation, revision](CommentWorker &worker) { worker.tick(tick, generation, revision); })) {
        m_pendingTick.reset();
        m_tickInFlight = true;
    }
}
bool CommentService::setRuntimeProfile(const RuntimeProfileConfig &profile) {
    if (m_control->stopping.load())
        return false;
    // Settings are latest-wins state, not a lossy mutation. Retain one value even
    // when all command slots are occupied, and admit it before the next tick.
    m_pendingProfile = profile;
    pumpProfile();
    return true;
}
void CommentService::pumpProfile() {
    if (!m_pendingProfile || m_control->stopping.load())
        return;
    const auto profile = *m_pendingProfile;
    if (post([profile](CommentWorker &worker) { worker.profile(profile); }))
        m_pendingProfile.reset();
}
bool CommentService::addNgUser(const QString &id) {
    return post([id](CommentWorker &worker) { worker.addNg(id); });
}
bool CommentService::removeNgUser(const QString &id) {
    return post([id](CommentWorker &worker) { worker.removeNg(id); });
}
bool CommentService::undoLastNg(const QString &token) {
    return post([token](CommentWorker &worker) { worker.undo(token); });
}
bool CommentService::addRegexFilter(const QString &pattern) {
    return post([pattern](CommentWorker &worker) { worker.addRegex(pattern); });
}
bool CommentService::removeRegexFilter(qint64 id) {
    return post([id](CommentWorker &worker) { worker.removeRegex(id); });
}
bool CommentService::listFilters() {
    return post([](CommentWorker &worker) { worker.listFilters(); });
}
void CommentService::shutdown() {
    if (m_control->stopping.exchange(true))
        return;
    ++m_control->generation;
    m_pendingTick.reset();
    m_pendingOpen.reset();
    m_pendingProfile.reset();
    if (m_thread && m_thread->isRunning()) {
        auto *worker = m_worker;
        QMetaObject::invokeMethod(worker, [worker] { worker->shutdown(); }, Qt::QueuedConnection);
    }
}
bool CommentService::isStopped() const {
    return !m_thread || m_thread->isFinished();
}
} // namespace niconeon
#include "CommentService.moc"
