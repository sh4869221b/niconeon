#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuRenderStyle.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetrics>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

class DanmakuTextWidthTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void defaultTargetFpsIs60();
    void commentFpsTracksPresentedFramesOnly();
    void wideFullwidthTextDoesNotUnderestimate();
    void japaneseTextDoesNotUnderestimate();
    void minimumWidthIsPreserved();
    void seekResumeLagCompensationPlacesCommentMidScroll();

private:
    static int requiredBubbleWidth(const QString &text);
    static QSharedPointer<const QVector<DanmakuController::RenderItem>> appendSingleComment(
        const QString &commentId,
        const QString &text,
        qint64 atMs = 0,
        qint64 playbackPositionMs = 0);
};

void DanmakuTextWidthTest::initTestCase() {
    qputenv("NICONEON_DANMAKU_WORKER", "off");
    qputenv("NICONEON_SIMD_MODE", "scalar");
}

void DanmakuTextWidthTest::defaultTargetFpsIs60() {
    DanmakuController controller;
    QCOMPARE(controller.targetFps(), 60);
}

void DanmakuTextWidthTest::commentFpsTracksPresentedFramesOnly() {
    DanmakuController controller;

    QTest::qWait(2200);
    QCOMPARE(controller.commentRenderFps(), 0.0);

    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 1000) {
        controller.recordPresentedCommentFrame();
        QCoreApplication::processEvents();
        QTest::qWait(16);
    }

    QTRY_VERIFY_WITH_TIMEOUT(controller.commentRenderFps() > 5.0, 2500);
}

void DanmakuTextWidthTest::wideFullwidthTextDoesNotUnderestimate() {
    const QString text = QStringLiteral("ｗｗｗｗｗｗｗｗｗｗ");
    const QSharedPointer<const QVector<DanmakuController::RenderItem>> snapshot =
        appendSingleComment(QStringLiteral("width-test-fullwidth"), text);
    QVERIFY(snapshot);
    QVERIFY(!snapshot->isEmpty());
    QCOMPARE((*snapshot)[0].text, text);
    const int actualWidth = (*snapshot)[0].widthEstimate;
    QVERIFY2(
        actualWidth >= requiredBubbleWidth(text),
        "widthEstimate should be at least measured text width + horizontal padding");
}

void DanmakuTextWidthTest::japaneseTextDoesNotUnderestimate() {
    const QString text = QStringLiteral("これはテストコメントです");
    const QSharedPointer<const QVector<DanmakuController::RenderItem>> snapshot =
        appendSingleComment(QStringLiteral("width-test-japanese"), text);
    QVERIFY(snapshot);
    QVERIFY(!snapshot->isEmpty());
    QCOMPARE((*snapshot)[0].text, text);
    const int actualWidth = (*snapshot)[0].widthEstimate;
    QVERIFY2(
        actualWidth >= requiredBubbleWidth(text),
        "widthEstimate should be at least measured text width + horizontal padding");
}

void DanmakuTextWidthTest::minimumWidthIsPreserved() {
    const QString text = QStringLiteral("a");
    const QSharedPointer<const QVector<DanmakuController::RenderItem>> snapshot =
        appendSingleComment(QStringLiteral("width-test-min"), text);
    QVERIFY(snapshot);
    QVERIFY(!snapshot->isEmpty());
    QCOMPARE((*snapshot)[0].text, text);
    const int actualWidth = (*snapshot)[0].widthEstimate;
    QCOMPARE(actualWidth, DanmakuRenderStyle::kMinWidthPx);
}

void DanmakuTextWidthTest::seekResumeLagCompensationPlacesCommentMidScroll() {
    const QString commentId = QStringLiteral("seek-resume-mid-scroll");
    const QString text = QStringLiteral("seek resume");
    const qint64 playbackPositionMs = 6000;
    const QSharedPointer<const QVector<DanmakuController::RenderItem>> snapshot =
        appendSingleComment(commentId, text, 0, playbackPositionMs);
    QVERIFY(snapshot);
    QVERIFY(!snapshot->isEmpty());
    QCOMPARE((*snapshot)[0].commentId, commentId);

    const qreal expectedSpeed = 120.0 + (qHash(commentId) % 70);
    const qreal expectedX = (1280.0 + 12.0) - expectedSpeed * (playbackPositionMs / 1000.0);
    QVERIFY2(
        std::abs((*snapshot)[0].x - expectedX) < 0.5,
        "seek-resumed comment should be positioned as if it had been flowing before the seek");
}

int DanmakuTextWidthTest::requiredBubbleWidth(const QString &text) {
    QFont font;
    font.setPixelSize(DanmakuRenderStyle::kTextPixelSize);
    QFontMetrics metrics(font);
    const int textWidth = metrics.horizontalAdvance(text);
    const int paddedWidth = textWidth + DanmakuRenderStyle::kHorizontalPaddingPx * 2;
    return std::max(DanmakuRenderStyle::kMinWidthPx, paddedWidth);
}

QSharedPointer<const QVector<DanmakuController::RenderItem>> DanmakuTextWidthTest::appendSingleComment(
    const QString &commentId,
    const QString &text,
    qint64 atMs,
    qint64 playbackPositionMs) {
    DanmakuController controller;
    controller.setGlyphWarmupEnabled(false);
    controller.setViewportSize(1280.0, 720.0);
    controller.setLaneMetrics(36, 6);
    controller.setPlaybackPaused(true);

    QVariantMap comment;
    comment.insert(QStringLiteral("comment_id"), commentId);
    comment.insert(QStringLiteral("user_id"), QStringLiteral("width-test-user"));
    comment.insert(QStringLiteral("text"), text);
    comment.insert(QStringLiteral("at_ms"), atMs);

    QVariantList comments;
    comments.push_back(comment);
    controller.appendFromCore(comments, playbackPositionMs);

    QCoreApplication::processEvents();

    return controller.renderSnapshot();
}

QTEST_MAIN(DanmakuTextWidthTest)

#include "danmaku_text_width_test.moc"
