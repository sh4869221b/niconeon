#include "domain/Domain.hpp"

#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

namespace niconeon {

QString commentSourceName(CommentSource source) {
    switch (source) {
    case CommentSource::Network:
        return QStringLiteral("network");
    case CommentSource::Cache:
        return QStringLiteral("cache");
    case CommentSource::None:
        return QStringLiteral("none");
    }
    return QStringLiteral("none");
}

QString profileName(RuntimeProfile profile) {
    switch (profile) {
    case RuntimeProfile::High:
        return QStringLiteral("high");
    case RuntimeProfile::Balanced:
        return QStringLiteral("balanced");
    case RuntimeProfile::LowSpec:
        return QStringLiteral("low_spec");
    }
    return QStringLiteral("balanced");
}

Result<RuntimeProfile> parseRuntimeProfile(const QString &name) {
    const auto normalized = name.trimmed().toLower();
    if (normalized == QStringLiteral("high"))
        return RuntimeProfile::High;
    if (normalized == QStringLiteral("balanced"))
        return RuntimeProfile::Balanced;
    if (normalized == QStringLiteral("low_spec") || normalized == QStringLiteral("lowspec"))
        return RuntimeProfile::LowSpec;
    return std::unexpected(AppError{QStringLiteral("invalid runtime profile: %1").arg(name)});
}

RuntimeProfileConfig makeRuntimeProfile(RuntimeProfile profile, const RuntimeProfileOverrides &overrides) {
    RuntimeProfileConfig config;
    config.profile = profile;
    switch (profile) {
    case RuntimeProfile::High:
        config.maxEmitPerTick = 0;
        break;
    case RuntimeProfile::Balanced:
        config.maxEmitPerTick = 96;
        break;
    case RuntimeProfile::LowSpec:
        config.maxEmitPerTick = 48;
        config.coalesceSameContent = true;
        break;
    }
    if (overrides.targetFps)
        config.targetFps = std::clamp(*overrides.targetFps, 10, 120);
    if (overrides.maxEmitPerTick)
        config.maxEmitPerTick = std::clamp<qsizetype>(*overrides.maxEmitPerTick, 0, 2000);
    if (overrides.coalesceSameContent)
        config.coalesceSameContent = *overrides.coalesceSameContent;
    return config;
}

std::optional<QString> extractVideoId(const QString &path) {
    static const QRegularExpression expression(QStringLiteral("(sm|nm|so)(\\d+)"),
                                               QRegularExpression::CaseInsensitiveOption |
                                                   QRegularExpression::UseUnicodePropertiesOption);
    const auto match = expression.match(QFileInfo(path).fileName());
    if (!match.hasMatch())
        return std::nullopt;
    return match.captured(1).toLower() + match.captured(2);
}

Result<void> validateUserId(const QString &userId) {
    if (userId.size() > MaxUserIdBytes || userId.toUtf8().size() > MaxUserIdBytes)
        return std::unexpected(AppError{QStringLiteral("user ID exceeds the 1024-byte safety limit")});
    return {};
}

Result<void> validateComments(const CommentList &comments) {
    if (comments.size() > MaxComments)
        return std::unexpected(AppError{QStringLiteral("comment count exceeds the 250000-comment safety limit")});
    // Include every field, JSON escaping, punctuation, keys and the signed timestamp.
    // Checking before serialization bounds even adversarial control-character text.
    qsizetype bytes = 2;
    for (const auto &comment : comments) {
        if (comment.text.size() > MaxTextBytes || comment.text.toUtf8().size() > MaxTextBytes)
            return std::unexpected(AppError{QStringLiteral("comment text exceeds the 16384-byte safety limit")});
        if (auto valid = validateUserId(comment.userId); !valid)
            return valid;
        bytes += 128;
        for (const auto *field : {&comment.commentId, &comment.userId, &comment.text}) {
            if (field->size() > MaxPayloadBytes)
                return std::unexpected(AppError{QStringLiteral("comment data exceeds the 64 MiB safety limit")});
            const auto utf8 = field->toUtf8();
            for (const auto character : utf8) {
                const auto byte = static_cast<unsigned char>(character);
                bytes += byte < 0x20 ? 6 : (byte == '"' || byte == '\\' ? 2 : 1);
                if (bytes > MaxPayloadBytes)
                    return std::unexpected(AppError{QStringLiteral("comment data exceeds the 64 MiB safety limit")});
            }
        }
        if (bytes > MaxPayloadBytes)
            return std::unexpected(AppError{QStringLiteral("comment data exceeds the 64 MiB safety limit")});
    }
    return {};
}

} // namespace niconeon
