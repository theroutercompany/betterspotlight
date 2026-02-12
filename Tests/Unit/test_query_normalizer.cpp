#include <QtTest/QtTest>

#include "core/query/query_normalizer.h"

class TestQueryNormalizer : public QObject {
    Q_OBJECT

private slots:
    void testLowercaseTrimAndSpaceCollapse();
    void testOuterQuoteStripping();
    void testNoisePunctuationAndDashNormalization();
};

void TestQueryNormalizer::testLowercaseTrimAndSpaceCollapse()
{
    const bs::NormalizedQuery result = bs::QueryNormalizer::normalize(
        QStringLiteral("   Hello    WORLD   "));
    QCOMPARE(result.original, QStringLiteral("   Hello    WORLD   "));
    QCOMPARE(result.normalized, QStringLiteral("hello world"));
}

void TestQueryNormalizer::testOuterQuoteStripping()
{
    const bs::NormalizedQuery doubleQuoted =
        bs::QueryNormalizer::normalize(QStringLiteral("  \"Project Plan\"  "));
    QCOMPARE(doubleQuoted.normalized, QStringLiteral("project plan"));

    const bs::NormalizedQuery singleQuoted =
        bs::QueryNormalizer::normalize(QStringLiteral("'Task-List'"));
    QCOMPARE(singleQuoted.normalized, QStringLiteral("task-list"));

    const bs::NormalizedQuery singleChar = bs::QueryNormalizer::normalize(QStringLiteral("\""));
    QCOMPARE(singleChar.normalized, QStringLiteral(""));
}

void TestQueryNormalizer::testNoisePunctuationAndDashNormalization()
{
    const QString raw = QStringLiteral("  [Alpha]  --  Beta  —  Gamma !!  ");
    const bs::NormalizedQuery result = bs::QueryNormalizer::normalize(raw);
    QCOMPARE(result.normalized, QStringLiteral("alpha-beta-gamma"));

    const bs::NormalizedQuery mixed =
        bs::QueryNormalizer::normalize(QStringLiteral("`R&D` @Home ## Focus"));
    QCOMPARE(mixed.normalized, QStringLiteral("rd home focus"));
}

QTEST_MAIN(TestQueryNormalizer)
#include "test_query_normalizer.moc"
