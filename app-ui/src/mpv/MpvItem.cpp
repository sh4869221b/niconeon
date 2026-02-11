#include "mpv/MpvItem.hpp"

#include <clocale>

#include <QDebug>
#include <QFileInfo>
#include <QMetaObject>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QQuickWindow>

extern "C" {
#include <mpv/client.h>
#include <mpv/render_gl.h>
}

namespace {
void *getProcAddress(void *ctx, const char *name) {
    Q_UNUSED(ctx)
    auto *context = QOpenGLContext::currentContext();
    if (!context) {
        return nullptr;
    }
    return reinterpret_cast<void *>(context->getProcAddress(QByteArray(name)));
}
} // namespace

class MpvRenderer : public QQuickFramebufferObject::Renderer {
public:
    explicit MpvRenderer(MpvItem *item) : m_item(item) {}

    ~MpvRenderer() override {
        if (m_item && m_item->m_renderContext) {
            mpv_render_context_free(m_item->m_renderContext);
            m_item->m_renderContext = nullptr;
        }
    }

    QOpenGLFramebufferObject *createFramebufferObject(const QSize &size) override {
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::CombinedDepthStencil);
        return new QOpenGLFramebufferObject(size, format);
    }

    void synchronize(QQuickFramebufferObject *item) override {
        m_item = static_cast<MpvItem *>(item);
    }

    void render() override {
        if (!m_item || !m_item->m_mpv) {
            return;
        }

        if (!m_item->m_renderContext) {
            mpv_opengl_init_params glInitParams;
            glInitParams.get_proc_address = getProcAddress;
            glInitParams.get_proc_address_ctx = nullptr;

            mpv_render_param params[] = {
                {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &glInitParams},
                {MPV_RENDER_PARAM_INVALID, nullptr},
            };

            if (mpv_render_context_create(&m_item->m_renderContext, m_item->m_mpv, params) < 0) {
                qWarning() << "failed to create mpv render context";
                return;
            }
            mpv_render_context_set_update_callback(m_item->m_renderContext, MpvItem::onMpvRenderUpdate, m_item);
        }

        auto *fbo = framebufferObject();
        if (!fbo) {
            return;
        }

        mpv_opengl_fbo mpfbo;
        mpfbo.fbo = static_cast<int>(fbo->handle());
        mpfbo.w = fbo->width();
        mpfbo.h = fbo->height();
        mpfbo.internal_format = 0;

        int flipY = 1;
        mpv_render_param renderParams[] = {
            {MPV_RENDER_PARAM_OPENGL_FBO, &mpfbo},
            {MPV_RENDER_PARAM_FLIP_Y, &flipY},
            {MPV_RENDER_PARAM_INVALID, nullptr},
        };

        mpv_render_context_render(m_item->m_renderContext, renderParams);

    }

private:
    MpvItem *m_item = nullptr;
};

MpvItem::MpvItem(QQuickItem *parent) : QQuickFramebufferObject(parent) {
    // Some environments reset locale after app startup; enforce C numeric locale at mpv init point.
    setlocale(LC_NUMERIC, "C");

    m_mpv = mpv_create();
    if (!m_mpv) {
        qWarning() << "failed to create mpv instance";
        return;
    }

    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "hwdec", "auto-safe");
    mpv_set_option_string(m_mpv, "terminal", "no");

    if (mpv_initialize(m_mpv) < 0) {
        qWarning() << "failed to initialize mpv";
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return;
    }

    connect(&m_pollTimer, &QTimer::timeout, this, &MpvItem::pollProperties);
    m_pollTimer.start(100);
}

MpvItem::~MpvItem() {
    if (m_renderContext) {
        mpv_render_context_free(m_renderContext);
        m_renderContext = nullptr;
    }
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

QQuickFramebufferObject::Renderer *MpvItem::createRenderer() const {
    return new MpvRenderer(const_cast<MpvItem *>(this));
}

bool MpvItem::openFile(const QString &path) {
    if (!m_mpv) {
        return false;
    }

    const QByteArray fullPath = QFileInfo(path).absoluteFilePath().toUtf8();
    const char *cmd[] = {"loadfile", fullPath.constData(), "replace", nullptr};
    const int status = mpv_command(m_mpv, cmd);
    return status >= 0;
}

void MpvItem::togglePause() {
    setPaused(!m_paused);
}

void MpvItem::setPaused(bool paused) {
    if (!m_mpv) {
        return;
    }
    int flag = paused ? 1 : 0;
    if (mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag) >= 0 && m_paused != paused) {
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
    mpv_command(m_mpv, cmd);
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

void MpvItem::setVolume(double volume) {
    if (!m_mpv) {
        return;
    }

    if (mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &volume) >= 0 && m_volume != volume) {
        m_volume = volume;
        emit volumeChanged();
    }
}

void MpvItem::pollProperties() {
    if (!m_mpv) {
        return;
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
}

void MpvItem::onMpvRenderUpdate(void *ctx) {
    auto *item = static_cast<MpvItem *>(ctx);
    QMetaObject::invokeMethod(item, [item]() { item->update(); }, Qt::QueuedConnection);
}
