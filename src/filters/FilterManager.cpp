#include "filters/FilterManager.hpp"

namespace niconeon {

FilterManager::FilterManager(Store &store) : m_store(store) {}

Result<void> FilterManager::initialize() {
    auto snapshot = m_store.loadFilters();
    if (!snapshot)
        return std::unexpected(snapshot.error());
    auto loaded = m_engine.replace(snapshot->ngUsers, snapshot->regexFilters);
    if (loaded)
        m_lastUndo.reset();
    return loaded;
}

Result<AddNgUserResult> FilterManager::addNgUser(const QString &userId) {
    if (auto valid = validateUserId(userId); !valid)
        return std::unexpected(valid.error());
    // A failed write must not hide comments or replace the current undo token.
    auto persisted = m_store.addNgUser(userId);
    if (!persisted)
        return std::unexpected(AppError{QStringLiteral("save ng user: %1").arg(persisted.error().message)});
    // Another instance may have inserted this user since we loaded our filters.
    m_engine.addNgUser(userId);
    QString token;
    if (*persisted) {
        token = **persisted;
        m_lastUndo = UndoState{token, userId};
    }
    return AddNgUserResult{persisted->has_value(), token, userId};
}

Result<RemoveNgUserResult> FilterManager::removeNgUser(const QString &userId) {
    if (auto valid = validateUserId(userId); !valid)
        return std::unexpected(valid.error());
    auto persisted = m_store.removeNgUser(userId);
    if (!persisted)
        return std::unexpected(AppError{QStringLiteral("delete ng user: %1").arg(persisted.error().message)});
    const bool removedMemory = m_engine.removeNgUser(userId);
    const bool removed = *persisted || removedMemory;
    if (removed && m_lastUndo && m_lastUndo->userId == userId)
        m_lastUndo.reset();
    return RemoveNgUserResult{removed, userId};
}

Result<UndoNgResult> FilterManager::undoLastNg(const QString &undoToken) {
    if (!m_lastUndo || m_lastUndo->token != undoToken)
        return UndoNgResult{};
    const QString userId = m_lastUndo->userId;
    auto removed = m_store.removeNgUser(userId, undoToken);
    if (!removed)
        return std::unexpected(AppError{QStringLiteral("remove ng user: %1").arg(removed.error().message)});
    // A missing or replaced row invalidates this token. Keep filtering a user whose row may have been replaced.
    m_lastUndo.reset();
    if (!*removed)
        return UndoNgResult{};
    m_engine.removeNgUser(userId);
    return UndoNgResult{true, userId};
}

Result<qint64> FilterManager::addRegexFilter(const QString &pattern) {
    if (auto valid = FilterEngine::compileRegex(pattern); !valid)
        return std::unexpected(valid.error());
    const bool needsRefresh = m_engine.atCapacity();
    FilterEngine replacement;
    const auto prepare = [&replacement](const FilterSnapshot &snapshot) {
        return replacement.replace(snapshot.ngUsers, snapshot.regexFilters);
    };
    auto inserted = needsRefresh ? m_store.insertRegexFilter(pattern, prepare) : m_store.insertRegexFilter(pattern);
    if (!inserted)
        return std::unexpected(AppError{QStringLiteral("insert regex filter: %1").arg(inserted.error().message)});
    if (needsRefresh) {
        // Publish the refreshed snapshot after commit. Undo still refers to the original registration ID.
        m_engine = std::move(replacement);
        return inserted->filterId;
    }
    auto added = m_engine.addRegexFilter(*inserted);
    if (!added) {
        // Normally unreachable after validation; preserve DB/memory agreement on failure.
        const auto rollback = m_store.removeRegexFilter(inserted->filterId);
        if (!rollback)
            return std::unexpected(AppError{
                QStringLiteral("%1; rollback failed: %2").arg(added.error().message, rollback.error().message)});
        return std::unexpected(added.error());
    }
    return inserted->filterId;
}

Result<bool> FilterManager::removeRegexFilter(qint64 filterId) {
    auto removed = m_store.removeRegexFilter(filterId);
    if (!removed)
        return std::unexpected(AppError{QStringLiteral("delete regex filter: %1").arg(removed.error().message)});
    const bool removedMemory = m_engine.removeRegexFilter(filterId);
    return *removed || removedMemory;
}

FilterSnapshot FilterManager::snapshot() const {
    return m_engine.snapshot();
}
const FilterEngine &FilterManager::engine() const {
    return m_engine;
}

} // namespace niconeon
