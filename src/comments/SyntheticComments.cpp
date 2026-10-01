#include "comments/SyntheticComments.hpp"

#include <algorithm>

namespace niconeon {
namespace {
qint64 environmentInteger(const char *name, qint64 fallback) {
    const auto value = qEnvironmentVariable(name);
    bool valid = false;
    const auto parsed = value.toLongLong(&valid);
    return valid && value.trimmed() == value ? parsed : fallback;
}
} // namespace

bool syntheticCommentModeEnabled() {
    return qEnvironmentVariable("NICONEON_SYNTHETIC_COMMENTS").compare(QStringLiteral("ramp"), Qt::CaseInsensitive) ==
           0;
}

Result<CommentList> generateSyntheticComments(const QString &videoId) {
    const auto duration = std::clamp<qint64>(environmentInteger("NICONEON_SYNTHETIC_DURATION_SEC", 120), 1, 3600);
    const auto base = std::clamp<qint64>(environmentInteger("NICONEON_SYNTHETIC_BASE_PER_SEC", 1), 1, 500);
    const auto ramp = std::clamp<qint64>(environmentInteger("NICONEON_SYNTHETIC_RAMP_PER_SEC", 1), 0, 100);
    const auto maximum = std::clamp<qint64>(environmentInteger("NICONEON_SYNTHETIC_MAX_PER_SEC", 160), 1, 2000);
    const auto userSpan = std::clamp<qint64>(environmentInteger("NICONEON_SYNTHETIC_USER_SPAN", 200), 1, 100000);
    qsizetype count = 0;
    for (qint64 second = 0; second < duration; ++second) {
        count += std::min(base + second * ramp, maximum);
        if (count > MaxComments)
            return std::unexpected(
                AppError{QStringLiteral("synthetic comment count exceeds the 250000-comment safety limit")});
    }
    CommentList comments;
    comments.reserve(count);
    for (qint64 second = 0; second < duration; ++second) {
        const auto perSecond = std::min(base + second * ramp, maximum);
        for (qint64 index = 0; index < perSecond; ++index) {
            comments.append({QStringLiteral("%1-dummy-%2-%3").arg(videoId).arg(second).arg(index),
                             second * 1000 + index * 1000 / perSecond,
                             QStringLiteral("dummy-user-%1").arg((second * 31 + index) % userSpan),
                             QStringLiteral("dummy comment %1-%2 / %3cps").arg(second).arg(index).arg(perSecond)});
        }
    }
    if (auto valid = validateComments(comments); !valid)
        return std::unexpected(valid.error());
    return comments;
}

} // namespace niconeon
