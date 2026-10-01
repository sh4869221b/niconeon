#include "storage/Store.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <limits>

using namespace niconeon;

namespace {
class Database {
  public:
    explicit Database(const QString &path)
        : name(QUuid::createUuid().toString()), db(QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name)) {
        db.setDatabaseName(path);
        opened = db.open();
    }
    ~Database() {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    bool exec(const QString &sql) {
        QSqlQuery query(db);
        return query.exec(sql);
    }
    QString name;
    QSqlDatabase db;
    bool opened;
};
class Environment {
  public:
    Environment(const char *name, const QByteArray &value, bool remove = false)
        : name(name), value(qgetenv(name)), wasSet(qEnvironmentVariableIsSet(name)) {
        if (remove)
            qunsetenv(name);
        else
            qputenv(name, value);
    }
    ~Environment() {
        if (wasSet)
            qputenv(name.constData(), value);
        else
            qunsetenv(name.constData());
    }
    QByteArray name, value;
    bool wasSet;
};
const auto cacheSchema = QStringLiteral(
    "CREATE TABLE comment_cache(video_id TEXT PRIMARY KEY,fetched_at TEXT NOT NULL,payload_json TEXT NOT NULL)");
const auto legacyInsert = QStringLiteral("INSERT INTO comment_cache "
                                         "VALUES('sm9','2024-01-01T00:00:00Z','[{\"comment_id\":\"c1\",\"at_ms\":1,"
                                         "\"user_id\":\"u1\",\"text\":\"legacy\"}]')");
} // namespace

class StoreTest : public QObject {
    Q_OBJECT
  private slots:
    void memoryCacheRoundtripPreservesUnicodeAndIntegers() {
        auto opened = Store::openMemory();
        QVERIFY(opened);
        auto &store = **opened;
        CommentList comments{
            {QStringLiteral("日本語"), 100, QStringLiteral("u1"), QStringLiteral("こんにちは😄\n\"quote\"")},
            {QStringLiteral("min"), std::numeric_limits<qint64>::min(), QStringLiteral("u2"),
             QStringLiteral("minimum")},
            {QStringLiteral("max"), std::numeric_limits<qint64>::max(), QStringLiteral("u2"),
             QStringLiteral("maximum")}};
        QVERIFY(store.saveCommentCache(QStringLiteral("sm9"), comments));
        const auto loaded = store.loadCommentCache(QStringLiteral("sm9"));
        QVERIFY2(loaded, loaded ? "" : qPrintable(loaded.error().message));
        QVERIFY(loaded->has_value());
        QCOMPARE(**loaded, comments);
        const auto missing = store.loadCommentCache(QStringLiteral("absent"));
        QVERIFY(missing && !missing->has_value());
        QVERIFY(store.saveCommentCache(QStringLiteral("sm9"), {}));
        QVERIFY(store.loadCommentCache(QStringLiteral("sm9"))->value().isEmpty());
    }

    void migratesLegacyTableAndRetainsData() {
        QTemporaryDir directory;
        const auto dataPath = directory.filePath(QStringLiteral("niconeon.db"));
        const auto cachePath = directory.filePath(QStringLiteral("comment-cache.db"));
        {
            Database legacy(dataPath);
            QVERIFY(legacy.opened);
            QVERIFY(legacy.exec(cacheSchema));
            QVERIFY(legacy.exec(legacyInsert));
            QVERIFY(legacy.exec(
                QStringLiteral("CREATE TABLE ng_users(user_id TEXT PRIMARY KEY,created_at TEXT NOT NULL)")));
            QVERIFY(legacy.exec(QStringLiteral("INSERT INTO ng_users VALUES('kept','2024-01-01T00:00:00Z')")));
        }
        {
            Database existingCache(cachePath);
            QVERIFY(existingCache.exec(cacheSchema));
            QVERIFY(existingCache.exec(
                QStringLiteral("INSERT INTO comment_cache VALUES('sm9','2026-01-01T00:00:00Z','[]')")));
        }
        {
            auto store = Store::openWithPaths(dataPath, cachePath);
            QVERIFY2(store, store ? "" : qPrintable(store.error().message));
            const auto loaded = (*store)->loadCommentCache(QStringLiteral("sm9"));
            QVERIFY(loaded && loaded->has_value());
            QCOMPARE(loaded->value()[0].text, QStringLiteral("legacy"));
            QCOMPARE((*store)->listNgUsers().value(), QStringList{QStringLiteral("kept")});
        }
        Database check(dataPath);
        QSqlQuery query(check.db);
        QVERIFY(query.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE name='comment_cache'")));
        QVERIFY(!query.next());
        // Opening again is idempotent.
        QVERIFY(Store::openWithPaths(dataPath, cachePath));
    }

    void oversizedLegacyPayloadIsRejectedWithoutDroppingSource() {
        QTemporaryDir directory;
        const auto dataPath = directory.filePath(QStringLiteral("data.db"));
        const auto cachePath = directory.filePath(QStringLiteral("cache.db"));
        {
            Database legacy(dataPath);
            QVERIFY(legacy.exec(cacheSchema));
            QVERIFY(legacy.exec(
                QStringLiteral("INSERT INTO comment_cache VALUES('large','2024-01-01T00:00:00Z',zeroblob(%1))")
                    .arg(MaxPayloadBytes + 1)));
        }
        auto failed = Store::openWithPaths(dataPath, cachePath);
        QVERIFY(!failed);
        QVERIFY(failed.error().message.contains(QStringLiteral("64 MiB")));
        Database legacy(dataPath), cache(cachePath);
        QSqlQuery source(legacy.db), destination(cache.db);
        QVERIFY(source.exec(QStringLiteral("SELECT COUNT(*) FROM comment_cache")) && source.next());
        QCOMPARE(source.value(0).toInt(), 1);
        QVERIFY(destination.exec(QStringLiteral("SELECT COUNT(*) FROM comment_cache")) && destination.next());
        QCOMPARE(destination.value(0).toInt(), 0);
    }

    void failedMigrationRetainsLegacyAndRollsBackDestination() {
        QTemporaryDir directory;
        const auto dataPath = directory.filePath(QStringLiteral("data.db"));
        const auto cachePath = directory.filePath(QStringLiteral("cache.db"));
        {
            Database legacy(dataPath), cache(cachePath);
            QVERIFY(legacy.exec(cacheSchema));
            QVERIFY(legacy.exec(legacyInsert));
            QVERIFY(
                legacy.exec(QStringLiteral("INSERT INTO comment_cache VALUES('sm10','2024-01-01T00:00:00Z','[]')")));
            QVERIFY(cache.exec(cacheSchema));
            QVERIFY(cache.exec(QStringLiteral("CREATE TRIGGER reject_copy BEFORE INSERT ON comment_cache WHEN "
                                              "new.video_id='sm10' BEGIN SELECT RAISE(FAIL,'cannot migrate'); END")));
        }
        auto failed = Store::openWithPaths(dataPath, cachePath);
        QVERIFY(!failed);
        Database legacy(dataPath), cache(cachePath);
        QSqlQuery source(legacy.db), destination(cache.db);
        QVERIFY(source.exec(QStringLiteral("SELECT COUNT(*) FROM comment_cache")) && source.next());
        QCOMPARE(source.value(0).toInt(), 2);
        QVERIFY(destination.exec(QStringLiteral("SELECT COUNT(*) FROM comment_cache")) && destination.next());
        QCOMPARE(destination.value(0).toInt(), 0);
    }

    void corruptCacheIsAnExplicitError() {
        QTemporaryDir directory;
        const auto dataPath = directory.filePath(QStringLiteral("data.db"));
        const auto cachePath = directory.filePath(QStringLiteral("cache.db"));
        auto store = Store::openWithPaths(dataPath, cachePath);
        QVERIFY(store);
        Database cache(cachePath);
        QVERIFY(
            cache.exec(QStringLiteral("INSERT INTO comment_cache VALUES('sm9','2024-01-01T00:00:00Z','{not-json}')")));
        auto broken = (*store)->loadCommentCache(QStringLiteral("sm9"));
        QVERIFY(!broken);
        QVERIFY(broken.error().message.contains(QStringLiteral("decode comment cache for sm9")));
        QVERIFY(cache.exec(
            QStringLiteral("UPDATE comment_cache SET "
                           "payload_json='[{\"comment_id\":\"a\",\"at_ms\":1.5,\"user_id\":\"u\",\"text\":\"t\"}]'")));
        QVERIFY(!(*store)->loadCommentCache(QStringLiteral("sm9")));
        QVERIFY(cache.exec(QStringLiteral("UPDATE comment_cache SET payload_json='[{}]'")));
        QVERIFY(!(*store)->loadCommentCache(QStringLiteral("sm9")));
    }

    void cacheSizeGuardRunsBeforeDecode() {
        QTemporaryDir directory;
        const auto cachePath = directory.filePath(QStringLiteral("cache.db"));
        auto store = Store::openWithPaths(directory.filePath(QStringLiteral("data.db")), cachePath);
        QVERIFY(store);
        Database cache(cachePath);
        QVERIFY(cache.exec(
            QStringLiteral("INSERT INTO comment_cache VALUES('oversize','2024-01-01T00:00:00Z',zeroblob(67108865))")));
        const auto loaded = (*store)->loadCommentCache(QStringLiteral("oversize"));
        QVERIFY(!loaded);
        QVERIFY(loaded.error().message.contains(QStringLiteral("64 MiB safety limit")));
    }

    void pathsPreserveLegacyLocations() {
#if defined(Q_OS_LINUX)
        QTemporaryDir data, cache;
        Environment dataRoot("XDG_DATA_HOME", data.path().toUtf8());
        Environment cacheRoot("XDG_CACHE_HOME", cache.path().toUtf8());
        auto paths = Store::defaultPaths();
        QVERIFY(paths);
        QCOMPARE(paths->dataPath, data.filePath(QStringLiteral("niconeon/niconeon.db")));
        QCOMPARE(paths->cachePath, cache.filePath(QStringLiteral("niconeon/comment-cache.db")));
        {
            Environment noData("XDG_DATA_HOME", {}, true), noCache("XDG_CACHE_HOME", {}, true);
            paths = Store::defaultPaths();
            QVERIFY(paths);
            QCOMPARE(paths->dataPath, QDir::homePath() + QStringLiteral("/.local/share/niconeon/niconeon.db"));
            QCOMPARE(paths->cachePath, QDir::homePath() + QStringLiteral("/.cache/niconeon/comment-cache.db"));
        }
        {
            Environment relativeData("XDG_DATA_HOME", "relative");
            paths = Store::defaultPaths();
            QVERIFY(paths);
            QVERIFY(QDir::isAbsolutePath(paths->dataPath));
        }
#else
        const auto paths = Store::defaultPaths();
        QVERIFY(paths);
        QVERIFY(QDir::isAbsolutePath(paths->dataPath));
        QCOMPARE(QFileInfo(paths->dataPath).fileName(), QStringLiteral("niconeon.db"));
#endif
    }

    void connectionLifetimeAndThreadGuard() {
        const auto before = QSqlDatabase::connectionNames().size();
        {
            auto opened = Store::openMemory();
            QVERIFY(opened);
            QCOMPARE(QSqlDatabase::connectionNames().size(), before + 2);
            bool rejected = false;
            auto worker = QThread::create([&] { rejected = !(*opened)->listNgUsers(); });
            worker->start();
            QVERIFY(worker->wait(5000));
            delete worker;
            QVERIFY(rejected);
        }
        QCOMPARE(QSqlDatabase::connectionNames().size(), before);
    }

    void refusesSameDataAndCacheFile() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("same.db"));
        QVERIFY(!Store::openWithPaths(path, path));
    }

    void capacityBoundIsExplicitAndDuplicateRemainsNoop() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("data.db"));
        auto store = Store::openWithPaths(path, directory.filePath(QStringLiteral("cache.db")));
        QVERIFY(store);
        Database data(path);
        QVERIFY(data.exec(QStringLiteral("WITH RECURSIVE n(x) AS (VALUES(0) UNION ALL SELECT x+1 FROM n WHERE x<9999) "
                                         "INSERT INTO ng_users SELECT 'u'||x,'2024-01-01T00:00:00Z' FROM n")));
        QCOMPARE((*store)->listNgUsers()->size(), MaxFilterEntries);
        auto duplicate = (*store)->addNgUser(QStringLiteral("u0"));
        QVERIFY(duplicate && !*duplicate);
        QVERIFY(!(*store)->addNgUser(QStringLiteral("new")));
        QVERIFY(!(*store)->insertRegexFilter(QStringLiteral("new")));
    }

    void timestampsAcceptRustPrecisionButRejectMissingZone() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("data.db"));
        auto store = Store::openWithPaths(path, directory.filePath(QStringLiteral("cache.db")));
        QVERIFY(store);
        Database data(path);
        QVERIFY(data.exec(QStringLiteral(
            "INSERT INTO regex_filters(pattern,created_at) VALUES('valid','2026-03-07T00:00:00.123456789+00:00')")));
        const auto filters = (*store)->listRegexFilters();
        QVERIFY(filters);
        QCOMPARE(filters->first().createdAt.time().msec(), 123);
        QVERIFY(data.exec(QStringLiteral("UPDATE regex_filters SET created_at='2026-03-07'")));
        QVERIFY(!(*store)->listRegexFilters());
    }
};

QTEST_GUILESS_MAIN(StoreTest)
#include "store_test.moc"
