#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>
#include <expected>
#include <optional>

namespace niconeon {

struct AppError {
    QString message;
};
template <typename T> using Result = std::expected<T, AppError>;

inline constexpr qsizetype MaxPayloadBytes = qsizetype{64} * 1024 * 1024;
inline constexpr qsizetype MaxComments = 250000;
inline constexpr qsizetype MaxTextBytes = qsizetype{16} * 1024;
inline constexpr qsizetype MaxUserIdBytes = 1024;
inline constexpr qsizetype MaxPatternBytes = 4096;
inline constexpr qsizetype MaxFilterEntries = 10000;
inline constexpr qsizetype MaxBatchTicks = 32;

struct CommentEvent {
    QString commentId;
    qint64 atMs = 0;
    QString userId;
    QString text;
    bool operator==(const CommentEvent &) const = default;
};
using CommentList = QVector<CommentEvent>;

struct RegexFilter {
    qint64 filterId = 0;
    QString pattern;
    QDateTime createdAt;
    bool operator==(const RegexFilter &) const = default;
};
struct FilterSnapshot {
    QStringList ngUsers;
    QVector<RegexFilter> regexFilters;
};
enum class CommentSource { Network, Cache, None };
QString commentSourceName(CommentSource source);

struct PlaybackTick {
    qint64 positionMs = 0;
    bool paused = false;
    bool isSeek = false;
};
struct PlaybackBatchResult {
    CommentList emitComments;
    qsizetype processedTicks = 0;
    qint64 lastPositionMs = -1;
    qsizetype droppedComments = 0;
    qsizetype coalescedComments = 0;
    bool emitOverBudget = false;
};
enum class RuntimeProfile { High, Balanced, LowSpec };
struct RuntimeProfileConfig {
    RuntimeProfile profile = RuntimeProfile::Balanced;
    int targetFps = 60;
    qsizetype maxEmitPerTick = 96;
    bool coalesceSameContent = false;
};
struct RuntimeProfileOverrides {
    std::optional<int> targetFps;
    std::optional<qsizetype> maxEmitPerTick;
    std::optional<bool> coalesceSameContent;
};
QString profileName(RuntimeProfile profile);
Result<RuntimeProfile> parseRuntimeProfile(const QString &name);
RuntimeProfileConfig makeRuntimeProfile(RuntimeProfile profile, const RuntimeProfileOverrides &overrides = {});
std::optional<QString> extractVideoId(const QString &path);
Result<void> validateComments(const CommentList &comments);
Result<void> validateUserId(const QString &userId);

struct AddNgUserResult {
    bool applied = false;
    QString undoToken;
    QString hiddenUserId;
};
struct RemoveNgUserResult {
    bool removed = false;
    QString userId;
};
struct UndoNgResult {
    bool restored = false;
    std::optional<QString> userId;
};

} // namespace niconeon

Q_DECLARE_METATYPE(niconeon::CommentEvent)
Q_DECLARE_METATYPE(niconeon::CommentList)
Q_DECLARE_METATYPE(niconeon::RegexFilter)
Q_DECLARE_METATYPE(niconeon::FilterSnapshot)
Q_DECLARE_METATYPE(niconeon::PlaybackTick)
Q_DECLARE_METATYPE(niconeon::PlaybackBatchResult)
Q_DECLARE_METATYPE(niconeon::RuntimeProfileConfig)
Q_DECLARE_METATYPE(niconeon::AddNgUserResult)
Q_DECLARE_METATYPE(niconeon::RemoveNgUserResult)
Q_DECLARE_METATYPE(niconeon::UndoNgResult)
