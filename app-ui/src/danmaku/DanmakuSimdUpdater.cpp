#include "danmaku/DanmakuSimdUpdater.hpp"

#include <algorithm>
#include <cmath>

#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define NICONEON_HAS_AVX2_IMPL 1
#endif
#endif

namespace {
DanmakuSimdMode normalizeMode(DanmakuSimdMode mode) {
    return DanmakuSimdUpdater::resolveMode(mode);
}

void updateScalar(
    QVector<qreal> &x,
    const QVector<qreal> &speed,
    const QVector<quint8> &movableMask,
    qreal movementFactor,
    QVector<quint8> &changedMask) {
    const int count = std::min({x.size(), speed.size(), movableMask.size(), changedMask.size()});
    for (int i = 0; i < count; ++i) {
        if (!movableMask[i]) {
            continue;
        }
        x[i] -= speed[i] * movementFactor;
        changedMask[i] = 1;
    }
}

#if defined(NICONEON_HAS_AVX2_IMPL)
__attribute__((target("avx2")))
void updateAvx2Doubles(
    QVector<qreal> &x,
    const QVector<qreal> &speed,
    const QVector<quint8> &movableMask,
    qreal movementFactor,
    QVector<quint8> &changedMask) {
    const int count = std::min({x.size(), speed.size(), movableMask.size(), changedMask.size()});
    if (count <= 0) {
        return;
    }

    QVector<qreal> effectiveSpeed(count);
    for (int i = 0; i < count; ++i) {
        effectiveSpeed[i] = movableMask[i] ? speed[i] : 0.0;
    }

    const __m256d factor = _mm256_set1_pd(static_cast<double>(movementFactor));
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        const __m256d xVec = _mm256_loadu_pd(reinterpret_cast<const double *>(&x[i]));
        const __m256d sVec = _mm256_loadu_pd(reinterpret_cast<const double *>(&effectiveSpeed[i]));
        const __m256d next = _mm256_sub_pd(xVec, _mm256_mul_pd(sVec, factor));
        _mm256_storeu_pd(reinterpret_cast<double *>(&x[i]), next);
    }

    for (; i < count; ++i) {
        x[i] -= effectiveSpeed[i] * movementFactor;
    }

    for (int idx = 0; idx < count; ++idx) {
        if (movableMask[idx]) {
            changedMask[idx] = 1;
        }
    }
}
#endif
}

DanmakuSimdMode DanmakuSimdUpdater::parseMode(const QString &raw) {
    const QString normalized = raw.trimmed().toLower();
    if (normalized == QStringLiteral("scalar")) {
        return DanmakuSimdMode::Scalar;
    }
    if (normalized == QStringLiteral("avx2")) {
        return DanmakuSimdMode::Avx2;
    }
    return DanmakuSimdMode::Auto;
}

QString DanmakuSimdUpdater::modeName(DanmakuSimdMode mode) {
    switch (mode) {
    case DanmakuSimdMode::Auto:
        return QStringLiteral("auto");
    case DanmakuSimdMode::Scalar:
        return QStringLiteral("scalar");
    case DanmakuSimdMode::Avx2:
        return QStringLiteral("avx2");
    }
    return QStringLiteral("auto");
}

DanmakuSimdMode DanmakuSimdUpdater::resolveMode(DanmakuSimdMode requested) {
    if (requested == DanmakuSimdMode::Auto) {
        return hasAvx2Runtime() ? DanmakuSimdMode::Avx2 : DanmakuSimdMode::Scalar;
    }
    if (requested == DanmakuSimdMode::Avx2 && !hasAvx2Runtime()) {
        return DanmakuSimdMode::Scalar;
    }
    return requested;
}

void DanmakuSimdUpdater::updatePositions(
    QVector<qreal> &x,
    const QVector<qreal> &speed,
    const QVector<quint8> &movableMask,
    qreal movementFactor,
    QVector<quint8> &changedMask,
    DanmakuSimdMode mode) {
    if (x.isEmpty()) {
        return;
    }

    const DanmakuSimdMode resolved = normalizeMode(mode);
    if (resolved == DanmakuSimdMode::Scalar || movementFactor == 0.0) {
        updateScalar(x, speed, movableMask, movementFactor, changedMask);
        return;
    }

#if defined(NICONEON_HAS_AVX2_IMPL)
    if constexpr (sizeof(qreal) == sizeof(double)) {
        updateAvx2Doubles(x, speed, movableMask, movementFactor, changedMask);
        return;
    }
#endif

    updateScalar(x, speed, movableMask, movementFactor, changedMask);
}

bool DanmakuSimdUpdater::hasAvx2Runtime() {
#if defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_cpu_supports("avx2");
#endif
#endif
    return false;
}
