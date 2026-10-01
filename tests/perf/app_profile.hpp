#pragma once
// Explicit opt-in observer for normal application/QoS trials. No timings or I/O
// are collected without NICONEON_APP_PROFILE_OUTPUT. All JSON/I/O is after exit.
#include "app/ApplicationController.hpp"
#include "danmaku/DanmakuRenderNodeItem.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSaveFile>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace niconeon::perf {
inline qint64 steadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
class AppProfile final {
  public:
    explicit AppProfile(const QList<QObject *> &roots) : m_output(qEnvironmentVariable("NICONEON_APP_PROFILE_OUTPUT")) {
        if (m_output.isEmpty())
            return;
        m_state->frames.reserve(capacity);
        m_heartbeats.reserve(capacity);
        m_state->epoch = steadyNs();
        for (auto *root : roots) {
            if (auto *window = qobject_cast<QQuickWindow *>(root)) {
                m_window = window;
                m_overlay = window->findChild<DanmakuRenderNodeItem *>();
                m_player = window->findChild<MpvItem *>();
                m_connection = QObject::connect(
                    window, &QQuickWindow::frameSwapped, window,
                    [state = m_state] {
                        std::lock_guard guard(state->mutex);
                        if (state->stopped)
                            return;
                        if (state->frames.size() < capacity)
                            state->frames.push_back(steadyNs() - state->epoch);
                        else
                            ++state->overflow;
                    },
                    Qt::DirectConnection);
                break;
            }
        }
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(16);
        QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] {
            if (m_heartbeats.size() < capacity)
                m_heartbeats.push_back({steadyNs() - m_state->epoch, m_player ? m_player->positionMs() : -1});
            else
                ++m_state->overflow;
        });
        m_timer.start();
    }
    ~AppProfile() {
        QObject::disconnect(m_connection);
    }
    bool finish(const ApplicationController &controller) {
        if (m_output.isEmpty())
            return true;
        m_timer.stop();
        QObject::disconnect(m_connection);
        const auto diagnostics = m_overlay ? m_overlay->takeRenderDiagnostics() : DanmakuRenderDiagnosticsBatch{};
        const auto totals = controller.performanceTotals();
        QJsonArray frames, heartbeat, renders, submissions;
        {
            std::lock_guard guard(m_state->mutex);
            m_state->stopped = true;
            qint64 last = 0;
            for (qint64 time : std::as_const(m_state->frames)) {
                frames.append(QJsonObject{{"elapsed_ns", time}, {"interval_ns", last ? time - last : 0}});
                last = time;
            }
        }
        qint64 last = 0;
        for (const auto &sample : std::as_const(m_heartbeats)) {
            heartbeat.append(QJsonObject{{"elapsed_ns", sample.first},
                                         {"interval_ns", last ? sample.first - last : 0},
                                         {"media_ms", sample.second}});
            last = sample.first;
        }
        for (const auto &f : diagnostics.frames)
            renders.append(QJsonObject{{"elapsed_ns", f.capturedAtNs - m_state->epoch},
                                       {"set_frame_ns", f.setFrameNs},
                                       {"residency_ns", f.residencyNs},
                                       {"repack_ns", f.repackNs},
                                       {"gl_upload_ns", f.glUploadNs},
                                       {"gl_allocation_ns", f.glAllocationNs},
                                       {"render_cpu_ns", f.renderCpuNs},
                                       {"gl_upload_bytes", qint64(f.glUploadBytes)},
                                       {"gl_allocation_bytes", qint64(f.glAllocationBytes)},
                                       {"repack_attempts", f.repackAttempts},
                                       {"repack_successes", f.repackSuccesses},
                                       {"repacked_sprites", f.repackedSprites},
                                       {"received_sprites", f.receivedSprites},
                                       {"received_sprite_bytes", qint64(f.receivedSpriteBytes)},
                                       {"missing_image_unique", f.missingImageUnique},
                                       {"unresident_unique", f.unresidentUnique},
                                       {"active_instances", f.activeInstances},
                                       {"submitted_instances", f.submittedInstances}});
        for (const auto &event : diagnostics.submissions)
            submissions.append(QJsonObject{{"elapsed_ns", event.capturedAtNs - m_state->epoch},
                                           {"comment_id", event.commentId},
                                           {"sprite_id", qint64(event.spriteId)}});
        const QJsonObject summary{
            {"offered", qint64(controller.totalComments())},
            {"source_emitted", qint64(totals.sourceEmitted)},
            {"source_qos_dropped", qint64(totals.sourceQosDropped)},
            {"source_queue_dropped", qint64(totals.sourceQueueDropped)},
            {"source_coalesced", qint64(totals.sourceCoalesced)},
            {"admitted", qint64(totals.admitted)},
            {"discarded_at_shutdown", qint64(totals.discardedAtShutdown)},
            {"source_position_ms", totals.sourcePositionMs},
            {"raster_counters_available", totals.rasterCountersAvailable},
            {"pending_raster_at_shutdown", qint64(totals.pendingRasterAtShutdown)},
            {"pending_comments_at_shutdown", qint64(totals.pendingCommentsAtShutdown)},
            {"raster_failed", qint64(totals.rasterFailed)},
            {"raster_expired", qint64(totals.rasterExpired)},
            {"raster_cancelled_before_shutdown", qint64(totals.rasterCancelledBeforeShutdown)},
            {"submitted_unique", diagnostics.submissions.size()},
            {"media_position_ms", m_player ? m_player->positionMs() : -1},
            {"final_profile", controller.perfProfile()},
            {"final_cap", controller.maxEmitPerTick()},
            {"diagnostics_enabled", diagnostics.enabled},
            {"diagnostics_overflow", qint64(diagnostics.droppedFrames + diagnostics.droppedSubmissionObservations)},
            {"sample_overflow", qint64(m_state->overflow.load())}};
        QStringList errors;
        if (!m_window || !m_overlay || !m_player)
            errors.append(QStringLiteral("Application observer could not resolve window, overlay, or player"));
        if (!diagnostics.enabled || diagnostics.frames.size() < 10 || frames.size() < 10)
            errors.append(QStringLiteral("Missing actual renderer/frameSwapped samples"));
        if (m_state->overflow.load() || diagnostics.droppedFrames || diagnostics.droppedSubmissionObservations)
            errors.append(QStringLiteral("Bounded observer capacity exceeded"));
        if (!m_player || m_player->positionMs() < 1000)
            errors.append(QStringLiteral("Video did not advance"));
        const int duration = qEnvironmentVariableIntValue("NICONEON_SYNTHETIC_DURATION_SEC");
        if (duration > 0 && totals.sourcePositionMs < qint64(duration) * 1000)
            errors.append(QStringLiteral("Source did not reach the end of the offered workload"));
        if (totals.admitted + totals.sourceQueueDropped + totals.discardedAtShutdown != totals.sourceEmitted)
            errors.append(QStringLiteral("Source emission/admission accounting does not close"));
        if (duration > 0 && totals.sourceEmitted + totals.sourceQosDropped + totals.sourceCoalesced !=
                                quint64(controller.totalComments()))
            errors.append(QStringLiteral("Offered/emitted/QoS accounting does not close"));
        if (quint64(diagnostics.submissions.size()) != totals.admitted)
            errors.append(QStringLiteral("Some admitted comments never reached a GL draw before shutdown"));
        if (totals.rasterCountersAvailable && (totals.rasterFailed || totals.rasterExpired ||
                                               totals.pendingCommentsAtShutdown || totals.pendingRasterAtShutdown))
            errors.append(QStringLiteral("Raster failure, expiry, or unfinished work at shutdown"));
        QSaveFile file(m_output);
        const QJsonDocument document(
            QJsonObject{{"schema_version", 1},
                        {"success", errors.isEmpty()},
                        {"errors", QJsonArray::fromStringList(errors)},
                        {"mode", "normal_adaptive_qos"},
                        {"timing", "steady-clock frameSwapped intervals; not physical scanout"},
                        {"summary", summary},
                        {"frame_samples", frames},
                        {"heartbeat_samples", heartbeat},
                        {"render_samples", renders},
                        {"submission_samples", submissions}});
        const bool written = file.open(QIODevice::WriteOnly) && file.write(document.toJson()) >= 0 && file.commit();
        return written && errors.isEmpty();
    }

  private:
    static constexpr qsizetype capacity = 32768;
    QString m_output;
    QPointer<QQuickWindow> m_window;
    QPointer<DanmakuRenderNodeItem> m_overlay;
    QPointer<MpvItem> m_player;
    QMetaObject::Connection m_connection;
    QTimer m_timer;
    struct SharedSamples {
        std::mutex mutex;
        qint64 epoch = 0;
        bool stopped = false;
        std::atomic<quint64> overflow = 0;
        QVector<qint64> frames;
    };
    std::shared_ptr<SharedSamples> m_state = std::make_shared<SharedSamples>();
    QVector<QPair<qint64, qint64>> m_heartbeats;
};
} // namespace niconeon::perf
