#include "filters/FilterManager.hpp"

#include <QUuid>

namespace niconeon {

FilterManager::FilterManager(Store &store) : m_store(store) {}

Result<void> FilterManager::initialize() {
    auto users = m_store.listNgUsers();
    if (!users)
        return std::unexpected(AppError{QStringLiteral("load ng users: %1").arg(users.error().message)});
    auto regexes = m_store.listRegexFilters();
    if (!regexes)
        return std::unexpected(AppError{QStringLiteral("load regex filters: %1").arg(regexes.error().message)});
    auto loaded = m_engine.replace(*users, *regexes);
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
    const bool applied = m_engine.addNgUser(userId);
    QString token;
    if (applied) {
        token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_lastUndo = UndoState{token, userId};
    }
    return AddNgUserResult{applied, token, userId};
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
    auto removed = m_store.removeNgUser(userId);
    if (!removed)
        return std::unexpected(AppError{QStringLiteral("remove ng user: %1").arg(removed.error().message)});
    if (!*removed)
        return UndoNgResult{};
    m_engine.removeNgUser(userId);
    m_lastUndo.reset();
    return UndoNgResult{true, userId};
}

Result<qint64> FilterManager::addRegexFilter(const QString &pattern) {
    if (auto valid = FilterEngine::compileRegex(pattern); !valid)
        return std::unexpected(valid.error());
    auto inserted = m_store.insertRegexFilter(pattern);
    if (!inserted)
        return std::unexpected(AppError{QStringLiteral("insert regex filter: %1").arg(inserted.error().message)});
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
