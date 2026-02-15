#include "danmaku/DanmakuController.hpp"
#include "danmaku/DanmakuRenderStyle.hpp"

#include <QCoreApplication>
#include <QFont>
#include <QFontMetrics>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <algorithm>

class DanmakuTextWidthTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void wideFullwidthTextDoesNotUnderestimate();
    void japaneseTextDoesNotUnderestimate();
    void minimumWidthIsPreserved();

private:
    static int requiredBubbleWidth(const QString &text);
    static QSharedPointer<const QVector<DanmakuController::RenderItem>> appendSingleComment(
        const QString &text);
};

void DanmakuTextWidthTest::initTestCase() {
    qputenv("NICONEON_DANMAKU_WORKER", "off");
    qputenv("NICONEON_SIMD_MODE", "scalar");
}

void DanmakuTextWidthTest::wideFullwidthTextDoesNotUnderestimate() {
    const QString text = QStringLiteral("ｗｗｗｗｗｗｗｗｗｗ");
    const QSharedPointer<const QVector<DanmakuController::RenderItem>> snapshot = appendSingleComment(text);
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
    const QSharedPointer<const QVector<DanmakuController::RenderItem>> snapshot = appendSingleComment(text);
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
    const QSharedPointer<const QVector<DanmakuController::RenderItem>> snapshot = appendSingleComment(text);
    QVERIFY(snapshot);
    QVERIFY(!snapshot->isEmpty());
    QCOMPARE((*snapshot)[0].text, text);
    const int actualWidth = (*snapshot)[0].widthEstimate;
    QCOMPARE(actualWidth, DanmakuRenderStyle::kMinWidthPx);
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
    const QString &text) {
    DanmakuController controller;
    controller.setViewportSize(1280.0, 720.0);
    controller.setLaneMetrics(36, 6);
    controller.setPlaybackPaused(true);

    QVariantMap comment;
    comment.insert(QStringLiteral("comment_id"), QStringLiteral("width-test-comment"));
    comment.insert(QStringLiteral("user_id"), QStringLiteral("width-test-user"));
    comment.insert(QStringLiteral("text"), text);
    comment.insert(QStringLiteral("at_ms"), 0);

    QVariantList comments;
    comments.push_back(comment);
    controller.appendFromCore(comments, 0);

    QCoreApplication::processEvents();

    return controller.renderSnapshot();
}

QTEST_MAIN(DanmakuTextWidthTest)

#include "danmaku_text_width_test.moc"
