#include "danmaku/DanmakuRenderNodeItem.hpp"

#include "danmaku/DanmakuController.hpp"

#include <QColor>
#include <QFont>
#include <QImage>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShader>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QPainter>
#include <QPen>
#include <QQuickWindow>
#include <QRectF>
#include <QSGNode>
#include <QSGRenderNode>
#include <QSize>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {
constexpr int kItemHeightPx = 42;
constexpr int kTextPixelSize = 24;

QImage renderSnapshotImage(
    const QVector<DanmakuController::RenderItem> &items,
    const QSize &itemSize,
    qreal devicePixelRatio) {
    const QSize imageSize(
        std::max(1, static_cast<int>(std::ceil(itemSize.width() * devicePixelRatio))),
        std::max(1, static_cast<int>(std::ceil(itemSize.height() * devicePixelRatio))));

    QImage image(imageSize, QImage::Format_RGBA8888_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font;
    font.setPixelSize(kTextPixelSize);
    painter.setFont(font);

    const QColor fillColor(QStringLiteral("#88000000"));
    const QColor normalBorder(QStringLiteral("#AAFFFFFF"));
    const QColor ngBorder(QStringLiteral("#FFFF4466"));
    const QColor textColor(Qt::white);

    for (const DanmakuController::RenderItem &item : items) {
        if (item.alpha <= 0.0) {
            continue;
        }

        painter.setOpacity(std::clamp(item.alpha, 0.0, 1.0));
        const QRectF rect(item.x, item.y, item.widthEstimate, kItemHeightPx);
        painter.setBrush(fillColor);
        painter.setPen(QPen(item.ngDropHovered ? ngBorder : normalBorder, item.ngDropHovered ? 2.0 : 1.0));
        painter.drawRoundedRect(rect, 8.0, 8.0);

        painter.setPen(textColor);
        const QRectF textRect = rect.adjusted(8.0, 0.0, -8.0, 0.0);
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignHCenter, item.text);
    }

    return image;
}

class DanmakuRenderNode final : public QSGRenderNode, protected QOpenGLFunctions {
public:
    DanmakuRenderNode() : m_vbo(QOpenGLBuffer::VertexBuffer) {}

    ~DanmakuRenderNode() override {
        releaseResources();
    }

    void setFrame(
        const QVector<DanmakuController::RenderItem> &items,
        const QSize &itemSize,
        qreal devicePixelRatio) {
        m_itemSize = itemSize;
        m_image = renderSnapshotImage(items, itemSize, devicePixelRatio);
        m_textureDirty = true;
        m_geometryDirty = true;
    }

    RenderingFlags flags() const override {
        return BoundedRectRendering;
    }

    StateFlags changedStates() const override {
        return BlendState | ScissorState | DepthState | StencilState | ColorState | CullState;
    }

    QRectF rect() const override {
        return QRectF(QPointF(0.0, 0.0), QSizeF(m_itemSize));
    }

    void render(const RenderState *state) override {
        if (m_itemSize.width() <= 0 || m_itemSize.height() <= 0) {
            return;
        }
        if (!ensureGlResources()) {
            return;
        }
        if (!updateTexture()) {
            return;
        }
        if (!updateGeometry()) {
            return;
        }

        const QMatrix4x4 *projection = state ? state->projectionMatrix() : nullptr;
        if (!projection || !m_program || !m_texture) {
            return;
        }

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        if (state && state->scissorEnabled()) {
            const QRect scissor = state->scissorRect();
            glEnable(GL_SCISSOR_TEST);
            glScissor(scissor.x(), scissor.y(), scissor.width(), scissor.height());
        } else {
            glDisable(GL_SCISSOR_TEST);
        }

        m_program->bind();
        m_program->setUniformValue(m_matrixLoc, *projection);
        m_program->setUniformValue(m_textureLoc, 0);

        m_texture->bind(0);
        m_vbo.bind();
        m_program->enableAttributeArray(m_positionLoc);
        m_program->enableAttributeArray(m_uvLoc);
        m_program->setAttributeBuffer(m_positionLoc, GL_FLOAT, offsetof(Vertex, x), 2, sizeof(Vertex));
        m_program->setAttributeBuffer(m_uvLoc, GL_FLOAT, offsetof(Vertex, u), 2, sizeof(Vertex));

        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        m_program->disableAttributeArray(m_positionLoc);
        m_program->disableAttributeArray(m_uvLoc);
        m_vbo.release();
        m_texture->release();
        m_program->release();
    }

private:
    struct Vertex {
        float x;
        float y;
        float u;
        float v;
    };

    bool ensureGlResources() {
        auto *ctx = QOpenGLContext::currentContext();
        if (!ctx) {
            return false;
        }
        if (!m_glInitialized) {
            initializeOpenGLFunctions();
            m_glInitialized = true;
        }

        if (!m_program) {
            m_program = new QOpenGLShaderProgram();
            static const char *kVertexShader = R"(
                #ifdef GL_ES
                precision mediump float;
                #endif
                attribute vec2 a_position;
                attribute vec2 a_uv;
                uniform mat4 u_matrix;
                varying vec2 v_uv;
                void main() {
                    v_uv = a_uv;
                    gl_Position = u_matrix * vec4(a_position, 0.0, 1.0);
                }
            )";
            static const char *kFragmentShader = R"(
                #ifdef GL_ES
                precision mediump float;
                #endif
                varying vec2 v_uv;
                uniform sampler2D u_texture;
                void main() {
                    gl_FragColor = texture2D(u_texture, v_uv);
                }
            )";

            if (!m_program->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader)
                || !m_program->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader)) {
                delete m_program;
                m_program = nullptr;
                return false;
            }

            m_program->bindAttributeLocation("a_position", 0);
            m_program->bindAttributeLocation("a_uv", 1);
            if (!m_program->link()) {
                delete m_program;
                m_program = nullptr;
                return false;
            }
            m_positionLoc = 0;
            m_uvLoc = 1;
            m_matrixLoc = m_program->uniformLocation("u_matrix");
            m_textureLoc = m_program->uniformLocation("u_texture");
        }

        if (!m_vbo.isCreated()) {
            m_vbo.create();
            m_vbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
            m_geometryDirty = true;
        }

        return true;
    }

    bool updateTexture() {
        if (!m_textureDirty) {
            return true;
        }
        if (m_image.isNull()) {
            return false;
        }

        if (!m_texture) {
            m_texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
            m_texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
            m_texture->setWrapMode(QOpenGLTexture::ClampToEdge);
            m_texture->setMinificationFilter(QOpenGLTexture::Linear);
            m_texture->setMagnificationFilter(QOpenGLTexture::Linear);
        }

        m_texture->setData(m_image, QOpenGLTexture::DontGenerateMipMaps);
        m_textureDirty = false;
        return true;
    }

    bool updateGeometry() {
        if (!m_geometryDirty) {
            return true;
        }
        if (!m_vbo.isCreated()) {
            return false;
        }

        const float width = static_cast<float>(m_itemSize.width());
        const float height = static_cast<float>(m_itemSize.height());
        const Vertex vertices[4] = {
            {0.0f, 0.0f, 0.0f, 0.0f},
            {width, 0.0f, 1.0f, 0.0f},
            {0.0f, height, 0.0f, 1.0f},
            {width, height, 1.0f, 1.0f},
        };

        m_vbo.bind();
        m_vbo.allocate(vertices, sizeof(vertices));
        m_vbo.release();

        m_geometryDirty = false;
        return true;
    }

    void releaseResources() {
        if (m_texture) {
            delete m_texture;
            m_texture = nullptr;
        }
        if (m_program) {
            delete m_program;
            m_program = nullptr;
        }
        if (m_vbo.isCreated()) {
            m_vbo.destroy();
        }
    }

    QSize m_itemSize;
    QImage m_image;
    bool m_glInitialized = false;
    bool m_textureDirty = true;
    bool m_geometryDirty = true;

    QOpenGLShaderProgram *m_program = nullptr;
    QOpenGLTexture *m_texture = nullptr;
    QOpenGLBuffer m_vbo;

    int m_positionLoc = -1;
    int m_uvLoc = -1;
    int m_matrixLoc = -1;
    int m_textureLoc = -1;
};
} // namespace

DanmakuRenderNodeItem::DanmakuRenderNodeItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(QQuickItem::ItemHasContents, true);
}

DanmakuController *DanmakuRenderNodeItem::controller() const {
    return m_controller.data();
}

void DanmakuRenderNodeItem::setController(DanmakuController *controller) {
    if (m_controller == controller) {
        return;
    }

    if (m_controller) {
        disconnect(m_controller, &DanmakuController::renderSnapshotChanged, this, &DanmakuRenderNodeItem::handleControllerRenderSnapshotChanged);
    }

    m_controller = controller;

    if (m_controller) {
        connect(
            m_controller,
            &DanmakuController::renderSnapshotChanged,
            this,
            &DanmakuRenderNodeItem::handleControllerRenderSnapshotChanged,
            Qt::QueuedConnection);
    }

    emit controllerChanged();
    update();
}

QSGNode *DanmakuRenderNodeItem::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *updatePaintNodeData) {
    Q_UNUSED(updatePaintNodeData)

    auto *node = static_cast<DanmakuRenderNode *>(oldNode);
    if (!node) {
        node = new DanmakuRenderNode();
    }

    const int itemWidth = static_cast<int>(std::ceil(width()));
    const int itemHeight = static_cast<int>(std::ceil(height()));
    if (itemWidth <= 0 || itemHeight <= 0 || !window()) {
        node->setFrame({}, QSize(1, 1), 1.0);
        return node;
    }

    QVector<DanmakuController::RenderItem> items;
    if (m_controller) {
        items = m_controller->renderSnapshot();
    }

    node->setFrame(items, QSize(itemWidth, itemHeight), window()->effectiveDevicePixelRatio());
    return node;
}

void DanmakuRenderNodeItem::handleControllerRenderSnapshotChanged() {
    update();
}
