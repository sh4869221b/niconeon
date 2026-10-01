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
#include <QMutex>
#include <QMutexLocker>
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
#include <QScopeGuard>
#include <QSet>
#include <QSharedPointer>
#include <QSize>
#include <QSurfaceFormat>
#include <QVector>
#include <QtGlobal>
#include <rhi/qrhi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <utility>

struct DanmakuRenderDiagnosticsState {
    QMutex mutex;
    DanmakuRenderDiagnosticsBatch pending;
    qsizetype framesRead = 0;
    qsizetype submissionsRead = 0;
    // Render-thread-only identity index survives scene graph node recreation.
    QSet<QString> submittedIds;

    DanmakuRenderDiagnosticsState() {
        pending.enabled = true;
        pending.frames.reserve(DanmakuRenderDiagnosticsBatch::FrameCapacity);
        pending.submissions.reserve(DanmakuRenderDiagnosticsBatch::SubmissionCapacity);
        submittedIds.reserve(DanmakuRenderDiagnosticsBatch::SubmissionCapacity);
    }
};

namespace {
constexpr int kAtlasPagePixelSize = 2048;
constexpr int kMaxAtlasPages = 8;
constexpr qint64 kPerfLogWindowMs = 2000;
using DiagnosticClock = std::chrono::steady_clock;
qint64 diagnosticElapsedNs(DiagnosticClock::time_point started) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(DiagnosticClock::now() - started).count();
}

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
    explicit DanmakuRenderNode(QSharedPointer<DanmakuRenderDiagnosticsState> diagnostics)
        : m_quadVbo(QOpenGLBuffer::VertexBuffer), m_instanceVbo(QOpenGLBuffer::VertexBuffer),
          m_frameVbo(QOpenGLBuffer::VertexBuffer), m_diagnostics(std::move(diagnostics)) {}

    ~DanmakuRenderNode() override {
        releaseResources();
    }

    void setFrame(const DanmakuRenderFrameConstPtr &frame, const QVector<DanmakuSpriteUpload> &uploads,
                  const QSize &itemSize, qreal devicePixelRatio, DanmakuRendererBackend backend) {
        const auto syncStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        if (m_diagnostics)
            m_frameDiagnostics = {};
        if (m_requestedBackend != backend) {
            m_requestedBackend = backend;
            m_runtimeBackend = backend;
            m_atlasInstancingUnsupported = false;
        }
        m_itemSize = itemSize;
        m_devicePixelRatio = std::max(devicePixelRatio, 1.0);
        m_frameSnapshot = frame;
        const QVector<DanmakuRenderInstance> &instances = currentInstances();
        ++m_frameSequence;
        ++m_perfFrameCount;
        m_perfInstanceTotal += instances.size();

        for (const DanmakuSpriteUpload &upload : uploads) {
            if (upload.spriteId == 0 || upload.image.isNull()) {
                continue;
            }
            if (m_diagnostics) {
                ++m_frameDiagnostics.receivedSprites;
                m_frameDiagnostics.receivedSpriteBytes += upload.image.sizeInBytes();
            }

            SpriteRecord &record = m_sprites[upload.spriteId];
            record.spriteId = upload.spriteId;
            record.logicalSize = upload.logicalSize;
            const auto normalizeStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
            record.image = normalizedImageForAtlas(upload.image);
            if (m_diagnostics)
                m_frameDiagnostics.normalizeNs += diagnosticElapsedNs(normalizeStarted);
            record.hoverImage = {};
            record.lastUsedFrame = m_frameSequence;
            for (qsizetype index = 0; index < record.tiles.size(); ++index)
                removeSpriteFromPage(tileKey(upload.spriteId, index), record.tiles[index].pageIndex);
            record.tiles.clear();
            for (const auto &region : danmakuAtlasTiles(record.image, kAtlasPagePixelSize))
                record.tiles.push_back({region, -1, {}});
            ++m_perfSpriteUploadCount;
            m_perfSpriteUploadBytes += static_cast<qulonglong>(record.image.sizeInBytes());
        }

        bool hasPendingResidency = false;
        for (const DanmakuRenderInstance &instance : instances) {
            auto spriteIt = m_sprites.find(instance.spriteId);
            if (spriteIt != m_sprites.end()) {
                spriteIt->lastUsedFrame = m_frameSequence;
                if (!fullyResident(*spriteIt) && !spriteIt->image.isNull()) {
                    hasPendingResidency = true;
                }
            }
        }

        if (m_runtimeBackend == DanmakuRendererBackend::Atlas) {
            if (hasPendingResidency) {
                const auto residencyStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
                ensureActiveSpritesResident();
                if (m_diagnostics)
                    m_frameDiagnostics.residencyNs += diagnosticElapsedNs(residencyStarted);
            }
            if (m_atlasInstancingUnsupported) {
                buildAtlasVertices();
                clearPageInstanceBuffers();
            } else {
                buildAtlasInstances();
                clearPageVertexBuffers();
            }
        } else {
            composeFrameImage();
        }
        if (m_diagnostics) {
            m_frameDiagnostics.frameSequence = m_frameSequence;
            m_frameDiagnostics.setFrameNs = diagnosticElapsedNs(syncStarted);
            m_frameDiagnostics.activeInstances = instances.size();
            QSet<DanmakuSpriteId> uniqueIds;
            uniqueIds.reserve(instances.size());
            for (const auto &instance : instances)
                uniqueIds.insert(instance.spriteId);
            m_frameDiagnostics.activeUniqueSprites = uniqueIds.size();
            m_frameDiagnostics.atlasPageCount = m_atlasPages.size();
            for (const auto id : std::as_const(uniqueIds)) {
                const auto sprite = m_sprites.constFind(id);
                if (sprite != m_sprites.constEnd())
                    m_frameDiagnostics.activeAtlasPageMask |= spritePageMask(*sprite);
                if (sprite == m_sprites.constEnd() || sprite->image.isNull())
                    ++m_frameDiagnostics.missingImageUnique;
                else if (m_runtimeBackend == DanmakuRendererBackend::Atlas && !fullyResident(*sprite))
                    ++m_frameDiagnostics.unresidentUnique;
            }
        }
    }

    RenderingFlags flags() const override {
        return BoundedRectRendering;
    }

    StateFlags changedStates() const override {
        return ViewportState | BlendState | ScissorState | DepthState | StencilState | ColorState | CullState;
    }

    QRectF rect() const override {
        return QRectF(QPointF(0.0, 0.0), QSizeF(m_itemSize));
    }

    void render(const RenderState *state) override {
        const auto renderStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        QVector<bool> submittedPages;
        if (m_diagnostics)
            submittedPages.fill(false, m_atlasPages.size());
        bool submittedFrameImage = false;
        const auto diagnosticsGuard = qScopeGuard([&] {
            if (m_diagnostics)
                publishDiagnostics(renderStarted, submittedPages, submittedFrameImage);
        });
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

        // Qt 6 does not establish dynamic GL state before a custom render node.
        // Query the actual target, including layer FBOs and high-DPI surfaces.
        const auto *target = renderTarget();
        if (!target || target->pixelSize().isEmpty()) {
            return;
        }
        const QSize targetSize = target->pixelSize();
        glViewport(0, 0, targetSize.width(), targetSize.height());

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
                    m_atlasProgram->setAttributeBuffer(m_atlasLocalPositionLoc, GL_FLOAT, offsetof(QuadVertex, x), 2,
                                                       sizeof(QuadVertex));
                    m_atlasProgram->setAttributeBuffer(m_atlasLocalUvLoc, GL_FLOAT, offsetof(QuadVertex, u), 2,
                                                       sizeof(QuadVertex));

                    m_instanceVbo.bind();
                    m_atlasProgram->enableAttributeArray(m_atlasRectLoc);
                    m_atlasProgram->enableAttributeArray(m_atlasUvRectLoc);
                    m_atlasProgram->enableAttributeArray(m_atlasColorLoc);
                    m_atlasProgram->setAttributeBuffer(m_atlasRectLoc, GL_FLOAT, offsetof(InstanceData, x), 4,
                                                       sizeof(InstanceData));
                    m_atlasProgram->setAttributeBuffer(m_atlasUvRectLoc, GL_FLOAT, offsetof(InstanceData, u0), 4,
                                                       sizeof(InstanceData));
                    m_atlasProgram->setAttributeBuffer(m_atlasColorLoc, GL_FLOAT, offsetof(InstanceData, r), 4,
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
                        m_instanceVbo.allocate(instances.constData(),
                                               instances.size() * static_cast<int>(sizeof(InstanceData)));
                        glDrawArraysInstanced(GL_TRIANGLES, 0, 6, instances.size());
                        if (m_diagnostics) {
                            submittedPages[pageIndex] = true;
                            m_frameDiagnostics.submittedQuads += instances.size();
                        }
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
                    m_frameProgram->setAttributeBuffer(m_framePositionLoc, GL_FLOAT, offsetof(Vertex, x), 2,
                                                       sizeof(Vertex));
                    m_frameProgram->setAttributeBuffer(m_frameUvLoc, GL_FLOAT, offsetof(Vertex, u), 2, sizeof(Vertex));
                    m_frameProgram->setAttributeBuffer(m_frameColorLoc, GL_FLOAT, offsetof(Vertex, r), 4,
                                                       sizeof(Vertex));

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
                        if (m_diagnostics) {
                            submittedPages[pageIndex] = true;
                            m_frameDiagnostics.submittedQuads += vertices.size() / 6;
                        }
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
            if (!ensureFrameGlResources() || !updateFrameTexture() || !m_frameTexture ||
                m_frameQuadVertices.isEmpty()) {
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
            m_frameVbo.allocate(m_frameQuadVertices.constData(),
                                m_frameQuadVertices.size() * static_cast<int>(sizeof(Vertex)));
            glDrawArrays(GL_TRIANGLES, 0, m_frameQuadVertices.size());
            submittedFrameImage = true;
            m_frameTexture->release();
            drawCallsThisFrame = m_frameQuadVertices.isEmpty() ? 0 : 1;

            m_frameProgram->disableAttributeArray(m_framePositionLoc);
            m_frameProgram->disableAttributeArray(m_frameUvLoc);
            m_frameProgram->disableAttributeArray(m_frameColorLoc);
            m_frameVbo.release();
            m_frameProgram->release();
        }

        m_perfDrawCalls += drawCallsThisFrame;
        if (m_diagnostics)
            m_frameDiagnostics.drawCalls = drawCallsThisFrame;
        maybeWritePerfLog();
    }

  private:
    void activateAtlasVertexFallback(const QString &reason) {
        if (m_atlasInstancingUnsupported) {
            return;
        }
        qWarning().noquote() << QString("[danmaku-render] fallback=atlas_vertices reason=%1 requested=%2")
                                    .arg(reason)
                                    .arg(QString::fromLatin1(rendererBackendName(m_requestedBackend)));
        m_atlasInstancingUnsupported = true;
    }

    void activateFrameImageFallback(const QString &reason) {
        if (m_runtimeBackend == DanmakuRendererBackend::FrameImage) {
            return;
        }
        qWarning().noquote() << QString("[danmaku-render] fallback=frame_image reason=%1 requested=%2")
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

    struct SpriteTile {
        DanmakuAtlasTileRegion region;
        int pageIndex = -1;
        QRect pixelRect;
    };
    struct SpriteRecord {
        DanmakuSpriteId spriteId = 0;
        QSize logicalSize;
        QImage image;
        QImage hoverImage;
        quint64 lastUsedFrame = 0;
        QVector<SpriteTile> tiles;
    };

    struct AtlasPage {
        DanmakuAtlasPacker packer;
        QImage image;
        QOpenGLTexture *texture = nullptr;
        bool textureDirty = true;
        QSet<quint64> residents;
    };

    static quint64 tileKey(DanmakuSpriteId spriteId, qsizetype index) {
        return (quint64(spriteId) << 32) | quint32(index);
    }
    static DanmakuSpriteId tileOwner(quint64 key) {
        return DanmakuSpriteId(key >> 32);
    }
    SpriteTile *tile(quint64 key) {
        auto owner = m_sprites.find(tileOwner(key));
        const auto index = quint32(key);
        return owner != m_sprites.end() && index < owner->tiles.size() ? &owner->tiles[index] : nullptr;
    }
    const SpriteTile *tile(quint64 key) const {
        const auto owner = m_sprites.constFind(tileOwner(key));
        const auto index = quint32(key);
        return owner != m_sprites.constEnd() && index < owner->tiles.size() ? &owner->tiles[index] : nullptr;
    }
    static bool fullyResident(const SpriteRecord &sprite) {
        return !sprite.tiles.empty() && std::all_of(sprite.tiles.begin(), sprite.tiles.end(),
                                                    [](const auto &part) { return part.pageIndex >= 0; });
    }
    static quint32 spritePageMask(const SpriteRecord &sprite) {
        quint32 mask = 0;
        for (const auto &part : sprite.tiles)
            if (part.pageIndex >= 0 && part.pageIndex < kMaxAtlasPages)
                mask |= quint32(1) << part.pageIndex;
        return mask;
    }

    bool ensureAtlasGlResources() {
        auto *ctx = QOpenGLContext::currentContext();
        if (!ctx) {
            return false;
        }
        ensureGlFunctionsInitialized(ctx);

        if (!m_atlasProgram) {
            const bool isGles = ctx->isOpenGLES();
            const char *vertexSource = isGles ? R"(
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

            const char *fragmentSource = isGles ? R"(
                    precision mediump float;
                    varying vec2 v_uv;
                    varying vec4 v_color;
                    uniform sampler2D u_texture;
                    void main() {
                        vec4 tex = texture2D(u_texture, v_uv);
                        if (tex.a == 0.0) discard;
                        // Preserve intrinsic color unless the NG SourceIn tint is active.
                        // RGB and alpha must both receive opacity for premultiplied blending.
                        vec3 rgb = all(equal(v_color.rgb, vec3(1.0))) ? tex.rgb : v_color.rgb * tex.a;
                        gl_FragColor = vec4(rgb * v_color.a, tex.a * v_color.a);
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
                        if (tex.a == 0.0) discard;
                        // Preserve intrinsic color unless the NG SourceIn tint is active.
                        // RGB and alpha must both receive opacity for premultiplied blending.
                        vec3 rgb = all(equal(v_color.rgb, vec3(1.0))) ? tex.rgb : v_color.rgb * tex.a;
                        fragColor = vec4(rgb * v_color.a, tex.a * v_color.a);
                    }
                )";

            m_atlasProgram = new QOpenGLShaderProgram();
            if (!m_atlasProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexSource) ||
                !m_atlasProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentSource)) {
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
                {0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f},
                {0.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f},
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
            return (format.majorVersion() > 3 || (format.majorVersion() == 3 && format.minorVersion() >= 0)) ||
                   ctx->hasExtension(QByteArrayLiteral("GL_EXT_instanced_arrays")) ||
                   ctx->hasExtension(QByteArrayLiteral("GL_ANGLE_instanced_arrays"));
        }

        const bool hasGlsl150 = format.majorVersion() > 3 || (format.majorVersion() == 3 && format.minorVersion() >= 2);
        const bool hasInstancingApi = format.majorVersion() > 3 ||
                                      (format.majorVersion() == 3 && format.minorVersion() >= 3) ||
                                      ctx->hasExtension(QByteArrayLiteral("GL_ARB_instanced_arrays"));
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
            const char *vertexSource = isGles ? R"(
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

            const char *fragmentSource = isGles ? R"(
                    precision mediump float;
                    varying vec2 v_uv;
                    varying vec4 v_color;
                    uniform sampler2D u_texture;
                    void main() {
                        vec4 tex = texture2D(u_texture, v_uv);
                        if (tex.a == 0.0) discard;
                        // Preserve intrinsic color unless the NG SourceIn tint is active.
                        // RGB and alpha must both receive opacity for premultiplied blending.
                        vec3 rgb = all(equal(v_color.rgb, vec3(1.0))) ? tex.rgb : v_color.rgb * tex.a;
                        gl_FragColor = vec4(rgb * v_color.a, tex.a * v_color.a);
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
                        if (tex.a == 0.0) discard;
                        // Preserve intrinsic color unless the NG SourceIn tint is active.
                        // RGB and alpha must both receive opacity for premultiplied blending.
                        vec3 rgb = all(equal(v_color.rgb, vec3(1.0))) ? tex.rgb : v_color.rgb * tex.a;
                        fragColor = vec4(rgb * v_color.a, tex.a * v_color.a);
                    }
                )";

            m_frameProgram = new QOpenGLShaderProgram();
            if (!m_frameProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexSource) ||
                !m_frameProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentSource)) {
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
            const auto allocationStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
            texture->allocateStorage(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8);
            if (m_diagnostics) {
                m_frameDiagnostics.glAllocationNs += diagnosticElapsedNs(allocationStarted);
                m_frameDiagnostics.glAllocationBytes += quint64(image.width()) * image.height() * 4;
                ++m_frameDiagnostics.glAllocationCount;
            }
        }

        QOpenGLPixelTransferOptions options;
        options.setAlignment(1);
        options.setRowLength(image.bytesPerLine() / 4);
        const auto uploadStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        texture->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8, image.constBits(), &options);
        if (m_diagnostics) {
            m_frameDiagnostics.glUploadNs += diagnosticElapsedNs(uploadStarted);
            m_frameDiagnostics.glUploadBytes += quint64(image.width()) * image.height() * 4;
            ++m_frameDiagnostics.glUploadCount;
        }
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
            if (m_diagnostics)
                ++m_frameDiagnostics.atlasPagesUploaded;
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

    const QVector<DanmakuRenderInstance> &currentInstances() const {
        static const QVector<DanmakuRenderInstance> emptyInstances;
        return m_frameSnapshot ? m_frameSnapshot->instances : emptyInstances;
    }

    void clearPageInstanceBuffers() {
        for (QVector<InstanceData> &instances : m_pageInstances) {
            instances.clear();
        }
    }

    void clearPageVertexBuffers() {
        for (QVector<Vertex> &vertices : m_pageVertices) {
            vertices.clear();
        }
    }

    void ensureActiveSpritesResident() {
        QSet<quint64> activeTiles;
        QSet<DanmakuSpriteId> seen;
        QVector<DanmakuAtlasRepackCandidate> pending;
        for (const auto &instance : currentInstances()) {
            if (seen.contains(instance.spriteId))
                continue;
            seen.insert(instance.spriteId);
            auto sprite = m_sprites.find(instance.spriteId);
            if (sprite == m_sprites.end() || sprite->image.isNull())
                continue;
            for (qsizetype index = 0; index < sprite->tiles.size(); ++index) {
                const auto key = tileKey(instance.spriteId, index);
                activeTiles.insert(key);
                auto &part = sprite->tiles[index];
                if (part.pageIndex >= 0)
                    continue;
                const auto size = part.region.source.size();
                bool placed = false;
                for (int pageIndex = 0; pageIndex < m_atlasPages.size(); ++pageIndex) {
                    const auto rect = m_atlasPages[pageIndex].packer.insert(size);
                    if (!rect.isValid())
                        continue;
                    placeSpriteOnPage(pageIndex, key, rect);
                    placed = true;
                    break;
                }
                if (!placed && m_atlasPages.size() < kMaxAtlasPages) {
                    createAtlasPage();
                    const auto rect = m_atlasPages.last().packer.insert(size);
                    if (rect.isValid()) {
                        placeSpriteOnPage(m_atlasPages.size() - 1, key, rect);
                        placed = true;
                    }
                }
                if (!placed)
                    pending.push_back({key, size});
            }
        }
        if (pending.empty())
            return;
        struct ReclaimablePage {
            int index;
            quint64 bytes;
        };
        QVector<ReclaimablePage> reclaimable;
        for (int pageIndex = 0; pageIndex < m_atlasPages.size(); ++pageIndex) {
            quint64 bytes = 0;
            for (const auto key : m_atlasPages[pageIndex].residents) {
                const auto *part = tile(key);
                if (!activeTiles.contains(key) && part)
                    bytes += quint64(part->region.source.width()) * part->region.source.height() * 4;
            }
            if (bytes)
                reclaimable.push_back({pageIndex, bytes});
        }
        std::sort(reclaimable.begin(), reclaimable.end(), [](const auto &a, const auto &b) {
            return a.bytes != b.bytes ? a.bytes > b.bytes : a.index < b.index;
        });
        for (const auto &page : std::as_const(reclaimable)) {
            if (repackPageForSprites(page.index, pending, activeTiles))
                pending.removeIf([this](const auto &candidate) {
                    const auto *part = tile(candidate.spriteId);
                    return part && part->pageIndex >= 0;
                });
            if (pending.empty())
                break;
        }
    }

    void placeSpriteOnPage(int pageIndex, quint64 key, const QRect &rect) {
        auto *part = tile(key);
        const auto owner = m_sprites.constFind(tileOwner(key));
        if (!part || owner == m_sprites.constEnd() || pageIndex < 0 || pageIndex >= m_atlasPages.size())
            return;
        auto &page = m_atlasPages[pageIndex];
        const auto started = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        {
            QPainter painter(&page.image);
            painter.drawImage(rect, owner->image, part->region.source);
        }
        part->pageIndex = pageIndex;
        part->pixelRect = rect;
        page.residents.insert(key);
        page.textureDirty = true;
        if (m_diagnostics) {
            m_frameDiagnostics.spriteCopyNs += diagnosticElapsedNs(started);
            m_frameDiagnostics.spriteCopyBytes += quint64(rect.width()) * rect.height() * 4;
        }
    }

    void removeSpriteFromPage(quint64 spriteId, int pageIndex) {
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

    bool repackPageForSprites(int pageIndex, const QVector<DanmakuAtlasRepackCandidate> &pending,
                              const QSet<quint64> &activeSpriteIds) {
        const auto repackStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        const auto repackGuard = qScopeGuard([&] {
            if (m_diagnostics)
                m_frameDiagnostics.repackNs += diagnosticElapsedNs(repackStarted);
        });
        if (m_diagnostics) {
            ++m_frameDiagnostics.repackPageAttempts;
            ++m_frameDiagnostics.repackAttempts;
        }
        AtlasPage &page = m_atlasPages[pageIndex];
        QVector<DanmakuAtlasRepackCandidate> protectedSprites;
        protectedSprites.reserve(page.residents.size());
        for (const auto id : page.residents) {
            if (!activeSpriteIds.contains(id))
                continue;
            const auto *part = tile(id);
            if (!part)
                return false;
            protectedSprites.push_back({id, part->region.source.size()});
        }
        QSet<DanmakuSpriteId> protectedOwners;
        if (m_diagnostics)
            for (const auto &candidate : std::as_const(protectedSprites))
                protectedOwners.insert(tileOwner(candidate.spriteId));
        auto plan = planDanmakuAtlasRepack(QSize(kAtlasPagePixelSize, kAtlasPagePixelSize), std::move(protectedSprites),
                                           pending);
        if (!plan.canCommit)
            return false;

        // Build a complete replacement privately. Allocation/planning failure
        // cannot invalidate active UVs or mutate the old page. At most one
        // extra 16 MiB CPU page exists at a time, with no extra GL texture.
        QImage rebuilt(page.image.size(), QImage::Format_RGBA8888_Premultiplied);
        if (rebuilt.isNull())
            return false;
        const auto clearStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        rebuilt.fill(Qt::transparent);
        if (m_diagnostics) {
            m_frameDiagnostics.pageClearNs += diagnosticElapsedNs(clearStarted);
            m_frameDiagnostics.pageClearBytes += rebuilt.sizeInBytes();
        }
        quint64 copiedBytes = 0;
        const auto copyStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        {
            QPainter painter(&rebuilt);
            if (!painter.isActive())
                return false;
            for (const auto &placement : std::as_const(plan.placements)) {
                const auto sprite = m_sprites.constFind(tileOwner(placement.spriteId));
                const auto *part = tile(placement.spriteId);
                if (!part || sprite == m_sprites.constEnd() || sprite->image.isNull())
                    return false;
                painter.drawImage(placement.rect, sprite->image, part->region.source);
                copiedBytes += quint64(placement.rect.width()) * placement.rect.height() * 4;
            }
        }
        if (m_diagnostics) {
            m_frameDiagnostics.spriteCopyNs += diagnosticElapsedNs(copyStarted);
            m_frameDiagnostics.spriteCopyBytes += copiedBytes;
        }

        // Commit the image, packing state, and every affected UV in one render
        // sync. All current-frame active residents are present in the plan.
        for (const auto id : page.residents) {
            if (auto *part = tile(id)) {
                part->pageIndex = -1;
                part->pixelRect = {};
            }
        }
        page.packer = std::move(plan.packer);
        page.image = std::move(rebuilt);
        page.residents.clear();
        for (const auto &placement : std::as_const(plan.placements)) {
            auto *part = tile(placement.spriteId);
            part->pageIndex = pageIndex;
            part->pixelRect = placement.rect;
            page.residents.insert(placement.spriteId);
        }
        page.textureDirty = true;
        if (m_diagnostics) {
            ++m_frameDiagnostics.repackSuccesses;
            m_frameDiagnostics.repackedSprites += plan.placements.size();
            m_frameDiagnostics.repackProtectedSprites += protectedOwners.size();
        }
        return true;
    }

    void createAtlasPage() {
        AtlasPage page;
        page.packer.reset(QSize(kAtlasPagePixelSize, kAtlasPagePixelSize));
        page.image = QImage(QSize(kAtlasPagePixelSize, kAtlasPagePixelSize), QImage::Format_RGBA8888_Premultiplied);
        const auto clearStarted = m_diagnostics ? DiagnosticClock::now() : DiagnosticClock::time_point{};
        page.image.fill(Qt::transparent);
        if (m_diagnostics) {
            m_frameDiagnostics.pageClearNs += diagnosticElapsedNs(clearStarted);
            m_frameDiagnostics.pageClearBytes += page.image.sizeInBytes();
        }
        page.textureDirty = true;
        m_atlasPages.push_back(std::move(page));
    }

    template <class Consumer> void forEachAtlasInstance(Consumer consume) {
        for (const auto &instance : currentInstances()) {
            const auto sprite = m_sprites.constFind(instance.spriteId);
            if (sprite == m_sprites.constEnd() || !fullyResident(*sprite))
                continue; // Never present only half of a logical text sprite.
            const float alpha = static_cast<float>(std::clamp(instance.alpha, 0.0, 1.0));
            for (const auto &part : sprite->tiles) {
                if (part.pageIndex < 0 || part.pageIndex >= m_atlasPages.size())
                    continue;
                const auto pageSize = m_atlasPages[part.pageIndex].image.size();
                if (pageSize.isEmpty())
                    continue;
                const auto target = danmakuAtlasTileLogicalRect(part.region.core, sprite->image.size(),
                                                                sprite->logicalSize, {instance.x, instance.y});
                const QRect pixels(part.pixelRect.topLeft() + part.region.core.topLeft() - part.region.source.topLeft(),
                                   part.region.core.size());
                consume(part.pageIndex,
                        InstanceData{static_cast<float>(target.x()), static_cast<float>(target.y()),
                                     static_cast<float>(target.width()), static_cast<float>(target.height()),
                                     float(pixels.left()) / pageSize.width(), float(pixels.top()) / pageSize.height(),
                                     float(pixels.right() + 1) / pageSize.width(),
                                     float(pixels.bottom() + 1) / pageSize.height(), 1.0f,
                                     instance.ngDropHovered ? 0.4f : 1.0f,
                                     instance.ngDropHovered ? 119.0f / 255.0f : 1.0f, alpha});
            }
        }
    }
    void buildAtlasInstances() {
        m_pageInstances.resize(m_atlasPages.size());
        clearPageInstanceBuffers();
        forEachAtlasInstance(
            [this](int page, const InstanceData &instance) { m_pageInstances[page].push_back(instance); });
    }
    void buildAtlasVertices() {
        m_pageVertices.resize(m_atlasPages.size());
        clearPageVertexBuffers();
        forEachAtlasInstance([this](int page, const InstanceData &instance) {
            const auto &i = instance;
            auto &vertices = m_pageVertices[page];
            const float right = i.x + i.width, bottom = i.y + i.height;
            vertices.append({Vertex{i.x, i.y, i.u0, i.v0, i.r, i.g, i.b, i.a},
                             Vertex{right, i.y, i.u1, i.v0, i.r, i.g, i.b, i.a},
                             Vertex{i.x, bottom, i.u0, i.v1, i.r, i.g, i.b, i.a},
                             Vertex{i.x, bottom, i.u0, i.v1, i.r, i.g, i.b, i.a},
                             Vertex{right, i.y, i.u1, i.v0, i.r, i.g, i.b, i.a},
                             Vertex{right, bottom, i.u1, i.v1, i.r, i.g, i.b, i.a}});
        });
    }

    void composeFrameImage() {
        const QSize imageSize(std::max(1, static_cast<int>(std::ceil(m_itemSize.width() * m_devicePixelRatio))),
                              std::max(1, static_cast<int>(std::ceil(m_itemSize.height() * m_devicePixelRatio))));
        m_frameImage = QImage(imageSize, QImage::Format_RGBA8888_Premultiplied);
        m_frameImage.setDevicePixelRatio(m_devicePixelRatio);
        m_frameImage.fill(Qt::transparent);

        QPainter painter(&m_frameImage);
        for (const DanmakuRenderInstance &instance : currentInstances()) {
            const auto spriteIt = m_sprites.find(instance.spriteId);
            if (spriteIt == m_sprites.end() || spriteIt->image.isNull()) {
                continue;
            }
            const QRectF targetRect(instance.x, instance.y, spriteIt->logicalSize.width(),
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
            Vertex{0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex{width, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex{0.0f, height, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex{0.0f, height, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex{width, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f},
            Vertex{width, height, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f},
        };
        m_frameTextureDirty = true;
    }

    void publishDiagnostics(DiagnosticClock::time_point renderStarted, const QVector<bool> &submittedPages,
                            bool submittedFrameImage) {
        m_frameDiagnostics.capturedAtNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(DiagnosticClock::now().time_since_epoch()).count();
        m_frameDiagnostics.renderCpuNs = diagnosticElapsedNs(renderStarted);
        QVector<DanmakuRenderSubmissionEvent> submissions;
        quint64 droppedObservations = 0;
        for (const auto &instance : currentInstances()) {
            const auto sprite = m_sprites.constFind(instance.spriteId);
            if (sprite == m_sprites.constEnd() || sprite->image.isNull())
                continue;
            const bool submitted = submittedFrameImage ||
                                   (fullyResident(*sprite) &&
                                    std::all_of(sprite->tiles.begin(), sprite->tiles.end(), [&](const auto &part) {
                                        return part.pageIndex < submittedPages.size() && submittedPages[part.pageIndex];
                                    }));
            if (!submitted)
                continue;
            ++m_frameDiagnostics.submittedInstances;
            if (m_diagnostics->submittedIds.contains(instance.commentId))
                continue;
            if (m_diagnostics->submittedIds.size() >= DanmakuRenderDiagnosticsBatch::SubmissionCapacity) {
                ++droppedObservations;
                continue;
            }
            m_diagnostics->submittedIds.insert(instance.commentId);
            submissions.push_back({m_frameDiagnostics.frameSequence, m_frameDiagnostics.capturedAtNs,
                                   instance.commentId, instance.spriteId,
                                   submittedFrameImage ? 0u : spritePageMask(*sprite)});
        }
        {
            QMutexLocker locker(&m_diagnostics->mutex);
            auto &pending = m_diagnostics->pending;
            if (pending.recordedFrames < DanmakuRenderDiagnosticsBatch::FrameCapacity) {
                pending.frames.push_back(m_frameDiagnostics);
                ++pending.recordedFrames;
            } else {
                ++pending.droppedFrames;
            }
            pending.recordedSubmissions += submissions.size();
            pending.submissions.append(std::move(submissions));
            pending.droppedSubmissionObservations += droppedObservations;
        }
        // render() can run again without a new sync. Retain the snapshot's
        // identity/counts but never charge its sync/upload work twice.
        DanmakuRenderFrameDiagnostics next;
        next.frameSequence = m_frameDiagnostics.frameSequence;
        next.activeInstances = m_frameDiagnostics.activeInstances;
        next.activeUniqueSprites = m_frameDiagnostics.activeUniqueSprites;
        next.missingImageUnique = m_frameDiagnostics.missingImageUnique;
        next.unresidentUnique = m_frameDiagnostics.unresidentUnique;
        next.atlasPageCount = m_frameDiagnostics.atlasPageCount;
        next.activeAtlasPageMask = m_frameDiagnostics.activeAtlasPageMask;
        m_frameDiagnostics = next;
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

        qInfo().noquote() << QString("[perf-render] backend=%1 window_ms=%2 frame_count=%3 instances=%4 "
                                     "sprite_upload_count=%5 sprite_upload_bytes=%6 atlas_pages=%7 draw_calls=%8")
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
        for (auto &sprite : m_sprites)
            for (auto &part : sprite.tiles) {
                part.pageIndex = -1;
                part.pixelRect = {};
            }
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
    DanmakuRenderFrameConstPtr m_frameSnapshot;
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
    QSharedPointer<DanmakuRenderDiagnosticsState> m_diagnostics;
    DanmakuRenderFrameDiagnostics m_frameDiagnostics;
};
} // namespace

DanmakuRenderNodeItem::DanmakuRenderNodeItem(QQuickItem *parent) : QQuickItem(parent) {
    if (qEnvironmentVariableIntValue("NICONEON_RENDER_DIAGNOSTICS") == 1)
        m_diagnostics = QSharedPointer<DanmakuRenderDiagnosticsState>::create();
    setFlag(QQuickItem::ItemHasContents, true);
    connect(this, &QQuickItem::windowChanged, this, &DanmakuRenderNodeItem::handleWindowChanged);
}

DanmakuRenderDiagnosticsBatch DanmakuRenderNodeItem::takeRenderDiagnostics() {
    DanmakuRenderDiagnosticsBatch result;
    if (!m_diagnostics)
        return result;
    QMutexLocker locker(&m_diagnostics->mutex);
    const auto &pending = m_diagnostics->pending;
    result.enabled = true;
    result.recordedFrames = pending.recordedFrames;
    result.recordedSubmissions = pending.recordedSubmissions;
    result.droppedFrames = pending.droppedFrames;
    result.droppedSubmissionObservations = pending.droppedSubmissionObservations;
    // Copy only unread values into independent output buffers. Sharing the
    // retained QVector would trigger detach/copies on the render thread.
    result.frames.reserve(pending.frames.size() - m_diagnostics->framesRead);
    while (m_diagnostics->framesRead < pending.frames.size())
        result.frames.push_back(pending.frames[m_diagnostics->framesRead++]);
    result.submissions.reserve(pending.submissions.size() - m_diagnostics->submissionsRead);
    while (m_diagnostics->submissionsRead < pending.submissions.size())
        result.submissions.push_back(pending.submissions[m_diagnostics->submissionsRead++]);
    return result;
}

DanmakuController *DanmakuRenderNodeItem::controller() const {
    return m_controller.data();
}

void DanmakuRenderNodeItem::setController(DanmakuController *controller) {
    if (m_controller == controller) {
        return;
    }

    if (m_controller) {
        disconnect(m_controller, &DanmakuController::renderSnapshotChanged, this,
                   &DanmakuRenderNodeItem::handleControllerRenderSnapshotChanged);
    }

    m_controller = controller;
    m_pendingPresentedFrame.store(false, std::memory_order_release);

    if (m_controller) {
        connect(m_controller, &DanmakuController::renderSnapshotChanged, this,
                &DanmakuRenderNodeItem::handleControllerRenderSnapshotChanged, Qt::QueuedConnection);
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
        node = new DanmakuRenderNode(m_diagnostics);
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
        node->setFrame({}, {}, QSize(1, 1), 1.0, rendererBackendFromEnv());
        return node;
    }

    QVector<DanmakuSpriteUpload> uploads;
    DanmakuRenderFrameConstPtr snapshot;
    if (m_controller) {
        snapshot = m_controller->renderSnapshot();
        uploads = m_controller->takePendingSpriteUploads();
    }
    m_pendingPresentedFrame.store(snapshot && !snapshot->instances.isEmpty(), std::memory_order_release);
    node->setFrame(snapshot, uploads, QSize(itemWidth, itemHeight), devicePixelRatio, rendererBackendFromEnv());
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
    m_frameSwappedConnection = connect(window, &QQuickWindow::frameSwapped, this,
                                       &DanmakuRenderNodeItem::handleWindowFrameSwapped, Qt::DirectConnection);
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
