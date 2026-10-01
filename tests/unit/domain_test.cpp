#include "comments/SyntheticComments.hpp"
#include "domain/Domain.hpp"

#include <QtTest>

using namespace niconeon;

namespace {
class Environment {
  public:
    Environment(const char *name, const QByteArray &value)
        : m_name(name), m_value(qgetenv(name)), m_set(qEnvironmentVariableIsSet(name)) {
        qputenv(name, value);
    }
    ~Environment() {
        if (m_set)
            qputenv(m_name.constData(), m_value);
        else
            qunsetenv(m_name.constData());
    }

  private:
    QByteArray m_name, m_value;
    bool m_set;
};
} // namespace

class DomainTest : public QObject {
    Q_OBJECT
  private slots:
    void extractsVideoIdFromBasename() {
        QCOMPARE(extractVideoId(QStringLiteral("/tmp/abc_sm9_test.mp4")).value(), QStringLiteral("sm9"));
        QCOMPARE(extractVideoId(QStringLiteral("/tmp/SO1234.mkv")).value(), QStringLiteral("so1234"));
        QCOMPARE(extractVideoId(QStringLiteral("clip_NM42.mkv")).value(), QStringLiteral("nm42"));
        QVERIFY(!extractVideoId(QStringLiteral("/tmp/no-id.mp4")));
        QVERIFY(!extractVideoId(QStringLiteral("/tmp/sm9/movie.mp4")));
    }

    void profilesMatchRustDefaultsAndClamps() {
        const auto high = makeRuntimeProfile(RuntimeProfile::High);
        const auto balanced = makeRuntimeProfile(RuntimeProfile::Balanced);
        const auto low = makeRuntimeProfile(RuntimeProfile::LowSpec);
        QCOMPARE(high.targetFps, 60);
        QCOMPARE(high.maxEmitPerTick, 0);
        QCOMPARE(balanced.targetFps, 60);
        QCOMPARE(balanced.maxEmitPerTick, 96);
        QVERIFY(!balanced.coalesceSameContent);
        QCOMPARE(low.targetFps, 60);
        QCOMPARE(low.maxEmitPerTick, 48);
        QVERIFY(low.coalesceSameContent);
        QCOMPARE(*parseRuntimeProfile(QStringLiteral(" LOWSPEC ")), RuntimeProfile::LowSpec);
        QVERIFY(!parseRuntimeProfile(QStringLiteral("unknown")));
        const auto minimum = makeRuntimeProfile(RuntimeProfile::LowSpec, {1, 0, false});
        QCOMPARE(minimum.targetFps, 10);
        QCOMPARE(minimum.maxEmitPerTick, 0);
        QVERIFY(!minimum.coalesceSameContent);
        const auto maximum = makeRuntimeProfile(RuntimeProfile::High, {500, 5000, true});
        QCOMPARE(maximum.targetFps, 120);
        QCOMPARE(maximum.maxEmitPerTick, 2000);
        QVERIFY(maximum.coalesceSameContent);
        QCOMPARE(commentSourceName(CommentSource::Cache), QStringLiteral("cache"));
        QCOMPARE(profileName(RuntimeProfile::LowSpec), QStringLiteral("low_spec"));
    }

    void validatesCountTextUserAndPayloadBounds() {
        CommentList comments{{QStringLiteral("日本語-id"), 0, QStringLiteral("user"), QStringLiteral("こんにちは😄")}};
        QVERIFY(validateComments(comments));
        comments[0].text = QString(MaxTextBytes + 1, QLatin1Char('a'));
        QVERIFY(!validateComments(comments));
        comments[0].text = QString(6000, QChar(0x3042));
        QVERIFY(!validateComments(comments));
        comments[0].text = QStringLiteral("small");
        comments[0].userId = QString(MaxUserIdBytes + 1, QLatin1Char('a'));
        QVERIFY(!validateComments(comments));
        comments[0].userId = QStringLiteral("user");
        comments.resize(MaxComments + 1);
        QVERIFY(!validateComments(comments));
        // 7000 distinct records can exceed the aggregate budget despite valid text sizes.
        comments = CommentList(7000, {QStringLiteral("id"), 0, QStringLiteral("u"), QString(10000, QLatin1Char('a'))});
        QVERIFY(!validateComments(comments));
        // JSON escapes must count too, not just the unescaped UTF-8 text length.
        comments = CommentList(1000, {QStringLiteral("id"), 0, QStringLiteral("u"), QString(16000, QChar(1))});
        QVERIFY(!validateComments(comments));
    }

    void syntheticRampPreservesFixture() {
        Environment mode("NICONEON_SYNTHETIC_COMMENTS", "RaMp");
        Environment duration("NICONEON_SYNTHETIC_DURATION_SEC", "3");
        Environment base("NICONEON_SYNTHETIC_BASE_PER_SEC", "1");
        Environment ramp("NICONEON_SYNTHETIC_RAMP_PER_SEC", "1");
        Environment maximum("NICONEON_SYNTHETIC_MAX_PER_SEC", "9");
        Environment users("NICONEON_SYNTHETIC_USER_SPAN", "2");
        QVERIFY(syntheticCommentModeEnabled());
        auto generated = generateSyntheticComments(QStringLiteral("sm9"));
        QVERIFY(generated);
        QCOMPARE(generated->size(), 6);
        QCOMPARE((*generated)[0].commentId, QStringLiteral("sm9-dummy-0-0"));
        QCOMPARE((*generated)[2].atMs, 1500);
        QCOMPARE((*generated)[4].atMs, 2333);
        QCOMPARE((*generated)[1].userId, QStringLiteral("dummy-user-1"));
        QCOMPARE((*generated)[5].text, QStringLiteral("dummy comment 2-2 / 3cps"));
    }

    void syntheticOversizeFailsBeforeAllocation() {
        Environment duration("NICONEON_SYNTHETIC_DURATION_SEC", "3600");
        Environment base("NICONEON_SYNTHETIC_BASE_PER_SEC", "500");
        Environment ramp("NICONEON_SYNTHETIC_RAMP_PER_SEC", "100");
        Environment maximum("NICONEON_SYNTHETIC_MAX_PER_SEC", "2000");
        const auto generated = generateSyntheticComments(QStringLiteral("sm9"));
        QVERIFY(!generated);
        QVERIFY(generated.error().message.contains(QStringLiteral("safety limit")));
    }
};

QTEST_GUILESS_MAIN(DomainTest)
#include "domain_test.moc"
