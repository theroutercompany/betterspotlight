#include <QtTest/QtTest>

#include "core/ranking/scorer.h"
#include "core/shared/search_result.h"

class TestQueryRanking : public QObject {
    Q_OBJECT

private slots:
    void testContentBm25RawConvertedToLexicalStrength();
    void testRankResultsUsesPerResultBm25RawScore();
};

void TestQueryRanking::testContentBm25RawConvertedToLexicalStrength()
{
    bs::Scorer scorer;
    bs::QueryContext context;

    bs::SearchResult result;
    result.matchType = bs::MatchType::Content;

    const auto fromNegative = scorer.computeScore(result, context, -4.5);
    QCOMPARE(fromNegative.baseMatchScore,
             4.5 * static_cast<double>(scorer.weights().contentMatchWeight));

    const auto fromPositive = scorer.computeScore(result, context, 4.5);
    QCOMPARE(fromPositive.baseMatchScore, 0.0);
}

void TestQueryRanking::testRankResultsUsesPerResultBm25RawScore()
{
    bs::Scorer scorer;
    bs::QueryContext context;

    bs::SearchResult strong;
    strong.itemId = 1;
    strong.path = QStringLiteral("/tmp/strong.txt");
    strong.matchType = bs::MatchType::Content;
    strong.bm25RawScore = -12.0;

    bs::SearchResult weak;
    weak.itemId = 2;
    weak.path = QStringLiteral("/tmp/weak.txt");
    weak.matchType = bs::MatchType::Content;
    weak.bm25RawScore = -1.0;

    std::vector<bs::SearchResult> results = { weak, strong };
    scorer.rankResults(results, context);

    QCOMPARE(results.at(0).itemId, static_cast<int64_t>(1));
    QVERIFY(results.at(0).score > results.at(1).score);
}

QTEST_MAIN(TestQueryRanking)
#include "test_query_ranking.moc"
