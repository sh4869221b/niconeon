#pragma once

#include "domain/Domain.hpp"
#include <QSqlDatabase>
#include <QThread>
#include <memory>

namespace niconeon {

struct StorePaths {
    QString dataPath;
    QString cachePath;
};

// Every operation, including destruction, belongs to the creating worker thread.
class Store {
  public:
    enum class CachePolicy { Required, Optional };

    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    static Result<StorePaths> defaultPaths(CachePolicy cachePolicy = CachePolicy::Required);
    static Result<std::unique_ptr<Store>> openDefault(CachePolicy cachePolicy = CachePolicy::Required);
    static Result<std::unique_ptr<Store>> openMemory();
    static Result<std::unique_ptr<Store>> openWithPath(const QString &dataPath);
    static Result<std::unique_ptr<Store>> openWithPaths(const QString &dataPath, const QString &cachePath,
                                                        CachePolicy cachePolicy = CachePolicy::Required);
    const std::optional<AppError> &cacheError() const {
        return m_cacheError;
    }
    Result<std::optional<CommentList>> loadCommentCache(const QString &videoId);
    Result<void> saveCommentCache(const QString &videoId, const CommentList &comments);
    Result<void> upsertVideoMap(const QString &videoPath, const QString &videoId);
    // A duplicate returns no registration ID.
    Result<std::optional<QString>> addNgUser(const QString &userId);
    Result<bool> removeNgUser(const QString &userId, const std::optional<QString> &registrationId = std::nullopt);
    Result<QStringList> listNgUsers();
    Result<RegexFilter> insertRegexFilter(const QString &pattern);
    Result<bool> removeRegexFilter(qint64 filterId);
    Result<QVector<RegexFilter>> listRegexFilters();

  private:
    Store();
    Result<void> open(const QString &dataPath, const QString &cachePath, bool memory, CachePolicy cachePolicy);
    Result<void> initializeDataSchema();
    Result<void> initializeCache(const QString &cachePath, bool memory);
    Result<void> migrateLegacyCache();
    Result<void> checkThread() const;
    Result<void> checkFilterCapacity();
    QString m_dataConnectionName;
    QString m_cacheConnectionName;
    QSqlDatabase m_data;
    QSqlDatabase m_cache;
    std::optional<AppError> m_cacheError;
    QThread *m_ownerThread = nullptr;
};

} // namespace niconeon
