#pragma once

#include <QMetaObject>
#include <QPointer>
#include <QQuickItem>
#include <QSharedPointer>
#include <QString>
#include <QVector>
#include <atomic>

class DanmakuController;
class QSGNode;
class QQuickWindow;
struct DanmakuRenderDiagnosticsState;

// Opt-in observer data. CPU durations and delayed GL timestamp observations are separate.
struct DanmakuRenderFrameDiagnostics {
    quint64 frameSequence = 0;
    qint64 capturedAtNs = 0; // std::chrono::steady_clock epoch, shared with the perf harness.
    qint64 setFrameNs = 0;
    qint64 normalizeNs = 0;
    qint64 residencyNs = 0;
    qint64 repackNs = 0;
    qint64 spriteCopyNs = 0;
    qint64 pageClearNs = 0;
    qint64 glAllocationNs = 0;
    qint64 glUploadNs = 0;
    qint64 renderCpuNs = 0;
    // Optional GL timestamp pair for an EARLIER frame, never charged to this frame.
    // The interval can include command-producer gaps; it is not physical scanout.
    bool gpuTimingRequested = false;
    bool gpuTimingSupported = false;
    bool gpuResultAvailable = false;
    quint64 gpuMeasuredFrameSequence = 0;
    qint64 gpuMeasuredCpuStartNs = 0;
    qint64 gpuElapsedNs = 0;
    int gpuMeasuredDrawCalls = 0;
    int gpuPendingQueries = 0;
    int gpuSkippedQueries = 0;
    int gpuInvalidResults = 0;
    quint64 glAllocationBytes = 0;
    quint64 glUploadBytes = 0;
    quint64 pageClearBytes = 0;
    quint64 spriteCopyBytes = 0;
    quint64 receivedSpriteBytes = 0;
    int receivedSprites = 0;
    int glAllocationCount = 0;
    int glUploadCount = 0;
    int atlasPagesUploaded = 0;
    int repackPageAttempts = 0;
    int repackAttempts = 0;
    int repackSuccesses = 0;
    int repackedSprites = 0;
    int repackProtectedSprites = 0;
    int atlasPageCount = 0;
    quint32 activeAtlasPageMask = 0;
    int activeInstances = 0;
    int activeUniqueSprites = 0;
    int missingImageUnique = 0;
    int unresidentUnique = 0;
    int submittedInstances = 0;
    int submittedQuads = 0;
    int drawCalls = 0;
};

struct DanmakuRenderSubmissionEvent {
    quint64 frameSequence = 0;
    qint64 capturedAtNs = 0;
    QString commentId;
    quint64 spriteId = 0;
    quint32 atlasPageMask = 0;
};

struct DanmakuRenderDiagnosticsBatch {
    bool enabled = false;
    QVector<DanmakuRenderFrameDiagnostics> frames;
    QVector<DanmakuRenderSubmissionEvent> submissions;
    // Cumulative run counters; draining does not reset the hard run limits.
    quint64 recordedFrames = 0;
    quint64 recordedSubmissions = 0;
    quint64 droppedFrames = 0;
    quint64 droppedSubmissionObservations = 0;
    static constexpr int FrameCapacity = 16384;
    static constexpr int SubmissionCapacity = 65536;
};

class DanmakuRenderNodeItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(DanmakuController *controller READ controller WRITE setController NOTIFY controllerChanged)

  public:
    explicit DanmakuRenderNodeItem(QQuickItem *parent = nullptr);

    DanmakuController *controller() const;
    void setController(DanmakuController *controller);

    // GUI-thread drain. Enable before item creation with NICONEON_RENDER_DIAGNOSTICS=1.
    // One event per distinct comment ID that reached a draw call, not pixel/presentation proof.
    DanmakuRenderDiagnosticsBatch takeRenderDiagnostics();

  signals:
    void controllerChanged();

  protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *updatePaintNodeData) override;

  private:
    void handleControllerRenderSnapshotChanged();
    void handleWindowChanged(QQuickWindow *window);
    void handleWindowFrameSwapped();

    QPointer<DanmakuController> m_controller;
    QMetaObject::Connection m_frameSwappedConnection;
    std::atomic_bool m_pendingPresentedFrame = false;
    qreal m_lastRenderDevicePixelRatio = 0.0;
    QSharedPointer<DanmakuRenderDiagnosticsState> m_diagnostics;
};
