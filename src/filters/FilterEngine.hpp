#pragma once

#include "domain/Domain.hpp"
#include <QRegularExpression>
#include <functional>
#include <set>

namespace niconeon {

class FilterEngine {
  public:
    Result<void> replace(const QStringList &ngUsers, const QVector<RegexFilter> &regexFilters);
    QStringList listNgUsers() const;
    QVector<RegexFilter> listRegexFilters() const;
    FilterSnapshot snapshot() const;
    bool containsNgUser(const QString &userId) const;
    bool addNgUser(const QString &userId);
    bool removeNgUser(const QString &userId);
    Result<void> addRegexFilter(const RegexFilter &filter);
    bool removeRegexFilter(qint64 filterId);
    Result<bool> shouldHide(const CommentEvent &comment, const std::function<bool()> &interrupted = {}) const;
    static Result<QRegularExpression> compileRegex(const QString &pattern);

  private:
    struct CompiledFilter {
        RegexFilter raw;
        QRegularExpression compiled;
    };
    std::set<QString> m_ngUsers;
    QVector<CompiledFilter> m_regexFilters;
};

} // namespace niconeon
