#include "comments/CommentTimeline.hpp"

#include <QtTest>
#include <limits>

using namespace niconeon;

namespace {
CommentEvent comment(const char *id, qint64 at, const char *user = "u", const char *text = "text") {
    return {QString::fromUtf8(id), at, QString::fromUtf8(user), QString::fromUtf8(text)};
}
const auto unlimited = makeRuntimeProfile(RuntimeProfile::High);
} // namespace

class CommentTimelineTest : public QObject {
    Q_OBJECT
  private slots:
    void emitsNormalWindowsAndInitialZero() {
        FilterEngine filters;
        CommentTimeline timeline(QStringLiteral("one"),
                                 {comment("later", 200), comment("zero", 0), comment("first", 100)});
        QCOMPARE(timeline.id(), QStringLiteral("one"));
        QCOMPARE(timeline.size(), 3);
        auto first = timeline.process({{150, false, false}}, filters, unlimited);
        QVERIFY(first);
        QCOMPARE(first->processedTicks, 1);
        QCOMPARE(first->lastPositionMs, 150);
        QCOMPARE(first->emitComments.size(), 2);
        QCOMPARE(first->emitComments[0].commentId, QStringLiteral("zero"));
        QCOMPARE(first->emitComments[1].commentId, QStringLiteral("first"));
        auto next = timeline.process({{250, false, false}}, filters, unlimited);
        QVERIFY(next);
        QCOMPARE(next->emitComments.size(), 1);
        QCOMPARE(next->emitComments[0].commentId, QStringLiteral("later"));
        QCOMPARE(timeline.process({{250, false, false}}, filters, unlimited)->emitComments.size(), 0);
    }

    void seeksRestoreHalfOpenFifteenSecondWindow() {
        FilterEngine filters;
        CommentTimeline timeline(QStringLiteral("seek"), {comment("old", 4999), comment("boundary", 5000),
                                                          comment("flight", 19500), comment("exact", 20000)});
        auto seek = timeline.process({{20000, false, true}}, filters, unlimited);
        QVERIFY(seek);
        QCOMPARE(seek->emitComments.size(), 2);
        QCOMPARE(seek->emitComments[0].commentId, QStringLiteral("boundary"));
        QCOMPARE(seek->emitComments[1].commentId, QStringLiteral("flight"));
        QCOMPARE(seek->lastPositionMs, 19999);
        auto exact = timeline.process({{20000, false, false}}, filters, unlimited);
        QVERIFY(exact);
        QCOMPARE(exact->emitComments.size(), 1);
        QCOMPARE(exact->emitComments[0].commentId, QStringLiteral("exact"));
    }

    void backwardAndPausedSeekStillRestoreComments() {
        FilterEngine filters;
        CommentTimeline timeline(QStringLiteral("seek"), {comment("a", 100), comment("b", 200), comment("c", 300)});
        QVERIFY(timeline.process({{350, false, false}}, filters, unlimited));
        auto backward = timeline.process({{200, true, false}}, filters, unlimited);
        QVERIFY(backward);
        QCOMPARE(backward->emitComments.size(), 1);
        QCOMPARE(backward->emitComments[0].commentId, QStringLiteral("a"));
        auto paused = timeline.process({{200, true, false}}, filters, unlimited);
        QVERIFY(paused);
        QVERIFY(paused->emitComments.isEmpty());
        QCOMPARE(paused->lastPositionMs, 200);
        auto resume = timeline.process({{350, false, false}}, filters, unlimited);
        QVERIFY(resume);
        QCOMPARE(resume->emitComments.size(), 1);
        QCOMPARE(resume->emitComments[0].commentId, QStringLiteral("c"));
        auto explicitSeek = timeline.process({{300, true, true}}, filters, unlimited);
        QVERIFY(explicitSeek);
        QCOMPARE(explicitSeek->emitComments.size(), 2);
    }

    void appliesBudgetPerTickBeforeBatchCoalescing() {
        FilterEngine filters;
        CommentTimeline timeline(QStringLiteral("budget"),
                                 {comment("a", 100), comment("b", 110), comment("c", 300), comment("d", 310)});
        auto profile = makeRuntimeProfile(RuntimeProfile::LowSpec, {{}, 1, false});
        auto result = timeline.process({{150, false, false}, {350, false, false}}, filters, profile);
        QVERIFY(result);
        QCOMPARE(result->emitComments.size(), 2);
        QCOMPARE(result->emitComments[0].commentId, QStringLiteral("a"));
        QCOMPARE(result->emitComments[1].commentId, QStringLiteral("c"));
        QCOMPARE(result->droppedComments, 2);
        QVERIFY(result->emitOverBudget);
        CommentTimeline coalesced(QStringLiteral("coalesce"),
                                  {comment("a", 100), comment("b", 100), comment("c", 120, "u2", "different")});
        result = coalesced.process({{200, false, false}}, filters,
                                   makeRuntimeProfile(RuntimeProfile::LowSpec, {{}, 0, true}));
        QVERIFY(result);
        QCOMPARE(result->emitComments.size(), 2);
        QCOMPARE(result->coalescedComments, 1);
        QCOMPARE(result->emitComments[0].commentId, QStringLiteral("a"));
        CommentTimeline capBeforeCoalesce(QStringLiteral("order"),
                                          {comment("a", 100), comment("b", 100), comment("c", 120)});
        result = capBeforeCoalesce.process({{200, false, false}}, filters,
                                           makeRuntimeProfile(RuntimeProfile::LowSpec, {{}, 2, true}));
        QVERIFY(result);
        QCOMPARE(result->emitComments.size(), 1);
        QCOMPARE(result->coalescedComments, 1);
        QCOMPARE(result->droppedComments, 1);
        CommentTimeline batchCoalescing(QStringLiteral("batch"), {comment("a", 100)});
        result = batchCoalescing.process({{200, false, true}, {200, false, true}}, filters,
                                         makeRuntimeProfile(RuntimeProfile::LowSpec, {{}, 0, true}));
        QVERIFY(result);
        QCOMPARE(result->emitComments.size(), 1);
        QCOMPARE(result->coalescedComments, 1);
    }

    void filtersBeforeCountingBudget() {
        FilterEngine filters;
        filters.addNgUser(QStringLiteral("blocked"));
        QVERIFY(filters.addRegexFilter({1, QStringLiteral("secret"), QDateTime::currentDateTimeUtc()}));
        CommentTimeline timeline(
            QStringLiteral("filters"),
            {comment("ng", 0, "blocked"), comment("regex", 1, "u", "secret"), comment("visible", 2)});
        auto result =
            timeline.process({{10, false, false}}, filters, makeRuntimeProfile(RuntimeProfile::High, {{}, 1, false}));
        QVERIFY(result);
        QCOMPARE(result->emitComments.size(), 1);
        QCOMPARE(result->emitComments[0].commentId, QStringLiteral("visible"));
        QCOMPARE(result->droppedComments, 0);
    }

    void capsTwentyToFive() {
        CommentList comments;
        for (int i = 0; i < 20; ++i)
            comments.append(comment(qPrintable(QString::number(i)), 100 + i));
        FilterEngine filters;
        CommentTimeline timeline(QStringLiteral("cap"), comments);
        auto result = timeline.process({{300, false, false}}, filters,
                                       makeRuntimeProfile(RuntimeProfile::LowSpec, {{}, 5, false}));
        QVERIFY(result);
        QCOMPARE(result->emitComments.size(), 5);
        QCOMPARE(result->droppedComments, 15);
        QVERIFY(result->emitOverBudget);
    }

    void emptyBatchAndExtremeSeekAreSafe() {
        FilterEngine filters;
        CommentTimeline timeline(QStringLiteral("empty"), {});
        auto empty = timeline.process({}, filters, unlimited);
        QVERIFY(empty);
        QCOMPARE(empty->lastPositionMs, -1);
        QCOMPARE(empty->processedTicks, 0);
        auto seek = timeline.process({{std::numeric_limits<qint64>::min(), false, true}}, filters, unlimited);
        QVERIFY(seek);
        QCOMPARE(seek->lastPositionMs, std::numeric_limits<qint64>::min());
    }

    void safetyErrorsDoNotCommitCursor() {
        FilterEngine filters;
        CommentList comments(MaxComments, comment("same", 0));
        CommentTimeline timeline(QStringLiteral("bounded"), std::move(comments));
        auto bad = timeline.process({{1, false, true}, {1, false, true}}, filters, unlimited);
        QVERIFY(!bad);
        QVERIFY(bad.error().message.contains(QStringLiteral("safety limit")));
        auto oversized = timeline.process(QVector<PlaybackTick>(MaxBatchTicks + 1), filters, unlimited);
        QVERIFY(!oversized);
        auto good = timeline.process({{0, false, false}}, filters, unlimited);
        QVERIFY(good);
        QCOMPARE(good->emitComments.size(), MaxComments);
    }
};

QTEST_GUILESS_MAIN(CommentTimelineTest)
#include "comment_timeline_test.moc"
