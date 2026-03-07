#include "danmaku/DanmakuController.hpp"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTest>
#include <QVariantList>
#include <QVariantMap>

#include <cmath>

namespace {

QVariantMap makeComment(const QString &commentId, const QString &userId, const QString &text) {
    QVariantMap comment;
    comment.insert(QStringLiteral("comment_id"), commentId);
    comment.insert(QStringLiteral("user_id"), userId);
    comment.insert(QStringLiteral("text"), text);
    comment.insert(QStringLiteral("at_ms"), 0);
    return comment;
}

const DanmakuController::RenderItem *findItem(
    const QVector<DanmakuController::RenderItem> &items, const QString &commentId) {
    for (const DanmakuController::RenderItem &item : items) {
        if (item.commentId == commentId) {
            return &item;
        }
    }
    return nullptr;
}

const DanmakuController::RenderItem *findItem(
    const QSharedPointer<const QVector<DanmakuController::RenderItem>> &snapshot,
    const QString &commentId) {
    if (!snapshot) {
        return nullptr;
    }
    return findItem(*snapshot, commentId);
}

double itemAlpha(DanmakuController &controller, const QString &commentId) {
    const auto snapshot = controller.renderSnapshot();
    const auto *item = findItem(snapshot, commentId);
    return item ? item->alpha : -1.0;
}

} // namespace

class DanmakuNgDropTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void pendingNgFadeRollbackRestoresDraggedComment();
};

void DanmakuNgDropTest::initTestCase() {
    qputenv("NICONEON_DANMAKU_WORKER", "off");
    qputenv("NICONEON_SIMD_MODE", "scalar");
}

void DanmakuNgDropTest::pendingNgFadeRollbackRestoresDraggedComment() {
    DanmakuController controller;
    controller.setGlyphWarmupEnabled(false);
    controller.setViewportSize(1280.0, 720.0);
    controller.setLaneMetrics(36, 6);
    controller.setPlaybackPaused(true);

    QVariantList comments;
    comments.push_back(makeComment(QStringLiteral("dragged"), QStringLiteral("u1"), QStringLiteral("drag me")));
    comments.push_back(makeComment(QStringLiteral("same-user"), QStringLiteral("u1"), QStringLiteral("same user")));
    comments.push_back(makeComment(QStringLiteral("other-user"), QStringLiteral("u2"), QStringLiteral("other")));

    controller.appendFromCore(comments, 0);
    QCoreApplication::processEvents();

    const auto initial = controller.renderSnapshot();
    QVERIFY(initial);

    const auto *draggedBefore = findItem(initial, QStringLiteral("dragged"));
    const auto *sameUserBefore = findItem(initial, QStringLiteral("same-user"));
    QVERIFY(draggedBefore);
    QVERIFY(sameUserBefore);

    const qreal originalY = draggedBefore->y;

    QSignalSpy ngSpy(&controller, &DanmakuController::ngDropRequested);
    QVERIFY(controller.beginDragAt(draggedBefore->x + 8.0, draggedBefore->y + 8.0));
    controller.moveActiveDrag(120.0, 420.0);
    controller.dropActiveDrag(true);

    QCOMPARE(ngSpy.count(), 1);
    QCOMPARE(ngSpy.takeFirst().value(0).toString(), QStringLiteral("u1"));

    QTRY_VERIFY_WITH_TIMEOUT(itemAlpha(controller, QStringLiteral("dragged")) < 1.0, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(itemAlpha(controller, QStringLiteral("same-user")) < 1.0, 1000);

    controller.rollbackPendingNgUserFade(QStringLiteral("u1"));

    QTRY_VERIFY_WITH_TIMEOUT(std::abs(itemAlpha(controller, QStringLiteral("dragged")) - 1.0) < 0.01, 1000);
    QTRY_VERIFY_WITH_TIMEOUT(std::abs(itemAlpha(controller, QStringLiteral("same-user")) - 1.0) < 0.01, 1000);

    const auto restored = controller.renderSnapshot();
    QVERIFY(restored);
    const auto *draggedAfter = findItem(restored, QStringLiteral("dragged"));
    const auto *sameUserAfter = findItem(restored, QStringLiteral("same-user"));
    QVERIFY(draggedAfter);
    QVERIFY(sameUserAfter);
    QVERIFY(std::abs(draggedAfter->y - originalY) < 0.5);
    QVERIFY(!draggedAfter->ngDropHovered);
    QVERIFY(!sameUserAfter->ngDropHovered);
}

QTEST_MAIN(DanmakuNgDropTest)

#include "danmaku_ng_drop_test.moc"
