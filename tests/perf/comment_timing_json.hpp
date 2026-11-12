#pragma once
// GUI-thread drain/serialization after measurement. Pending is a non-consuming
// snapshot and must not be added to terminal outcome counts.
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace niconeon::perf {
inline QJsonValue relativeTimestamp(qint64 timestamp, qint64 epoch) {
    return timestamp > 0 ? QJsonValue(timestamp - epoch) : QJsonValue(QJsonValue::Null);
}
template <class Record> QJsonObject commentTimingRecordJson(const Record &record, qint64 epoch) {
    using Outcome = decltype(record.outcome);
    using Origin = decltype(record.expiryOrigin);
    using Cancellation = decltype(record.cancellationReason);
    const auto outcome = [&] {
        switch (record.outcome) {
        case Outcome::Pending:
            return QStringLiteral("pending");
        case Outcome::Activated:
            return QStringLiteral("activated");
        case Outcome::Expired:
            return QStringLiteral("expired");
        case Outcome::Failed:
            return QStringLiteral("failed");
        case Outcome::Cancelled:
            return QStringLiteral("cancelled");
        }
        return QStringLiteral("unknown");
    }();
    const auto origin = [&] {
        switch (record.expiryOrigin) {
        case Origin::None:
            return QStringLiteral("none");
        case Origin::SourceLag:
            return QStringLiteral("source_lag");
        case Origin::SourceMotion:
            return QStringLiteral("source_motion");
        case Origin::RasterWait:
            return QStringLiteral("raster_wait");
        }
        return QStringLiteral("unknown");
    }();
    const auto cancellation = [&] {
        switch (record.cancellationReason) {
        case Cancellation::None:
            return QStringLiteral("none");
        case Cancellation::Seek:
            return QStringLiteral("seek");
        case Cancellation::Session:
            return QStringLiteral("session");
        case Cancellation::Ng:
            return QStringLiteral("ng");
        case Cancellation::Shutdown:
            return QStringLiteral("shutdown");
        }
        return QStringLiteral("unknown");
    }();
    return {{"comment_id", record.commentId},
            {"sprite_id", static_cast<qint64>(record.spriteId)},
            {"outcome", outcome},
            {"expiry_origin", origin},
            {"cancellation_reason", cancellation},
            {"admitted_elapsed_ns", relativeTimestamp(record.admittedAtNs, epoch)},
            {"raster_completed_elapsed_ns", relativeTimestamp(record.rasterCompletedAtNs, epoch)},
            {"gui_ready_elapsed_ns", relativeTimestamp(record.guiReadyAtNs, epoch)},
            {"resolved_elapsed_ns", relativeTimestamp(record.resolvedAtNs, epoch)},
            {"source_lag_ms", record.sourceLagMs},
            {"source_motion_delay_seconds", record.sourceMotionDelaySeconds},
            {"raster_motion_delay_seconds", record.rasterMotionDelaySeconds},
            {"initial_x", record.initialX},
            {"admission_x", record.admissionX},
            {"resolved_x", record.resolvedX},
            {"width_estimate", record.widthEstimate}};
}
template <class Controller> QJsonObject commentTimingJson(Controller &controller, qint64 epoch) {
    if constexpr (requires { controller.takeCommentTimingDiagnostics(); }) {
        const auto batch = controller.takeCommentTimingDiagnostics();
        QJsonArray records;
        QJsonArray pending;
        for (const auto &record : batch.records)
            records.append(commentTimingRecordJson(record, epoch));
        for (const auto &record : batch.pending)
            pending.append(commentTimingRecordJson(record, epoch));
        return {{"available", true},
                {"enabled", batch.enabled},
                {"overflow", static_cast<qint64>(batch.droppedComments)},
                {"recorded_comments", static_cast<qint64>(batch.recordedComments)},
                {"records", records},
                {"pending", pending},
                {"timestamp_note", "null means unavailable; completion/readiness may precede admission"},
                {"raster_wait_note", "post-admission wait includes mailbox/order/NG delay, not only CPU painting"}};
    }
    return {{"available", false},
            {"enabled", false},
            {"reason", "controller does not provide comment timing diagnostics"},
            {"records", QJsonArray{}},
            {"pending", QJsonArray{}}};
}
} // namespace niconeon::perf
