#include "storage/Store.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <cmath>
#include <limits>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <shlobj.h>
#endif

namespace niconeon {
namespace {

AppError sqlError(const QString &context, const QSqlQuery &query) {
    return {QStringLiteral("%1: %2").arg(context, query.lastError().text())};
}

Result<void> execute(QSqlDatabase &database, const QString &statement, const QString &context) {
    QSqlQuery query(database);
    if (!query.exec(statement))
        return std::unexpected(sqlError(context, query));
    return {};
}

QString timestamp() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
QString nonNull(const QString &value) {
    return value.isNull() ? QStringLiteral("") : value;
}

#ifdef Q_OS_WIN
QString knownFolder(REFKNOWNFOLDERID id) {
    PWSTR value = nullptr;
    const auto status = SHGetKnownFolderPath(id, 0, nullptr, &value);
    if (FAILED(status))
        return {};
    const auto path = QString::fromWCharArray(value);
    CoTaskMemFree(value);
    return path;
}
#endif

QString absoluteEnvironmentRoot(const char *name, const QString &fallback) {
    const auto root = qEnvironmentVariable(name);
    return !root.isEmpty() && QDir::isAbsolutePath(root) ? root : fallback;
}

Result<CommentList> decodeComments(const QByteArray &payload, const QString &videoId) {
    const auto context = QStringLiteral("decode comment cache for %1").arg(videoId);
    if (payload.size() > MaxPayloadBytes)
        return std::unexpected(AppError{context + QStringLiteral(": payload exceeds the 64 MiB safety limit")});
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray())
        return std::unexpected(
            AppError{context + QStringLiteral(": invalid comment array: ") + parseError.errorString()});
    const auto array = document.array();
    if (array.size() > MaxComments)
        return std::unexpected(
            AppError{context + QStringLiteral(": comment count exceeds the 250000-comment safety limit")});
    CommentList comments;
    comments.reserve(array.size());
    for (const auto &value : array) {
        if (!value.isObject())
            return std::unexpected(AppError{context + QStringLiteral(": comment must be an object")});
        const auto object = value.toObject();
        const auto id = object.value(QStringLiteral("comment_id"));
        const auto at = object.value(QStringLiteral("at_ms"));
        const auto user = object.value(QStringLiteral("user_id"));
        const auto text = object.value(QStringLiteral("text"));
        // Qt 6 preserves qint64 JSON numbers. Reject fractional and out-of-range values
        // instead of silently coercing corrupt persisted data to timestamp zero.
        const auto numeric = at.toDouble();
        const auto minimum = static_cast<double>(std::numeric_limits<qint64>::min());
        const auto maximumExclusive = -minimum;
        if (!id.isString() || !at.isDouble() || !user.isString() || !text.isString() || !std::isfinite(numeric) ||
            std::floor(numeric) != numeric || numeric < minimum || numeric >= maximumExclusive) {
            // INT64_MAX is internally exact in QJsonValue although its double rounds
            // to 2^63. A round-trip through toInteger distinguishes that integer.
            if (!(id.isString() && at.isDouble() && user.isString() && text.isString() && numeric == maximumExclusive &&
                  at.toInteger() == std::numeric_limits<qint64>::max()))
                return std::unexpected(AppError{context + QStringLiteral(": invalid comment fields")});
        }
        comments.append({id.toString(), at.toInteger(), user.toString(), text.toString()});
    }
    if (auto valid = validateComments(comments); !valid)
        return std::unexpected(AppError{context + QStringLiteral(": ") + valid.error().message});
    return comments;
}

} // namespace

Store::Store()
    : m_dataConnectionName(QStringLiteral("niconeon-data-") + QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_cacheConnectionName(QStringLiteral("niconeon-cache-") + QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_ownerThread(QThread::currentThread()) {}

Store::~Store() {
    Q_ASSERT(m_ownerThread == QThread::currentThread());
    // No query or database handle may remain when removing a named connection.
    if (m_data.isValid())
        m_data.close();
    if (m_cache.isValid())
        m_cache.close();
    m_data = QSqlDatabase();
    m_cache = QSqlDatabase();
    if (QSqlDatabase::contains(m_dataConnectionName))
        QSqlDatabase::removeDatabase(m_dataConnectionName);
    if (QSqlDatabase::contains(m_cacheConnectionName))
        QSqlDatabase::removeDatabase(m_cacheConnectionName);
}

Result<void> Store::checkThread() const {
    if (QThread::currentThread() != m_ownerThread)
        return std::unexpected(AppError{QStringLiteral("SQLite store used outside its owning worker thread")});
    return {};
}

Result<StorePaths> Store::defaultPaths() {
    QString dataRoot;
    QString cacheRoot;
#ifdef Q_OS_WIN
    dataRoot = knownFolder(FOLDERID_RoamingAppData);
    cacheRoot = knownFolder(FOLDERID_LocalAppData);
    if (dataRoot.isEmpty() || cacheRoot.isEmpty())
        return std::unexpected(AppError{QStringLiteral("failed to determine data/cache directory")});
    dataRoot += QStringLiteral("/sh4869221b/niconeon/data");
    cacheRoot += QStringLiteral("/sh4869221b/niconeon/cache");
#elif defined(Q_OS_MACOS)
    const auto home = QDir::homePath();
    if (home.isEmpty() || !QDir::isAbsolutePath(home))
        return std::unexpected(AppError{QStringLiteral("failed to determine data directory")});
    dataRoot = home + QStringLiteral("/Library/Application Support/com.sh4869221b.niconeon");
    cacheRoot = home + QStringLiteral("/Library/Caches/com.sh4869221b.niconeon");
#else
    const auto home = QDir::homePath();
    if (home.isEmpty() || !QDir::isAbsolutePath(home))
        return std::unexpected(AppError{QStringLiteral("failed to determine data directory")});
    dataRoot =
        absoluteEnvironmentRoot("XDG_DATA_HOME", home + QStringLiteral("/.local/share")) + QStringLiteral("/niconeon");
    cacheRoot =
        absoluteEnvironmentRoot("XDG_CACHE_HOME", home + QStringLiteral("/.cache")) + QStringLiteral("/niconeon");
#endif
    return StorePaths{QDir::cleanPath(dataRoot + QStringLiteral("/niconeon.db")),
                      QDir::cleanPath(cacheRoot + QStringLiteral("/comment-cache.db"))};
}

Result<std::unique_ptr<Store>> Store::openDefault() {
    auto paths = defaultPaths();
    if (!paths)
        return std::unexpected(paths.error());
    return openWithPaths(paths->dataPath, paths->cachePath);
}

Result<std::unique_ptr<Store>> Store::openMemory() {
    auto store = std::unique_ptr<Store>(new Store);
    auto opened = store->open(QStringLiteral(":memory:"), QStringLiteral(":memory:"), true);
    if (!opened)
        return std::unexpected(opened.error());
    return store;
}

Result<std::unique_ptr<Store>> Store::openWithPath(const QString &dataPath) {
    return openWithPaths(dataPath, QFileInfo(dataPath).dir().filePath(QStringLiteral("niconeon-cache.db")));
}

Result<std::unique_ptr<Store>> Store::openWithPaths(const QString &dataPath, const QString &cachePath) {
    auto store = std::unique_ptr<Store>(new Store);
    auto opened = store->open(dataPath, cachePath, false);
    if (!opened)
        return std::unexpected(opened.error());
    return store;
}

Result<void> Store::open(const QString &dataPath, const QString &cachePath, bool memory) {
    if (!memory) {
        const QFileInfo dataInfo(dataPath), cacheInfo(cachePath);
        if (dataInfo.absoluteFilePath() == cacheInfo.absoluteFilePath() ||
            (!dataInfo.canonicalFilePath().isEmpty() && dataInfo.canonicalFilePath() == cacheInfo.canonicalFilePath()))
            return std::unexpected(AppError{QStringLiteral("data and cache databases must use distinct paths")});
        for (const auto &path : {dataPath, cachePath}) {
            if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath()))
                return std::unexpected(AppError{QStringLiteral("create directory for %1").arg(path)});
        }
    }
    m_data = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_dataConnectionName);
    m_cache = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_cacheConnectionName);
    m_data.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=2000"));
    m_cache.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=2000"));
    m_data.setDatabaseName(dataPath);
    m_cache.setDatabaseName(cachePath);
    if (!m_data.open())
        return std::unexpected(AppError{QStringLiteral("open sqlite %1: %2").arg(dataPath, m_data.lastError().text())});
    if (!m_cache.open())
        return std::unexpected(
            AppError{QStringLiteral("open sqlite %1: %2").arg(cachePath, m_cache.lastError().text())});
    if (!memory) {
        if (auto result =
                execute(m_data, QStringLiteral("PRAGMA journal_mode=WAL"), QStringLiteral("set WAL mode for data db"));
            !result)
            return result;
        if (auto result = execute(m_cache, QStringLiteral("PRAGMA journal_mode=WAL"),
                                  QStringLiteral("set WAL mode for cache db"));
            !result)
            return result;
    }
    if (auto result = initializeSchema(); !result)
        return result;
    return migrateLegacyCache();
}

Result<void> Store::initializeSchema() {
    const QStringList dataStatements{
        QStringLiteral("CREATE TABLE IF NOT EXISTS ng_users (user_id TEXT PRIMARY KEY, created_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS regex_filters (id INTEGER PRIMARY KEY AUTOINCREMENT, pattern TEXT "
                       "NOT NULL, created_at TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS video_map (video_path TEXT PRIMARY KEY, video_id TEXT NOT NULL, "
                       "last_opened_at TEXT NOT NULL)")};
    for (const auto &statement : dataStatements) {
        if (auto result = execute(m_data, statement, QStringLiteral("initialize data schema")); !result)
            return result;
    }
    return execute(m_cache,
                   QStringLiteral("CREATE TABLE IF NOT EXISTS comment_cache (video_id TEXT PRIMARY KEY, fetched_at "
                                  "TEXT NOT NULL, payload_json TEXT NOT NULL)"),
                   QStringLiteral("initialize cache schema"));
}

Result<void> Store::migrateLegacyCache() {
    {
        QSqlQuery exists(m_data);
        if (!exists.exec(
                QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='comment_cache' LIMIT 1")))
            return std::unexpected(sqlError(QStringLiteral("find legacy comment cache"), exists));
        if (!exists.next())
            return {};
    }
    if (!m_cache.transaction())
        return std::unexpected(AppError{QStringLiteral("begin cache migration: %1").arg(m_cache.lastError().text())});
    Result<void> copied;
    {
        QSqlQuery rows(m_data);
        rows.setForwardOnly(true);
        if (!rows.exec(QStringLiteral(
                "SELECT video_id, fetched_at, length(CAST(payload_json AS BLOB)), payload_json FROM comment_cache"))) {
            copied = std::unexpected(sqlError(QStringLiteral("prepare legacy comment cache select"), rows));
        } else {
            QSqlQuery insert(m_cache);
            if (!insert.prepare(QStringLiteral(
                    "INSERT INTO comment_cache(video_id,fetched_at,payload_json) VALUES(?,?,?) ON CONFLICT(video_id) "
                    "DO UPDATE SET fetched_at=excluded.fetched_at,payload_json=excluded.payload_json")))
                copied = std::unexpected(sqlError(QStringLiteral("prepare cache migration insert"), insert));
            while (copied && rows.next()) {
                if (rows.value(2).toLongLong() > MaxPayloadBytes) {
                    copied = std::unexpected(
                        AppError{QStringLiteral("legacy cache payload exceeds the 64 MiB safety limit")});
                    break;
                }
                insert.bindValue(0, rows.value(0));
                insert.bindValue(1, rows.value(1));
                insert.bindValue(2, rows.value(3));
                if (!insert.exec())
                    copied = std::unexpected(sqlError(QStringLiteral("copy legacy comment cache"), insert));
            }
            if (copied && rows.lastError().isValid())
                copied = std::unexpected(sqlError(QStringLiteral("read legacy comment cache"), rows));
        }
    }
    if (!copied) {
        m_cache.rollback();
        return copied;
    }
    if (!m_cache.commit()) {
        const auto error = AppError{QStringLiteral("commit cache migration: %1").arg(m_cache.lastError().text())};
        m_cache.rollback();
        return std::unexpected(error);
    }
    // Durable copy first. A crash or failed DROP leaves a retryable legacy table.
    return execute(m_data, QStringLiteral("DROP TABLE IF EXISTS comment_cache"),
                   QStringLiteral("drop legacy comment_cache table"));
}

Result<std::optional<CommentList>> Store::loadCommentCache(const QString &videoId) {
    if (auto valid = checkThread(); !valid)
        return std::unexpected(valid.error());
    QSqlQuery sizeQuery(m_cache);
    sizeQuery.prepare(QStringLiteral("SELECT length(CAST(payload_json AS BLOB)) FROM comment_cache WHERE video_id=?"));
    sizeQuery.addBindValue(nonNull(videoId));
    if (!sizeQuery.exec())
        return std::unexpected(sqlError(QStringLiteral("read comment cache size"), sizeQuery));
    if (!sizeQuery.next())
        return std::optional<CommentList>{};
    if (sizeQuery.value(0).toLongLong() > MaxPayloadBytes)
        return std::unexpected(AppError{QStringLiteral("comment cache payload exceeds the 64 MiB safety limit")});
    sizeQuery.finish();
    QSqlQuery query(m_cache);
    // Repeat the size predicate so a concurrent writer cannot bypass the allocation guard.
    query.prepare(QStringLiteral(
        "SELECT payload_json FROM comment_cache WHERE video_id=? AND length(CAST(payload_json AS BLOB))<=?"));
    query.addBindValue(nonNull(videoId));
    query.addBindValue(MaxPayloadBytes);
    if (!query.exec())
        return std::unexpected(sqlError(QStringLiteral("load comment cache"), query));
    if (!query.next())
        return std::unexpected(
            AppError{QStringLiteral("comment cache changed while reading; retry the open operation")});
    auto comments = decodeComments(query.value(0).toString().toUtf8(), videoId);
    if (!comments)
        return std::unexpected(comments.error());
    return std::optional<CommentList>{std::move(*comments)};
}

Result<void> Store::saveCommentCache(const QString &videoId, const CommentList &comments) {
    if (auto valid = checkThread(); !valid)
        return valid;
    if (auto valid = validateComments(comments); !valid)
        return valid;
    QJsonArray array;
    for (const auto &comment : comments) {
        array.append(QJsonObject{{QStringLiteral("comment_id"), comment.commentId},
                                 {QStringLiteral("at_ms"), comment.atMs},
                                 {QStringLiteral("user_id"), comment.userId},
                                 {QStringLiteral("text"), comment.text}});
    }
    const auto payload = QJsonDocument(array).toJson(QJsonDocument::Compact);
    if (payload.size() > MaxPayloadBytes)
        return std::unexpected(AppError{QStringLiteral("comment cache payload exceeds the 64 MiB safety limit")});
    QSqlQuery query(m_cache);
    query.prepare(QStringLiteral(
        "INSERT INTO comment_cache(video_id,fetched_at,payload_json) VALUES(?,?,?) ON CONFLICT(video_id) DO UPDATE SET "
        "fetched_at=excluded.fetched_at,payload_json=excluded.payload_json"));
    query.addBindValue(nonNull(videoId));
    query.addBindValue(timestamp());
    query.addBindValue(QString::fromUtf8(payload));
    if (!query.exec())
        return std::unexpected(sqlError(QStringLiteral("save comment cache"), query));
    return {};
}

Result<void> Store::upsertVideoMap(const QString &videoPath, const QString &videoId) {
    if (auto valid = checkThread(); !valid)
        return valid;
    QSqlQuery query(m_data);
    query.prepare(QStringLiteral(
        "INSERT INTO video_map(video_path,video_id,last_opened_at) VALUES(?,?,?) ON CONFLICT(video_path) DO UPDATE SET "
        "video_id=excluded.video_id,last_opened_at=excluded.last_opened_at"));
    query.addBindValue(nonNull(videoPath));
    query.addBindValue(nonNull(videoId));
    query.addBindValue(timestamp());
    if (!query.exec())
        return std::unexpected(sqlError(QStringLiteral("save video mapping"), query));
    return {};
}

Result<void> Store::checkFilterCapacity() {
    QSqlQuery query(m_data);
    if (!query.exec(QStringLiteral("SELECT (SELECT COUNT(*) FROM ng_users)+(SELECT COUNT(*) FROM regex_filters)")) ||
        !query.next())
        return std::unexpected(sqlError(QStringLiteral("count filter entries"), query));
    if (query.value(0).toLongLong() >= MaxFilterEntries)
        return std::unexpected(AppError{QStringLiteral("filter count exceeds the 10000-entry safety limit")});
    return {};
}

Result<bool> Store::addNgUser(const QString &userId) {
    if (auto valid = checkThread(); !valid)
        return std::unexpected(valid.error());
    if (auto valid = validateUserId(userId); !valid)
        return std::unexpected(valid.error());
    {
        QSqlQuery exists(m_data);
        exists.prepare(QStringLiteral("SELECT 1 FROM ng_users WHERE user_id=?"));
        exists.addBindValue(nonNull(userId));
        if (!exists.exec())
            return std::unexpected(sqlError(QStringLiteral("find ng user"), exists));
        if (exists.next())
            return false;
    }
    if (auto capacity = checkFilterCapacity(); !capacity)
        return std::unexpected(capacity.error());
    QSqlQuery query(m_data);
    query.prepare(QStringLiteral("INSERT OR IGNORE INTO ng_users(user_id,created_at) VALUES(?,?)"));
    query.addBindValue(nonNull(userId));
    query.addBindValue(timestamp());
    if (!query.exec())
        return std::unexpected(sqlError(QStringLiteral("save ng user"), query));
    return query.numRowsAffected() > 0;
}

Result<bool> Store::removeNgUser(const QString &userId) {
    if (auto valid = checkThread(); !valid)
        return std::unexpected(valid.error());
    if (auto valid = validateUserId(userId); !valid)
        return std::unexpected(valid.error());
    QSqlQuery query(m_data);
    query.prepare(QStringLiteral("DELETE FROM ng_users WHERE user_id=?"));
    query.addBindValue(nonNull(userId));
    if (!query.exec())
        return std::unexpected(sqlError(QStringLiteral("delete ng user"), query));
    return query.numRowsAffected() > 0;
}

Result<QStringList> Store::listNgUsers() {
    if (auto valid = checkThread(); !valid)
        return std::unexpected(valid.error());
    QSqlQuery query(m_data);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("SELECT user_id FROM ng_users ORDER BY created_at ASC LIMIT 10001")))
        return std::unexpected(sqlError(QStringLiteral("list ng users"), query));
    QStringList users;
    while (query.next()) {
        if (users.size() >= MaxFilterEntries)
            return std::unexpected(
                AppError{QStringLiteral("persisted filter count exceeds the 10000-entry safety limit")});
        if (query.value(0).isNull())
            return std::unexpected(AppError{QStringLiteral("invalid NULL user ID in persisted ng_users")});
        const auto user = query.value(0).toString();
        if (auto valid = validateUserId(user); !valid)
            return std::unexpected(valid.error());
        users.append(user);
    }
    if (query.lastError().isValid())
        return std::unexpected(sqlError(QStringLiteral("read ng users"), query));
    return users;
}

Result<RegexFilter> Store::insertRegexFilter(const QString &pattern) {
    if (auto valid = checkThread(); !valid)
        return std::unexpected(valid.error());
    if (pattern.size() > MaxPatternBytes || pattern.toUtf8().size() > MaxPatternBytes)
        return std::unexpected(AppError{QStringLiteral("invalid regex: pattern exceeds the 4096-byte safety limit")});
    if (auto capacity = checkFilterCapacity(); !capacity)
        return std::unexpected(capacity.error());
    const auto createdAt = QDateTime::currentDateTimeUtc();
    QSqlQuery query(m_data);
    query.prepare(QStringLiteral("INSERT INTO regex_filters(pattern,created_at) VALUES(?,?)"));
    query.addBindValue(nonNull(pattern));
    query.addBindValue(createdAt.toString(Qt::ISODateWithMs));
    if (!query.exec())
        return std::unexpected(sqlError(QStringLiteral("insert regex filter"), query));
    return RegexFilter{query.lastInsertId().toLongLong(), pattern, createdAt};
}

Result<bool> Store::removeRegexFilter(qint64 filterId) {
    if (auto valid = checkThread(); !valid)
        return std::unexpected(valid.error());
    QSqlQuery query(m_data);
    query.prepare(QStringLiteral("DELETE FROM regex_filters WHERE id=?"));
    query.addBindValue(filterId);
    if (!query.exec())
        return std::unexpected(sqlError(QStringLiteral("delete regex filter"), query));
    return query.numRowsAffected() > 0;
}

Result<QVector<RegexFilter>> Store::listRegexFilters() {
    if (auto valid = checkThread(); !valid)
        return std::unexpected(valid.error());
    QSqlQuery query(m_data);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("SELECT id,pattern,created_at FROM regex_filters ORDER BY id ASC LIMIT 10001")))
        return std::unexpected(sqlError(QStringLiteral("list regex filters"), query));
    QVector<RegexFilter> filters;
    while (query.next()) {
        if (filters.size() >= MaxFilterEntries)
            return std::unexpected(
                AppError{QStringLiteral("persisted filter count exceeds the 10000-entry safety limit")});
        const auto pattern = query.value(1).toString();
        if (pattern.size() > MaxPatternBytes || pattern.toUtf8().size() > MaxPatternBytes)
            return std::unexpected(AppError{QStringLiteral("persisted regex exceeds the 4096-byte safety limit")});
        const auto createdRaw = query.value(2).toString();
        static const QRegularExpression rfc3339(
            QStringLiteral("^\\d{4}-\\d{2}-\\d{2}[Tt]\\d{2}:\\d{2}:\\d{2}(?:\\.\\d+)?(?:[Zz]|[+-]\\d{2}:\\d{2})$"));
        if (!rfc3339.match(createdRaw).hasMatch())
            return std::unexpected(
                AppError{QStringLiteral("invalid created_at for regex filter %1").arg(query.value(0).toLongLong())});
        auto createdAt = QDateTime::fromString(createdRaw, Qt::ISODateWithMs);
        if (!createdAt.isValid())
            createdAt = QDateTime::fromString(createdRaw, Qt::ISODate);
        if (!createdAt.isValid())
            return std::unexpected(
                AppError{QStringLiteral("invalid created_at for regex filter %1").arg(query.value(0).toLongLong())});
        filters.append({query.value(0).toLongLong(), pattern, createdAt.toUTC()});
    }
    if (query.lastError().isValid())
        return std::unexpected(sqlError(QStringLiteral("read regex filters"), query));
    return filters;
}

} // namespace niconeon
