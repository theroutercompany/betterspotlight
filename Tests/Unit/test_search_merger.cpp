#include <QtTest/QtTest>
#include "core/vector/search_merger.h"
#include "core/shared/search_result.h"

#include <algorithm>

class TestSearchMerger : public QObject {
    Q_OBJECT

private slots:
    void testEmptyInputs();
    void testMergeLexicalOnly();
    void testMergeSemanticOnly();
    void testMergeBothSources();
    void testWeightsApplied();
    void testSimilarityThreshold();
    void testMaxResultsRespected();
    void testNormalization();
    void testNormalizeLexicalScore();
    void testNormalizeSemanticScore();
    void testCategoryBoth();

private:
    static bs::SearchResult makeLexicalResult(int64_t itemId, double score);
};

bs::SearchResult TestSearchMerger::makeLexicalResult(int64_t itemId, double score)
{
    bs::SearchResult result;
    result.itemId = itemId;
    result.path = QStringLiteral("/tmp/file_%1.txt").arg(itemId);
    result.name = QStringLiteral("file_%1.txt").arg(itemId);
    result.matchType = bs::MatchType::Content;
    result.score = score;
    return result;
}

void TestSearchMerger::testEmptyInputs()
{
    const std::vector<bs::SearchResult> lexical;
    const std::vector<bs::SemanticResult> semantic;

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, semantic);
    QVERIFY(merged.empty());
}

void TestSearchMerger::testMergeLexicalOnly()
{
    const std::vector<bs::SearchResult> lexical = {
        makeLexicalResult(1, 120.0),
        makeLexicalResult(2, 80.0),
    };
    const std::vector<bs::SemanticResult> semantic;

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, semantic);
    QCOMPARE(static_cast<int>(merged.size()), 2);
    QCOMPARE(merged[0].itemId, static_cast<int64_t>(1));
}

void TestSearchMerger::testMergeSemanticOnly()
{
    const std::vector<bs::SearchResult> lexical;
    const std::vector<bs::SemanticResult> semantic = {
        {10, 0.92F},
        {11, 0.83F},
    };

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, semantic);
    QCOMPARE(static_cast<int>(merged.size()), 2);
    QVERIFY(merged[0].itemId == 10 || merged[0].itemId == 11);
}

void TestSearchMerger::testMergeBothSources()
{
    const std::vector<bs::SearchResult> lexical = {
        makeLexicalResult(1, 150.0),
        makeLexicalResult(2, 80.0),
    };
    const std::vector<bs::SemanticResult> semantic = {
        {2, 0.95F},
        {3, 0.90F},
    };

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, semantic);
    QCOMPARE(static_cast<int>(merged.size()), 3);

    const bool hasLexical = std::any_of(merged.begin(), merged.end(),
        [](const bs::SearchResult& result) { return result.itemId == 1; });
    const bool hasSemanticOnly = std::any_of(merged.begin(), merged.end(),
        [](const bs::SearchResult& result) { return result.itemId == 3; });
    QVERIFY(hasLexical);
    QVERIFY(hasSemanticOnly);
}

void TestSearchMerger::testWeightsApplied()
{
    const std::vector<bs::SearchResult> lexical = {
        makeLexicalResult(1, 100.0),
    };
    const std::vector<bs::SemanticResult> semantic = {
        {1, 0.90F},
    };

    bs::MergeConfig config;
    config.lexicalWeight = 0.6F;
    config.semanticWeight = 0.4F;
    config.similarityThreshold = 0.7F;
    config.rrfK = 60;

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, semantic, config);
    QCOMPARE(static_cast<int>(merged.size()), 1);

    const double expected = (0.6 / 61.0) + (0.4 / 61.0);
    QVERIFY(std::fabs(merged[0].score - expected) < 0.0001);
}

void TestSearchMerger::testSimilarityThreshold()
{
    const std::vector<bs::SearchResult> lexical = {
        makeLexicalResult(42, 200.0),
    };
    const std::vector<bs::SemanticResult> semantic = {
        {42, 0.50F},
    };

    bs::MergeConfig config;
    config.similarityThreshold = 0.7F;

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, semantic, config);
    QCOMPARE(static_cast<int>(merged.size()), 1);
    QCOMPARE(merged[0].itemId, static_cast<int64_t>(42));
    QVERIFY(merged[0].score > 0.0);
}

void TestSearchMerger::testMaxResultsRespected()
{
    std::vector<bs::SearchResult> lexical;
    lexical.reserve(100);
    for (int i = 0; i < 100; ++i) {
        lexical.push_back(makeLexicalResult(i + 1, static_cast<double>(100 - i)));
    }

    bs::MergeConfig config;
    config.maxResults = 20;
    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, {}, config);
    QCOMPARE(static_cast<int>(merged.size()), 20);
}

void TestSearchMerger::testNormalization()
{
    const float lexical = bs::SearchMerger::normalizeLexicalScore(150.0F, 200.0F);
    const float semantic = bs::SearchMerger::normalizeSemanticScore(0.9F, 0.7F);

    QVERIFY(lexical >= 0.0F && lexical <= 1.0F);
    QVERIFY(semantic >= 0.0F && semantic <= 1.0F);
}

void TestSearchMerger::testNormalizeLexicalScore()
{
    QCOMPARE(bs::SearchMerger::normalizeLexicalScore(100.0F, 200.0F), 0.5F);
    QCOMPARE(bs::SearchMerger::normalizeLexicalScore(200.0F, 200.0F), 1.0F);
    QCOMPARE(bs::SearchMerger::normalizeLexicalScore(0.0F, 200.0F), 0.0F);
}

void TestSearchMerger::testNormalizeSemanticScore()
{
    const float above = bs::SearchMerger::normalizeSemanticScore(0.9F, 0.7F);
    QVERIFY(above > 0.0F);
    QVERIFY(above <= 1.0F);

    const float atThreshold = bs::SearchMerger::normalizeSemanticScore(0.7F, 0.7F);
    QVERIFY(atThreshold >= 0.0F);

    const float below = bs::SearchMerger::normalizeSemanticScore(0.5F, 0.7F);
    QCOMPARE(below, 0.0F);
}

void TestSearchMerger::testCategoryBoth()
{
    const std::vector<bs::SearchResult> lexical = {
        makeLexicalResult(42, 100.0),
    };
    const std::vector<bs::SemanticResult> semantic = {
        {42, 0.95F},
    };

    const std::vector<bs::SearchResult> merged = bs::SearchMerger::merge(lexical, semantic);
    QCOMPARE(static_cast<int>(merged.size()), 1);
    QCOMPARE(merged[0].itemId, static_cast<int64_t>(42));
    QVERIFY(merged[0].score > 0.0);
}

QTEST_MAIN(TestSearchMerger)
#include "test_search_merger.moc"
