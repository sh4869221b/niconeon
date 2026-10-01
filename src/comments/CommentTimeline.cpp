#include "comments/CommentTimeline.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <tuple>

namespace niconeon {
namespace {
qint64 subtractSaturated(qint64 value, qint64 amount) {
    const auto minimum = std::numeric_limits<qint64>::min();
    return value < minimum + amount ? minimum : value - amount;
}
} // namespace

CommentTimeline::CommentTimeline(QString sessionId, CommentList comments)
    : m_sessionId(std::move(sessionId)), m_comments(std::move(comments)) {
    std::stable_sort(m_comments.begin(), m_comments.end(),
                     [](const auto &left, const auto &right) { return left.atMs < right.atMs; });
    m_cursor = std::lower_bound(m_comments.cbegin(), m_comments.cend(), qint64{0},
                                [](const auto &comment, qint64 position) { return comment.atMs < position; }) -
               m_comments.cbegin();
}

const QString &CommentTimeline::id() const {
    return m_sessionId;
}
qsizetype CommentTimeline::size() const {
    return m_comments.size();
}

Result<PlaybackBatchResult> CommentTimeline::process(const QVector<PlaybackTick> &ticks, const FilterEngine &filters,
                                                     const RuntimeProfileConfig &profile,
                                                     const std::function<bool()> &cancelled) {
    if (ticks.size() > MaxBatchTicks)
        return std::unexpected(AppError{QStringLiteral("playback batch exceeds the 32-tick safety limit")});
    if (m_comments.size() > MaxComments)
        return std::unexpected(AppError{QStringLiteral("comment count exceeds the 250000-comment safety limit")});
    const auto interrupted = [&] { return cancelled && cancelled(); };
    qsizetype regexEvaluations = 0;
    const auto filterInterrupted = [&] { return interrupted() || ++regexEvaluations > 100000; };
    PlaybackBatchResult result;
    auto cursor = m_cursor;
    auto lastPosition = m_lastPositionMs;
    std::set<std::tuple<qint64, QString, QString>> seen;
    auto cursorAt = [this](qint64 position) {
        return std::lower_bound(m_comments.cbegin(), m_comments.cend(), position,
                                [](const auto &comment, qint64 at) { return comment.atMs < at; }) -
               m_comments.cbegin();
    };
    for (const auto &tick : ticks) {
        if (interrupted())
            return std::unexpected(
                AppError{QStringLiteral("comment filtering interrupted or exceeded its work budget")});
        qsizetype emittedThisTick = 0;
        auto append = [&](const CommentEvent &comment) -> Result<void> {
            if (interrupted())
                return std::unexpected(
                    AppError{QStringLiteral("comment filtering interrupted or exceeded its work budget")});
            auto hidden = filters.shouldHide(comment, filterInterrupted);
            if (!hidden)
                return std::unexpected(hidden.error());
            if (*hidden)
                return {};
            if (profile.maxEmitPerTick > 0 && emittedThisTick >= profile.maxEmitPerTick) {
                ++result.droppedComments;
                result.emitOverBudget = true;
                return {};
            }
            ++emittedThisTick;
            // Coalescing is logically after each tick's budget, across the entire batch.
            if (profile.coalesceSameContent && !seen.emplace(comment.atMs, comment.userId, comment.text).second) {
                ++result.coalescedComments;
                return {};
            }
            if (result.emitComments.size() >= MaxComments)
                return std::unexpected(
                    AppError{QStringLiteral("playback result exceeds the 250000-comment safety limit")});
            result.emitComments.append(comment);
            return {};
        };

        if (tick.isSeek || tick.positionMs < lastPosition) {
            const auto start = cursorAt(subtractSaturated(tick.positionMs, 15000));
            const auto end = cursorAt(tick.positionMs);
            for (auto index = start; index < end; ++index) {
                if (auto added = append(m_comments[index]); !added)
                    return std::unexpected(added.error());
            }
            cursor = end;
            lastPosition = subtractSaturated(tick.positionMs, 1);
        } else if (tick.paused) {
            lastPosition = tick.positionMs;
        } else {
            while (cursor < m_comments.size() && m_comments[cursor].atMs <= tick.positionMs) {
                if (interrupted())
                    return std::unexpected(
                        AppError{QStringLiteral("comment filtering interrupted or exceeded its work budget")});
                if (m_comments[cursor].atMs > lastPosition) {
                    if (auto added = append(m_comments[cursor]); !added)
                        return std::unexpected(added.error());
                }
                ++cursor;
            }
            lastPosition = tick.positionMs;
        }
    }
    // Only commit the cursor after the whole bounded operation succeeds.
    m_cursor = cursor;
    m_lastPositionMs = lastPosition;
    result.processedTicks = ticks.size();
    result.lastPositionMs = lastPosition;
    return result;
}

} // namespace niconeon
