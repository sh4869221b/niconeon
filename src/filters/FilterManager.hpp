#pragma once

#include "filters/FilterEngine.hpp"
#include "storage/Store.hpp"

namespace niconeon {

class FilterManager {
  public:
    explicit FilterManager(Store &store);
    Result<void> initialize();
    Result<AddNgUserResult> addNgUser(const QString &userId);
    Result<RemoveNgUserResult> removeNgUser(const QString &userId);
    Result<UndoNgResult> undoLastNg(const QString &undoToken);
    Result<qint64> addRegexFilter(const QString &pattern);
    Result<bool> removeRegexFilter(qint64 filterId);
    FilterSnapshot snapshot() const;
    const FilterEngine &engine() const;

  private:
    struct UndoState {
        QString token;
        QString userId;
    };
    Store &m_store;
    FilterEngine m_engine;
    std::optional<UndoState> m_lastUndo;
};

} // namespace niconeon
