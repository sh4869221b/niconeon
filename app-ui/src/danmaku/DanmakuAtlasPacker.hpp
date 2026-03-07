#pragma once

#include <QRect>
#include <QSize>
#include <QVector>

class DanmakuAtlasPacker {
public:
    DanmakuAtlasPacker() = default;
    explicit DanmakuAtlasPacker(const QSize &pageSize);

    void reset(const QSize &pageSize);
    QRect insert(const QSize &size);
    QSize pageSize() const;

private:
    struct Shelf {
        int y = 0;
        int height = 0;
        int nextX = 0;
    };

    QSize m_pageSize;
    QVector<Shelf> m_shelves;
};
