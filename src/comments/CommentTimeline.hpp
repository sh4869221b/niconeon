#pragma once

#include "domain/Domain.hpp"
#include "filters/FilterEngine.hpp"

namespace niconeon {

class CommentTimeline {
  public:
    CommentTimeline(QString sessionId, CommentList comments);
    const QString &id() const;
    qsizetype size() const;
    Result<PlaybackBatchResult> process(const QVector<PlaybackTick> &ticks, const FilterEngine &filters,
                                        const RuntimeProfileConfig &profile);

  private:
    QString m_sessionId;
    CommentList m_comments;
    qsizetype m_cursor = 0;
    qint64 m_lastPositionMs = -1;
};

} // namespace niconeon
