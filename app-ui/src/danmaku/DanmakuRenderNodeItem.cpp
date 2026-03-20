#include "danmaku/DanmakuRenderNodeItem.hpp"

#include "danmaku/DanmakuAtlasPacker.hpp"
#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuRenderFrame.hpp"
#include "danmaku/DanmakuRenderStyle.hpp"

#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <QImage>
#include <QMetaObject>
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLPixelTransferOptions>
#include <QOpenGLShader>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QPainter>
#include <QQuickWindow>
#include <QRectF>
#include <QSGNode>
#include <QSGRenderNode>
#include <QSet>
#include <QSharedPointer>
#include <QSize>
#include <QSurfaceFormat>
#include <QVector>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace {
constexpr int kAtlasPagePixelSize = 2048;
constexpr int kMaxAtlasPages = 8;
constexpr qint64 kPerfLogWindowMs = 2000;

enum class DanmakuRendererBackend {
    Atlas,
    FrameImage,
};

DanmakuRendererBackend rendererBackendFromEnv() {
    const QString raw = qEnvironmentVariable("NICONEON_DANMAKU_RENDERER").trimmed().toLower();
    if (raw == QStringLiteral("frame_image")) {
        return DanmakuRendererBackend::FrameImage;
    }
    return DanmakuRendererBackend::Atlas;
}

const char *rendererBackendName(DanmakuRendererBackend backend) {
    switch (backend) {
    case DanmakuRendererBackend::Atlas:
        return "atlas";
    case DanmakuRendererBackend::FrameImage:
        return "frame_image";
    }
    return "atlas";
}

QImage normalizedImageForAtlas(const QImage &image) {
    if (image.isNull()) {
        return {};
    }

    QImage normalized = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    normalized.setDevicePixelRatio(1.0);
    return normalized;
}

QImage colorizeSpriteImage(const QImage &source, const QColor &color) {
    if (source.isNull()) {
        return {};
    }

    QImage tinted(source.size(), QImage::Format_RGBA8888_Premultiplied);
    tinted.setDevicePixelRatio(source.devicePixelRatio());
    tinted.fill(Qt::transparent);

    QPainter painter(&tinted);
    painter.drawImage(QPoint(0, 0), source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(QRect(QPoint(0, 0), source.size()), color);
    return tinted;
}

class DanmakuRenderNode final : public QSGRenderNode, protected QOpenGLExtraFunctions {
public:
    DanmakuRenderNode()
        : m_quadVbo(QOpenGLBuffer::VertexBuffer),
          m_instanceVbo(QOpenGLBuffer::VertexBuffer),
          m_frameVbo(QOpenGLBuffer::VertexBuffer) {}

    ~DanmakuRenderNode() override {
        releaseResources();
    }

    void setFrame(
        const DanmakuRenderFrame &frame,
        const QVector<DanmakuSpriteUpload> &uploads,
        const QSize &itemSize,
        qreal devicePixelRatio,
        DanmakuRendererBackend backend) {
        if (m_requestedBackend != backend) {
            m_requestedBackend = backend;
            m_runtimeBackend = backend;
            m_atlasInstancingUnsupported = false;
        }
        m_itemSize = itemSize;
        m_devicePixelRatio = std::max(devicePixelRatio, 1.0);
        ++m_frameSequence;
        ++m_perfFrameCount;
        m_perfInstanceTotal += frame.instances.size();

        for (const DanmakuSpriteUpload &upload : uploads) {
            if (upload.spriteId == 0 || upload.image.isNull()) {
                continue;
            }

            SpriteRecord &record = m_sprites[upload.spriteId];
            record.spriteId = upload.spriteId;
            record.logicalSize = upload.logicalSize;
            record.image = normalizedImageForAtlas(upload.image);
            record.hoverImage = {};
            record.lastUsedFrame = m_frameSequence;
            if (record.pageIndex >= 0) {
                removeSpriteFromPage(upload.spriteId, record.pageIndex);
            }
            record.pageIndex = -1;
            record.pixelRect = {};
            ++m_perfSpriteUploadCount;
            m_perfSpriteUploadBytes += static_cast<qulonglong>(record.image.sizeInBytes());
        }

        m_instances = frame.instances;
        QSet<DanmakuSpriteId> activeSpriteIds;
        activeSpriteIds.reserve(m_instances.size());
        for (const DanmakuRenderInstance &instance : m_instances) {
            activeSpriteIds.insert(instance.spriteId);
            auto spriteIt = m_sprites.find(instance.spriteId);
            if (spriteIt != m_sprites.end()) {
                spriteIt->lastUsedFrame = m_frameSequence;
            }
        }

        if (m_runtimeBackend == DanmakuRendererBackend::Atlas) {
            ensureActiveSpritesResident(activeSpriteIds);
            if (m_atlasInstancingUnsupported) {
                buildAtlasVertices();
                m_pageInstances.clear();
            } else {
                buildAtlasInstances();
                m_pageVertices.clear();
            }
        } else {
            composeFrameImage();
        }
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
        auto *ctx = QOpenGLContext::currentContext();
        if (!ctx) {
            return;
        }
        ensureGlFunctionsInitialized(ctx);

        const QMatrix4x4 *projection = state ? state->projectionMatrix() : nullptr;
        if (!projection) {
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

        QMatrix4x4 mvp = *projection;
        const QMatrix4x4 *model = matrix();
        if (model) {
            mvp *= *model;
        }

        int drawCallsThisFrame = 0;
        DanmakuRendererBackend backendForRender = m_runtimeBackend;
        if (backendForRender == DanmakuRendererBackend::Atlas) {
            const bool useInstancing = !m_atlasInstancingUnsupported && supportsAtlasInstancing(ctx);
            if (useInstancing) {
                if (!ensureAtlasGlResources() || !updateAtlasTextures() || !m_atlasProgram) {
                    activateFrameImageFallback(QStringLiteral("atlas_gl_resources_unavailable"));
                    backendForRender = m_runtimeBackend;
                } else {
                    m_atlasProgram->bind();
                    m_atlasProgram->setUniformValue(m_atlasMatrixLoc, mvp);
                    m_atlasProgram->setUniformValue(m_atlasTextureLoc, 0);

                    m_quadVbo.bind();
                    m_atlasProgram->enableAttributeArray(m_atlasLocalPositionLoc);
                    m_atlasProgram->enableAttributeArray(m_atlasLocalUvLoc);
                    m_atlasProgram->setAttributeBuffer(
                        m_atlasLocalPositionLoc,
                        GL_FLOAT,
                        offsetof(QuadVertex, x),
                        2,
                        sizeof(QuadVertex));
                    m_atlasProgram->setAttributeBuffer(
                        m_atlasLocalUvLoc,
                        GL_FLOAT,
                        offsetof(QuadVertex, u),
                        2,
                        sizeof(QuadVertex));

                    m_instanceVbo.bind();
                    m_atlasProgram->enableAttributeArray(m_atlasRectLoc);
                    m_atlasProgram->enableAttributeArray(m_atlasUvRectLoc);
                    m_atlasProgram->enableAttributeArray(m_atlasColorLoc);
                    m_atlasProgram->setAttributeBuffer(
                        m_atlasRectLoc,
                        GL_FLOAT,
                        offsetof(InstanceData, x),
                        4,
                        sizeof(InstanceData));
                    m_atlasProgram->setAttributeBuffer(
                        m_atlasUvRectLoc,
                        GL_FLOAT,
                        offsetof(InstanceData, u0),
                        4,
                        sizeof(InstanceData));
                    m_atlasProgram->setAttributeBuffer(
                        m_atlasColorLoc,
                        GL_FLOAT,
                        offsetof(InstanceData, r),
                        4,
                        sizeof(InstanceData));
                    glVertexAttribDivisor(static_cast<GLuint>(m_atlasRectLoc), 1);
                    glVertexAttribDivisor(static_cast<GLuint>(m_atlasUvRectLoc), 1);
                    glVertexAttribDivisor(static_cast<GLuint>(m_atlasColorLoc), 1);

                    for (int pageIndex = 0; pageIndex < m_pageInstances.size(); ++pageIndex) {
                        const QVector<InstanceData> &instances = m_pageInstances[pageIndex];
                        if (instances.isEmpty()) {
                            continue;
                        }
                        AtlasPage &page = m_atlasPages[pageIndex];
                        if (!page.texture) {
                            continue;
                        }
                        page.texture->bind(0);
                        m_instanceVbo.allocate(instances.constData(), instances.size() * static_cast<int>(sizeof(InstanceData)));
                        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, instances.size());
                        page.texture->release();
                        ++drawCallsThisFrame;
                    }

                    glVertexAttribDivisor(static_cast<GLuint>(m_atlasRectLoc), 0);
                    glVertexAttribDivisor(static_cast<GLuint>(m_atlasUvRectLoc), 0);
                    glVertexAttribDivisor(static_cast<GLuint>(m_atlasColorLoc), 0);
                    m_atlasProgram->disableAttributeArray(m_atlasLocalPositionLoc);
                    m_atlasProgram->disableAttributeArray(m_atlasLocalUvLoc);
                    m_atlasProgram->disableAttributeArray(m_atlasRectLoc);
                    m_atlasProgram->disableAttributeArray(m_atlasUvRectLoc);
                    m_atlasProgram->disableAttributeArray(m_atlasColorLoc);
                    m_instanceVbo.release();
                    m_quadVbo.release();
                    m_atlasProgram->release();
                }
            } else {
                activateAtlasVertexFallback(QStringLiteral("instancing_unavailable"));
                if (m_pageVertices.size() != m_atlasPages.size()) {
                    buildAtlasVertices();
                }
                if (!ensureFrameGlResources() || !updateAtlasTextures() || !m_frameProgram) {
                    activateFrameImageFallback(QStringLiteral("atlas_vertex_path_unavailable"));
                    backendForRender = m_runtimeBackend;
                } else {
                    m_frameProgram->bind();
                    m_frameProgram->setUniformValue(m_frameMatrixLoc, mvp);
                    m_frameProgram->setUniformValue(m_frameTextureLoc, 0);
                    m_frameVbo.bind();
                    m_frameProgram->enableAttributeArray(m_framePositionLoc);
                    m_frameProgram->enableAttributeArray(m_frameUvLoc);
                    m_frameProgram->enableAttributeArray(m_frameColorLoc);
                    m_frameProgram->setAttributeBuffer(m_framePositionLoc, GL_FLOAT, offsetof(Vertex, x), 2, sizeof(Vertex));
                    m_frameProgram->setAttributeBuffer(m_frameUvLoc, GL_FLOAT, offsetof(Vertex, u), 2, sizeof(Vertex));
                    m_frameProgram->setAttributeBuffer(m_frameColorLoc, GL_FLOAT, offsetof(Vertex, r), 4, sizeof(Vertex));

                    for (int pageIndex = 0; pageIndex < m_pageVertices.size(); ++pageIndex) {
                        const QVector<Vertex> &vertices = m_pageVertices[pageIndex];
                        if (vertices.isEmpty()) {
                            continue;
                        }
                        AtlasPage &page = m_atlasPages[pageIndex];
                        if (!page.texture) {
                            continue;
                        }
                        page.texture->bind(0);
                        m_frameVbo.allocate(vertices.constData(), vertices.size() * static_cast<int>(sizeof(Vertex)));
                        glDrawArrays(GL_TRIANGLES, 0, vertices.size());
                        page.texture->release();
                        ++drawCallsThisFrame;
                    }

                    m_frameProgram->disableAttributeArray(m_framePositionLoc);
                    m_frameProgram->disableAttributeArray(m_frameUvLoc);
                    m_frameProgram->disableAttributeArray(m_frameColorLoc);
                    m_frameVbo.release();
                    m_frameProgram->release();
                }
            }
        }

        if (backendForRender == DanmakuRendererBackend::FrameImage) {
            if (!ensureFrameGlResources() || !updateFrameTexture() || !m_frameTexture || m_frameQuadVertices.isEmpty()) {
                return;
            }

            m_frameProgram->bind();
            m_frameProgram->setUniformValue(m_frameMatrixLoc, mvp);
            m_frameProgram->setUniformValue(m_frameTextureLoc, 0);
            m_frameVbo.bind();
            m_frameProgram->enableAttributeArray(m_framePositionLoc);
            m_frameProgram->enableAttributeArray(m_frameUvLoc);
            m_frameProgram->enableAttributeArray(m_frameColorLoc);
            m_frameProgram->setAttributeBuffer(m_framePositionLoc, GL_FLOAT, offsetof(Vertex, x), 2, sizeof(Vertex));
            m_frameProgram->setAttributeBuffer(m_frameUvLoc, GL_FLOAT, offsetof(Vertex, u), 2, sizeof(Vertex));
            m_frameProgram->setAttributeBuffer(m_frameColorLoc, GL_FLOAT, offsetof(Vertex, r), 4, sizeof(Vertex));
            m_frameTexture->bind(0);
            m_frameVbo.allocate(
                m_frameQuadVertices.constData(),
                m_frameQuadVertices.size() * static_cast<int>(sizeof(Vertex)));
            glDrawArrays(GL_TRIANGLES, 0, m_frameQuadVertices.size());
            m_frameTexture->release();
            drawCallsThisFrame = m_frameQuadVertices.isEmpty() ? 0 : 1;

            m_frameProgram->disableAttributeArray(m_framePositionLoc);
            m_frameProgram->disableAttributeArray(m_frameUvLoc);
            m_frameProgram->disableAttributeArray(m_frameColorLoc);
            m_frameVbo.release();
            m_frameProgram->release();
        }

        m_perfDrawCalls += drawCallsThisFrame;
        maybeWritePerfLog();
    }

private:
    void activateAtlasVertexFallback(const QString &reason) {
        if (m_atlasInstancingUnsupported) {
            return;
        }
        qWarning().noquote()
            << QString("[danmaku-render] fallback=atlas_vertices reason=%1 requested=%2")
                   .arg(reason)
                   .arg(QString::fromLatin1(rendererBackendName(m_requestedBackend)));
        m_atlasInstancingUnsupported = true;
    }

    void activateFrameImageFallback(const QString &reason) {
        if (m_runtimeBackend == DanmakuRendererBackend::FrameImage) {
            return;
        }
        qWarning().noquote()
            << QString("[danmaku-render] fallback=frame_image reason=%1 requested=%2")
                   .arg(reason)
                   .arg(QString::fromLatin1(rendererBackendName(m_requestedBackend)));
        m_runtimeBackend = DanmakuRendererBackend::FrameImage;
        composeFrameImage();
    }

    void ensureGlFunctionsInitialized(QOpenGLContext *ctx) {
        if (!ctx) {
            return;
        }
        if (m_glContext == ctx && m_glInitialized) {
            return;
        }

        initializeOpenGLFunctions();
        m_glContext = ctx;
        m_glInitialized = true;
    }

    struct QuadVertex {
        float x = 0.0f;
        float y = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
    };

    struct Vertex {
        float x = 0.0f;
        float y = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    struct InstanceData {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float u0 = 0.0f;
        float v0 = 0.0f;
        float u1 = 0.0f;
        float v1 = 0.0f;
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;
    };

    struct SpriteRecord {
        DanmakuSpriteId spriteId = 0;
        QSize logicalSize;
        QImage image;
        QImage hoverImage;
        quint64 lastUsedFrame = 0;
        int pageIndex = -1;
        QRect pixelRect;
    };

    struct AtlasPage {
        DanmakuAtlasPacker packer;
        QImage image;
        QOpenGLTexture *texture = nullptr;
        bool textureDirty = true;
        QSet<DanmakuSpriteId> residents;
    };

    struct PackCandidate {
        DanmakuSpriteId spriteId = 0;
        QSize size;
        quint64 lastUsedFrame = 0;
    };

    bool ensureAtlasGlResources() {
        auto *ctx = QOpenGLContext::currentContext();
        if (!ctx) {
            return false;
        }
        ensureGlFunctionsInitialized(ctx);

        if (!m_atlasProgram) {
            const bool isGles = ctx->isOpenGLES();
            const char *vertexSource = isGles
                ? R"(
                    precision mediump float;
                    attribute vec2 a_localPos;
                    attribute vec2 a_localUv;
                    attribute vec4 a_instanceRect;
                    attribute vec4 a_instanceUvRect;
                    attribute vec4 a_instanceColor;
                    uniform mat4 u_matrix;
                    varying vec2 v_uv;
                    varying vec4 v_color;
                    void main() {
                        vec2 position = a_instanceRect.xy + (a_localPos * a_instanceRect.zw);
                        vec2 uv = mix(a_instanceUvRect.xy, a_instanceUvRect.zw, a_localUv);
                        v_uv = uv;
                        v_color = a_instanceColor;
                        gl_Position = u_matrix * vec4(position, 0.0, 1.0);
                    }
                )"
                : R"(
                    #version 150
                    in vec2 a_localPos;
                    in vec2 a_localUv;
                    in vec4 a_instanceRect;
                    in vec4 a_instanceUvRect;
                    in vec4 a_instanceColor;
                    uniform mat4 u_matrix;
                    out vec2 v_uv;
                    out vec4 v_color;
                    void main() {
                        vec2 position = a_instanceRect.xy + (a_localPos * a_instanceRect.zw);
                        vec2 uv = mix(a_instanceUvRect.xy, a_instanceUvRect.zw, a_localUv);
                        v_uv = uv;
                        v_color = a_instanceColor;
                        gl_Position = u_matrix * vec4(position, 0.0, 1.0);
                    }
                )";

            const char *fragmentSource = isGles
                ? R"(
                    precision mediump float;
                    varying vec2 v_uv;
                    varying vec4 v_color;
                    uniform sampler2D u_texture;
                    void main() {
                        vec4 tex = texture2D(u_texture, v_uv);
                        gl_FragColor = vec4(v_color.rgb * tex.a, tex.a * v_color.a);
                    }
                )"
                : R"(
                    #version 150
                    in vec2 v_uv;
                    in vec4 v_color;
                    uniform sampler2D u_texture;
                    out vec4 fragColor;
                    void main() {
                        vec4 tex = texture(u_texture, v_uv);
                        fragColor = vec4(v_color.rgb * tex.a, tex.a * v_color.a);
                    }
                )";

            m_atlasProgram = new QOpenGLShaderProgram();
            if (!m_atlasProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexSource)
                || !m_atlasProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentSource)) {
                delete m_atlasProgram;
                m_atlasProgram = nullptr;
                return false;
            }

            m_atlasProgram->bindAttributeLocation("a_localPos", 0);
            m_atlasProgram->bindAttributeLocation("a_localUv", 1);
            m_atlasProgram->bindAttributeLocation("a_instanceRect", 2);
            m_atlasProgram->bindAttributeLocation("a_instanceUvRect", 3);
            m_atlasProgram->bindAttributeLocation("a_instanceColor", 4);
            if (!m_atlasProgram->link()) {
                delete m_atlasProgram;
                m_atlasProgram = nullptr;
                return false;
            }

            m_atlasLocalPositionLoc = 0;
            m_atlasLocalUvLoc = 1;
            m_atlasRectLoc = 2;
            m_atlasUvRectLoc = 3;
            m_atlasColorLoc = 4;
            m_atlasMatrixLoc = m_atlasProgram->uniformLocation("u_matrix");
            m_atlasTextureLoc = m_atlasProgram->uniformLocation("u_texture");
        }

        if (!m_quadVbo.isCreated()) {
            m_quadVbo.create();
            m_quadVbo.bind();
            m_quadVbo.setUsagePattern(QOpenGLBuffer::StaticDraw);
            const QuadVertex quadVertices[] = {
                {0.0f, 0.0f, 0.0f, 0.0f},
                {1.0f, 0.0f, 1.0f, 0.0f},
                {0.0f, 1.0f, 0.0f, 1.0f},
                {0.0f, 1.0f, 0.0f, 1.0f},
                {1.0f, 0.0f, 1.0f, 0.0f},
                {1.0f, 1.0f, 1.0f, 1.0f},
            };
            m_quadVbo.allocate(quadVertices, static_cast<int>(sizeof(quadVertices)));
            m_quadVbo.release();
        }
        if (!m_instanceVbo.isCreated()) {
            m_instanceVbo.create();
            m_instanceVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        }
        return true;
    }

    bool supportsAtlasInstancing(QOpenGLContext *ctx) const {
        if (!ctx) {
            return false;
        }
        const QSurfaceFormat format = ctx->format();
        if (ctx->isOpenGLES()) {
            return (format.majorVersion() > 3 || (format.majorVersion() == 3 && format.minorVersion() >= 0))
                || ctx->hasExtension(QByteArrayLiteral("GL_EXT_instanced_arrays"))
                || ctx->hasExtension(QByteArrayLiteral("GL_ANGLE_instanced_arrays"));
        }

        const bool hasGlsl150 =
            format.majorVersion() > 3 || (format.majorVersion() == 3 && format.minorVersion() >= 2);
        const bool hasInstancingApi =
            format.majorVersion() > 3
            || (format.majorVersion() == 3 && format.minorVersion() >= 3)
            || ctx->hasExtension(QByteArrayLiteral("GL_ARB_instanced_arrays"));
        return hasGlsl150 && hasInstancingApi;
    }

    bool ensureFrameGlResources() {
        auto *ctx = QOpenGLContext::currentContext();
        if (!ctx) {
            return false;
        }
        ensureGlFunctionsInitialized(ctx);

        if (!m_frameProgram) {
            const bool isGles = ctx->isOpenGLES();
            const char *vertexSource = isGles
                ? R"(
                    precision mediump float;
                    attribute vec2 a_position;
                    attribute vec2 a_uv;
                    attribute vec4 a_color;
                    uniform mat4 u_matrix;
                    varying vec2 v_uv;
                    varying vec4 v_color;
                    void main() {
                        v_uv = a_uv;
                        v_color = a_color;
                        gl_Position = u_matrix * vec4(a_position, 0.0, 1.0);
                    }
                )"
                : R"(
                    #version 150
                    in vec2 a_position;
                    in vec2 a_uv;
                    in vec4 a_color;
                    uniform mat4 u_matrix;
                    out vec2 v_uv;
                    out vec4 v_color;
                    void main() {
                        v_uv = a_uv;
                        v_color = a_color;
                        gl_Position = u_matrix * vec4(a_position, 0.0, 1.0);
                    }
                )";

            const char *fragmentSource = isGles
                ? R"(
                    precision mediump float;
                    varying vec2 v_uv;
                    varying vec4 v_color;
                    uniform sampler2D u_texture;
                    void main() {
                        vec4 tex = texture2D(u_texture, v_uv);
                        gl_FragColor = vec4(v_color.rgb * tex.a, tex.a * v_color.a);
                    }
                )"
                : R"(
                    #version 150
                    in vec2 v_uv;
                    in vec4 v_color;
                    uniform sampler2D u_texture;
                    out vec4 fragColor;
                    void main() {
                        vec4 tex = texture(u_texture, v_uv);
                        fragColor = vec4(v_color.rgb * tex.a, tex.a * v_color.a);
                    }
                )";

            m_frameProgram = new QOpenGLShaderProgram();
            if (!m_frameProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexSource)
                || !m_frameProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentSource)) {
                delete m_frameProgram;
                m_frameProgram = nullptr;
                return false;
            }

            m_frameProgram->bindAttributeLocation("a_position", 0);
            m_frameProgram->bindAttributeLocation("a_uv", 1);
            m_frameProgram->bindAttributeLocation("a_color", 2);
            if (!m_frameProgram->link()) {
                delete m_frameProgram;
                m_frameProgram = nullptr;
                return false;
            }

            m_framePositionLoc = 0;
            m_frameUvLoc = 1;
            m_frameColorLoc = 2;
            m_frameMatrixLoc = m_frameProgram->uniformLocation("u_matrix");
            m_frameTextureLoc = m_frameProgram->uniformLocation("u_texture");
        }

        if (!m_frameVbo.isCreated()) {
            m_frameVbo.create();
            m_frameVbo.setUsagePattern(QOpenGLBuffer::DynamicDraw);
        }
        return true;
    }

    bool updateTextureFromImage(QOpenGLTexture *&texture, const QImage &image) {
        if (image.isNull()) {
            return false;
        }

        if (!texture) {
            texture = new QOpenGLTexture(QOpenGLTexture::Target2D);
            texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
            texture->setWrapMode(QOpenGLTexture::ClampToEdge);
            texture->setMinificationFilter(QOpenGLTexture::Linear);
            texture->setMagnificationFilter(QOpenGLTexture::Linear);
        }
        if (!texture->isCreated() && !texture->create()) {
            return false;
        }
        if (texture->width() != image.width() || texture->height() != image.height()) {
            texture->destroy();
            if (!texture->create()) {
                return false;
            }
            texture->setFormat(QOpenGLTexture::RGBA8_UNorm);
            texture->setSize(image.width(), image.height());
            texture->setMipLevels(1);
            texture->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8);
        }

        QOpenGLPixelTransferOptions options;
        options.setAlignment(1);
        options.setRowLength(image.bytesPerLine() / 4);
        texture->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, image.constBits(), &options);
        return true;
    }

    bool updateAtlasTextures() {
        for (AtlasPage &page : m_atlasPages) {
            if (!page.textureDirty) {
                continue;
            }
            if (!updateTextureFromImage(page.texture, page.image)) {
                return false;
            }
            page.textureDirty = false;
        }
        return true;
    }

    bool updateFrameTexture() {
        if (!m_frameTextureDirty) {
            return true;
        }
        if (!updateTextureFromImage(m_frameTexture, m_frameImage)) {
            return false;
        }
        m_frameTextureDirty = false;
        return true;
    }

    void ensureActiveSpritesResident(const QSet<DanmakuSpriteId> &activeSpriteIds) {
        for (const DanmakuRenderInstance &instance : m_instances) {
            ensureSpriteResident(instance.spriteId, activeSpriteIds);
        }
    }

    bool ensureSpriteResident(DanmakuSpriteId spriteId, const QSet<DanmakuSpriteId> &activeSpriteIds) {
        auto spriteIt = m_sprites.find(spriteId);
        if (spriteIt == m_sprites.end()) {
            return false;
        }
        SpriteRecord &record = spriteIt.value();
        if (record.pageIndex >= 0) {
            return true;
        }
        if (record.image.isNull()) {
            return false;
        }

        const QSize spriteSize = record.image.size();
        for (int pageIndex = 0; pageIndex < m_atlasPages.size(); ++pageIndex) {
            AtlasPage &page = m_atlasPages[pageIndex];
            const QRect rect = page.packer.insert(spriteSize);
            if (!rect.isValid()) {
                continue;
            }
            placeSpriteOnPage(pageIndex, spriteId, rect);
            return true;
        }

        if (m_atlasPages.size() < kMaxAtlasPages) {
            createAtlasPage();
            AtlasPage &page = m_atlasPages.last();
            const QRect rect = page.packer.insert(spriteSize);
            if (rect.isValid()) {
                placeSpriteOnPage(m_atlasPages.size() - 1, spriteId, rect);
                return true;
            }
        }

        for (int pageIndex = 0; pageIndex < m_atlasPages.size(); ++pageIndex) {
            if (repackPageForSprite(pageIndex, spriteId, activeSpriteIds)) {
                return true;
            }
        }
        return false;
    }

    void placeSpriteOnPage(int pageIndex, DanmakuSpriteId spriteId, const QRect &rect) {
        if (pageIndex < 0 || pageIndex >= m_atlasPages.size()) {
            return;
        }
        auto spriteIt = m_sprites.find(spriteId);
        if (spriteIt == m_sprites.end()) {
            return;
        }

        AtlasPage &page = m_atlasPages[pageIndex];
        SpriteRecord &record = spriteIt.value();
        QPainter painter(&page.image);
        painter.drawImage(rect, record.image);
        record.pageIndex = pageIndex;
        record.pixelRect = rect;
        page.residents.insert(spriteId);
        page.textureDirty = true;
    }

    void removeSpriteFromPage(DanmakuSpriteId spriteId, int pageIndex) {
        if (pageIndex < 0 || pageIndex >= m_atlasPages.size()) {
            return;
        }

        AtlasPage &page = m_atlasPages[pageIndex];
        if (!page.residents.contains(spriteId)) {
            return;
        }

        page.residents.remove(spriteId);
        page.textureDirty = true;
    }

    bool repackPageForSprite(int pageIndex, DanmakuSpriteId requestedSpriteId, const QSet<DanmakuSpriteId> &activeSpriteIds) {
        if (pageIndex < 0 || pageIndex >= m_atlasPages.size()) {
            return false;
        }

        AtlasPage &page = m_atlasPages[pageIndex];
        QVector<DanmakuSpriteId> residents = page.residents.values();
        QVector<DanmakuSpriteId> evictable;
        evictable.reserve(residents.size());
        for (const DanmakuSpriteId spriteId : residents) {
            if (!activeSpriteIds.contains(spriteId)) {
                evictable.push_back(spriteId);
            }
        }
        std::sort(evictable.begin(), evictable.end(), [this](DanmakuSpriteId lhs, DanmakuSpriteId rhs) {
            return m_sprites.value(lhs).lastUsedFrame < m_sprites.value(rhs).lastUsedFrame;
        });
        if (evictable.isEmpty()) {
            return false;
        }

        for (int evictCount = 1; evictCount <= evictable.size(); ++evictCount) {
            QSet<DanmakuSpriteId> survivors = page.residents;
            for (int i = 0; i < evictCount; ++i) {
                survivors.remove(evictable[i]);
            }
            survivors.insert(requestedSpriteId);

            QVector<PackCandidate> candidates;
            candidates.reserve(survivors.size());
            for (const DanmakuSpriteId spriteId : std::as_const(survivors)) {
                const auto spriteIt = m_sprites.constFind(spriteId);
                if (spriteIt == m_sprites.constEnd() || spriteIt.value().image.isNull()) {
                    candidates.clear();
                    break;
                }
                candidates.push_back(PackCandidate {
                    spriteId,
                    spriteIt.value().image.size(),
                    spriteIt.value().lastUsedFrame,
                });
            }
            if (candidates.isEmpty()) {
                continue;
            }

            std::sort(candidates.begin(), candidates.end(), [](const PackCandidate &lhs, const PackCandidate &rhs) {
                if (lhs.size.height() != rhs.size.height()) {
                    return lhs.size.height() > rhs.size.height();
                }
                if (lhs.size.width() != rhs.size.width()) {
                    return lhs.size.width() > rhs.size.width();
                }
                return lhs.spriteId < rhs.spriteId;
            });

            DanmakuAtlasPacker packer(QSize(kAtlasPagePixelSize, kAtlasPagePixelSize));
            QHash<DanmakuSpriteId, QRect> rects;
            bool packed = true;
            for (const PackCandidate &candidate : std::as_const(candidates)) {
                const QRect rect = packer.insert(candidate.size);
                if (!rect.isValid()) {
                    packed = false;
                    break;
                }
                rects.insert(candidate.spriteId, rect);
            }
            if (!packed) {
                continue;
            }

            page.packer = packer;
            page.image.fill(Qt::transparent);
            for (const DanmakuSpriteId spriteId : std::as_const(residents)) {
                SpriteRecord &record = m_sprites[spriteId];
                record.pageIndex = -1;
                record.pixelRect = {};
            }
            page.residents.clear();

            for (const PackCandidate &candidate : std::as_const(candidates)) {
                const QRect rect = rects.value(candidate.spriteId);
                placeSpriteOnPage(pageIndex, candidate.spriteId, rect);
            }
            return true;
        }

        return false;
    }

    void createAtlasPage() {
        AtlasPage page;
        page.packer.reset(QSize(kAtlasPagePixelSize, kAtlasPagePixelSize));
        page.image = QImage(QSize(kAtlasPagePixelSize, kAtlasPagePixelSize), QImage::Format_RGBA8888_Premultiplied);
        page.image.fill(Qt::transparent);
        page.textureDirty = true;
        m_atlasPages.push_back(std::move(page));
    }

    void buildAtlasInstances() {
        m_pageInstances.clear();
        m_pageInstances.resize(m_atlasPages.size());

        const QColor normalColor(Qt::white);
        const QColor hoverColor(QStringLiteral("#FFFF6677"));
        for (const DanmakuRenderInstance &instance : m_instances) {
            const auto spriteIt = m_sprites.constFind(instance.spriteId);
            if (spriteIt == m_sprites.constEnd()) {
                continue;
            }
            const SpriteRecord &record = spriteIt.value();
            if (record.pageIndex < 0 || record.pageIndex >= m_pageInstances.size()) {
                continue;
            }

            const QSize pageSize = m_atlasPages[record.pageIndex].image.size();
            if (!pageSize.isValid()) {
                continue;
            }
            const QColor color = instance.ngDropHovered ? hoverColor : normalColor;
            const float u0 = static_cast<float>(record.pixelRect.left()) / pageSize.width();
            const float v0 = static_cast<float>(record.pixelRect.top()) / pageSize.height();
            const float u1 = static_cast<float>(record.pixelRect.right() + 1) / pageSize.width();
            const float v1 = static_cast<float>(record.pixelRect.bottom() + 1) / pageSize.height();
            const float alpha = static_cast<float>(std::clamp(instance.alpha, 0.0, 1.0));
            QVector<InstanceData> &instances = m_pageInstances[record.pageIndex];
            instances.push_back(InstanceData {
                static_cast<float>(instance.x),
                static_cast<float>(instance.y),
                static_cast<float>(record.logicalSize.width()),
                static_cast<float>(record.logicalSize.height()),
                u0,
                v0,
                u1,
                v1,
                color.redF(),
                color.greenF(),
                color.blueF(),
                alpha,
            });
        }
    }

    void buildAtlasVertices() {
        m_pageVertices.clear();
        m_pageVertices.resize(m_atlasPages.size());

        const QColor normalColor(Qt::white);
        const QColor hoverColor(QStringLiteral("#FFFF6677"));
        for (const DanmakuRenderInstance &instance : m_instances) {
            const auto spriteIt = m_sprites.constFind(instance.spriteId);
            if (spriteIt == m_sprites.constEnd()) {
                continue;
            }
            const SpriteRecord &record = spriteIt.value();
            if (record.pageIndex < 0 || record.pageIndex >= m_pageVertices.size()) {
                continue;
            }

            const QSize pageSize = m_atlasPages[record.pageIndex].image.size();
            if (!pageSize.isValid()) {
                continue;
            }
            const QColor color = instance.ngDropHovered ? hoverColor : normalColor;
            const float left = static_cast<float>(instance.x);
            const float top = static_cast<float>(instance.y);
            const float right = static_cast<float>(instance.x + record.logicalSize.width());
            const float bottom = static_cast<float>(instance.y + record.logicalSize.height());
            const float u0 = static_cast<float>(record.pixelRect.left()) / pageSize.width();
            const float v0 = static_cast<float>(record.pixelRect.top()) / pageSize.height();
            const float u1 = static_cast<float>(record.pixelRect.right() + 1) / pageSize.width();
            const float v1 = static_cast<float>(record.pixelRect.bottom() + 1) / pageSize.height();
            const float alpha = static_cast<float>(std::clamp(instance.alpha, 0.0, 1.0));
            QVector<Vertex> &vertices = m_pageVertices[record.pageIndex];
            vertices.push_back(Vertex {left, top, u0, v0, color.redF(), color.greenF(), color.blueF(), alpha});
            vertices.push_back(Vertex {right, top, u1, v0, color.redF(), color.greenF(), color.blueF(), alpha});
            vertices.push_back(Vertex {left, bottom, u0, v1, color.redF(), color.greenF(), color.blueF(), alpha});
            vertices.push_back(Vertex {left, bottom, u0, v1, color.redF(), color.greenF(), color.blueF(), alpha});
            vertices.push_back(Vertex {right, top, u1, v0, color.redF(), color.greenF(), color.blueF(), alpha});
            vertices.push_back(Vertex {right, bottom, u1, v1, color.redF(), color.greenF(), color.blueF(), alpha});
        }
    }

    void composeFrameImage() {
        const QSize imageSize(
            std::max(1, static_cast<int>(std::ceil(m_itemSize.width() * m_devicePixelRatio))),
            std::max(1, static_cast<int>(std::ceil(m_itemSize.height() * m_devicePixelRatio))));
        m_frameImage = QImage(imageSize, QImage::Format_RGBA8888_Premultiplied);
        m_frameImage.setDevicePixelRatio(m_devicePixelRatio);
        m_frameImage.fill(Qt::transparent);

        QPainter painter(&m_frameImage);
        for (const DanmakuRenderInstance &instance : m_instances) {
            const auto spriteIt = m_sprites.find(instance.spriteId);
            if (spriteIt == m_sprites.end() || spriteIt->image.isNull()) {
                continue;
            }
            const QRectF targetRect(
                instance.x,
                instance.y,
                spriteIt->logicalSize.width(),
                spriteIt->logicalSize.height());
            painter.setOpacity(std::clamp(instance.alpha, 0.0, 1.0));
            if (instance.ngDropHovered) {
                if (spriteIt->hoverImage.isNull()) {
                    spriteIt->hoverImage = colorizeSpriteImage(spriteIt->image, QColor(QStringLiteral("#FFFF6677")));
                }
                painter.drawImage(targetRect, spriteIt->hoverImage);
            } else {
                painter.drawImage(targetRect, spriteIt->image);
            }
        }

        const float width = static_cast<float>(m_itemSize.width());
        const float height = static_cast<float>(m_itemSize.height());
        m_frameQuadVertices = {
            Vertex {0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex {width, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex {0.0f, height, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex {0.0f, height, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex {width, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex {width, height, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        };
        m_frameTextureDirty = true;
    }

    void maybeWritePerfLog() {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (m_perfWindowStartMs <= 0) {
            m_perfWindowStartMs = nowMs;
            return;
        }

        const qint64 elapsedMs = nowMs - m_perfWindowStartMs;
        if (elapsedMs < kPerfLogWindowMs) {
            return;
        }

        qInfo().noquote()
            << QString("[perf-render] backend=%1 window_ms=%2 frame_count=%3 instances=%4 sprite_upload_count=%5 sprite_upload_bytes=%6 atlas_pages=%7 draw_calls=%8")
                   .arg(QString::fromLatin1(rendererBackendName(m_runtimeBackend)))
                   .arg(elapsedMs)
                   .arg(m_perfFrameCount)
                   .arg(m_perfInstanceTotal)
                   .arg(m_perfSpriteUploadCount)
                   .arg(m_perfSpriteUploadBytes)
                   .arg(m_atlasPages.size())
                   .arg(m_perfDrawCalls);

        m_perfWindowStartMs = nowMs;
        m_perfFrameCount = 0;
        m_perfInstanceTotal = 0;
        m_perfSpriteUploadCount = 0;
        m_perfSpriteUploadBytes = 0;
        m_perfDrawCalls = 0;
    }

    void releaseResources() {
        for (AtlasPage &page : m_atlasPages) {
            delete page.texture;
            page.texture = nullptr;
        }
        m_atlasPages.clear();
        if (m_frameTexture) {
            delete m_frameTexture;
            m_frameTexture = nullptr;
        }
        if (m_atlasProgram) {
            delete m_atlasProgram;
            m_atlasProgram = nullptr;
        }
        if (m_frameProgram) {
            delete m_frameProgram;
            m_frameProgram = nullptr;
        }
        if (m_quadVbo.isCreated()) {
            m_quadVbo.destroy();
        }
        if (m_instanceVbo.isCreated()) {
            m_instanceVbo.destroy();
        }
        if (m_frameVbo.isCreated()) {
            m_frameVbo.destroy();
        }
    }

    QSize m_itemSize;
    qreal m_devicePixelRatio = 1.0;
    DanmakuRendererBackend m_requestedBackend = DanmakuRendererBackend::Atlas;
    DanmakuRendererBackend m_runtimeBackend = DanmakuRendererBackend::Atlas;
    QVector<DanmakuRenderInstance> m_instances;
    QHash<DanmakuSpriteId, SpriteRecord> m_sprites;
    QVector<AtlasPage> m_atlasPages;
    QVector<QVector<InstanceData>> m_pageInstances;
    QVector<QVector<Vertex>> m_pageVertices;
    QVector<Vertex> m_frameQuadVertices;
    QImage m_frameImage;
    bool m_glInitialized = false;
    QOpenGLContext *m_glContext = nullptr;
    bool m_atlasInstancingUnsupported = false;
    bool m_frameTextureDirty = true;
    quint64 m_frameSequence = 0;

    QOpenGLShaderProgram *m_atlasProgram = nullptr;
    QOpenGLShaderProgram *m_frameProgram = nullptr;
    QOpenGLTexture *m_frameTexture = nullptr;
    QOpenGLBuffer m_quadVbo;
    QOpenGLBuffer m_instanceVbo;
    QOpenGLBuffer m_frameVbo;
    int m_atlasLocalPositionLoc = -1;
    int m_atlasLocalUvLoc = -1;
    int m_atlasRectLoc = -1;
    int m_atlasUvRectLoc = -1;
    int m_atlasColorLoc = -1;
    int m_atlasMatrixLoc = -1;
    int m_atlasTextureLoc = -1;
    int m_framePositionLoc = -1;
    int m_frameUvLoc = -1;
    int m_frameColorLoc = -1;
    int m_frameMatrixLoc = -1;
    int m_frameTextureLoc = -1;

    qint64 m_perfWindowStartMs = 0;
    int m_perfFrameCount = 0;
    qulonglong m_perfInstanceTotal = 0;
    qulonglong m_perfSpriteUploadCount = 0;
    qulonglong m_perfSpriteUploadBytes = 0;
    qulonglong m_perfDrawCalls = 0;
};
} // namespace

DanmakuRenderNodeItem::DanmakuRenderNodeItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(QQuickItem::ItemHasContents, true);
    connect(this, &QQuickItem::windowChanged, this, &DanmakuRenderNodeItem::handleWindowChanged);
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
    m_pendingPresentedFrame.store(false, std::memory_order_release);

    if (m_controller) {
        connect(
            m_controller,
            &DanmakuController::renderSnapshotChanged,
            this,
            &DanmakuRenderNodeItem::handleControllerRenderSnapshotChanged,
            Qt::QueuedConnection);
        if (window()) {
            const qreal dpr = std::max(window()->effectiveDevicePixelRatio(), 1.0);
            m_controller->setRenderDevicePixelRatio(dpr);
            m_lastRenderDevicePixelRatio = dpr;
        }
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
    const qreal devicePixelRatio = window() ? std::max(window()->effectiveDevicePixelRatio(), 1.0) : 1.0;
    if (m_controller && !qFuzzyCompare(m_lastRenderDevicePixelRatio, devicePixelRatio)) {
        m_lastRenderDevicePixelRatio = devicePixelRatio;
        QMetaObject::invokeMethod(
            m_controller,
            [controller = QPointer<DanmakuController>(m_controller), devicePixelRatio]() {
                if (controller) {
                    controller->setRenderDevicePixelRatio(devicePixelRatio);
                }
            },
            Qt::QueuedConnection);
    }
    if (itemWidth <= 0 || itemHeight <= 0 || !window()) {
        m_pendingPresentedFrame.store(false, std::memory_order_release);
        node->setFrame(
            DanmakuRenderFrame {},
            {},
            QSize(1, 1),
            1.0,
            rendererBackendFromEnv());
        return node;
    }

    DanmakuRenderFrame frame;
    QVector<DanmakuSpriteUpload> uploads;
    DanmakuRenderFrameConstPtr snapshot;
    if (m_controller) {
        snapshot = m_controller->renderSnapshot();
        uploads = m_controller->takePendingSpriteUploads();
    }
    if (snapshot) {
        frame = *snapshot;
    }
    m_pendingPresentedFrame.store(snapshot && !snapshot->instances.isEmpty(), std::memory_order_release);
    node->setFrame(frame, uploads, QSize(itemWidth, itemHeight), devicePixelRatio, rendererBackendFromEnv());
    return node;
}

void DanmakuRenderNodeItem::handleControllerRenderSnapshotChanged() {
    update();
}

void DanmakuRenderNodeItem::handleWindowChanged(QQuickWindow *window) {
    if (m_frameSwappedConnection) {
        disconnect(m_frameSwappedConnection);
        m_frameSwappedConnection = {};
    }
    m_pendingPresentedFrame.store(false, std::memory_order_release);
    if (!window) {
        return;
    }
    if (m_controller) {
        const qreal dpr = std::max(window->effectiveDevicePixelRatio(), 1.0);
        m_controller->setRenderDevicePixelRatio(dpr);
        m_lastRenderDevicePixelRatio = dpr;
    }
    m_frameSwappedConnection = connect(
        window,
        &QQuickWindow::frameSwapped,
        this,
        &DanmakuRenderNodeItem::handleWindowFrameSwapped,
        Qt::DirectConnection);
}

void DanmakuRenderNodeItem::handleWindowFrameSwapped() {
    if (!m_pendingPresentedFrame.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const QPointer<DanmakuController> controller = m_controller;
    if (!controller) {
        return;
    }
    const qint64 presentedAtMs = QDateTime::currentMSecsSinceEpoch();
    QMetaObject::invokeMethod(
        this,
        [controller, presentedAtMs]() {
            if (controller) {
                controller->recordPresentedCommentFrame(presentedAtMs);
            }
        },
        Qt::QueuedConnection);
}
