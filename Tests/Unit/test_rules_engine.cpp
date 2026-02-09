#include <QtTest/QtTest>

#include "core/query/rules_engine.h"

class TestRulesEngine : public QObject {
    Q_OBJECT

private slots:
    void testNaturalLanguageQuery();
    void testSimpleQuery();
    void testNluConfidenceZero();
};

void TestRulesEngine::testNaturalLanguageQuery()
{
    auto sq = bs::RulesEngine::analyze(
        QStringLiteral("that summer when Alex went to the Gobi Desert"));

    // Should have entities (Alex and Gobi Desert at minimum)
    QVERIFY(!sq.entities.empty());

    // Should have temporal (summer)
    QVERIFY(sq.temporal.has_value());

    // Key tokens should be populated (excluding stopwords and short tokens)
    QVERIFY(!sq.keyTokens.empty());

    // Original query preserved
    QCOMPARE(sq.originalQuery,
             QStringLiteral("that summer when Alex went to the Gobi Desert"));
}

void TestRulesEngine::testSimpleQuery()
{
    auto sq = bs::RulesEngine::analyze(QStringLiteral("readme"));

    QCOMPARE(sq.originalQuery, QStringLiteral("readme"));
    QVERIFY(!sq.cleanedQuery.isEmpty());
    QVERIFY(sq.entities.empty());
    QVERIFY(!sq.temporal.has_value());
    QCOMPARE(sq.nluConfidence, 0.0f);
}

void TestRulesEngine::testNluConfidenceZero()
{
    auto sq1 = bs::RulesEngine::analyze(QStringLiteral("complex query with entities"));
    QCOMPARE(sq1.nluConfidence, 0.0f);

    auto sq2 = bs::RulesEngine::analyze(QStringLiteral("january 2023 report"));
    QCOMPARE(sq2.nluConfidence, 0.0f);

    auto sq3 = bs::RulesEngine::analyze(QString());
    QCOMPARE(sq3.nluConfidence, 0.0f);
}

QTEST_MAIN(TestRulesEngine)
#include "test_rules_engine.moc"
