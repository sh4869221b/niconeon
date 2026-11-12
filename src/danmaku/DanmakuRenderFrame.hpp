#pragma once

#include <QImage>
#include <QSharedPointer>
#include <QString>
#include <QVector>
#include <QtGlobal>

using DanmakuSpriteId = quint32;

enum class DanmakuCommentTimingOutcome { Pending, Activated, Expired, Failed, Cancelled };
enum class DanmakuCommentExpiryOrigin { None, SourceLag, SourceMotion, RasterWait };
enum class DanmakuCommentCancellationReason { None, Seek, Session, Ng, Shutdown };

// Opt-in observer data, owned and drained only by the GUI thread. These events
// describe admission/activation, not draw submission or visible pixels.
struct DanmakuCommentTimingRecord {
    QString commentId;
    DanmakuSpriteId spriteId = 0;
    DanmakuCommentTimingOutcome outcome = DanmakuCommentTimingOutcome::Pending;
    DanmakuCommentExpiryOrigin expiryOrigin = DanmakuCommentExpiryOrigin::None;
    DanmakuCommentCancellationReason cancellationReason = DanmakuCommentCancellationReason::None;
    // steady_clock epoch, identical to the render diagnostics. Zero means not
    // observed; shared/cached sprite timestamps can precede this admission.
    qint64 admittedAtNs = 0;
    qint64 rasterCompletedAtNs = 0;
    qint64 guiReadyAtNs = 0;
    qint64 resolvedAtNs = 0;
    qint64 sourceLagMs = 0;
    qreal sourceMotionDelaySeconds = 0;
    qreal rasterMotionDelaySeconds = 0;
    qreal initialX = 0;
    qreal admissionX = 0;
    qreal resolvedX = 0;
    int widthEstimate = 0;
};

struct DanmakuCommentTimingBatch {
    bool enabled = false;
    QVector<DanmakuCommentTimingRecord> records;
    // Non-consuming snapshot, never add these to terminal outcome totals.
    QVector<DanmakuCommentTimingRecord> pending;
    quint64 recordedComments = 0;
    quint64 droppedComments = 0;
    static constexpr int CommentCapacity = 65536;
};

struct DanmakuSpriteUpload {
    DanmakuSpriteId spriteId = 0;
    QSize logicalSize;
    QImage image;
};

struct DanmakuRenderInstance {
    QString commentId;
    DanmakuSpriteId spriteId = 0;
    qreal x = 0.0;
    qreal y = 0.0;
    qreal alpha = 1.0;
    int widthEstimate = 0;
    bool ngDropHovered = false;
};

struct DanmakuRenderFrame {
    QVector<DanmakuRenderInstance> instances;
};

using DanmakuRenderFramePtr = QSharedPointer<DanmakuRenderFrame>;
using DanmakuRenderFrameConstPtr = QSharedPointer<const DanmakuRenderFrame>;
