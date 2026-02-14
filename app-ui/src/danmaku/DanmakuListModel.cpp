#include "danmaku/DanmakuListModel.hpp"

#include <QtGlobal>
#include <algorithm>

namespace {
bool nearlyEqual(qreal lhs, qreal rhs) {
    return qAbs(lhs - rhs) < 0.001;
}
}

DanmakuListModel::DanmakuListModel(QObject *parent) : QAbstractListModel(parent) {}

int DanmakuListModel::rowCount(const QModelIndex &parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return m_rows.size();
}

QVariant DanmakuListModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }

    const Row &row = m_rows.at(index.row());
    switch (role) {
    case CommentIdRole:
        return row.commentId;
    case UserIdRole:
        return row.userId;
    case TextRole:
        return row.text;
    case PosXRole:
        return row.posX;
    case PosYRole:
        return row.posY;
    case AlphaRole:
        return row.alpha;
    case LaneRole:
        return row.lane;
    case DraggingRole:
        return row.dragging;
    case WidthEstimateRole:
        return row.widthEstimate;
    case SpeedPxPerSecRole:
        return row.speedPxPerSec;
    case NgDropHoveredRole:
        return row.ngDropHovered;
    case ActiveRole:
        return row.active;
    default:
        return {};
    }
}

QHash<int, QByteArray> DanmakuListModel::roleNames() const {
    return {
        {CommentIdRole, "commentId"},
        {UserIdRole, "userId"},
        {TextRole, "text"},
        {PosXRole, "posX"},
        {PosYRole, "posY"},
        {AlphaRole, "alpha"},
        {LaneRole, "lane"},
        {DraggingRole, "dragging"},
        {WidthEstimateRole, "widthEstimate"},
        {SpeedPxPerSecRole, "speedPxPerSec"},
        {NgDropHoveredRole, "ngDropHovered"},
        {ActiveRole, "active"},
    };
}

void DanmakuListModel::clear() {
    if (m_rows.isEmpty()) {
        return;
    }
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

void DanmakuListModel::append(const Row &row) {
    const int insertAt = m_rows.size();
    beginInsertRows(QModelIndex(), insertAt, insertAt);
    m_rows.push_back(row);
    endInsertRows();
}

void DanmakuListModel::overwriteRow(int row, const Row &rowData) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }

    m_rows[row] = rowData;
    const QModelIndex modelIndex = index(row, 0);
    emit dataChanged(
        modelIndex,
        modelIndex,
        {
            CommentIdRole,
            UserIdRole,
            TextRole,
            PosXRole,
            PosYRole,
            AlphaRole,
            LaneRole,
            DraggingRole,
            WidthEstimateRole,
            SpeedPxPerSecRole,
            NgDropHoveredRole,
            ActiveRole,
        });
}

void DanmakuListModel::resetRows(const QVector<Row> &rows) {
    beginResetModel();
    m_rows = rows;
    endResetModel();
}

void DanmakuListModel::setGeometry(int row, qreal posX, qreal posY, qreal alpha) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }

    Row &target = m_rows[row];
    QVector<int> changedRoles;

    if (!nearlyEqual(target.posX, posX)) {
        target.posX = posX;
        changedRoles.push_back(PosXRole);
    }
    if (!nearlyEqual(target.posY, posY)) {
        target.posY = posY;
        changedRoles.push_back(PosYRole);
    }
    if (!nearlyEqual(target.alpha, alpha)) {
        target.alpha = alpha;
        changedRoles.push_back(AlphaRole);
    }

    if (changedRoles.isEmpty()) {
        return;
    }

    const QModelIndex modelIndex = index(row, 0);
    emit dataChanged(modelIndex, modelIndex, changedRoles);
}

void DanmakuListModel::setGeometryBatch(const QVector<GeometryUpdate> &updates) {
    if (updates.isEmpty()) {
        return;
    }

    QVector<GeometryUpdate> sorted = updates;
    std::sort(sorted.begin(), sorted.end(), [](const GeometryUpdate &lhs, const GeometryUpdate &rhs) {
        return lhs.row < rhs.row;
    });

    QVector<int> changedRows;
    changedRows.reserve(sorted.size());

    for (int i = 0; i < sorted.size(); ++i) {
        const GeometryUpdate &update = sorted[i];
        if (update.row < 0 || update.row >= m_rows.size()) {
            continue;
        }

        // If the same row appears multiple times in one frame, apply only the last update.
        if (i + 1 < sorted.size() && sorted[i + 1].row == update.row) {
            continue;
        }

        Row &target = m_rows[update.row];
        bool changed = false;
        if (!nearlyEqual(target.posX, update.posX)) {
            target.posX = update.posX;
            changed = true;
        }
        if (!nearlyEqual(target.posY, update.posY)) {
            target.posY = update.posY;
            changed = true;
        }
        if (!nearlyEqual(target.alpha, update.alpha)) {
            target.alpha = update.alpha;
            changed = true;
        }

        if (changed) {
            changedRows.push_back(update.row);
        }
    }

    if (changedRows.isEmpty()) {
        return;
    }

    int rangeStart = changedRows.first();
    int rangeEnd = rangeStart;
    for (int i = 1; i < changedRows.size(); ++i) {
        const int row = changedRows[i];
        if (row == rangeEnd + 1) {
            rangeEnd = row;
            continue;
        }

        emit dataChanged(
            index(rangeStart, 0),
            index(rangeEnd, 0),
            {PosXRole, PosYRole, AlphaRole});
        rangeStart = row;
        rangeEnd = row;
    }

    emit dataChanged(
        index(rangeStart, 0),
        index(rangeEnd, 0),
        {PosXRole, PosYRole, AlphaRole});
}

void DanmakuListModel::setDragState(int row, bool dragging) {
    if (row < 0 || row >= m_rows.size() || m_rows[row].dragging == dragging) {
        return;
    }

    m_rows[row].dragging = dragging;
    const QModelIndex modelIndex = index(row, 0);
    emit dataChanged(modelIndex, modelIndex, {DraggingRole});
}

void DanmakuListModel::setLane(int row, int lane) {
    if (row < 0 || row >= m_rows.size() || m_rows[row].lane == lane) {
        return;
    }

    m_rows[row].lane = lane;
    const QModelIndex modelIndex = index(row, 0);
    emit dataChanged(modelIndex, modelIndex, {LaneRole});
}

void DanmakuListModel::setNgDropHovered(int row, bool hovered) {
    if (row < 0 || row >= m_rows.size() || m_rows[row].ngDropHovered == hovered) {
        return;
    }

    m_rows[row].ngDropHovered = hovered;
    const QModelIndex modelIndex = index(row, 0);
    emit dataChanged(modelIndex, modelIndex, {NgDropHoveredRole});
}

void DanmakuListModel::setActive(int row, bool active) {
    if (row < 0 || row >= m_rows.size() || m_rows[row].active == active) {
        return;
    }

    m_rows[row].active = active;
    const QModelIndex modelIndex = index(row, 0);
    emit dataChanged(modelIndex, modelIndex, {ActiveRole});
}
