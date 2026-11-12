#pragma once
// Bounded, opt-in Qt Quick phase observations. These are CPU signal timestamps,
// not GPU elapsed queries. Serialization happens only after the measurement.
#include <QJsonArray>
#include <QJsonObject>
#include <QQuickWindow>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace niconeon::perf {
class FramePhases final {
  public:
    explicit FramePhases(QQuickWindow *window, bool enabled = true) {
        if (!window || !enabled)
            return;
        m_state = std::make_shared<State>();
        m_state->samples.reserve(capacity);
        const auto connect = [this, window](auto signal, int phase) {
            m_connections.push_back(QObject::connect(
                window, signal, window,
                [state = m_state, phase] {
                    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
                    std::lock_guard guard(state->mutex);
                    if (state->closed)
                        return;
                    if (phase == 0)
                        ++state->serial;
                    if (state->samples.size() < capacity)
                        state->samples.push_back({state->serial, now, phase});
                    else
                        ++state->overflow;
                },
                Qt::DirectConnection));
        };
        connect(&QQuickWindow::beforeFrameBegin, 0);
        connect(&QQuickWindow::beforeSynchronizing, 1);
        connect(&QQuickWindow::afterSynchronizing, 2);
        connect(&QQuickWindow::beforeRendering, 3);
        connect(&QQuickWindow::afterRendering, 4);
        connect(&QQuickWindow::frameSwapped, 5);
        connect(&QQuickWindow::afterFrameEnd, 6);
    }
    ~FramePhases() {
        stop();
    }
    QJsonObject finish(qint64 epoch) {
        stop();
        QJsonArray values;
        if (!m_state)
            return {{"enabled", false}, {"samples", values}};
        constexpr std::array<const char *, 7> names{"before_frame_begin", "before_synchronizing", "after_synchronizing",
                                                    "before_rendering",   "after_rendering",      "frame_swapped",
                                                    "after_frame_end"};
        std::lock_guard guard(m_state->mutex);
        for (const auto &sample : m_state->samples)
            values.append(QJsonObject{{"serial", qint64(sample.serial)},
                                      {"elapsed_ns", sample.timestamp - epoch},
                                      {"event", QString::fromLatin1(names[sample.phase])}});
        return {{"enabled", true}, {"overflow", qint64(m_state->overflow)}, {"samples", values}};
    }

  private:
    static constexpr size_t capacity = 131072;
    struct Sample {
        quint64 serial;
        qint64 timestamp;
        int phase;
    };
    struct State {
        std::mutex mutex;
        bool closed = false;
        quint64 serial = 0;
        quint64 overflow = 0;
        std::vector<Sample> samples;
    };
    void stop() {
        if (m_state) {
            std::lock_guard guard(m_state->mutex);
            m_state->closed = true;
        }
        for (const auto &connection : m_connections)
            QObject::disconnect(connection);
        m_connections.clear();
    }
    std::shared_ptr<State> m_state;
    QVector<QMetaObject::Connection> m_connections;
};
} // namespace niconeon::perf
