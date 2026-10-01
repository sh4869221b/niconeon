#include "filters/FilterManager.hpp"

#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace niconeon;

namespace {
bool executeExternal(const QString &path, const QString &statement) {
    const auto name = QUuid::createUuid().toString();
    bool success = false;
    {
        auto database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), name);
        database.setDatabaseName(path);
        if (database.open()) {
            QSqlQuery query(database);
            success = query.exec(statement);
        }
    }
    QSqlDatabase::removeDatabase(name);
    return success;
}
} // namespace

class FilterManagerTest : public QObject {
    Q_OBJECT
  private slots:
    void ngAndRegexBehavior() {
        FilterEngine engine;
        QVERIFY(engine.addRegexFilter({1, QStringLiteral("hello"), QDateTime::currentDateTimeUtc()}));
        engine.addNgUser(QStringLiteral("u1"));
        QCOMPARE(engine.shouldHide({QStringLiteral("c1"), 100, QStringLiteral("u1"), QStringLiteral("anything")})
                     .value_or(false),
                 true);
        QCOMPARE(engine.shouldHide({QStringLiteral("c2"), 100, QStringLiteral("u2"), QStringLiteral("hello world")})
                     .value_or(false),
                 true);
        QCOMPARE(engine.shouldHide({QStringLiteral("c3"), 100, QStringLiteral("u2"), QStringLiteral("other")})
                     .value_or(true),
                 false);
        QVERIFY(!engine.addRegexFilter({2, QStringLiteral("("), QDateTime::currentDateTimeUtc()}));
        QCOMPARE(engine.listRegexFilters().size(), 1);
        QVERIFY(engine.addRegexFilter({3, QStringLiteral("^\\w+$"), QDateTime::currentDateTimeUtc()}));
        QCOMPARE(engine.shouldHide({QStringLiteral("jp"), 0, QStringLiteral("u2"), QStringLiteral("日本語")})
                     .value_or(false),
                 true);
    }

    void regexLeadingOptionsRetainTheirMeaning() {
        for (const auto &pattern : {QStringLiteral("(?i)^hello$"), QStringLiteral("(?x)^ h e l l o $"),
                                    QStringLiteral("(*UTF)^hello$"), QStringLiteral("(*LIMIT_MATCH=1000)^hello$")}) {
            auto compiled = FilterEngine::compileRegex(pattern);
            QVERIFY2(compiled, qPrintable(pattern));
            QVERIFY(compiled->match(QStringLiteral("hello")).hasMatch());
        }
        auto insensitive = FilterEngine::compileRegex(QStringLiteral("(?i)^hello$"));
        QVERIFY(insensitive->match(QStringLiteral("HELLO")).hasMatch());
        auto lowerLimit = FilterEngine::compileRegex(QStringLiteral("(*LIMIT_MATCH=1)^(a+)+$"));
        QVERIFY(lowerLimit);
        QVERIFY(!lowerLimit->match(QString(16000, QLatin1Char('a')) + QLatin1Char('!')).isValid());
    }

    void regexResourceFailureIsExplicitAndNgStillWins() {
        FilterEngine engine;
        QVERIFY(engine.addRegexFilter({7, QStringLiteral("^(a+)+$"), QDateTime::currentDateTimeUtc()}));
        const CommentEvent hostile{QStringLiteral("c"), 0, QStringLiteral("u"),
                                   QString(16000, QLatin1Char('a')) + QLatin1Char('!')};
        const auto failed = engine.shouldHide(hostile);
        QVERIFY(!failed);
        QVERIFY(failed.error().message.contains(QStringLiteral("regex filter 7 execution failed")));
        engine.addNgUser(QStringLiteral("u"));
        QCOMPARE(engine.shouldHide(hostile).value_or(false), true);
    }

    void regexCancellationDoesNotBecomeNonMatch() {
        FilterEngine engine;
        QVERIFY(engine.addRegexFilter({1, QStringLiteral("not present"), QDateTime::currentDateTimeUtc()}));
        const auto result = engine.shouldHide({QStringLiteral("c"), 0, QStringLiteral("u"), QStringLiteral("text")},
                                              [] { return true; });
        QVERIFY(!result);
        QVERIFY(result.error().message.contains(QStringLiteral("interrupted")));
    }

    void onlyLatestSuccessfulAdditionCanBeUndone() {
        auto opened = Store::openMemory();
        QVERIFY(opened);
        auto store = std::move(*opened);
        FilterManager manager(*store);
        QVERIFY(manager.initialize());
        auto first = manager.addNgUser(QStringLiteral("u1"));
        QVERIFY(first && first->applied && !first->undoToken.isEmpty());
        auto second = manager.addNgUser(QStringLiteral("u2"));
        QVERIFY(second && second->applied);
        QVERIFY(!manager.undoLastNg(QStringLiteral("wrong"))->restored);
        QVERIFY(!manager.undoLastNg(first->undoToken)->restored);
        auto duplicate = manager.addNgUser(QStringLiteral("u2"));
        QVERIFY(duplicate && !duplicate->applied && duplicate->undoToken.isEmpty());
        auto undone = manager.undoLastNg(second->undoToken);
        QVERIFY(undone && undone->restored);
        QCOMPARE(undone->userId.value(), QStringLiteral("u2"));
        QVERIFY(!manager.undoLastNg(second->undoToken)->restored);
        QCOMPARE(manager.snapshot().ngUsers, QStringList{QStringLiteral("u1")});
        QCOMPARE(store->listNgUsers().value(), QStringList{QStringLiteral("u1")});
    }

    void removalClearsMatchingUndoAndListsAreSorted() {
        auto opened = Store::openMemory();
        QVERIFY(opened);
        auto store = std::move(*opened);
        FilterManager manager(*store);
        QVERIFY(manager.initialize());
        QVERIFY(manager.addNgUser(QStringLiteral("z")));
        auto latest = manager.addNgUser(QStringLiteral("a"));
        QVERIFY(latest);
        QCOMPARE(manager.snapshot().ngUsers, (QStringList{QStringLiteral("a"), QStringLiteral("z")}));
        auto removed = manager.removeNgUser(QStringLiteral("a"));
        QVERIFY(removed && removed->removed);
        QCOMPARE(removed->userId, QStringLiteral("a"));
        QVERIFY(!manager.undoLastNg(latest->undoToken)->restored);
        QVERIFY(!manager.removeNgUser(QStringLiteral("missing"))->removed);
    }

    void regexValidationPrecedesPersistence() {
        auto opened = Store::openMemory();
        QVERIFY(opened);
        auto store = std::move(*opened);
        FilterManager manager(*store);
        QVERIFY(manager.initialize());
        auto invalid = manager.addRegexFilter(QStringLiteral("("));
        QVERIFY(!invalid);
        QVERIFY(invalid.error().message.startsWith(QStringLiteral("invalid regex:")));
        QVERIFY(store->listRegexFilters()->isEmpty());
        auto first = manager.addRegexFilter(QStringLiteral("hello"));
        auto second = manager.addRegexFilter(QStringLiteral("goodbye"));
        QVERIFY(first && second && *first < *second);
        QCOMPARE(manager.snapshot().regexFilters[0].filterId, *first);
        QVERIFY(*manager.removeRegexFilter(*first));
        QVERIFY(!*manager.removeRegexFilter(*first));
        QCOMPARE(manager.snapshot().regexFilters.size(), 1);
    }

    void failedWritesDoNotChangeMemoryOrUndo() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath(QStringLiteral("data.db"));
        auto opened = Store::openWithPaths(path, directory.filePath(QStringLiteral("cache.db")));
        QVERIFY(opened);
        auto store = std::move(*opened);
        FilterManager manager(*store);
        QVERIFY(manager.initialize());
        const auto first = manager.addNgUser(QStringLiteral("kept"));
        QVERIFY(first);
        QVERIFY(executeExternal(path, QStringLiteral("CREATE TRIGGER reject_ng BEFORE INSERT ON ng_users BEGIN SELECT "
                                                     "RAISE(FAIL,'simulated write failure'); END")));
        auto failed = manager.addNgUser(QStringLiteral("failed"));
        QVERIFY(!failed);
        QVERIFY(failed.error().message.contains(QStringLiteral("save ng user")));
        QVERIFY(!manager.engine().containsNgUser(QStringLiteral("failed")));
        QVERIFY(manager.undoLastNg(first->undoToken)->restored);
    }

    void failuresOnDeletePreserveFilterAndUndo() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("data.db"));
        auto opened = Store::openWithPaths(path, directory.filePath(QStringLiteral("cache.db")));
        QVERIFY(opened);
        auto store = std::move(*opened);
        FilterManager manager(*store);
        QVERIFY(manager.initialize());
        const auto added = manager.addNgUser(QStringLiteral("u1"));
        QVERIFY(added);
        QVERIFY(executeExternal(path, QStringLiteral("CREATE TRIGGER reject_delete BEFORE DELETE ON ng_users BEGIN "
                                                     "SELECT RAISE(FAIL,'simulated delete failure'); END")));
        QVERIFY(!manager.removeNgUser(QStringLiteral("u1")));
        QVERIFY(!manager.undoLastNg(added->undoToken));
        QVERIFY(manager.engine().containsNgUser(QStringLiteral("u1")));
        QVERIFY(executeExternal(path, QStringLiteral("DROP TRIGGER reject_delete")));
        QVERIFY(manager.undoLastNg(added->undoToken)->restored);
    }

    void persistedFiltersReloadAndInvalidFiltersReportErrors() {
        QTemporaryDir directory;
        const auto path = directory.filePath(QStringLiteral("data.db"));
        const auto cache = directory.filePath(QStringLiteral("cache.db"));
        {
            auto opened = Store::openWithPaths(path, cache);
            QVERIFY(opened);
            FilterManager manager(**opened);
            QVERIFY(manager.initialize());
            QVERIFY(manager.addNgUser(QStringLiteral("persisted")));
            QVERIFY(manager.addRegexFilter(QStringLiteral("secret")));
        }
        {
            auto opened = Store::openWithPaths(path, cache);
            QVERIFY(opened);
            FilterManager manager(**opened);
            QVERIFY(manager.initialize());
            QCOMPARE(manager.snapshot().ngUsers.size(), 1);
            QCOMPARE(manager.snapshot().regexFilters.size(), 1);
            QVERIFY(!manager.undoLastNg(QStringLiteral("old-token"))->restored);
        }
        QVERIFY(executeExternal(path, QStringLiteral("UPDATE regex_filters SET pattern='('")));
        auto opened = Store::openWithPaths(path, cache);
        QVERIFY(opened);
        FilterManager manager(**opened);
        auto loaded = manager.initialize();
        QVERIFY(!loaded);
        QVERIFY(loaded.error().message.startsWith(QStringLiteral("invalid regex:")));
    }

    void oversizedMutationsAreRejected() {
        auto opened = Store::openMemory();
        QVERIFY(opened);
        FilterManager manager(**opened);
        QVERIFY(manager.initialize());
        QVERIFY(!manager.addNgUser(QString(MaxUserIdBytes + 1, QLatin1Char('u'))));
        QVERIFY(!manager.addRegexFilter(QString(MaxPatternBytes + 1, QLatin1Char('a'))));
        QVERIFY(manager.snapshot().ngUsers.isEmpty());
        QVERIFY(manager.snapshot().regexFilters.isEmpty());
    }

    void emptyStringsRetainOriginalMeaning() {
        auto opened = Store::openMemory();
        QVERIFY(opened);
        FilterManager manager(**opened);
        QVERIFY(manager.initialize());
        auto emptyUser = manager.addNgUser(QString());
        QVERIFY(emptyUser && emptyUser->applied);
        QVERIFY(manager.removeNgUser(QString())->removed);
        auto emptyPattern = manager.addRegexFilter(QString());
        QVERIFY(emptyPattern);
        QCOMPARE(manager.engine()
                     .shouldHide({QStringLiteral("c"), 0, QStringLiteral("u"), QStringLiteral("anything")})
                     .value_or(false),
                 true);
        QVERIFY((*opened)->listRegexFilters()->first().pattern.isEmpty());
    }
};

QTEST_GUILESS_MAIN(FilterManagerTest)
#include "filter_manager_test.moc"
