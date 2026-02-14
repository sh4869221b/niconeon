#pragma once

#include <QAbstractListModel>
#include <QVector>

class DanmakuListModel : public QAbstractListModel {
    Q_OBJECT

public:
    struct Row {
        QString commentId;
        QString userId;
        QString text;
        qreal posX = 0;
        qreal posY = 0;
        qreal alpha = 1.0;
        int lane = 0;
        bool dragging = false;
        int widthEstimate = 120;
        qreal speedPxPerSec = 120;
        bool ngDropHovered = false;
        bool active = true;
    };

    enum Role {
        CommentIdRole = Qt::UserRole + 1,
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
    };

    explicit DanmakuListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    void clear();
    void append(const Row &row);
    void overwriteRow(int row, const Row &rowData);
    void resetRows(const QVector<Row> &rows);

    void setGeometry(int row, qreal posX, qreal posY, qreal alpha);
    void setDragState(int row, bool dragging);
    void setLane(int row, int lane);
    void setNgDropHovered(int row, bool hovered);
    void setActive(int row, bool active);

private:
    QVector<Row> m_rows;
};
