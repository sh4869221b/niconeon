#pragma once
// Independent full-image oracle, deliberately unaware of atlas crops/tiles/UVs.
// Pixel centers are (x + 0.5, y + 0.5) in the destination physical image.
// Translate in logical coordinates, convert to source physical coordinates,
// then subtract 0.5 to address source texel centers. This is GL_LINEAR's
// four-neighbor interpolation followed by premultiplied source-over blending.
// QPainter's equal-scale translate fast path snaps even with SmoothPixmapTransform;
// retain QPainter for integer cases, and use this oracle only for subpixel cases.
#include <QImage>
#include <QPointF>
#include <QSizeF>
#include <algorithm>
#include <array>
#include <cmath>

namespace niconeon::perf {
inline void drawBilinearReference(QImage &target, const QImage &input, QPointF position, qreal opacity = 1,
                                  QSizeF logicalSize = {}) {
    Q_ASSERT(target.format() == QImage::Format_RGBA8888_Premultiplied);
    const QImage source = input.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (source.isNull() || target.isNull())
        return;
    if (logicalSize.isEmpty())
        logicalSize = QSizeF(source.size()) / source.devicePixelRatio();
    // The declared full-image logical extent, not nominal DPR, controls mapping:
    // ceil(logicalWidth * DPR) can add a fractional physical pixel at DPR1.5.
    const qreal sx = source.width() / logicalSize.width();
    const qreal sy = source.height() / logicalSize.height();
    const qreal targetDpr = target.devicePixelRatio();
    opacity = std::clamp(opacity, qreal(0), qreal(1));
    const int left = std::max(0, int(std::floor(position.x() * targetDpr)));
    const int top = std::max(0, int(std::floor(position.y() * targetDpr)));
    const int right = std::min(target.width(), int(std::ceil((position.x() + logicalSize.width()) * targetDpr)));
    const int bottom = std::min(target.height(), int(std::ceil((position.y() + logicalSize.height()) * targetDpr)));
    const auto pixel = [&](int x, int y, int channel) {
        return source.constScanLine(
            std::clamp(y, 0, source.height() - 1))[std::clamp(x, 0, source.width() - 1) * 4 + channel];
    };
    for (int y = top; y < bottom; ++y) {
        const qreal syEdge = ((y + 0.5) / targetDpr - position.y()) * sy;
        if (syEdge < 0 || syEdge >= source.height())
            continue;
        const qreal sampleY = syEdge - 0.5;
        const int y0 = int(std::floor(sampleY));
        const qreal fy = sampleY - y0;
        auto *dest = target.scanLine(y);
        for (int x = left; x < right; ++x) {
            const qreal sxEdge = ((x + 0.5) / targetDpr - position.x()) * sx;
            if (sxEdge < 0 || sxEdge >= source.width())
                continue;
            const qreal sampleX = sxEdge - 0.5;
            const int x0 = int(std::floor(sampleX));
            const qreal fx = sampleX - x0;
            std::array<qreal, 4> sampled{};
            for (int c = 0; c < 4; ++c)
                sampled[c] = opacity * ((1 - fy) * ((1 - fx) * pixel(x0, y0, c) + fx * pixel(x0 + 1, y0, c)) +
                                        fy * ((1 - fx) * pixel(x0, y0 + 1, c) + fx * pixel(x0 + 1, y0 + 1, c)));
            for (int c = 0; c < 4; ++c)
                dest[x * 4 + c] = static_cast<uchar>(
                    std::clamp(std::lround(sampled[c] + dest[x * 4 + c] * (1 - sampled[3] / 255)), 0L, 255L));
        }
    }
}
} // namespace niconeon::perf
