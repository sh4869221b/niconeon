#include "playback/MpvItem.hpp"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickOpenGLUtils>
#include <QQuickWindow>
#include <QThread>
#include <QUrl>
#include <algorithm>
#include <clocale>
#include <cmath>
extern "C" {
#include <mpv/client.h>
#include <mpv/render_gl.h>
}

// The notifier outlives callbacks. Connections to an item disconnect automatically
// on item destruction, so neither mpv callback holds a raw QQuickItem pointer.
class MpvNotifier : public QObject {
    Q_OBJECT
  signals:
    void updateRequested();
    void renderReady();
    void renderFailed();
};
struct MpvState {
    mpv_handle *handle = nullptr;
    mpv_render_context *renderContext = nullptr; // exclusively render-thread owned
    std::shared_ptr<MpvNotifier> notifier;
    ~MpvState() {
        Q_ASSERT(!renderContext);
        if (handle)
            mpv_terminate_destroy(handle);
    }
};
namespace {
void *getProcAddress(void *, const char *name) {
    auto *context = QOpenGLContext::currentContext();
    return context ? reinterpret_cast<void *>(context->getProcAddress(QByteArray(name))) : nullptr;
}
void renderUpdate(void *context) {
    emit static_cast<MpvNotifier *>(context)->updateRequested();
}
} // namespace
class MpvRenderer : public QQuickFramebufferObject::Renderer {
  public:
    explicit MpvRenderer(std::shared_ptr<MpvState> state) : m_state(std::move(state)) {}
    ~MpvRenderer() override {
        if (m_state && m_state->renderContext) {
            mpv_render_context_set_update_callback(m_state->renderContext, nullptr, nullptr);
            mpv_render_context_free(m_state->renderContext);
            m_state->renderContext = nullptr;
        }
    }
    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, format);
    }
    void render() override {
        if (!m_state || !m_state->handle)
            return;
        // Both context creation and rendering expect a baseline GL state.
        QQuickOpenGLUtils::resetOpenGLState();
        if (!m_state->renderContext) {
            mpv_opengl_init_params init{getProcAddress, nullptr};
            mpv_render_param parameters[] = {
                {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init},
                {MPV_RENDER_PARAM_INVALID, nullptr}};
            if (mpv_render_context_create(&m_state->renderContext, m_state->handle, parameters) < 0) {
                if (!m_reportedFailure) {
                    emit m_state->notifier->renderFailed();
                    m_reportedFailure = true;
                }
                QQuickOpenGLUtils::resetOpenGLState();
                return;
            }
            mpv_render_context_set_update_callback(m_state->renderContext, renderUpdate, m_state->notifier.get());
            emit m_state->notifier->renderReady();
        }
        auto *fbo = framebufferObject();
        if (!fbo)
            return;
        mpv_opengl_fbo target{static_cast<int>(fbo->handle()), fbo->width(), fbo->height(), 0};
        int flip = 0;
        mpv_render_param parameters[] = {{MPV_RENDER_PARAM_OPENGL_FBO, &target},
                                         {MPV_RENDER_PARAM_FLIP_Y, &flip},
                                         {MPV_RENDER_PARAM_INVALID, nullptr}};
        mpv_render_context_render(m_state->renderContext, parameters);
        // libmpv alters GL state; restore Qt Quick's expected baseline before comments/controls.
        QQuickOpenGLUtils::resetOpenGLState();
    }

  private:
    std::shared_ptr<MpvState> m_state;
    bool m_reportedFailure = false;
};
MpvItem::MpvItem(QQuickItem *parent) : QQuickFramebufferObject(parent), m_state(std::make_shared<MpvState>()) {
    setlocale(LC_NUMERIC, "C");
    m_state->notifier = std::shared_ptr<MpvNotifier>(new MpvNotifier, [](MpvNotifier *notifier) {
        if (notifier->thread() == QThread::currentThread())
            delete notifier;
        else
            notifier->deleteLater();
    });
    connect(m_state->notifier.get(), &MpvNotifier::updateRequested, this, &MpvItem::update, Qt::QueuedConnection);
    connect(
        m_state->notifier.get(), &MpvNotifier::renderReady, this,
        [this] {
            m_renderReady = true;
            loadPendingFile();
        },
        Qt::QueuedConnection);
    connect(
        m_state->notifier.get(), &MpvNotifier::renderFailed, this,
        [this] { emit errorOccurred(QStringLiteral("動画描画用OpenGL contextを作成できませんでした")); },
        Qt::QueuedConnection);
    m_mpv = mpv_create();
    m_state->handle = m_mpv;
    if (!m_mpv) {
        qWarning() << "failed to create mpv instance";
        return;
    }
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", "auto-safe");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    const auto audioOutput = qgetenv("NICONEON_MPV_AO").trimmed();
    if (!audioOutput.isEmpty())
        mpv_set_option_string(m_mpv, "ao", audioOutput.constData());
    if (mpv_initialize(m_mpv) < 0) {
        qWarning() << "failed to initialize mpv";
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        m_state->handle = nullptr;
        return;
    }
    connect(&m_pollTimer, &QTimer::timeout, this, &MpvItem::pollProperties);
    m_pollTimer.start(100);
}
MpvItem::~MpvItem() {
    m_pollTimer.stop();
    // Renderer retains shared state until its current-context resource release.
    if (m_state && m_state->notifier)
        disconnect(m_state->notifier.get(), nullptr, this, nullptr);
    m_mpv = nullptr;
}
QQuickFramebufferObject::Renderer *MpvItem::createRenderer() const {
    return new MpvRenderer(m_state);
}
void MpvItem::loadPendingFile() {
    if (!m_mpv || !m_renderReady || m_pendingPath.isEmpty())
        return;
    const auto path = m_pendingPath.toUtf8();
    m_pendingPath.clear();
    const char *command[] = {"loadfile", path.constData(), "replace", nullptr};
    const int status = mpv_command_async(m_mpv, 0, command);
    if (status < 0)
        emit errorOccurred(
            QStringLiteral("動画を開けませんでした: %1").arg(QString::fromUtf8(mpv_error_string(status))));
    else
        setPaused(false);
}
bool MpvItem::openFile(const QString &path) {
    if (!m_mpv) {
        return false;
    }

    QString normalizedPath = path.trimmed();

    if (normalizedPath.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
        const QUrl url(normalizedPath);
        if (url.isLocalFile()) {
            normalizedPath = url.toLocalFile();
        }
    }

#if defined(Q_OS_WIN)
    if (normalizedPath.size() >= 3 && normalizedPath.front() == QChar('/') && normalizedPath.at(1).isLetter() &&
        normalizedPath.at(2) == QChar(':')) {
        normalizedPath.remove(0, 1);
    }
#endif

    normalizedPath = QDir::cleanPath(normalizedPath);
    const QFileInfo fileInfo(normalizedPath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        qWarning() << "video file does not exist:" << normalizedPath;
        return false;
    }

    m_pendingPath = fileInfo.absoluteFilePath();
    m_positionMs = 0;
    m_durationMs = 0;
    emit positionMsChanged();
    emit durationMsChanged();
    if (m_renderReady)
        loadPendingFile();
    else
        update();
    return true;
}

void MpvItem::togglePause() {
    setPaused(!m_paused);
}

void MpvItem::setPaused(bool paused) {
    if (!m_mpv) {
        return;
    }
    int flag = paused ? 1 : 0;
    if (mpv_set_property_async(m_mpv, 0, "pause", MPV_FORMAT_FLAG, &flag) >= 0 && m_paused != paused) {
        m_paused = paused;
        emit pausedChanged();
    }
}

void MpvItem::seek(qint64 ms) {
    if (!m_mpv) {
        return;
    }

    const QByteArray sec = QByteArray::number(ms / 1000.0, 'f', 3);
    const char *cmd[] = {"seek", sec.constData(), "absolute+exact", nullptr};
    mpv_command_async(m_mpv, 0, cmd);
}

qint64 MpvItem::positionMs() const {
    return m_positionMs;
}

qint64 MpvItem::durationMs() const {
    return m_durationMs;
}

bool MpvItem::paused() const {
    return m_paused;
}

double MpvItem::volume() const {
    return m_volume;
}

double MpvItem::speed() const {
    return m_speed;
}

double MpvItem::videoFps() const {
    return m_videoFps;
}

void MpvItem::setVolume(double volume) {
    if (!m_mpv) {
        return;
    }

    if (mpv_set_property_async(m_mpv, 0, "volume", MPV_FORMAT_DOUBLE, &volume) >= 0 && m_volume != volume) {
        m_volume = volume;
        emit volumeChanged();
    }
}

void MpvItem::setSpeed(double speed) {
    if (!m_mpv) {
        return;
    }

    double normalized = std::clamp(speed, 0.5, 3.0);
    if (mpv_set_property_async(m_mpv, 0, "speed", MPV_FORMAT_DOUBLE, &normalized) >= 0 &&
        !qFuzzyCompare(normalized + 1.0, m_speed + 1.0)) {
        m_speed = normalized;
        emit speedChanged();
    }
}

void MpvItem::pollProperties() {
    if (!m_mpv) {
        return;
    }

    for (int count = 0; count < 128; ++count) {
        const auto *event = mpv_wait_event(m_mpv, 0);
        if (!event || event->event_id == MPV_EVENT_NONE)
            break;
        if ((event->event_id == MPV_EVENT_COMMAND_REPLY || event->event_id == MPV_EVENT_SET_PROPERTY_REPLY) &&
            event->error < 0)
            emit errorOccurred(
                QStringLiteral("再生操作に失敗しました: %1").arg(QString::fromUtf8(mpv_error_string(event->error))));
        if (event->event_id == MPV_EVENT_END_FILE && event->data) {
            const auto *end = static_cast<const mpv_event_end_file *>(event->data);
            if (end->reason == MPV_END_FILE_REASON_ERROR)
                emit errorOccurred(
                    QStringLiteral("動画再生に失敗しました: %1").arg(QString::fromUtf8(mpv_error_string(end->error))));
        }
    }
    double posSec = 0.0;
    if (mpv_get_property(m_mpv, "time-pos", MPV_FORMAT_DOUBLE, &posSec) >= 0) {
        const qint64 newPos = static_cast<qint64>(posSec * 1000.0);
        if (newPos != m_positionMs) {
            m_positionMs = newPos;
            emit positionMsChanged();
        }
    }

    double durSec = 0.0;
    if (mpv_get_property(m_mpv, "duration", MPV_FORMAT_DOUBLE, &durSec) >= 0) {
        const qint64 newDur = static_cast<qint64>(durSec * 1000.0);
        if (newDur != m_durationMs) {
            m_durationMs = newDur;
            emit durationMsChanged();
        }
    }

    int pauseFlag = 0;
    if (mpv_get_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pauseFlag) >= 0) {
        const bool paused = pauseFlag != 0;
        if (paused != m_paused) {
            m_paused = paused;
            emit pausedChanged();
        }
    }

    double volumeValue = 0.0;
    if (mpv_get_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &volumeValue) >= 0) {
        if (!qFuzzyCompare(volumeValue + 1.0, m_volume + 1.0)) {
            m_volume = volumeValue;
            emit volumeChanged();
        }
    }

    double speedValue = 1.0;
    if (mpv_get_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &speedValue) >= 0) {
        if (!qFuzzyCompare(speedValue + 1.0, m_speed + 1.0)) {
            m_speed = speedValue;
            emit speedChanged();
        }
    }

    // UI 表示用途のため、無効な値を取得した場合は直前の値を保持する。
    double fpsValue = 0.0;
    int fpsStatus = mpv_get_property(m_mpv, "estimated-vf-fps", MPV_FORMAT_DOUBLE, &fpsValue);
    if (fpsStatus < 0 || !std::isfinite(fpsValue) || fpsValue <= 0.0) {
        fpsValue = 0.0;
        fpsStatus = mpv_get_property(m_mpv, "container-fps", MPV_FORMAT_DOUBLE, &fpsValue);
    }
    if (fpsStatus >= 0 && std::isfinite(fpsValue) && fpsValue > 0.0 &&
        !qFuzzyCompare(fpsValue + 1.0, m_videoFps + 1.0)) {
        m_videoFps = fpsValue;
        emit videoFpsChanged();
    }
}

#include "MpvItem.moc"
