#include "filters/FilterEngine.hpp"

#include <algorithm>

namespace niconeon {

Result<QRegularExpression> FilterEngine::compileRegex(const QString &pattern) {
    if (pattern.size() > MaxPatternBytes || pattern.toUtf8().size() > MaxPatternBytes)
        return std::unexpected(AppError{QStringLiteral("invalid regex: pattern exceeds the 4096-byte safety limit")});
    // PCRE2 uses the lowest duplicate limit, so a user pattern cannot raise these.
    // Keep the original pattern unchanged in storage and diagnostics.
    const auto limits = QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)(*LIMIT_HEAP=1024)");
    QRegularExpression expression(limits + pattern, QRegularExpression::UseUnicodePropertiesOption);
    if (!expression.isValid())
        return std::unexpected(
            AppError{QStringLiteral("invalid regex: %1 (offset %2)")
                         .arg(expression.errorString())
                         .arg(std::max(qsizetype{0}, expression.patternErrorOffset() - limits.size()))});
    return expression;
}

Result<void> FilterEngine::replace(const QStringList &ngUsers, const QVector<RegexFilter> &regexFilters) {
    if (ngUsers.size() + regexFilters.size() > MaxFilterEntries)
        return std::unexpected(AppError{QStringLiteral("filter count exceeds the 10000-entry safety limit")});
    FilterEngine replacement;
    for (const auto &user : ngUsers) {
        if (auto valid = validateUserId(user); !valid)
            return valid;
        replacement.addNgUser(user);
    }
    for (const auto &filter : regexFilters) {
        if (auto result = replacement.addRegexFilter(filter); !result)
            return result;
    }
    *this = std::move(replacement);
    return {};
}

QStringList FilterEngine::listNgUsers() const {
    QStringList result;
    result.reserve(static_cast<qsizetype>(m_ngUsers.size()));
    for (const auto &user : m_ngUsers)
        result.append(user);
    return result;
}

QVector<RegexFilter> FilterEngine::listRegexFilters() const {
    QVector<RegexFilter> result;
    result.reserve(m_regexFilters.size());
    for (const auto &filter : m_regexFilters)
        result.append(filter.raw);
    return result;
}

FilterSnapshot FilterEngine::snapshot() const {
    return {listNgUsers(), listRegexFilters()};
}
bool FilterEngine::containsNgUser(const QString &userId) const {
    return m_ngUsers.contains(userId);
}
bool FilterEngine::addNgUser(const QString &userId) {
    return m_ngUsers.insert(userId).second;
}
bool FilterEngine::removeNgUser(const QString &userId) {
    return m_ngUsers.erase(userId) != 0;
}

Result<void> FilterEngine::addRegexFilter(const RegexFilter &filter) {
    auto compiled = compileRegex(filter.pattern);
    if (!compiled)
        return std::unexpected(compiled.error());
    if (m_ngUsers.size() + static_cast<std::size_t>(m_regexFilters.size()) >= MaxFilterEntries)
        return std::unexpected(AppError{QStringLiteral("filter count exceeds the 10000-entry safety limit")});
    m_regexFilters.append({filter, std::move(*compiled)});
    return {};
}

bool FilterEngine::removeRegexFilter(qint64 filterId) {
    const auto before = m_regexFilters.size();
    m_regexFilters.erase(std::remove_if(m_regexFilters.begin(), m_regexFilters.end(),
                                        [filterId](const auto &filter) { return filter.raw.filterId == filterId; }),
                         m_regexFilters.end());
    return before != m_regexFilters.size();
}

Result<bool> FilterEngine::shouldHide(const CommentEvent &comment, const std::function<bool()> &interrupted) const {
    // Deliberately short-circuit before any regular-expression work.
    if (m_ngUsers.contains(comment.userId))
        return true;
    for (const auto &filter : m_regexFilters) {
        if (interrupted && interrupted())
            return std::unexpected(
                AppError{QStringLiteral("comment filtering interrupted or exceeded its work budget")});
        const auto match = filter.compiled.match(comment.text);
        if (!match.isValid())
            return std::unexpected(
                AppError{QStringLiteral("regex filter %1 execution failed (resource limit or invalid subject)")
                             .arg(filter.raw.filterId)});
        if (match.hasMatch())
            return true;
    }
    return false;
}

} // namespace niconeon
