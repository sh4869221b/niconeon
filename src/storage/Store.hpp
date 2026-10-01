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
    ~Store();
    Store(const Store &) = delete;
    Store &operator=(const Store &) = delete;
    static Result<StorePaths> defaultPaths();
    static Result<std::unique_ptr<Store>> openDefault();
    static Result<std::unique_ptr<Store>> openMemory();
    static Result<std::unique_ptr<Store>> openWithPath(const QString &dataPath);
    static Result<std::unique_ptr<Store>> openWithPaths(const QString &dataPath, const QString &cachePath);
    Result<std::optional<CommentList>> loadCommentCache(const QString &videoId);
    Result<void> saveCommentCache(const QString &videoId, const CommentList &comments);
    Result<void> upsertVideoMap(const QString &videoPath, const QString &videoId);
    Result<bool> addNgUser(const QString &userId);
    Result<bool> removeNgUser(const QString &userId);
    Result<QStringList> listNgUsers();
    Result<RegexFilter> insertRegexFilter(const QString &pattern);
    Result<bool> removeRegexFilter(qint64 filterId);
    Result<QVector<RegexFilter>> listRegexFilters();

  private:
    Store();
    Result<void> open(const QString &dataPath, const QString &cachePath, bool memory);
    Result<void> initializeSchema();
    Result<void> migrateLegacyCache();
    Result<void> checkThread() const;
    Result<void> checkFilterCapacity();
    QString m_dataConnectionName;
    QString m_cacheConnectionName;
    QSqlDatabase m_data;
    QSqlDatabase m_cache;
    QThread *m_ownerThread = nullptr;
};

} // namespace niconeon
