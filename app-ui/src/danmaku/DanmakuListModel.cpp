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

void DanmakuListModel::removeAt(int row) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }

    beginRemoveRows(QModelIndex(), row, row);
    m_rows.removeAt(row);
    endRemoveRows();
}

void DanmakuListModel::removeRowsDescending(const QVector<int> &rowsDescending) {
    if (rowsDescending.isEmpty()) {
        return;
    }

    QVector<int> rows = rowsDescending;
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
    for (const int row : rows) {
        removeAt(row);
    }
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
